# Handoff for Claude Code

This document describes the state of the CensorCut project as left by the chat-Claude that designed and scaffolded it, so a Claude Code session (or a human developer) can pick up cleanly. Read `CLAUDE.md` first — it's shorter and has the architectural rules. This file is the deeper context.

---

## How this project came to be

The user is a parent who wants to remove scary scenes from kids' movies so younger children can watch films their older siblings already enjoy. The conversation that produced this codebase (in the chat-Claude product, separate from Claude Code) walked through:

1. Whether to fork VLC or FFmpeg → decided **neither**; embed libVLC + shell to FFmpeg from a Qt app
2. Whether ML-assisted detection is feasible → yes, with honest scope: audio is reliable, vision and dialogue extend coverage, but no detector is 100% so the parent must approve every cut
3. Category list expansion → Jump scare, Scary music, Screaming, Crying, Yelling, Gunfire, Hitting, Sword fight, Chase, Violence, Cruelty, Pushing, Kill/threat — categories are user-customizable, with each category being a recipe of detectors with weights
4. Aggressive defaults by age → single age slider drives the whole analyzer; "Under 5 = cut everything plausible" scaling to "15+ = no edits." Multi-child households use the youngest age.
5. Age in the filename → output is `<base> CENSORED-<age>.<ext>` so `Title CENSORED-4.mp4` and `Title CENSORED-9.mp4` coexist for siblings of different ages

The full plan is `docs/PLAN.md`. **Read it.** Especially §10 ("Honest caveats") which is load-bearing for setting user expectations.

---

## Where the project currently stands

### What's committed and working (M1)

- Qt + libVLC native player
- `[` / `]` shortcuts to mark cut start/end at the playhead
- Timeline widget with playhead, pending-start handle, and colored marker bands by status
- Marker list with right-click confirm/reject/delete, double-click to seek
- Sidecar JSON save/load (`<movie>.censorcut.json`) with sha1-based file-change warning
- `Project::censoredOutputPathFor(moviePath, age)` helper for the M2 export step
- AgeProfile data model with seven built-in profiles (Under 5, 5–6, 7–8, 9–10, 11–12, 13–14, 15+) — data only, the analyzer that consumes them is M3
- Unit tests for marker model, sidecar round-trip, and the censored-output-path helper

### What's deliberately NOT built yet

- Any FFmpeg integration. There's no export. The user's primary use case (producing the censored file) **does not yet work end to end** — that's M2.
- Any analysis. Markers are placed manually only.
- The `AnalysisResult` struct mentioned in PLAN.md is not yet in C++; add it in M3.
- The Analyzer panel in the UI is a placeholder label.
- CategoryEditor UI is M4.5.
- Vision (CLIP) and dialogue (Whisper) detectors are explicitly out of scope until M5/M6.

### Build status

The scaffold is **untested in compilation**. The chat-Claude that wrote it had no Qt or libVLC available in its environment. The code is written carefully and the path-construction logic was sanity-checked against a non-Qt harness, but expect to fix one or two small issues on first build (a missing include, a Qt version quirk, a platform-specific path). If `cmake -B build` or the first `cmake --build build` fails, treat that as the very first task — don't pile new code on top of a non-building scaffold.

---

## Recommended next task: build and verify

Before writing M2, get the M1 scaffold building and running:

1. `cmake -B build -S .` — fix any FindPackage issues (Qt6, libVLC). On Windows the user's setup is `D:\censorcut-repo\` with Qt and libVLC presumed installed.
2. `cmake --build build -j` — fix any compile errors. Most likely culprits: missing `#include <QHash>` or similar, namespace nits, MSVC vs GCC differences in `std::clamp` headers (`<algorithm>`).
3. `ctest --test-dir build --output-on-failure` — should print four passing tests in `test_marker_model`.
4. `./build/censorcut` — open a movie via `Ctrl+O`, scrub, mark a cut with `[` and `]`, confirm a marker appears in the list, save with `Ctrl+S`, verify the sidecar JSON is created next to the movie.

If any of those steps fail, **fix them first**. Commit and push.

---

## Then: Milestone M2 — FFmpeg export

This is the milestone that makes the app actually do its core job. After M2 the user has a working tool for the manual case; ML detection (M3+) is then strictly an enhancement.

### Files to create

```
src/core/FfmpegRunner.{h,cpp}
src/core/ExportController.{h,cpp}
src/ui/ExportDialog.{h,cpp}
tests/test_export_planner.cpp
```

### `FfmpegRunner` responsibilities

- Wraps `QProcess` running an `ffmpeg` binary
- Locates `ffmpeg` via (in order): bundled `third_party/ffmpeg/bin/`, `PATH` lookup, then a configurable override
- Parses `-progress pipe:1` output. The format is key=value pairs separated by newlines, terminated by a line `progress=continue` (each tick) or `progress=end` (final). Useful keys: `out_time_ms` (microseconds despite the name), `frame`, `fps`, `speed`, `bitrate`.
- Emits `progressChanged(double fraction)` (caller computes fraction from `out_time_ms / total_duration_ms`), `finished(bool ok, QString stderrTail)`, `failed(QString reason)`.
- Supports cancellation: `cancel()` should `kill()` the subprocess and clean up.

### `ExportController` responsibilities

Given a `Project` and a destination path, it produces the censored file. Algorithm:

