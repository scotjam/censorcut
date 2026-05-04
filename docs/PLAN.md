# CensorCut — Project Plan

A native C++ video editor for marking and removing scary scenes from kids' movies, with optional age-aware ML-assisted scene detection.

---

## 1. Goals and non-goals

**Goals**

- Play any common video file (mp4, mkv, avi, mov, webm)
- Let the user mark cut ranges with frame-accurate start/end points while watching
- Optionally auto-suggest cuts based on the watching child's age
- Produce a new file `<original> CENSORED.<ext>` with marked sections removed; original is never touched
- Save/load editing decisions as a sidecar so work can be resumed
- Run fully offline on a single machine (Windows/macOS/Linux)

**Non-goals**

- Not a general video editor (no trimming for taste, transitions, color, titles)
- No cloud, no accounts, no telemetry
- Not real-time censoring during playback — produces a new file
- Not a transcoder UI — sensible defaults, minimally exposed

---

## 2. Tech stack

| Layer | Choice | Why |
|---|---|---|
| Language | C++17 | Stated preference; good fit for Qt + libVLC |
| UI | Qt 6 (Widgets) | Mature, cross-platform, VLC itself uses it for reference |
| Playback | libVLC | Handles every codec kids' movies use, stable C API, LGPL |
| Cutting | FFmpeg CLI (subprocess) | Simpler than libav*, easy to debug, easy to swap encoders |
| Analysis | Python helper (TensorFlow Lite + librosa, later CLIP, later Whisper) | YAMNet easiest in Python; runs as a subprocess |
| Sidecar | JSON (nlohmann/json) | Human-readable, easy to diff |
| Build | CMake + vcpkg | Standard, handles Qt + libVLC + nlohmann |

External binaries shipped/required: `ffmpeg`, `ffprobe`, `python` with a virtual env containing the analysis dependencies. Bundle them on Windows/macOS, document install on Linux.

---

## 3. High-level architecture

```
┌─────────────────────────────────────────────────────────┐
│                      Qt UI Layer                        │
│   MainWindow · Timeline · MarkerList · AnalyzerPanel    │
└──────────────────┬──────────────────────────────────────┘
                   │
┌──────────────────┴──────────────────────────────────────┐
│                   Application Core                      │
│  Project · MarkerModel · PlaybackController             │
│  AnalysisController · ExportController · AgeProfile     │
└───┬──────────────┬────────────────────┬─────────────────┘
    │              │                    │
┌───┴──────┐  ┌────┴──────────┐  ┌──────┴────────────┐
│ libVLC   │  │ Python helper │  │ FFmpeg subprocess │
│ playback │  │ (analysis)    │  │ (export)          │
└──────────┘  └───────────────┘  └───────────────────┘
```

Core is pure C++ with no Qt dependencies, so it's testable without a UI. Qt sits on top and observes via signals. Subprocesses (Python analyzer, FFmpeg) communicate over stdout with structured progress messages.

---

## 4. Data model

```cpp
// Times are int64_t milliseconds throughout. No floats for time.

enum class Source     { Manual, Suggested, Imported };
enum class Status     { Pending, Confirmed, Rejected };

struct Marker {
    QUuid       id;
    int64_t     startMs;
    int64_t     endMs;
    QString     category;       // "Jump scare", "Sword fight", custom
    QString     note;            // user free text
    Source      source;
    double      confidence;      // 0.0–1.0 for Suggested
    Status      status;
};

struct AgeProfile {
    int                 minAge;       // youngest viewer
    QString             label;        // "Under 5", custom
    double              thresholdMul; // global multiplier on category thresholds
    double              padMul;       // global multiplier on category padding
    QHash<QString, CategoryOverride> overrides;
};

struct ExportSettings {
    QString     videoCodec = "libx264";
    int         crf        = 18;
    QString     preset     = "medium";
    QString     audioCodec = "aac";
    int         audioBitrateKbps = 192;
    bool        copyAllAudioTracks = false;
    bool        keyframeAlignedFast = false; // M2: optional fast mode
};

struct Project {
    QString             sourceFile;     // absolute path
    QString             sourceHash;     // sha1 of first+last 1MB + size
    int64_t             durationMs;
    QList<Marker>       markers;
    ExportSettings      exportSettings;
    AgeProfile          activeProfile;
    AnalysisResult      lastAnalysis;   // raw scores, for re-thresholding
    int                 schemaVersion;
};
```

