# Project context for Claude Code

This file is auto-loaded by Claude Code when running in this repo. It captures the project's intent, conventions, and current state so any future Claude Code session can pick up cleanly.

## What this project is

CensorCut is a native C++ desktop app that lets a parent watch a kids' movie, mark scenes to cut, and produce a censored copy with the originals untouched. Output files use the suffix ` CENSORED-<age>` so multiple cuts of the same film for kids of different ages can sit side-by-side.

The intended end state is age-aware: a single "watching with age N" slider drives the analyzer, which suggests scenes to cut. ML detection (YAMNet for audio, CLIP for vision, Whisper for dialogue) is staged across milestones M3–M6.

The full design is in [`docs/PLAN.md`](docs/PLAN.md). Read it before making non-trivial changes — it covers the data model, detector/category framework, age profile system, FFmpeg export pipeline, and the honest caveats about what audio-only detection can and can't catch.

## Stack

- **C++17**, no exceptions in core paths (Qt convention)
- **Qt 6.5+** (Core, Widgets, Gui, Test) for UI
- **libVLC** for playback (embedded into a `QWidget` via native window handle)
- **nlohmann/json** for sidecar I/O
- **CMake 3.21+**, vcpkg-friendly
- **FFmpeg CLI** (subprocess, M2+) for cutting
- **Python helper** (M3+) for ML analysis — separate process invoked from C++

## Architecture rules to preserve

These are deliberate decisions from the design conversation. Don't undo them without thinking about it:

1. **Core has no Qt UI dependency.** Anything in `src/core/` should link against `Qt::Core` and `Qt::Gui` only — never `Qt::Widgets`. This keeps the door open for a CLI mode and makes core unit-testable. UI code lives in `src/ui/` and is the only place `Qt::Widgets` is allowed.

2. **All time values are `qint64` milliseconds.** No floats for time, anywhere. Frame durations are computed from fps when needed, but the canonical type is ms.

3. **Markers are sorted by `startMs` in the model.** `MarkerModel` enforces this on insertion and resort. Don't bypass `addMarker`/`updateMarkerById`.

4. **`Marker::category` is a free-form `QString`,** not an enum. Categories are user-defined; the M4 category editor needs schema flexibility. Built-in defaults live in (future) `python/censorcut/data/default_categories.json`.

5. **Output filename helper is `Project::censoredOutputPathFor(moviePath, age)`.** The export step in M2 must use it, not hand-roll the suffix. The age that goes into the filename is `Project::activeProfile.minAge`. Tests are in `tests/test_marker_model.cpp::censoredOutputPath`.

6. **The original file is never touched.** Export writes to a temp directory, then moves the result next to the original. If the destination exists, refuse to overwrite without explicit user confirmation.

7. **Sidecar JSON is `<movie>.censorcut.json` next to the movie.** Falls back to app data dir when the source folder is read-only (M2+).

8. **libVLC events fire on libVLC's thread.** `PlaybackController::onVlcEvent` marshals to the GUI thread via `QMetaObject::invokeMethod(Qt::QueuedConnection)`. Don't emit signals directly from the VLC callback.

9. **Subprocess communication uses line-oriented stdout.** FFmpeg uses `-progress pipe:1`; Python helpers emit `PROGRESS <0–1> phase=<name>` lines. JSON results are written to disk, not piped, so partial output doesn't corrupt parsing.

## Coding conventions

- Namespace everything under `censorcut::`.
- Header guards via `#pragma once`.
- Forward-declare libVLC opaque types in headers (`struct libvlc_instance_t;`) so `<vlc/vlc.h>` only appears in `.cpp` files. Keeps the include surface small.
- Prefer `QStringLiteral` over `QString::fromUtf8` for compile-time strings.
- Avoid `auto` for types that aren't obvious from the right-hand side. Loops over containers are fine.
- Tests use Qt Test (`QTEST_MAIN`). Mirror the source layout: `tests/test_<module>.cpp`.

## Current state (as of last commit)

**Milestone M1 is complete and pushed:** player, manual marker placement with `[`/`]`, timeline widget, marker list, sidecar save/load with content-hash file-change detection, status bar with cut percentage, age-aware output filename helper.

**The most recent commit** added `Project::censoredOutputPathFor(moviePath, age)` and updated docs/tests. See `git log` for the exact commits.

