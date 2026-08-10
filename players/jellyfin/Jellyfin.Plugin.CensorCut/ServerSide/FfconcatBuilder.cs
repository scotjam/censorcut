using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Text;

namespace Jellyfin.Plugin.CensorCut.ServerSide;

/// <summary>
/// One contiguous range of the source that survives into the censored version.
/// </summary>
/// <param name="StartMs">Inclusive start, in milliseconds.</param>
/// <param name="EndMs">Exclusive end, in milliseconds.</param>
public readonly record struct KeepSegment(long StartMs, long EndMs)
{
    /// <summary>Gets the segment length in milliseconds.</summary>
    public long DurationMs => EndMs - StartMs;
}

/// <summary>
/// How a piece of the censored version is produced.
/// </summary>
public enum PlanEntryKind
{
    /// <summary>
    /// Taken from the original file untouched, via keyframe-aligned in/out
    /// points. No re-encoding, bit-identical output.
    /// </summary>
    Original = 0,

    /// <summary>
    /// Re-encoded into a small file because it starts mid-GOP. Only the
    /// partial group of pictures at a resume point needs this.
    /// </summary>
    Rendered = 1,
}

/// <summary>
/// One entry in the concat script.
/// </summary>
/// <param name="Kind">Whether this piece is referenced or re-encoded.</param>
/// <param name="StartMs">Inclusive start in the source, in milliseconds.</param>
/// <param name="EndMs">Exclusive end in the source, in milliseconds.</param>
public readonly record struct PlanEntry(PlanEntryKind Kind, long StartMs, long EndMs)
{
    /// <summary>Gets the entry length in milliseconds.</summary>
    public long DurationMs => EndMs - StartMs;
}

/// <summary>
/// Turns an edit list into an ffmpeg concat-demuxer script that plays exactly
/// the kept ranges of the original file.
/// </summary>
/// <remarks>
/// <para>
/// A concat <c>inpoint</c> that is not on a keyframe rewinds to the preceding
/// keyframe, which would replay the tail of the scene being cut. Snapping the
/// resume point forward to the next keyframe would avoid that but throws away
/// wanted footage — up to a whole GOP after every cut.
/// </para>
/// <para>
/// So the boundary is smart-rendered instead: the partial GOP between the true
/// resume point and the next keyframe is re-encoded into a small file, and
/// everything from that keyframe onward is referenced from the original
/// untouched. Resume points are frame-exact, and the re-encoded portion is a
/// few seconds per cut rather than the whole film.
/// </para>
/// </remarks>
public static class FfconcatBuilder
{
    /// <summary>
    /// Computes the ranges to keep — the exact complement of the cuts, with
    /// each cut treated as starting a lead-in early.
    /// </summary>
    /// <param name="cuts">Cuts from the edit list, in any order.</param>
    /// <param name="durationMs">Source duration in milliseconds.</param>
    /// <param name="leadInMs">How early each cut is treated as starting.</param>
    /// <returns>The keep-segments, ordered and non-overlapping.</returns>
    public static IReadOnlyList<KeepSegment> BuildKeepSegments(
        IReadOnlyList<EditCut> cuts,
        long durationMs,
        long leadInMs)
    {
        ArgumentNullException.ThrowIfNull(cuts);

        if (durationMs <= 0)
        {
            return Array.Empty<KeepSegment>();
        }

        var widened = new List<KeepSegment>(cuts.Count);
        foreach (var cut in cuts)
        {
            if (cut.EndMs <= cut.StartMs)
            {
                continue;
            }

            var start = Math.Clamp(cut.StartMs - leadInMs, 0, durationMs);
            var end = Math.Clamp(cut.EndMs, 0, durationMs);
            if (end > start)
            {
                widened.Add(new KeepSegment(start, end));
            }
        }

        widened.Sort((a, b) => a.StartMs.CompareTo(b.StartMs));

        var merged = new List<KeepSegment>(widened.Count);
        foreach (var range in widened)
        {
            if (merged.Count > 0 && range.StartMs <= merged[^1].EndMs)
            {
                merged[^1] = new KeepSegment(merged[^1].StartMs, Math.Max(merged[^1].EndMs, range.EndMs));
            }
            else
            {
                merged.Add(range);
            }
        }

        var keeps = new List<KeepSegment>(merged.Count + 1);
        long cursor = 0;
        foreach (var cut in merged)
        {
            if (cut.StartMs > cursor)
            {
                keeps.Add(new KeepSegment(cursor, cut.StartMs));
            }

            cursor = Math.Max(cursor, cut.EndMs);
        }

        if (cursor < durationMs)
        {
            keeps.Add(new KeepSegment(cursor, durationMs));
        }

        return keeps;
    }