The `sourceHash` lets the app warn if a sidecar is loaded against a different file. Storing raw `AnalysisResult` means thresholds can be re-tuned without re-running ML.

**Sidecar location**: `<movie>.censorcut.json` next to the movie file (with fallback to app data when source folder is read-only).

---

## 5. Categories and detectors

A **detector** is a code module that produces a per-second 0–1 score over the movie. A **category** is a user-defined recipe combining one or more detectors with weights, a threshold, and padding before/after the matched range.

### Detector types

| Type | Status | What it does |
|---|---|---|
| Audio-label (YAMNet) | M4 | Scores each window against a configurable set of AudioSet labels |
| Audio-heuristic | M3 | Hand-coded: jump-scare loudness delta, low-frequency drone, brightness, fast-tempo proxy |
| Vision (CLIP) | M5 | Zero-shot scoring against text prompts on 1 fps frames |
| Dialogue (Whisper) | M6 | Transcribes audio, matches keywords/phrases |

### Default categories shipped

| Category | Audio-only viable? | Notes |
|---|---|---|
| Jump scare | Yes | Loudness spike + scream |
| Scary music | Yes | Drone + dissonance + AudioSet `Scary music` |
| Screaming | Yes | Direct YAMNet labels |
| Crying / distress (sad) | Yes | YAMNet `Crying, sobbing` + sad music heuristic |
| Yelling | Yes | YAMNet `Yell`, `Shout` (non-scream angry voices) |
| Gunfire | Yes | Direct YAMNet labels |
| Hitting / impacts | Yes | `Slap`, `Thump`, `Smash` + grunt |
| Sword fight | Partial | Audio: clangs, yells. Vision adds visual confirmation. |
| Chase | Partial | Audio: tempo, footsteps, breathing. Vision adds running. |
| Violence (general) | Partial | Bundles impacts + screams. Vision adds visual fighting. |
| Cruelty | Vision-required | Disabled until M5 |
| Pushing | Vision-required | Disabled until M5 |
| Kill / threat | Vision + dialogue | Disabled until M5/M6 |

Categories that need detectors not yet available are shown disabled with a tooltip explaining why.

### Category recipe format

```json
{
  "name": "Sword fight",
  "enabled": true,
  "threshold": 0.55,
  "padBeforeMs": 1000,
  "padAfterMs": 1000,
  "requires": [],
  "detectors": [
    { "id": "audio.label", "weight": 0.7,
      "params": { "labels": ["Sword", "Whoosh", "Clang"] } },
    { "id": "audio.label", "weight": 0.3,
      "params": { "labels": ["Yell", "Battle cry", "Grunt"] } },
    { "id": "vision.clip", "weight": 0.8,
      "params": { "prompts": ["sword fight", "people dueling with swords"] } }
  ]
}
```

Categories are user-customizable through an editor UI (M4.5): rename, adjust thresholds and padding, edit AudioSet label sets via autocomplete, add CLIP prompts, duplicate, delete.

---

## 6. Age profiles

A single "watching with age N" control drives the entire analyzer. Each age has a **profile** that adjusts every category's threshold, padding, and on/off state. The user sees a slider; the system translates it into a full category configuration.

### Default age profiles

| Age | Stance | What gets cut |
|---|---|---|
| Under 5 | Maximum protection | Everything plausible. Low thresholds (0.35–0.45), 1.5× padding. Even mild peril, raised voices, sad scenes flagged. Expect false positives — that's the trade. |
| 5–6 | Strong protection | Jump scares, screaming, scary music, monsters, gunfire, visible violence, cruelty, kill/threat dialogue. Thresholds ~0.45. Sad scenes optional. |
| 7–8 | Moderate protection | Jump scares, real fear/screaming, gunfire, cruelty, explicit kill threats, intense chases. Cartoonish slapstick allowed. Thresholds ~0.55. |
| 9–10 | Light protection | Jump scares, graphic violence, cruelty, real-feeling threat. Sword fights and chases generally kept. Thresholds ~0.65. |
| 11–12 | Minimal | Only jump scares and genuinely cruel/graphic content. Thresholds ~0.75. |
| 13–14 | Very minimal | Jump scares only by default. Most kids' movies produce zero cuts. |
| 15+ | Off | All categories disabled by default. Manual marking still available. |

