using System;
using System.Collections.Generic;
using System.Linq;
using Jellyfin.Plugin.CensorCut.ServerSide;
using Xunit;

namespace Jellyfin.Plugin.CensorCut.Tests;

/// <summary>
/// Tests for turning an edit list into a concat script.
/// </summary>
/// <remarks>
/// The rule under test throughout: every adjustment must move in the direction
/// of removing more footage. Measured against ffmpeg 8.1, a concat
/// <c>inpoint</c> that is not on a keyframe rewinds to the preceding keyframe,
/// so an unsnapped keep-segment start would replay the end of the scene being
/// cut.
/// </remarks>
public class FfconcatBuilderTests
{
    private static EditCut Cut(long startMs, long endMs) => new() { StartMs = startMs, EndMs = endMs };

    [Fact]
    public void KeepSegmentsAreTheComplementOfTheCuts()
    {
        var keeps = FfconcatBuilder.BuildKeepSegments(
            new[] { Cut(10000, 20000) },
            durationMs: 60000,
            leadInMs: 0,
            keyframesMs: new long[] { 0, 20000, 40000 });

        Assert.Equal(2, keeps.Count);
        Assert.Equal(new KeepSegment(0, 10000), keeps[0]);
        Assert.Equal(new KeepSegment(20000, 60000), keeps[1]);
    }

    [Fact]
    public void ResumePointSnapsForwardToTheNextKeyframe()
    {
        // The cut ends at 21s but the next keyframe is 24s. Resuming at 21s
        // would replay 3s of the cut scene, so the keep starts at 24s.
        var keeps = FfconcatBuilder.BuildKeepSegments(
            new[] { Cut(10000, 21000) },
            durationMs: 60000,
            leadInMs: 0,
            keyframesMs: new long[] { 0, 12000, 24000, 36000 });

        Assert.Equal(24000, keeps[1].StartMs);
    }

    [Fact]
    public void ResumePointNeverSnapsBackward()
    {
        var keeps = FfconcatBuilder.BuildKeepSegments(
            new[] { Cut(10000, 21000) },
            durationMs: 60000,
            leadInMs: 0,
            keyframesMs: new long[] { 0, 12000, 24000 });

        Assert.All(keeps, k => Assert.True(
            k.StartMs == 0 || k.StartMs >= 21000,
            "a keep may start later than the cut ends, never earlier"));
    }

    [Fact]
    public void LeadInEndsTheKeepBeforeTheCut()
    {
        var keeps = FfconcatBuilder.BuildKeepSegments(
            new[] { Cut(10000, 20000) },
            durationMs: 60000,
            leadInMs: 150,
            keyframesMs: new long[] { 0, 20000 });

        Assert.Equal(9850, keeps[0].EndMs);
    }

    [Fact]
    public void WideningCanMergeTwoNearbyCuts()
    {
        // The first cut's resume point snaps to 24s, past the start of the
        // second cut — they have to become one range, not overlap.
        var keeps = FfconcatBuilder.BuildKeepSegments(
            new[] { Cut(10000, 21000), Cut(22000, 30000) },
            durationMs: 60000,
            leadInMs: 0,
            keyframesMs: new long[] { 0, 24000, 36000 });

        Assert.Equal(2, keeps.Count);
        Assert.Equal(new KeepSegment(0, 10000), keeps[0]);
        Assert.Equal(new KeepSegment(36000, 60000), keeps[1]);
    }

    [Fact]
    public void KeepSegmentsNeverOverlapOrGoBackwards()
    {
        var keeps = FfconcatBuilder.BuildKeepSegments(
            new[] { Cut(30000, 35000), Cut(5000, 9000), Cut(8000, 12000) },
            durationMs: 60000,
            leadInMs: 200,
            keyframesMs: new long[] { 0, 13000, 26000, 39000, 52000 });

        for (var i = 1; i < keeps.Count; i++)
        {
            Assert.True(keeps[i].StartMs >= keeps[i - 1].EndMs);
        }

        Assert.All(keeps, k => Assert.True(k.DurationMs > 0));
    }

    [Fact]
    public void CutAtTheStartLeavesNoLeadingKeep()
    {
        var keeps = FfconcatBuilder.BuildKeepSegments(
            new[] { Cut(0, 5000) },
            durationMs: 60000,
            leadInMs: 150,
            keyframesMs: new long[] { 0, 5000, 10000 });

        Assert.Single(keeps);
        Assert.Equal(5000, keeps[0].StartMs);
    }

    [Fact]
    public void CutRunningToTheEndLeavesNoTrailingKeep()
    {
        var keeps = FfconcatBuilder.BuildKeepSegments(
            new[] { Cut(50000, 60000) },
            durationMs: 60000,
            leadInMs: 0,
            keyframesMs: new long[] { 0, 25000, 50000 });

        Assert.Single(keeps);
        Assert.Equal(0, keeps[0].StartMs);
        Assert.Equal(50000, keeps[0].EndMs);
    }

    [Fact]
    public void NoKeyframePastTheCutLeavesTheEndUnsnapped()
    {
        // Nothing known past the cut: the value is returned as-is and the
        // caller decides. The provider treats an empty probe as a refusal.
        Assert.Equal(21000, FfconcatBuilder.SnapForwardToKeyframe(21000, 60000, Array.Empty<long>()));
    }

    [Fact]
    public void RenderProducesAConcatScriptWithBareFileNames()
    {
        var text = FfconcatBuilder.Render(
            "Example.mkv",
            new[] { new KeepSegment(0, 9850), new KeepSegment(24000, 60000) });

        Assert.Equal(
            "ffconcat version 1.0\n"
            + "file 'Example.mkv'\ninpoint 0\noutpoint 9.85\n"
            + "file 'Example.mkv'\ninpoint 24\noutpoint 60\n",
            text);
    }

    [Fact]
    public void RenderQuotesAwkwardFileNames()
    {
        var text = FfconcatBuilder.Render("It's Here.mkv", new[] { new KeepSegment(0, 1000) });
        Assert.Contains(@"file 'It'\''s Here.mkv'", text, StringComparison.Ordinal);
    }

    [Fact]
    public void RenderRefusesPathsBecauseFfmpegCallsThemUnsafe()
    {
        Assert.Throws<ArgumentException>(() =>
            FfconcatBuilder.Render(@"C:\films\Example.mkv", new[] { new KeepSegment(0, 1000) }));
        Assert.Throws<ArgumentException>(() =>
            FfconcatBuilder.Render("films/Example.mkv", new[] { new KeepSegment(0, 1000) }));
    }

    [Fact]
    public void ProbeOutputParsesToAscendingMilliseconds()
    {
        var times = KeyframeProbe.ParseTimes("20.000000\n24.500000\n\n12.000000\n24.500000\n");
        Assert.Equal(new long[] { 12000, 20000, 24500 }, times.ToArray());
    }

    [Fact]
    public void ProbeOutputSurvivesJunk()
    {
        Assert.Empty(KeyframeProbe.ParseTimes("N/A\n\nnot-a-number\n"));
        Assert.Empty(KeyframeProbe.ParseTimes(string.Empty));
    }
}
