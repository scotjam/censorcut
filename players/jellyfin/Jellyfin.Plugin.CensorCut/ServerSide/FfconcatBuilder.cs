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
/// Turns an edit list into an ffmpeg concat-demuxer script that plays only the
/// kept ranges of the original file.
/// </summary>
/// <remarks>
/// Two measured properties of the concat demuxer drive the rules here:
/// <list type="bullet">
/// <item>
/// <c>inpoint</c> at a non-keyframe snaps <i>backward</i> to the preceding
/// keyframe. A keep-segment starts where a cut ends, so an unsnapped inpoint
/// would replay the tail of the very scene being removed. Starts are therefore
/// moved <i>forward</i> to the next keyframe, which can only ever discard
/// wanted footage, never reveal unwanted footage.
/// </item>
/// <item>
/// <c>outpoint</c> is accurate to within a frame, and keep-segments already end
/// a lead-in ahead of the cut, so the overshoot stays inside that margin.
/// </item>
/// </list>
/// </remarks>
public static class FfconcatBuilder
{
    /// <summary>
    /// Computes the ranges to keep, given the cuts and the source's keyframe
    /// positions.
    /// </summary>
    /// <param name="cuts">Cuts from the edit list, in any order.</param>
    /// <param name="durationMs">Source duration in milliseconds.</param>
    /// <param name="leadInMs">How early each cut is treated as starting.</param>
    /// <param name="keyframesMs">
    /// Known keyframe timestamps in milliseconds, ascending. May be empty, in
    /// which case starts are left unsnapped — the caller is expected to have
    /// probed, and an empty list means the probe failed.
    /// </param>
    /// <returns>The keep-segments, ordered and non-overlapping.</returns>
    public static IReadOnlyList<KeepSegment> BuildKeepSegments(
        IReadOnlyList<EditCut> cuts,
        long durationMs,
        long leadInMs,
        IReadOnlyList<long> keyframesMs)
    {
        ArgumentNullException.ThrowIfNull(cuts);
        ArgumentNullException.ThrowIfNull(keyframesMs);

        if (durationMs <= 0)
        {
            return Array.Empty<KeepSegment>();
        }

        // Widen every cut outward: earlier at the front by the lead-in, and
        // later at the back to the next keyframe. Both directions err toward
        // removing more, never less.
        var widened = new List<KeepSegment>(cuts.Count);
        foreach (var cut in cuts)
        {
            if (cut.EndMs <= cut.StartMs)
            {
                continue;
            }

            var start = Math.Clamp(cut.StartMs - leadInMs, 0, durationMs);
            var end = Math.Clamp(SnapForwardToKeyframe(cut.EndMs, durationMs, keyframesMs), 0, durationMs);
            if (end > start)
            {
                widened.Add(new KeepSegment(start, end));
            }
        }

        widened.Sort((a, b) => a.StartMs.CompareTo(b.StartMs));

        // Merge overlaps created by widening — two cuts a second apart can
        // easily become one after snapping.
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

        // The keeps are the complement of the cuts.
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
    /// Returns the first keyframe at or after <paramref name="ms"/>, or
    /// <paramref name="ms"/> itself when nothing is known past it.
    /// </summary>
    /// <param name="ms">The time to snap.</param>
    /// <param name="durationMs">Source duration.</param>
    /// <param name="keyframesMs">Ascending keyframe timestamps.</param>
    /// <returns>The snapped timestamp.</returns>
    public static long SnapForwardToKeyframe(long ms, long durationMs, IReadOnlyList<long> keyframesMs)
    {
        ArgumentNullException.ThrowIfNull(keyframesMs);

        foreach (var keyframe in keyframesMs)
        {
            if (keyframe >= ms)
            {
                return Math.Min(keyframe, durationMs);
            }
        }

        return ms;
    }

    /// <summary>
    /// Renders the concat script. The referenced file is written as a bare
    /// name, so the script has to live in the same directory as the movie:
    /// ffmpeg rejects absolute entries as unsafe unless invoked with
    /// <c>-safe 0</c>, which is not ours to add inside Jellyfin.
    /// </summary>
    /// <param name="mediaFileName">The movie's file name, without directory.</param>
    /// <param name="keeps">The ranges to keep.</param>
    /// <returns>The contents of the .ffconcat file.</returns>
    public static string Render(string mediaFileName, IReadOnlyList<KeepSegment> keeps)
    {
        ArgumentException.ThrowIfNullOrEmpty(mediaFileName);
        ArgumentNullException.ThrowIfNull(keeps);

        if (Path.IsPathRooted(mediaFileName) || mediaFileName.Contains('/', StringComparison.Ordinal)
            || mediaFileName.Contains('\\', StringComparison.Ordinal))
        {
            throw new ArgumentException(
                "The concat script must reference the movie by bare file name; ffmpeg rejects paths as unsafe.",
                nameof(mediaFileName));
        }

        var text = new StringBuilder();
        text.Append("ffconcat version 1.0\n");

        foreach (var keep in keeps)
        {
            if (keep.DurationMs <= 0)
            {
                continue;
            }

            text.Append("file ").Append(Quote(mediaFileName)).Append('\n');
            text.Append("inpoint ").Append(Seconds(keep.StartMs)).Append('\n');
            text.Append("outpoint ").Append(Seconds(keep.EndMs)).Append('\n');
        }

        return text.ToString();
    }

    private static string Seconds(long ms) =>
        (ms / 1000.0).ToString("0.###", CultureInfo.InvariantCulture);

    /// <summary>
    /// Single-quotes a file name for the concat demuxer, which treats a
    /// quoted string as literal except for the quote itself.
    /// </summary>
    private static string Quote(string name) =>
        "'" + name.Replace("'", @"'\''", StringComparison.Ordinal) + "'";
}