The slider is non-linear — the gap between "under 5" and "5–6" is smaller than between "11–12" and "13–14" because the youngest tier needs the widest net.

### Multi-child households

Slider accepts a single age or a "multiple kids" mode with two age inputs. **Always uses the lower bound** for cut decisions, with a banner: *"Using profile for age 4 (youngest)"*.

### Profile layers

Three layers, in priority order:

1. **Built-in age profiles** — shipped with the app, never modified
2. **User custom profiles** — saved in app config, e.g. *"Sam (6, sensitive to dogs in peril, OK with sword fights)"*
3. **Per-project overrides** — saved in the movie's sidecar; survive across re-analyses

---

## 7. UI layout

```
┌─────────────────────────────────────────────────────────────┐
│ File  Edit  Analyze  Export  Help                           │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│                    Video Surface (libVLC)                   │
│                                                             │
├─────────────────────────────────────────────────────────────┤
│  ⏮  ▶  ⏭   00:42:13 / 01:34:22    [   ]  [  ]   1x ▾        │
├─────────────────────────────────────────────────────────────┤
│ Timeline:                                                   │
│ ░░░░░▓▓▓░░░░░░░▓▓▓▓▓░░░░░░░░░░▓▓░░░░░░░░░░░░░▓▓▓▓░░░░░░░    │
│      ↑pending  ↑confirmed              ↑playhead            │
├──────────────────────────┬──────────────────────────────────┤
│ Marker list              │ Analyzer                         │
│ ┌──────────────────────┐ │ Watching with: Age [4 ▾]         │
│ │ ✓ 00:12:04–00:12:31  │ │ Profile: Under 5                 │
│ │ ? 00:42:08–00:42:55  │ │ [Customize categories ▾]         │
│ │ ✗ 01:08:22–01:08:30  │ │                                  │
│ │ ✓ 01:21:11–01:21:48  │ │ [Run analysis]   ⏱ ~4 min        │
│ └──────────────────────┘ │                                  │
│                          │ ▼ Categories                     │
│                          │ ☑ Jump scare      0.35  18 hits  │
│                          │ ☑ Scary music     0.35  24 hits  │
│                          │ ☑ Screaming       0.40  11 hits  │
│                          │ ☑ Cruelty (vision — M5)          │
│                          │                                  │
│                          │ Total: ~37% of runtime           │
│                          │ ⚠ Aggressive — review carefully  │
└──────────────────────────┴──────────────────────────────────┘
```

### Key UI behaviors

- `[` sets cut start at playhead; `]` sets cut end at playhead and creates the marker
- `J/K/L` for shuttle (back/pause/forward), `,` and `.` for frame step, `Space` for play/pause — standard NLE shortcuts
- Hovering a marker on the timeline scrubs to it; double-click to play it; right-click to confirm/reject/delete
- Suggested markers are colored by category; confirmed = solid; pending = striped; rejected = greyed
- Per-category threshold slider re-filters live without re-running analysis
- "Reset to age default" button per category, since once thresholds are nudged the slider's "Under 5" label becomes a partial lie
- Live runtime estimate (*"would remove ~37% of runtime"*) so parents see the impact before reviewing
- Aggressive-profile warning when total cuts exceed 25% of runtime

---

## 8. Components in detail

### 8.1 PlaybackController (libVLC wrapper)

- Owns `libvlc_instance_t` and `libvlc_media_player_t`
- Public: `open(path)`, `play()`, `pause()`, `seek(ms)`, `stepFrame(±1)`, `setRate(x)`
- Signals: `positionChanged(ms)`, `durationKnown(ms)`, `stateChanged`, `error`
- Embeds video as a child window of a `QWidget` via `libvlc_media_player_set_hwnd`/`set_xwindow`/`set_nsobject` — simpler/faster than Qt-painted overlays for v1
- Frame stepping: `libvlc_media_player_next_frame` for forward; backward = `seek(currentMs - frameDuration)` (approximate)

