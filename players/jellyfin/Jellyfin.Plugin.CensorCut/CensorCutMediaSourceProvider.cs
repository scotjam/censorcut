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

            var (scriptPath, plan) = built.Value;
            var keptMs = plan.Sum(e => e.DurationMs);

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
        IReadOnlyList<KeepSegment> keeps,
        PluginConfiguration config,
        CancellationToken cancellationToken)
    {
        // Each keep-segment needs the first keyframe at or after its start, so
        // those are the only windows worth reading. A whole-file keyframe scan
        // means demuxing the entire movie.
        var starts = keeps
            .Where(k => k.DurationMs > 0)
            .Select(k => k.StartMs)
            .Distinct()
            .OrderBy(ms => ms)
            .ToList();

        return await KeyframeProbe.FindKeyframesAsync(
            _mediaEncoder.ProbePath,
            mediaPath,
            starts,
            config.KeyframeSearchWindowSeconds,
            cancellationToken).ConfigureAwait(false);
    }

    private async Task<(string ScriptPath, IReadOnlyList<PlanEntry> Plan)?> BuildScriptAsync(
        string mediaPath,
        EditProfile profile,
        long durationMs,
        long leadInMs,
        PluginConfiguration config,
        CancellationToken cancellationToken)
    {
        var keeps = FfconcatBuilder.BuildKeepSegments(profile.Cuts, durationMs, leadInMs);
        if (keeps.Count == 0)
        {
            _logger.LogWarning("CensorCut: every second of {Path} is cut; nothing to play", mediaPath);
            return null;
        }

        var format = await BoundaryRenderer.ProbeFormatAsync(
            _mediaEncoder.ProbePath, mediaPath, cancellationToken).ConfigureAwait(false);
        if (!BoundaryRenderer.CanRender(format))
        {
            _logger.LogError(
                "CensorCut: no matching encoder for {Path} (video {Video}, audio {Audio}); "
                + "refusing rather than splicing a mismatched chunk",
                mediaPath,
                format?.VideoCodec,
                format?.AudioCodec);
            return null;
        }

        var keyframes = await KeyframesAsync(mediaPath, keeps, config, cancellationToken).ConfigureAwait(false);
        var plan = FfconcatBuilder.BuildPlan(keeps, keyframes);
        if (plan.Count == 0)
        {
            return null;
        }

        var mediaFileName = Path.GetFileName(mediaPath);
        var directory = Path.GetDirectoryName(mediaPath) ?? string.Empty;

        string ChunkName(PlanEntry entry) => string.Create(
            CultureInfo.InvariantCulture,
            $"{mediaFileName}.{profile.Id}.{entry.StartMs}-{entry.EndMs}.censorcut{BoundaryRenderer.ChunkExtensionFor(mediaPath)}");

        // Render the partial GOP at each resume point. Everything else is
        // referenced from the original and never re-encoded.
        foreach (var entry in plan)
        {
            if (entry.Kind != PlanEntryKind.Rendered)
            {
                continue;
            }

            var chunkPath = Path.Combine(directory, ChunkName(entry));
            if (File.Exists(chunkPath) && new FileInfo(chunkPath).Length > 0)
            {
                continue;  // name encodes the exact range, so an existing file is current
            }

            var rendered = await BoundaryRenderer.RenderAsync(
                _mediaEncoder.EncoderPath, mediaPath, entry, format!, chunkPath, cancellationToken)
                .ConfigureAwait(false);

            if (!rendered)
            {
                _logger.LogError(
                    "CensorCut: failed to render the boundary chunk {Start}-{End} ms for {Path}",
                    entry.StartMs,
                    entry.EndMs,
                    mediaPath);
                return null;
            }

            _logger.LogInformation(
                "CensorCut: rendered {Ms} ms boundary chunk at {Start} ms for {Path}",
                entry.DurationMs,
                entry.StartMs,
                mediaPath);
        }

        var text = FfconcatBuilder.Render(mediaFileName, plan, ChunkName);
        var scriptPath = Path.Combine(directory, mediaFileName + "." + profile.Id + ".censorcut.ffconcat");

        if (!File.Exists(scriptPath)
            || !string.Equals(
                await File.ReadAllTextAsync(scriptPath, cancellationToken).ConfigureAwait(false),
                text,
                StringComparison.Ordinal))
        {
            await File.WriteAllTextAsync(scriptPath, text, cancellationToken).ConfigureAwait(false);
            _logger.LogInformation(
                "CensorCut: wrote {Script} ({Total} entries, {Rendered} re-encoded)",
                scriptPath,
                plan.Count,
                plan.Count(e => e.Kind == PlanEntryKind.Rendered));
        }

        return (scriptPath, plan);
    }

    private static string DeterministicSourceId(Guid itemId, string? profileId)
        => string.Create(
            CultureInfo.InvariantCulture,
            $"censorcut_{itemId:N}_{profileId ?? "default"}");
}
