# CensorCut

A native C++ video editor for marking and removing scary scenes from kids' movies. Original file is never touched; output is written alongside as `<original> CENSORED-<age>.<ext>` (e.g. `Title CENSORED-7.mp4`), so different cuts for different kids live side-by-side without confusion.

This repository contains the **M1 scaffold**: player + manual marker placement + sidecar JSON save/load. Export and analysis land in M2 and beyond. See [`docs/PLAN.md`](docs/PLAN.md) for the full project plan.

## Disclaimer

CensorCut is **not** designed to automatically take care of legal requirements as to what children can be shown, **nor is it a substitute for parental review**. It should only be used to further censor videos that are already legally able to be shown to children at the appropriate age.

Automated detection (M3 and later milestones) may miss things, make mistakes, or hallucinate content that isn't there. It may also cut more than is necessary. The parent remains responsible for confirming every cut and for deciding whether the resulting edit is appropriate for their child.

The software is provided **as is**, with no warranty of any kind. No responsibility can be taken for any data loss.

## Build prerequisites

- **CMake 3.21+**
- **Qt 6.5+** (Core, Widgets, Gui, Test)
- **libVLC** development headers (3.0+)
  - Linux: `apt install libvlc-dev` / `dnf install vlc-devel`
  - macOS: `brew install --cask vlc` then point CMake at the VLC.app contents
  - Windows: install the VLC SDK and set `LIBVLC_ROOT` to its root
- **nlohmann_json 3.11+** — easiest via vcpkg
- **A C++17 compiler** (GCC 11+, Clang 14+, MSVC 2022)

## Build

```bash
cmake -B build -S . -DCMAKE_PREFIX_PATH="/path/to/qt;/path/to/vcpkg/installed/<triplet>"
cmake --build build -j
```

To use vcpkg's manifest mode for `nlohmann_json` and Qt:

```bash
cmake -B build -S . \
  -DCMAKE_TOOLCHAIN_FILE=/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build -j
```

## Run

```bash
./build/censorcut
```

Then `File → Open Movie...` (or Ctrl+O).

## Keyboard shortcuts

| Key | Action |
|---|---|
| Space | Play / Pause |
| `[` | Mark cut **start** at playhead |
| `]` | Mark cut **end** at playhead (creates the marker) |
| `,` / `.` | Step back / forward one frame (back is approximate) |
| `←` / `→` | Seek −5s / +5s |
| J / K / L | Slow down / pause / speed up (NLE-style shuttle) |
| Ctrl+S | Save markers to sidecar |
| Ctrl+O | Open movie |

Markers are saved next to the movie as `<movie>.censorcut.json`.

## What works in M1

- Open mp4/mkv/avi/mov/webm via libVLC
- Frame-accurate `[`/`]` marker placement
- Timeline with playhead, pending-start handle, and colored marker bands
- Marker list with right-click confirm/reject/delete
- Sidecar JSON save/load (with file-changed warning via content hash)
- Status bar showing total runtime that would be cut

## What's coming

- **M2** — FFmpeg-based export to a `CENSORED` file
- **M3** — Heuristic analyzer (loudness, brightness) with age profiles
- **M4** — YAMNet audio classification, full default category set
- **M5** — CLIP vision detector
- **M6** — Whisper dialogue detector

See `docs/PLAN.md`.