### 8.2 MarkerModel

- `QAbstractListModel` for the marker list; same data drives `TimelineWidget`
- Operations: add, update, remove, merge overlapping, split. All reversible via `QUndoStack`.
- Validates: `endMs > startMs`, no zero-length, snaps within 1 frame of duration boundaries

### 8.3 AnalysisController

- Spawns `python -m censorcut.analyze --input <movie> --out <json> --profile <profile.json>` as `QProcess`
- Reads progress lines like `PROGRESS 0.42 phase=yamnet` from stdout
- On completion, parses JSON: array of `{startMs, endMs, score, category, reasons[]}`
- Inserts markers as `Source::Suggested, Status::Pending`
- Caches raw per-second scores so threshold/age changes re-filter without re-running

### 8.4 ExportController + FfmpegRunner

Builds an FFmpeg command from the **inverse** of confirmed markers (the keep-segments).

Two-pass strategy:

1. For each keep-segment, encode a temp file with input seeking + accurate cut:
   `ffmpeg -ss <start> -i input -to <duration> -c:v libx264 -crf 18 -preset medium -c:a aac -b:a 192k segN.mp4`
2. Concat losslessly:
   `ffmpeg -f concat -safe 0 -i list.txt -c copy "<base> CENSORED.<ext>"`

Uses `-progress pipe:1` for parseable progress; UI shows percent and ETA. Writes to a temp directory; on success, moves the final file next to the original; on failure, cleans up. Never touches the source file. Refuses to start if output exists unless user confirms overwrite.

**Edge cases:**

- Cut at exactly 0 or duration (no leading/trailing keep-segment)
- Adjacent cuts with nothing between (merged upstream in the model)
- Multiple audio tracks: copy track 0 by default; "preserve all tracks" checkbox
- Variable framerate: add `-vsync cfr -r <source_fps>` to avoid drift across concat boundaries
- HDR/10-bit: detect via `ffprobe`, warn that re-encoding will tonemap to SDR unless explicitly opted in (out of scope for v1)
- `-pix_fmt yuv420p` for TV/mobile compatibility

---

## 9. Analysis pipeline (Python helper)

Entry point: `python -m censorcut.analyze --input <path> --out <json> --profile <profile.json>`.

**Steps:**

1. Probe duration via `ffprobe`
2. Extract: 16 kHz mono WAV (for audio ML), per-second loudness via `ebur128`, brightness via `signalstats`
3. **Audio loudness/brightness pass** (always on, fast)
   - Short-term LUFS in 400 ms windows
   - Flag windows where LUFS rises >12 LU within 1 s after ≥1 s below −30 LUFS → "jump scare"
   - Sustained 60–200 Hz energy above threshold for >3 s → "rumble/drone"
4. **YAMNet pass** (TFLite, ~10× realtime on CPU)
   - Score each 0.96 s window against AudioSet's 521 labels
   - Emit per-second max scores for all labels referenced by enabled categories
5. **Brightness**: average luma per second from `signalstats`
6. **Vision pass** (M5+): sample 1 fps, score frames against CLIP prompts
7. **Dialogue pass** (M6+): Whisper transcribe, keyword/phrase match
8. **Fusion**: per category, combine detector scores using configured weights → 0–1 score per second; merge contiguous high-score seconds into ranges; pad each range using category's padBefore/padAfter
9. **Output**: JSON with both merged ranges *and* raw per-second scores so UI can re-threshold without re-running

---

## 10. Honest caveats

