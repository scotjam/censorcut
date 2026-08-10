using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using Jellyfin.Plugin.CensorCut.Configuration;
using Jellyfin.Plugin.CensorCut.ServerSide;
using MediaBrowser.Controller.Entities;
using MediaBrowser.Controller.Library;
using MediaBrowser.Controller.MediaEncoding;
using MediaBrowser.Model.Dto;
using MediaBrowser.Model.MediaInfo;
using Microsoft.Extensions.Logging;

namespace Jellyfin.Plugin.CensorCut;

/// <summary>
/// Offers a censored version of a movie as an extra media source, cut by the
/// server rather than by the client.
/// </summary>
/// <remarks>
/// The source points at a generated concat script listing only the ranges to
/// keep. Direct play and direct stream are refused for it, so Jellyfin always
/// runs it through ffmpeg — which means the stream leaving the server is
/// already cut and no client can opt out of that. This is the difference
/// between failing open and failing closed.
/// </remarks>
public class CensorCutMediaSourceProvider : IMediaSourceProvider
{
    private readonly IMediaEncoder _mediaEncoder;
    private readonly ILogger<CensorCutMediaSourceProvider> _logger;

    /// <summary>
    /// Initializes a new instance of the <see cref="CensorCutMediaSourceProvider"/> class.
    /// </summary>
    /// <param name="mediaEncoder">Supplies the path to ffprobe.</param>
    /// <param name="logger">The logger.</param>
    public CensorCutMediaSourceProvider(
        IMediaEncoder mediaEncoder,
        ILogger<CensorCutMediaSourceProvider> logger)
    {
        _mediaEncoder = mediaEncoder;
        _logger = logger;
    }

    /// <inheritdoc />
    public async Task<IEnumerable<MediaSourceInfo>> GetMediaSources(
        BaseItem item,
        CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(item);

        var config = CensorCutPlugin.Instance?.Configuration;
        if (config is null || config.Mode != CensorCutMode.ServerSide)
        {
            return Array.Empty<MediaSourceInfo>();
        }

        if (item is not Video || string.IsNullOrEmpty(item.Path))
        {
            return Array.Empty<MediaSourceInfo>();
        }

        var list = EditList.LoadFor(item.Path);
        var profile = list?.PickProfile(config.ProfileId);
        if (list is null || profile is null || profile.Cuts.Count == 0)
        {
            return Array.Empty<MediaSourceInfo>();
        }

        var durationMs = list.DurationMs > 0
            ? list.DurationMs
            : (item.RunTimeTicks ?? 0) / TimeSpan.TicksPerMillisecond;
        if (durationMs <= 0)
        {
            _logger.LogWarning("CensorCut: unknown duration for {Path}; skipping", item.Path);
            return Array.Empty<MediaSourceInfo>();
        }

        var leadInMs = config.LeadInOverrideMs > 0 ? config.LeadInOverrideMs : profile.LeadInMs;

        try
        {
            var built = await BuildScriptAsync(
                item.Path, profile, durationMs, leadInMs, config, cancellationToken)
                .ConfigureAwait(false);

            if (built is null)
            {
                return Array.Empty<MediaSourceInfo>();
            }

            var (scriptPath, keeps) = built.Value;
            var keptMs = keeps.Sum(k => k.DurationMs);

            var source = new MediaSourceInfo
            {
                Id = DeterministicSourceId(item.Id, profile.Id),
                Path = scriptPath,
                Protocol = MediaProtocol.File,
                Name = string.IsNullOrEmpty(profile.Label)
                    ? "Censored"
                    : "Censored — " + profile.Label,
                IsRemote = false,
                RunTimeTicks = keptMs * TimeSpan.TicksPerMillisecond,

                // The whole point: never hand the client the file itself.
                // A concat script is not playable by any client, and even if it
                // were, direct play would bypass the cutting entirely.
                SupportsDirectPlay = false,
                SupportsDirectStream = false,
                SupportsTranscoding = true,
                SupportsProbing = true,
                RequiresOpening = false,
                RequiresClosing = false,
                IsInfiniteStream = false,
            };

            return new[] { source };
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            // The concat script has to sit beside the movie (ffmpeg rejects
            // absolute entries as unsafe), so a read-only media directory is a
            // real possibility and must not take playback down with it.
            _logger.LogError(
                ex,
                "CensorCut: cannot write the concat script beside {Path}; the censored version will not be offered",
                item.Path);
            return Array.Empty<MediaSourceInfo>();
        }
    }

    /// <inheritdoc />
    public Task<ILiveStream> OpenMediaSource(
        string openToken,
        List<ILiveStream> currentLiveStreams,
        CancellationToken cancellationToken)
        => throw new NotSupportedException(
            "CensorCut sources are plain files and are reported with RequiresOpening = false.");

    private async Task<IReadOnlyList<long>> KeyframesAsync(
        string mediaPath,
        EditProfile profile,
        PluginConfiguration config,
        CancellationToken cancellationToken)
    {
        var cutEnds = profile.Cuts
            .Where(c => c.EndMs > c.StartMs)
            .Select(c => c.EndMs)
            .Distinct()
            .OrderBy(ms => ms)
            .ToList();

        var keyframes = await KeyframeProbe.FindKeyframesAsync(
            _mediaEncoder.ProbePath,
            mediaPath,
            cutEnds,
            config.KeyframeSearchWindowSeconds,
            cancellationToken).ConfigureAwait(false);

        if (keyframes.Count == 0)
        {
            // Without a keyframe index an inpoint would snap backward into the
            // scene we are removing. Refusing is the only safe answer.
            _logger.LogError(
                "CensorCut: keyframe probe found nothing for {Path}; refusing to offer a censored version "
                + "rather than risk replaying cut content",
                mediaPath);
        }

        return keyframes;
    }

    private async Task<(string ScriptPath, IReadOnlyList<KeepSegment> Keeps)?> BuildScriptAsync(
        string mediaPath,
        EditProfile profile,
        long durationMs,
        long leadInMs,
        PluginConfiguration config,
        CancellationToken cancellationToken)
    {
        var keyframes = await KeyframesAsync(mediaPath, profile, config, cancellationToken).ConfigureAwait(false);
        if (keyframes.Count == 0)
        {
            return null;
        }

        var keeps = FfconcatBuilder.BuildKeepSegments(profile.Cuts, durationMs, leadInMs, keyframes);
        if (keeps.Count == 0)
        {
            _logger.LogWarning("CensorCut: every second of {Path} is cut; nothing to play", mediaPath);
            return null;
        }

        var fileName = Path.GetFileName(mediaPath);
        var text = FfconcatBuilder.Render(fileName, keeps);

        var scriptPath = Path.Combine(
            Path.GetDirectoryName(mediaPath) ?? string.Empty,
            fileName + "." + profile.Id + ".censorcut.ffconcat");

        // Rewrite only when the content actually changed, so a library scan
        // does not keep touching files in the media directory.
        if (!File.Exists(scriptPath)
            || !string.Equals(await File.ReadAllTextAsync(scriptPath, cancellationToken).ConfigureAwait(false), text, StringComparison.Ordinal))
        {
            await File.WriteAllTextAsync(scriptPath, text, cancellationToken).ConfigureAwait(false);
            _logger.LogInformation(
                "CensorCut: wrote {Script} with {Count} kept ranges",
                scriptPath,
                keeps.Count);
        }

        return (scriptPath, keeps);
    }

    private static string DeterministicSourceId(Guid itemId, string? profileId)
        => string.Create(
            CultureInfo.InvariantCulture,
            $"censorcut_{itemId:N}_{profileId ?? "default"}");
}