    /// <summary>
    /// Splits each keep-segment into the part that must be re-encoded and the
    /// part that can be referenced from the original.
    /// </summary>
    /// <param name="keeps">Keep-segments from <see cref="BuildKeepSegments"/>.</param>
    /// <param name="keyframesMs">Ascending keyframe timestamps, in milliseconds.</param>
    /// <returns>The plan, in playback order.</returns>
    public static IReadOnlyList<PlanEntry> BuildPlan(
        IReadOnlyList<KeepSegment> keeps,
        IReadOnlyList<long> keyframesMs)
    {
        ArgumentNullException.ThrowIfNull(keeps);
        ArgumentNullException.ThrowIfNull(keyframesMs);

        var plan = new List<PlanEntry>(keeps.Count);
        foreach (var keep in keeps)
        {
            if (keep.DurationMs <= 0)
            {
                continue;
            }

            var keyframe = FirstKeyframeIn(keep, keyframesMs);

            if (keyframe is null)
            {
                // No keyframe anywhere inside this range, so none of it can be
                // referenced. Short segments between close cuts land here.
                plan.Add(new PlanEntry(PlanEntryKind.Rendered, keep.StartMs, keep.EndMs));
                continue;
            }

            if (keyframe.Value > keep.StartMs)
            {
                plan.Add(new PlanEntry(PlanEntryKind.Rendered, keep.StartMs, keyframe.Value));
            }

            plan.Add(new PlanEntry(PlanEntryKind.Original, keyframe.Value, keep.EndMs));
        }

        return plan;
    }

    /// <summary>
    /// Finds the first keyframe at or after a segment's start and strictly
    /// before its end.
    /// </summary>
    /// <param name="keep">The segment.</param>
    /// <param name="keyframesMs">Ascending keyframe timestamps.</param>
    /// <returns>The keyframe, or null when the segment holds none.</returns>
    public static long? FirstKeyframeIn(KeepSegment keep, IReadOnlyList<long> keyframesMs)
    {
        ArgumentNullException.ThrowIfNull(keyframesMs);

        foreach (var keyframe in keyframesMs)
        {
            if (keyframe >= keep.EndMs)
            {
                break;
            }

            if (keyframe >= keep.StartMs)
            {
                return keyframe;
            }
        }

        return null;
    }

    /// <summary>
    /// Renders the concat script.
    /// </summary>
    /// <remarks>
    /// Every referenced file is written as a bare name, so the script and the
    /// rendered chunks have to sit in the same directory as the movie: ffmpeg
    /// rejects absolute entries as unsafe unless invoked with <c>-safe 0</c>,
    /// which is not ours to add inside Jellyfin. Explicit <c>duration</c>
    /// directives are included because without them the demuxer reports no
    /// duration at all for a mixed script.
    /// </remarks>
    /// <param name="mediaFileName">The movie's file name, without directory.</param>
    /// <param name="plan">The plan entries.</param>
    /// <param name="chunkFileName">Maps a rendered entry to its file name.</param>
    /// <returns>The contents of the .ffconcat file.</returns>
    public static string Render(
        string mediaFileName,
        IReadOnlyList<PlanEntry> plan,
        Func<PlanEntry, string> chunkFileName)
    {
        ArgumentException.ThrowIfNullOrEmpty(mediaFileName);
        ArgumentNullException.ThrowIfNull(plan);
        ArgumentNullException.ThrowIfNull(chunkFileName);

        RequireBareName(mediaFileName, nameof(mediaFileName));

        var text = new StringBuilder();
        text.Append("ffconcat version 1.0\n");

        foreach (var entry in plan)
        {
            if (entry.DurationMs <= 0)
            {
                continue;
            }

            if (entry.Kind == PlanEntryKind.Rendered)
            {
                var name = chunkFileName(entry);
                RequireBareName(name, nameof(chunkFileName));
                text.Append("file ").Append(Quote(name)).Append('\n');
                text.Append("duration ").Append(Seconds(entry.DurationMs)).Append('\n');
            }
            else
            {
                text.Append("file ").Append(Quote(mediaFileName)).Append('\n');
                text.Append("inpoint ").Append(Seconds(entry.StartMs)).Append('\n');
                text.Append("outpoint ").Append(Seconds(entry.EndMs)).Append('\n');
                text.Append("duration ").Append(Seconds(entry.DurationMs)).Append('\n');
            }
        }

        return text.ToString();
    }

    private static void RequireBareName(string name, string paramName)
    {
        if (string.IsNullOrEmpty(name)
            || Path.IsPathRooted(name)
            || name.Contains('/', StringComparison.Ordinal)
            || name.Contains('\\', StringComparison.Ordinal))
        {
            throw new ArgumentException(
                "The concat script must reference files by bare name; ffmpeg rejects paths as unsafe.",
                paramName);
        }
    }

    private static string Seconds(long ms) =>
        (ms / 1000.0).ToString("0.###", CultureInfo.InvariantCulture);

    private static string Quote(string name) =>
        "'" + name.Replace("'", @"'\''", StringComparison.Ordinal) + "'";
}
