using System;
using System.Collections.Generic;
using System.Threading;
using System.Threading.Tasks;
using Jellyfin.Database.Implementations.Enums;
using Jellyfin.Plugin.CensorCut.Configuration;
using MediaBrowser.Controller.Entities;
using MediaBrowser.Controller.MediaSegments;
using MediaBrowser.Controller.Persistence;
using MediaBrowser.Model;
using MediaBrowser.Model.MediaSegments;
using Microsoft.Extensions.Logging;

namespace Jellyfin.Plugin.CensorCut;

/// <summary>
/// Exposes the cuts in a movie's CensorCut edit list as media segments, so
/// clients skip them without the film ever being re-encoded.
/// </summary>
public class CensorCutSegmentProvider : IMediaSegmentProvider
{
    private readonly IItemRepository _itemRepository;
    private readonly ILogger<CensorCutSegmentProvider> _logger;

    /// <summary>
    /// Initializes a new instance of the <see cref="CensorCutSegmentProvider"/> class.
    /// </summary>
    /// <param name="itemRepository">The item repository.</param>
    /// <param name="logger">The logger.</param>
    public CensorCutSegmentProvider(
        IItemRepository itemRepository,
        ILogger<CensorCutSegmentProvider> logger)
    {
        _itemRepository = itemRepository;
        _logger = logger;
    }

    /// <inheritdoc />
    public string Name => "CensorCut";

    /// <inheritdoc />
    public ValueTask<bool> Supports(BaseItem item) => new(item is IHasMediaSources);

    /// <summary>
    /// Called when Jellyfin prunes extracted segment data for an item.
    /// </summary>
    /// <param name="itemId">The item being pruned.</param>
    /// <param name="cancellationToken">Abort token.</param>
    /// <returns>A completed task.</returns>
    /// <remarks>
    /// Nothing to do: segments are read straight from the edit list beside the
    /// movie, so this provider caches nothing of its own. Declared because the
    /// interface gained this member in a later server release and the package
    /// reference floats.
    /// </remarks>
    public Task CleanupExtractedData(Guid itemId, CancellationToken cancellationToken)
        => Task.CompletedTask;

    /// <inheritdoc />
    public Task<IReadOnlyList<MediaSegmentDto>> GetMediaSegments(
        MediaSegmentGenerationRequest request,
        CancellationToken cancellationToken)
    {
        var empty = Task.FromResult<IReadOnlyList<MediaSegmentDto>>(Array.Empty<MediaSegmentDto>());

        var config = CensorCutPlugin.Instance?.Configuration;
        if (config is null || config.Mode != CensorCutMode.ClientSegments)
        {
            // Server-side mode already delivers a cut stream. Reporting
            // segments as well would make the client skip forward inside
            // footage that had been cut already, losing wanted scenes.
            return empty;
        }

        var item = _itemRepository.RetrieveItem(request.ItemId);
        if (item is null)
        {
            return empty;
        }

        var list = EditList.LoadFor(item.Path);
        if (list is null)
        {
            return empty;
        }

        var profile = list.PickProfile(config.ProfileId);
        if (profile is null || profile.Cuts.Count == 0)
        {
            _logger.LogWarning(
                "CensorCut edit list for {Path} holds no usable cuts; the item will play uncut",
                item.Path);
            return empty;
        }

        var segmentType = config.SegmentType;
        var leadInMs = config.LeadInOverrideMs > 0
            ? config.LeadInOverrideMs
            : profile.LeadInMs;

        var segments = new List<MediaSegmentDto>(profile.Cuts.Count);
        foreach (var cut in profile.Cuts)
        {
            if (cut.EndMs <= cut.StartMs)
            {
                continue;
            }

            // Widen the segment at the front. A client only acts once playback
            // has entered the segment, and it notices late; starting early
            // costs a fraction of a second of clean footage instead of showing
            // a frame of the cut.
            var startMs = Math.Max(0, cut.StartMs - leadInMs);

            segments.Add(new MediaSegmentDto
            {
                Id = Guid.NewGuid(),
                ItemId = item.Id,
                Type = segmentType,
                StartTicks = startMs * TimeSpan.TicksPerMillisecond,
                EndTicks = cut.EndMs * TimeSpan.TicksPerMillisecond,
            });
        }

        _logger.LogInformation(
            "CensorCut: {Count} segments for {Path} (profile {Profile})",
            segments.Count,
            item.Path,
            profile.Id);

        return Task.FromResult<IReadOnlyList<MediaSegmentDto>>(segments);
    }
}