- **The "Under 5 = cut everything" promise is aspirational until M5/M6.** Audio alone reliably catches jump scares, screams, gunfire, music cues. It will *not* catch silent cruelty, a menacing knife shot, or a sad goodbye where nothing dramatic happens acoustically. UI must say so clearly: at younger ages, also manually scrub through to verify.
- **Age recommendations don't transfer across cultures or families.** Profiles are starting points, not prescriptions.
- **Profile aggressiveness compounds with movie choice.** A G-rated Pixar film at "Under 5" can produce 15–20% cuts; a PG action film at "Under 5" produces a different movie. Surface percentage upfront so parents can decide whether to switch films instead.
- **Frame stepping in libVLC is asymmetric.** Forward is exact; backward is approximate (seek-based). Users will need to nudge for tight marks.
- **CLIP is not a classifier — it's a similarity scorer.** Vision-detected categories will need per-prompt threshold tuning; expect false positives from visually-similar-but-benign frames.

---

## 11. Project layout

```
censorcut/
├── CMakeLists.txt
├── vcpkg.json
├── README.md
├── docs/
│   └── PLAN.md                 # this file
├── third_party/
│   └── ffmpeg/                 # bundled binaries on Win/macOS
├── python/
│   ├── pyproject.toml
│   └── censorcut/
│       ├── __init__.py
│       ├── analyze.py          # entry point
│       ├── categories.py       # recipe loading + fusion
│       ├── audioset_labels.json
│       ├── data/
│       │   ├── default_categories.json
│       │   └── default_age_profiles.json
│       └── detectors/
│           ├── __init__.py
│           ├── base.py
│           ├── audio_loudness.py
│           ├── audio_label.py  # YAMNet
│           ├── audio_tempo.py
│           ├── vision_clip.py        # M5
│           └── dialogue_whisper.py   # M6
├── src/
│   ├── main.cpp
│   ├── core/
│   │   ├── Project.{h,cpp}
│   │   ├── Marker.{h,cpp}
│   │   ├── MarkerModel.{h,cpp}
│   │   ├── AgeProfile.{h,cpp}
│   │   ├── Category.{h,cpp}
│   │   ├── PlaybackController.{h,cpp}
│   │   ├── AnalysisController.{h,cpp}
│   │   ├── ExportController.{h,cpp}
│   │   └── FfmpegRunner.{h,cpp}
│   └── ui/
│       ├── MainWindow.{h,cpp,ui}
│       ├── TimelineWidget.{h,cpp}
│       ├── MarkerListView.{h,cpp}
│       ├── AnalyzerPanel.{h,cpp}
│       ├── CategoryEditor.{h,cpp}
│       └── ExportDialog.{h,cpp}
└── tests/
    ├── test_marker_model.cpp
    ├── test_export_planner.cpp
    ├── test_age_profile.cpp
    └── fixtures/
```

---

## 12. Build milestones

| # | What | Effort |
|---|---|---|
| **M1** | Player + manual markers: Qt + libVLC, timeline, `[`/`]` shortcuts, marker list, JSON sidecar save/load. **No export, no analysis.** Already useful for planning cuts. | 1–2 weekends |
| **M2** | Export pipeline: `ExportController` + `FfmpegRunner`, progress UI, two-pass encode, output naming, overwrite safety. **App now does its core job end-to-end.** | 1 weekend |
| **M3** | Heuristic detectors (loudness, brightness, drone) + age profile system in place from day one (most categories show "needs M4" but the slider works) | 1 weekend |
| **M4** | YAMNet detector + audio-viable categories (Jump scare, Screaming, Crying, Yelling, Gunfire, Hitting, partial Sword/Chase/Violence) | 1–2 weekends |
| **M4.5** | Category editor UI: custom categories, label autocomplete, weight tuning, custom age profiles | 1 weekend |
| **M5** | CLIP vision detector activates Cruelty, Pushing, visual halves of Sword/Chase/Violence | 1–2 weekends |
| **M6** | Whisper dialogue detector activates Kill/threat; bonus language filtering | 1–2 weekends |

Building age profiles in M3 even when half the categories are stubs is worth doing — it's the user's mental model from day one, and retrofitting later means rebuilding the analyzer panel.

---

## 13. Decisions to confirm before scaffolding

1. **Single-window, one movie at a time** — yes (recommended for simplicity)
2. **Sidecar location** — next to movie, fallback to app data when read-only (recommended)
3. **CLI mode** — useful for batch re-export after tweaking; cheap because core has no Qt deps
4. **Frame-accurate vs keyframe-fast export** — frame-accurate default, fast option exposed in export dialog
