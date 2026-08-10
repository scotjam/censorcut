using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using Jellyfin.Plugin.CensorCut.ServerSide;
using Xunit;

namespace Jellyfin.Plugin.CensorCut.Tests;

/// <summary>
/// Runs the real planner, renderer and concat script through ffmpeg and checks
/// the frames that come out.
/// </summary>
/// <remarks>
/// The unit tests assert what the plan says; this asserts what ffmpeg actually
/// decodes from it. Skipped when ffmpeg is not on PATH so it never blocks a
/// build, but it is the test that would catch the concat demuxer changing
/// behaviour underneath us.
/// </remarks>
public sealed class SmartRenderIntegrationTests : IDisposable
{
    private const long CutStartMs = 12000;
    private const long CutEndMs = 23320;   // deliberately mid-GOP
    private const long DurationMs = 60000;

    private readonly string _dir;
    private readonly string? _ffmpeg = Which("ffmpeg");
    private readonly string? _ffprobe = Which("ffprobe");

    /// <summary>
    /// Initializes a new instance of the <see cref="SmartRenderIntegrationTests"/> class.
    /// </summary>
    public SmartRenderIntegrationTests()
    {
        _dir = Path.Combine(Path.GetTempPath(), "censorcut-smart-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(_dir);
    }

    /// <inheritdoc />
    public void Dispose()
    {
        try
        {
            Directory.Delete(_dir, true);
        }
        catch (IOException)
        {
            // Best effort.
        }
    }

    [SkippableFact]
    public async Task PlannedScriptLeaksNoCutFrameAndResumesExactly()
    {
        Skip.If(_ffmpeg is null || _ffprobe is null, "ffmpeg/ffprobe not on PATH");

        // A 60 s clip with keyframes every 10 s and scene detection off, so the
        // cut end at 23.32 s is a long way from the next keyframe (30 s) —
        // exactly the case where snapping forward would discard 6.7 s.
        var movie = Path.Combine(_dir, "Source.mp4");
        await RunAsync(_ffmpeg!, new[]
        {
            "-v", "error", "-y",
            "-f", "lavfi", "-i", "testsrc=size=320x180:rate=25:duration=60",
            "-f", "lavfi", "-i", "sine=frequency=440:duration=60",
            "-c:v", "libx264", "-g", "250", "-keyint_min", "250", "-sc_threshold", "0",
            "-pix_fmt", "yuv420p", "-c:a", "aac", movie,
        }).ConfigureAwait(false);

        var cuts = new List<EditCut> { new() { StartMs = CutStartMs, EndMs = CutEndMs } };

        // The production default lead-in, and it is load-bearing: an outpoint
        // includes the frame sitting on it, so with leadInMs 0 the first one or
        // two frames of every cut survive. Ending the keep 150 ms early absorbs
        // that overshoot.
        var keeps = FfconcatBuilder.BuildKeepSegments(cuts, DurationMs, leadInMs: 150);

        var keyframes = await KeyframeProbe.FindKeyframesAsync(
            _ffprobe!, movie, keeps.Select(k => k.StartMs).ToList(), 30, CancellationToken.None)
            .ConfigureAwait(false);
        Assert.NotEmpty(keyframes);

        var plan = FfconcatBuilder.BuildPlan(keeps, keyframes);

        // The resume must be the exact cut end, and the untouched tail must
        // still be referenced rather than re-encoded.
        var resume = plan.First(e => e.StartMs >= CutEndMs);
        Assert.Equal(CutEndMs, resume.StartMs);
        Assert.Equal(PlanEntryKind.Rendered, resume.Kind);
        Assert.Contains(plan, e => e.Kind == PlanEntryKind.Original && e.StartMs == 30000);

        var format = await BoundaryRenderer.ProbeFormatAsync(_ffprobe!, movie, CancellationToken.None)
            .ConfigureAwait(false);
        Assert.True(BoundaryRenderer.CanRender(format));

        string ChunkName(PlanEntry e) =>
            $"chunk-{e.StartMs}-{e.EndMs}{BoundaryRenderer.ChunkExtensionFor(movie)}";

        foreach (var entry in plan.Where(e => e.Kind == PlanEntryKind.Rendered))
        {
            var ok = await BoundaryRenderer.RenderAsync(
                _ffmpeg!, movie, entry, format!, Path.Combine(_dir, ChunkName(entry)), CancellationToken.None)
                .ConfigureAwait(false);
            Assert.True(ok, $"failed to render chunk {entry.StartMs}-{entry.EndMs}");
        }

        var scriptPath = Path.Combine(_dir, "Source.mp4.censorcut.ffconcat");
        await File.WriteAllTextAsync(
            scriptPath,
            FfconcatBuilder.Render(Path.GetFileName(movie), plan, ChunkName)).ConfigureAwait(false);

        // What ffmpeg actually produces from the script.
        var output = await FrameHashesAsync(scriptPath, null, null).ConfigureAwait(false);
        var cutRegion = await FrameHashesAsync(
            movie,
            (CutStartMs / 1000.0).ToString("0.###", System.Globalization.CultureInfo.InvariantCulture),
            (CutEndMs / 1000.0).ToString("0.###", System.Globalization.CultureInfo.InvariantCulture))
            .ConfigureAwait(false);

        Assert.NotEmpty(output);
        Assert.NotEmpty(cutRegion);

        // The censored stream must contain none of the cut scene. The rendered
        // chunk is re-encoded so it cannot hash-match anyway; this is really
        // asserting that the referenced portion never rewinds into the cut.
        var leaked = output.Intersect(cutRegion, StringComparer.Ordinal).ToList();
        Assert.True(leaked.Count == 0, $"{leaked.Count} frames of the cut leaked into the output");

        // And the kept footage is all there.
        var expectedFrames = (int)Math.Round(keeps.Sum(k => k.DurationMs) / 1000.0 * 25);
        Assert.InRange(output.Count, expectedFrames - 3, expectedFrames + 3);
    }

    private async Task<List<string>> FrameHashesAsync(string input, string? ss, string? to)
    {
        // Both -ss and -to go AFTER -i, on purpose. With -ss before the input,
        // ffmpeg resets output timestamps to zero and -to is then measured from
        // the seek point rather than from the start of the file — which quietly
        // widens the range and pulls in footage that was legitimately kept.
        var args = new List<string> { "-v", "error", "-i", input };

        if (ss is not null)
        {
            args.Add("-ss");
            args.Add(ss);
        }

        if (to is not null)
        {
            args.Add("-to");
            args.Add(to);
        }

        args.AddRange(new[] { "-map", "0:v", "-f", "framemd5", "-" });

        var stdout = await RunAsync(_ffmpeg!, args).ConfigureAwait(false);
        return stdout
            .Split('\n', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries)
            .Where(l => !l.StartsWith('#'))
            .Select(l => l.Split(',').Last().Trim())
            .Where(h => h.Length > 0)
            .ToList();
    }

    private static async Task<string> RunAsync(string exe, IReadOnlyList<string> args)
    {
        var startInfo = new ProcessStartInfo
        {
            FileName = exe,
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
        var stdout = await process.StandardOutput.ReadToEndAsync().ConfigureAwait(false);
        var stderr = await process.StandardError.ReadToEndAsync().ConfigureAwait(false);
        await process.WaitForExitAsync().ConfigureAwait(false);

        if (process.ExitCode != 0)
        {
            throw new InvalidOperationException($"{exe} failed: {stderr}");
        }

        return stdout;
    }

    private static string? Which(string name)
    {
        var paths = Environment.GetEnvironmentVariable("PATH")?.Split(Path.PathSeparator) ?? Array.Empty<string>();
        var candidates = OperatingSystem.IsWindows() ? new[] { name + ".exe", name } : new[] { name };

        foreach (var dir in paths)
        {
            foreach (var candidate in candidates)
            {
                try
                {
                    var full = Path.Combine(dir, candidate);
                    if (File.Exists(full))
                    {
                        return full;
                    }
                }
                catch (ArgumentException)
                {
                    // Malformed PATH entry.
                }
            }
        }

        return null;
    }
}
