using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

namespace Jellyfin.Plugin.CensorCut.ServerSide;

/// <summary>
/// Finds the source's keyframe positions near each cut, using the ffprobe
/// binary Jellyfin already ships.
/// </summary>
public static class KeyframeProbe
{
    /// <summary>
    /// Probes for keyframes in a window after each of the given times.
    /// </summary>
    /// <remarks>
    /// Only the windows that matter are read. A whole-file keyframe scan means
    /// demuxing the entire movie, which for a feature is tens of seconds;
    /// every keep-segment starts at a cut end, so short windows after those
    /// points are all that is needed.
    /// </remarks>
    /// <param name="ffprobePath">Path to the ffprobe binary.</param>
    /// <param name="mediaPath">Path to the movie.</param>
    /// <param name="afterMs">Times to search forward from, in milliseconds.</param>
    /// <param name="windowSeconds">How far past each time to look.</param>
    /// <param name="cancellationToken">Abort token.</param>
    /// <returns>Ascending keyframe timestamps in milliseconds, deduplicated.</returns>
    public static async Task<IReadOnlyList<long>> FindKeyframesAsync(
        string ffprobePath,
        string mediaPath,
        IReadOnlyList<long> afterMs,
        int windowSeconds,
        CancellationToken cancellationToken)
    {
        ArgumentException.ThrowIfNullOrEmpty(ffprobePath);
        ArgumentException.ThrowIfNullOrEmpty(mediaPath);
        ArgumentNullException.ThrowIfNull(afterMs);

        if (afterMs.Count == 0)
        {
            return Array.Empty<long>();
        }

        var intervals = new StringBuilder();
        foreach (var ms in afterMs)
        {
            if (intervals.Length > 0)
            {
                intervals.Append(',');
            }

            intervals
                .Append((ms / 1000.0).ToString("0.###", CultureInfo.InvariantCulture))
                .Append("%+")
                .Append(windowSeconds.ToString(CultureInfo.InvariantCulture));
        }

        var startInfo = new ProcessStartInfo
        {
            FileName = ffprobePath,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            UseShellExecute = false,
            CreateNoWindow = true,
        };

        foreach (var arg in new[]
                 {
                     "-v", "error",
                     "-select_streams", "v:0",
                     "-skip_frame", "nokey",
                     "-read_intervals", intervals.ToString(),
                     "-show_entries", "frame=pts_time",
                     "-of", "csv=p=0",
                     mediaPath,
                 })
        {
            startInfo.ArgumentList.Add(arg);
        }

        using var process = new Process { StartInfo = startInfo };
        process.Start();

        var stdout = await process.StandardOutput.ReadToEndAsync(cancellationToken).ConfigureAwait(false);
        await process.WaitForExitAsync(cancellationToken).ConfigureAwait(false);

        if (process.ExitCode != 0)
        {
            return Array.Empty<long>();
        }

        return ParseTimes(stdout);
    }

    /// <summary>
    /// Parses ffprobe's csv output into ascending, deduplicated milliseconds.
    /// </summary>
    /// <param name="stdout">Raw ffprobe output.</param>
    /// <returns>Keyframe timestamps in milliseconds.</returns>
    public static IReadOnlyList<long> ParseTimes(string stdout)
    {
        ArgumentNullException.ThrowIfNull(stdout);

        var seen = new SortedSet<long>();
        foreach (var rawLine in stdout.Split('\n'))
        {
            var line = rawLine.Trim().TrimEnd(',');
            if (line.Length == 0)
            {
                continue;
            }

            if (double.TryParse(line, NumberStyles.Float, CultureInfo.InvariantCulture, out var seconds)
                && seconds >= 0)
            {
                seen.Add((long)Math.Round(seconds * 1000.0));
            }
        }

        var result = new List<long>(seen.Count);
        result.AddRange(seen);
        return result;
    }
}