1. Compute **keep-segments** as the inverse of confirmed markers within `[0, durationMs)`. Skip empty segments. Adjacent confirmed markers with no gap should merge upstream — `MarkerModel::totalConfirmedCutMs` already does this kind of merge; replicate the merging logic here for the segment list.
2. Write each keep-segment to a temp directory as a separately-encoded mp4:
   - `ffmpeg -y -ss <startSec> -i <input> -to <durationSec> -c:v libx264 -crf 18 -preset medium -c:a aac -b:a 192k -pix_fmt yuv420p -vsync cfr -r <fps> <tempDir>/segN.mp4`
   - `<startSec>` and the implied `<durationSec>` come from `to_string(seg.startMs / 1000.0)` etc. Use `-to` not `-t` (output-side, frame-accurate).
   - `<fps>` comes from `ffprobe`. If the source is variable framerate, use the average framerate and add `-vsync cfr` (already shown above) so concat boundaries don't drift.
3. Write a concat list file:
   ```
   file 'seg0.mp4'
   file 'seg1.mp4'
   ```
4. Concat losslessly:
   - `ffmpeg -y -f concat -safe 0 -i list.txt -c copy <output>`
5. On success, move the output file from temp to its final location (`Project::censoredOutputPathFor(sourceFile, activeProfile.minAge)`). On failure, clean up temp.

### Edge cases the export planner must handle

- Confirmed marker at exactly `startMs == 0` (no leading keep-segment)
- Confirmed marker ending exactly at `durationMs` (no trailing keep-segment)
- Adjacent confirmed markers with no gap (merge into one)
- Overlapping markers (merge — already merged in `MarkerModel::totalConfirmedCutMs` logic; mirror it)
- Zero-duration source (refuse, surface error)
- All confirmed markers cover the whole movie (refuse, surface error)
- HDR/10-bit source: detect via `ffprobe` (`pix_fmt` containing `10le` or color space `bt2020`), warn that re-encoding will tonemap to SDR unless explicit HDR-preserving encode is requested (out of scope for v1 — just warn).

### `ExportDialog` UI

Modal dialog with:

- Source filename and computed output path (read-only, with a "Choose…" button to override)
- Quality preset radio: **Frame-accurate (re-encode, slow)** — default — vs **Fast (keyframe-aligned, near-instant)**. The fast path is `ffmpeg -ss <start> -to <end> -i <input> -c copy <segN.mp4>` per segment.
- Progress bar bound to `FfmpegRunner::progressChanged`
- Phase label: "Encoding segment 3 of 7" (for the per-segment phase) and "Combining" (for the concat phase)
- Cancel button that calls `runner->cancel()` and cleans up temp
- On completion: show a "Show in Folder" / "Open" / "Close" choice

Refuse to start if the destination file already exists — present a "Replace existing file?" dialog before proceeding.

### Tests for M2

`tests/test_export_planner.cpp` should cover the planner without actually running ffmpeg:

```
- planSimple: one cut in the middle → two keep-segments
- planLeadingCut: cut at startMs=0 → one trailing keep-segment
- planTrailingCut: cut at endMs==duration → one leading keep-segment
- planFullCut: confirmed markers cover the whole movie → error
- planOverlapping: two overlapping confirmed markers → merged into one cut, two keep-segments
- planAdjacent: two adjacent confirmed markers with no gap → merged
- planRejectedIgnored: rejected/pending markers don't affect the plan
- buildFfmpegArgsAccurate: verify the per-segment arg list for the accurate path
- buildFfmpegArgsFast: verify the per-segment arg list for the fast path
- outputPathUsesAge: integration check that ExportController uses Project::censoredOutputPathFor
```

Don't shell out to ffmpeg in tests. Build the arg vectors as data and assert on them.

---

## Workflow notes

- **Branch convention:** main only for solo development. If the codebase grows, switch to per-milestone branches (`m2-export`, `m3-analyzer`).
- **Commit style:** the existing two commits use a short subject + an explanatory body. Match that.
- **Author:** GitHub user is `scotjam`. Make sure `git config user.name` and `user.email` on the build machine match what the user wants on commits.
- **Build machine:** Windows, repo at `D:\censorcut-repo`. PowerShell is the default shell. Keep file paths cross-platform in code (use `QDir`, `QFileInfo`, `QStandardPaths`) — never hardcode `\` or `/`.

---

## When the user asks about features beyond M2

Defer with: "M3 (heuristic detectors), M4 (YAMNet), M5 (vision), M6 (dialogue) are designed in PLAN.md but not yet implemented." Don't accidentally start M3 work while M2 is incomplete — the temptation is real because the design doc lays it all out, but the export pipeline is the core deliverable and needs to ship first.

The plan deliberately stages so each milestone produces an independently useful tool. Don't compress.

---

## Things that might trip you up

1. **`Project::activeProfile.minAge` is the source of truth for the filename age.** Don't read it from a UI control or pass it as a parameter — it's already on the Project.

2. **libVLC asymmetric frame stepping.** `next_frame` is exact; backward step is a `seek(currentMs - 42)` approximation. If M2 needs frame-accurate marking (it does for the user-facing cut points), the user is expected to nudge — don't try to fix this in M2.

3. **The video widget uses `Qt::WA_PaintOnScreen`.** libVLC paints directly. Don't try to draw overlays from Qt onto the video surface; you'll get tearing or nothing. If you ever need overlays, switch to libVLC's video callbacks (`libvlc_video_set_callbacks`) and let Qt paint — that's an explicit, larger refactor.

4. **`completeBaseName()` vs `baseName()`.** The output-path helper uses `completeBaseName` so `Foo.Bar.2020.mkv` becomes `Foo.Bar.2020 CENSORED-7.mkv`. Don't switch to `baseName` "for cleanliness" — it would mangle multi-dot filenames.

5. **Sidecar hash check.** The current code warns but doesn't refuse to load a sidecar against a changed file. The user can still proceed — marker times might be slightly off but usually still close. Don't make this a hard block without discussing with the user; movies do get re-encoded sometimes (e.g. by Plex) without changing meaningfully.
