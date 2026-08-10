using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Threading;
using System.Threading.Tasks;

namespace Jellyfin.Plugin.CensorCut.ServerSide;

/// <summary>
/// The source's codec details, needed so a re-encoded boundary chunk
/// concatenates cleanly with the untouched remainder.
/// </summary>
/// <param name="VideoCodec">Source video codec name as ffprobe reports it.</param>
/// <param name="PixelFormat">Source pixel format, or null if unknown.</param>
/// <param name="AudioCodec">Source audio codec name, or null when there is no audio.</param>
public sealed record SourceFormat(string VideoCodec, string? PixelFormat, string? AudioCodec);

/// <summary>
/// Re-encodes the partial group of pictures at a resume point.
/// </summary>
public static class BoundaryRenderer
{
    /// <summary>
    /// Encoders that produce a stream the concat demuxer will splice onto the
    /// same codec from the original.
    /// </summary>
    /// <remarks>
    /// A chunk encoded in a <i>different</i> codec from the source was measured
    /// to corrupt the following entry's in/out points — the second entry played
    /// far past its outpoint. Matching the codec is therefore a correctness
    /// requirement, not a quality preference, and an unmapped codec means no
    /// censored version rather than a wrong one.
    /// </remarks>
    private static readonly IReadOnlyDictionary<string, string> _videoEncoders =
        new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase)
        {
            ["h264"] = "libx264",
            ["hevc"] = "libx265",
            ["mpeg4"] = "mpeg4",
            ["mpeg2video"] = "mpeg2video",
            ["vp8"] = "libvpx",
            ["vp9"] = "libvpx-vp9",
            ["av1"] = "libsvtav1",
        };

    private static readonly IReadOnlyDictionary<string, string> _audioEncoders =
        new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase)
        {
            ["aac"] = "aac",
            ["ac3"] = "ac3",
            ["eac3"] = "eac3",
            ["mp3"] = "libmp3lame",
            ["opus"] = "libopus",
            ["vorbis"] = "libvorbis",
            ["flac"] = "flac",
        };

    /// <summary>
    /// Returns the container a chunk must use: the same one as the source.
    /// </summary>
    /// <remarks>
    /// Not a free choice. A chunk in a different container carries a different
    /// timebase — matroska counts in milliseconds, mp4 usually does not — and
    /// splicing one between two references to the source was measured to
    /// produce non-monotonic timestamps and silently drop the whole chunk from
    /// the output. Matching the source's container keeps the timeline
    /// consistent across the splice.
    /// </remarks>
    /// <param name="mediaPath">Path to the source movie.</param>
    /// <returns>The chunk file extension, including the dot.</returns>
    public static string ChunkExtensionFor(string mediaPath)
    {
        var extension = Path.GetExtension(mediaPath);
        return string.IsNullOrEmpty(extension) ? ".mkv" : extension;
    }

    /// <summary>
    /// Returns true when a source's codecs can be re-encoded into a chunk that
    /// will splice correctly.
    /// </summary>
    /// <param name="format">The probed source format.</param>
    /// <returns>True when rendering is safe.</returns>
    public static bool CanRender(SourceFormat? format)
        => format is not null
           && _videoEncoders.ContainsKey(format.VideoCodec)
           && (format.AudioCodec is null || _audioEncoders.ContainsKey(format.AudioCodec));

    /// <summary>
    /// Builds the ffmpeg arguments that render one boundary chunk.
    /// </summary>
    /// <param name="mediaPath">Path to the source movie.</param>
    /// <param name="entry">The range to render.</param>
    /// <param name="format">The probed source format.</param>
    /// <param name="outputPath">Where to write the chunk.</param>
    /// <returns>The argument list, without the executable itself.</returns>
    public static IReadOnlyList<string> BuildArgs(
        string mediaPath,
        PlanEntry entry,
        SourceFormat format,
        string outputPath)
    {
        ArgumentException.ThrowIfNullOrEmpty(mediaPath);
        ArgumentNullException.ThrowIfNull(format);
        ArgumentException.ThrowIfNullOrEmpty(outputPath);

        if (!CanRender(format))
        {
            throw new NotSupportedException(
                $"No matching encoder for video '{format.VideoCodec}' / audio '{format.AudioCodec}'.");
        }

        var args = new List<string>
        {
            "-nostdin",
            "-y",
            "-v", "error",

            // Input-side seek: ffmpeg decodes from the preceding keyframe and
            // discards, so the first output frame is the exact one asked for.
            "-ss", Seconds(entry.StartMs),
            "-i", mediaPath,
            "-t", Seconds(entry.DurationMs),

            "-map", "0:v:0",
            "-c:v", _videoEncoders[format.VideoCodec],
        };

        if (!string.IsNullOrEmpty(format.PixelFormat))
        {
            // A pixel-format change would alter the stream parameters the
            // following entry is spliced onto.
            args.Add("-pix_fmt");
            args.Add(format.PixelFormat);
        }

        if (format.AudioCodec is null)
        {
            args.Add("-an");
        }
        else
        {
            args.Add("-map");
            args.Add("0:a:0?");
            args.Add("-c:a");
            args.Add(_audioEncoders[format.AudioCodec]);
        }

        args.Add(outputPath);
        return args;
    }

    /// <summary>
    /// Probes the source's codec details.
    /// </summary>
    /// <param name="ffprobePath">Path to ffprobe.</param>
    /// <param name="mediaPath">Path to the movie.</param>
    /// <param name="cancellationToken">Abort token.</param>
    /// <returns>The format, or null when it could not be determined.</returns>
    public static async Task<SourceFormat?> ProbeFormatAsync(
        string ffprobePath,
        string mediaPath,
        CancellationToken cancellationToken)
    {
        var video = await RunAsync(
            ffprobePath,
            new[]
            {
                "-v", "error", "-select_streams", "v:0",
                "-show_entries", "stream=codec_name,pix_fmt",
                "-of", "default=nw=1:nk=1", mediaPath,
            },
            cancellationToken).ConfigureAwait(false);

        if (video is null)
        {
            return null;
        }

        var videoLines = video.Split('\n', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);
        if (videoLines.Length == 0)
        {
            return null;
        }

        var audio = await RunAsync(
            ffprobePath,
            new[]
            {
                "-v", "error", "-select_streams", "a:0",
                "-show_entries", "stream=codec_name",
                "-of", "default=nw=1:nk=1", mediaPath,
            },
            cancellationToken).ConfigureAwait(false);

        var audioLines = (audio ?? string.Empty)
            .Split('\n', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);

        return new SourceFormat(
            videoLines[0],
            videoLines.Length > 1 ? videoLines[1] : null,
            audioLines.Length > 0 ? audioLines[0] : null);
    }

    /// <summary>
    /// Renders one chunk to disk.
    /// </summary>
    /// <param name="ffmpegPath">Path to ffmpeg.</param>
    /// <param name="mediaPath">Path to the movie.</param>
    /// <param name="entry">The range to render.</param>
    /// <param name="format">The probed source format.</param>
    /// <param name="outputPath">Where to write the chunk.</param>
    /// <param name="cancellationToken">Abort token.</param>
    /// <returns>True when the chunk was written.</returns>
    public static async Task<bool> RenderAsync(
        string ffmpegPath,
        string mediaPath,
        PlanEntry entry,
        SourceFormat format,
        string outputPath,
        CancellationToken cancellationToken)
    {
        var args = BuildArgs(mediaPath, entry, format, outputPath);
        var startInfo = new ProcessStartInfo
        {
            FileName = ffmpegPath,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            UseShellExecute = false,
            CreateNoWindow = true,
        };

        foreach (var arg in args)
        {
            startInfo.ArgumentList.Add(arg);
        }

        using var process = new Process { StartInfo = startInfo };
        process.Start();
        await process.StandardError.ReadToEndAsync(cancellationToken).ConfigureAwait(false);
        await process.WaitForExitAsync(cancellationToken).ConfigureAwait(false);

        return process.ExitCode == 0 && File.Exists(outputPath) && new FileInfo(outputPath).Length > 0;
    }

    private static async Task<string?> RunAsync(
        string exePath,
        IReadOnlyList<string> args,
        CancellationToken cancellationToken)
    {
        var startInfo = new ProcessStartInfo
        {
            FileName = exePath,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            UseShellExecute = false,
            CreateNoWindow = true,
        };

        foreach (var arg in args)
        {
            startInfo.ArgumentList.Add(arg);
        }

        using var process = new Process { StartInfo = startInfo };
        process.Start();
        var stdout = await process.StandardOutput.ReadToEndAsync(cancellationToken).ConfigureAwait(false);
        await process.WaitForExitAsync(cancellationToken).ConfigureAwait(false);

        return process.ExitCode == 0 ? stdout : null;
    }

    private static string Seconds(long ms) =>
        (ms / 1000.0).ToString("0.###", CultureInfo.InvariantCulture);
}
