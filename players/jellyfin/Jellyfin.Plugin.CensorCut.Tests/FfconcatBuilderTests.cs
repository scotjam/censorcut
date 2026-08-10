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
/// Measured against ffmpeg 8.1: a concat <c>inpoint</c> that is not on a
/// keyframe rewinds to the preceding keyframe, which would replay the end of
/// the scene being cut. Rather than give up wanted footage by snapping resume
/// points forward, the partial GOP is re-encoded and everything else is
/// referenced untouched. The invariants here: resume points stay exact, and no
/// more than one partial GOP per cut is ever re-encoded.
/// </remarks>
public class FfconcatBuilderTests
{
    private static EditCut Cut(long startMs, long endMs) => new() { StartMs = startMs, EndMs = endMs };

    [Fact]
    public void KeepSegmentsAreTheExactComplementOfTheCuts()
    {
        var keeps = FfconcatBuilder.BuildKeepSegments(
            new[] { Cut(10000, 20000) }, durationMs: 60000, leadInMs: 0);

        Assert.Equal(2, keeps.Count);
        Assert.Equal(new KeepSegment(0, 10000), keeps[0]);
        Assert.Equal(new KeepSegment(20000, 60000), keeps[1]);
    }

    [Fact]
    public void ResumePointIsExactNotSnapped()
    {
        // The cut ends at 21s; the next keyframe is 24s. The resume must still
        // be 21s — the 21-24s remainder is re-encoded rather than discarded.
        var keeps = FfconcatBuilder.BuildKeepSegments(
            new[] { Cut(10000, 21000) }, durationMs: 60000, leadInMs: 0);
        var plan = FfconcatBuilder.BuildPlan(keeps, new long[] { 0, 12000, 24000, 36000 });

        var resume = plan.First(e => e.StartMs >= 21000);
        Assert.Equal(21000, resume.StartMs);
        Assert.Equal(PlanEntryKind.Rendered, resume.Kind);
        Assert.Equal(24000, resume.EndMs);
    }

    [Fact]
    public void OnlyThePartialGopIsReEncoded()
    {
        var keeps = FfconcatBuilder.BuildKeepSegments(
            new[] { Cut(10000, 21000) }, durationMs: 60000, leadInMs: 0);
        var plan = FfconcatBuilder.BuildPlan(keeps, new long[] { 0, 12000, 24000, 36000 });

        var rendered = plan.Where(e => e.Kind == PlanEntryKind.Rendered).ToList();
        Assert.Single(rendered);
        Assert.Equal(3000, rendered[0].DurationMs);

        // Everything else is referenced from the original untouched.
        Assert.Equal(
            60000 - 11000 - 3000,
            plan.Where(e => e.Kind == PlanEntryKind.Original).Sum(e => e.DurationMs));
    }

    [Fact]
    public void ASegmentStartingOnAKeyframeNeedsNoRendering()
    {
        var keeps = FfconcatBuilder.BuildKeepSegments(
            new[] { Cut(10000, 24000) }, durationMs: 60000, leadInMs: 0);
        var plan = FfconcatBuilder.BuildPlan(keeps, new long[] { 0, 12000, 24000 });

        Assert.All(plan, e => Assert.Equal(PlanEntryKind.Original, e.Kind));
    }

    [Fact]
    public void ASegmentWithNoKeyframeInsideIsFullyRendered()
    {
        // A short gap between two close cuts can contain no keyframe at all.
        var plan = FfconcatBuilder.BuildPlan(
            new[] { new KeepSegment(21000, 22000) },
            new long[] { 0, 12000, 24000 });

        Assert.Single(plan);
        Assert.Equal(PlanEntryKind.Rendered, plan[0].Kind);
        Assert.Equal(1000, plan[0].DurationMs);
    }

    [Fact]
    public void PlanCoversEveryKeptMillisecondExactlyOnce()
    {
        var keeps = FfconcatBuilder.BuildKeepSegments(
            new[] { Cut(30000, 35000), Cut(5000, 9000), Cut(8000, 12000) },
            durationMs: 60000,
            leadInMs: 200);
        var plan = FfconcatBuilder.BuildPlan(keeps, new long[] { 0, 13000, 26000, 39000, 52000 });

        Assert.Equal(keeps.Sum(k => k.DurationMs), plan.Sum(e => e.DurationMs));

        for (var i = 1; i < plan.Count; i++)
        {
            Assert.True(plan[i].StartMs >= plan[i - 1].EndMs, "plan entries must not overlap");
        }
    }

    [Fact]
    public void LeadInEndsTheKeepBeforeTheCut()
    {
        var keeps = FfconcatBuilder.BuildKeepSegments(
            new[] { Cut(10000, 20000) }, durationMs: 60000, leadInMs: 150);

        Assert.Equal(9850, keeps[0].EndMs);
    }

    [Fact]
    public void OverlappingCutsMerge()
    {
        var keeps = FfconcatBuilder.BuildKeepSegments(
            new[] { Cut(10000, 21000), Cut(20000, 30000) }, durationMs: 60000, leadInMs: 0);

        Assert.Equal(2, keeps.Count);
        Assert.Equal(new KeepSegment(0, 10000), keeps[0]);
        Assert.Equal(new KeepSegment(30000, 60000), keeps[1]);
    }