**What does NOT exist yet** (don't assume it does):

- Any FFmpeg integration — there is no `FfmpegRunner` or `ExportController` class yet. M2 builds them.
- Any analysis pipeline — `python/censorcut/` is just a directory; no scripts yet. M3 starts here.
- The `AnalysisResult` field referenced in PLAN.md's Project struct is not yet in the C++ code; add it when M3 needs it.
- Vision and dialogue detectors are M5/M6 and are explicitly not in scope for M2/M3.
- A CategoryEditor UI is M4.5.

## How to build and test

```bash
cmake -B build -S . [-DCMAKE_PREFIX_PATH=...] [-DCMAKE_TOOLCHAIN_FILE=...]
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/censorcut
```

If libVLC isn't found via pkg-config, set `LIBVLC_ROOT` to a directory containing `include/vlc/vlc.h` and `lib/libvlc.{so,dylib,dll}`. See README.md for full notes.

## Next milestone: M2 — Export pipeline

When the user asks for "export," "cutting," or "M2," this is the work:

1. **`src/core/FfmpegRunner.{h,cpp}`** — owns a `QProcess` running ffmpeg, parses `-progress pipe:1` output (key=value lines terminated by `progress=continue|end`), emits `progressChanged(double)` and `finished(bool ok, QString message)`. Supports cancel.

2. **`src/core/ExportController.{h,cpp}`** — given a `Project` and a destination path, computes keep-segments (the inverse of confirmed markers), writes them to a temp dir as separately-encoded mp4s, then concats losslessly:
   - Per-segment encode: `ffmpeg -ss <start> -i input -to <duration> -c:v libx264 -crf 18 -preset medium -c:a aac -b:a 192k segN.mp4`
   - Concat: `ffmpeg -f concat -safe 0 -i list.txt -c copy "<output>"`
   - The output path comes from `Project::censoredOutputPathFor(sourceFile, activeProfile.minAge)`.

3. **`src/ui/ExportDialog.{h,cpp}`** — modal dialog with a progress bar, a fast/accurate toggle (sets `ExportSettings::keyframeAlignedFast`), and a cancel button. Refuse to start if the destination already exists, with a "Replace existing" override.

4. **Edge cases the export planner must handle** (see PLAN.md §8 for full list):
   - Confirmed markers at exactly 0 or duration
   - Adjacent confirmed markers with no gap (merge upstream)
   - Variable framerate sources (add `-vsync cfr -r <source_fps>`)
   - HDR/10-bit detection via `ffprobe`, warn the user
   - Always include `-pix_fmt yuv420p` for compatibility

5. **Tests:** `tests/test_export_planner.cpp` covering keep-segment computation, edge cases above, and command-line generation (build the args, don't actually run ffmpeg in tests).

The user has been running everything from a Windows build machine (`D:\censorcut-repo`). Keep paths cross-platform — use `QDir`, `QFileInfo`, `QStandardPaths`, never hardcoded separators.

## Git workflow

Push to `origin/main`. The user's GitHub account is `scotjam`. Commits should be authored as them; either let `git config user.name`/`user.email` on the local machine handle it, or use `--reset-author` after applying patches generated by other tools.

## When in doubt

Re-read `docs/PLAN.md` — especially §3 (architecture), §5 (categories), §6 (age profiles), §8 (component detail), and §10 (caveats). It's deliberately thorough so future changes stay aligned with the original design intent.


<!-- BEGIN BEADS INTEGRATION v:1 profile:minimal hash:ca08a54f -->
## Beads Issue Tracker

This project uses **bd (beads)** for issue tracking. Run `bd prime` to see full workflow context and commands.

### Quick Reference

```bash
bd ready              # Find available work
bd show <id>          # View issue details
bd update <id> --claim  # Claim work
bd close <id>         # Complete work
```

### Rules

- Use `bd` for ALL task tracking — do NOT use TodoWrite, TaskCreate, or markdown TODO lists
- Run `bd prime` for detailed command reference and session close protocol
- Use `bd remember` for persistent knowledge — do NOT use MEMORY.md files

## Session Completion

**When ending a work session**, you MUST complete ALL steps below. Work is NOT complete until `git push` succeeds.

**MANDATORY WORKFLOW:**

1. **File issues for remaining work** - Create issues for anything that needs follow-up
2. **Run quality gates** (if code changed) - Tests, linters, builds
3. **Update issue status** - Close finished work, update in-progress items
4. **PUSH TO REMOTE** - This is MANDATORY:
   ```bash
   git pull --rebase
   bd dolt push
   git push
   git status  # MUST show "up to date with origin"
   ```
5. **Clean up** - Clear stashes, prune remote branches
6. **Verify** - All changes committed AND pushed
7. **Hand off** - Provide context for next session

**CRITICAL RULES:**
- Work is NOT complete until `git push` succeeds
- NEVER stop before pushing - that leaves work stranded locally
- NEVER say "ready to push when you are" - YOU must push
- If push fails, resolve and retry until it succeeds
<!-- END BEADS INTEGRATION -->