    [Fact]
    public void CutAtTheStartLeavesNoLeadingKeep()
    {
        var keeps = FfconcatBuilder.BuildKeepSegments(
            new[] { Cut(0, 5000) }, durationMs: 60000, leadInMs: 150);

        Assert.Single(keeps);
        Assert.Equal(5000, keeps[0].StartMs);
    }

    [Fact]
    public void CutRunningToTheEndLeavesNoTrailingKeep()
    {
        var keeps = FfconcatBuilder.BuildKeepSegments(
            new[] { Cut(50000, 60000) }, durationMs: 60000, leadInMs: 0);

        Assert.Single(keeps);
        Assert.Equal(new KeepSegment(0, 50000), keeps[0]);
    }

    [Fact]
    public void RenderEmitsChunksAndKeyframeAlignedReferences()
    {
        var plan = new[]
        {
            new PlanEntry(PlanEntryKind.Original, 0, 9850),
            new PlanEntry(PlanEntryKind.Rendered, 21000, 24000),
            new PlanEntry(PlanEntryKind.Original, 24000, 60000),
        };

        var text = FfconcatBuilder.Render("Example.mkv", plan, e => "chunk" + e.StartMs + ".mkv");

        Assert.Equal(
            "ffconcat version 1.0\n"
            + "file 'Example.mkv'\ninpoint 0\noutpoint 9.85\nduration 9.85\n"
            + "file 'chunk21000.mkv'\nduration 3\n"
            + "file 'Example.mkv'\ninpoint 24\noutpoint 60\nduration 36\n",
            text);
    }

    [Fact]
    public void RenderQuotesAwkwardFileNames()
    {
        var text = FfconcatBuilder.Render(
            "It's Here.mkv",
            new[] { new PlanEntry(PlanEntryKind.Original, 0, 1000) },
            _ => "chunk.mkv");

        Assert.Contains(@"file 'It'\''s Here.mkv'", text, StringComparison.Ordinal);
    }

    [Fact]
    public void RenderRefusesPathsBecauseFfmpegCallsThemUnsafe()
    {
        var plan = new[] { new PlanEntry(PlanEntryKind.Original, 0, 1000) };
        Assert.Throws<ArgumentException>(() =>
            FfconcatBuilder.Render(@"C:\films\Example.mkv", plan, _ => "chunk.mkv"));

        var rendered = new[] { new PlanEntry(PlanEntryKind.Rendered, 0, 1000) };
        Assert.Throws<ArgumentException>(() =>
            FfconcatBuilder.Render("Example.mkv", rendered, _ => "sub/chunk.mkv"));
    }

    [Fact]
    public void ChunkEncoderMatchesTheSourceCodec()
    {
        // A codec-mismatched chunk was measured to corrupt the following
        // entry's in/out points, so this is correctness, not quality.
        var args = BoundaryRenderer.BuildArgs(
            "/films/Example.mkv",
            new PlanEntry(PlanEntryKind.Rendered, 21000, 24000),
            new SourceFormat("h264", "yuv420p", "aac"),
            "chunk.mkv").ToList();

        Assert.Equal("libx264", args[args.IndexOf("-c:v") + 1]);
        Assert.Equal("yuv420p", args[args.IndexOf("-pix_fmt") + 1]);
        Assert.Equal("aac", args[args.IndexOf("-c:a") + 1]);
        Assert.Equal("21", args[args.IndexOf("-ss") + 1]);
        Assert.Equal("3", args[args.IndexOf("-t") + 1]);
    }

    [Fact]
    public void ChunksUseTheSourceContainer()
    {
        // A mismatched container carries a different timebase, which was
        // measured to drop the spliced chunk from the output entirely.
        Assert.Equal(".mp4", BoundaryRenderer.ChunkExtensionFor("/films/Example.mp4"));
        Assert.Equal(".mkv", BoundaryRenderer.ChunkExtensionFor("/films/Example.mkv"));
        Assert.Equal(".avi", BoundaryRenderer.ChunkExtensionFor(@"C:ilms\Example.avi"));
        Assert.Equal(".mkv", BoundaryRenderer.ChunkExtensionFor("/films/Example"));
    }

    [Fact]
    public void SourcesWithoutAMatchingEncoderAreRefused()
    {
        Assert.False(BoundaryRenderer.CanRender(new SourceFormat("prores", "yuv422p10le", "pcm_s16le")));
        Assert.False(BoundaryRenderer.CanRender(new SourceFormat("h264", "yuv420p", "truehd")));
        Assert.False(BoundaryRenderer.CanRender(null));
        Assert.True(BoundaryRenderer.CanRender(new SourceFormat("hevc", "yuv420p10le", null)));
    }

    [Fact]
    public void SilentSourcesRenderWithoutAudio()
    {
        var args = BoundaryRenderer.BuildArgs(
            "/films/Example.mkv",
            new PlanEntry(PlanEntryKind.Rendered, 0, 1000),
            new SourceFormat("h264", "yuv420p", null),
            "chunk.mkv");

        Assert.Contains("-an", args);
        Assert.DoesNotContain("-c:a", args);
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
