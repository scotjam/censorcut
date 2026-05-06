"""CensorCut audio analyzer entry point.

Pipeline:
  1. Probe duration via ffprobe.
  2. Run the loudness detector (always available — pure ffmpeg ebur128).
  3. Run the YAMNet detector if installed (M4) for the labels referenced
     by the default category set.
  4. Fuse each enabled category, threshold, pad, emit suggestions.
  5. Write a result JSON with all raw scores + suggestions.

Usage:
    python -m censorcut.analyze --input MOVIE --out RESULT.json [--profile PROFILE.json]

Stdout emits "PROGRESS <0..1> phase=<name>" lines for the host UI;
diagnostics go to stderr.
"""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Dict, List, Optional

from . import categories as cat_mod
from .detectors import audio_loudness
from .detectors.base import DetectorOutput, Series

PROGRESS_PREFIX = "PROGRESS"


def emit_progress(frac: float, phase: str) -> None:
    frac = max(0.0, min(1.0, float(frac)))
    print(f"{PROGRESS_PREFIX} {frac:.3f} phase={phase}", flush=True)


def probe_duration_ms(input_path: str) -> int:
    ffprobe = shutil.which("ffprobe") or "ffprobe"
    cmd = [ffprobe, "-v", "error", "-print_format", "json",
           "-show_entries", "format=duration", input_path]
    out = subprocess.check_output(cmd, encoding="utf-8")
    duration = float(json.loads(out)["format"]["duration"])
    return int(duration * 1000)


def _try_run_yamnet(input_path, duration_ms, labels, progress):
    """Run the YAMNet detector if its dependencies and model are available."""
    try:
        from .detectors import audio_label  # noqa: WPS433
    except Exception as e:  # pragma: no cover
        print(f"censorcut.analyze: YAMNet module not loadable: {e}", file=sys.stderr)
        return None
    try:
        return audio_label.run(input_path, duration_ms=duration_ms,
                               labels=labels, progress=progress)
    except audio_label.YamnetUnavailable as e:
        print(f"censorcut.analyze: YAMNet not available — {e}", file=sys.stderr)
        return None
    except Exception as e:
        print(f"censorcut.analyze: YAMNet pass failed: {e}", file=sys.stderr)
        return None


def _try_run_clip(input_path, duration_ms, prompts, progress):
    """Run CLIP zero-shot scoring if torch+open_clip+model are available."""
    try:
        from .detectors import vision_clip  # noqa: WPS433
    except Exception as e:
        print(f"censorcut.analyze: vision_clip module not loadable: {e}",
              file=sys.stderr)
        return None
    try:
        return vision_clip.run(input_path, duration_ms=duration_ms,
                               prompts=prompts, progress=progress)
    except vision_clip.ClipUnavailable as e:
        print(f"censorcut.analyze: CLIP not available — {e}", file=sys.stderr)
        return None
    except Exception as e:
        print(f"censorcut.analyze: CLIP pass failed: {e}", file=sys.stderr)
        return None


def _try_run_whisper(input_path, duration_ms, keywords, progress):
    """Run Whisper transcription + keyword-spotting if available."""
    try:
        from .detectors import dialogue_whisper  # noqa: WPS433
    except Exception as e:
        print(f"censorcut.analyze: dialogue_whisper module not loadable: {e}",
              file=sys.stderr)
        return None
    try:
        return dialogue_whisper.run(input_path, duration_ms=duration_ms,
                                    keywords=keywords, progress=progress)
    except dialogue_whisper.WhisperUnavailable as e:
        print(f"censorcut.analyze: Whisper not available — {e}", file=sys.stderr)
        return None
    except Exception as e:
        print(f"censorcut.analyze: Whisper pass failed: {e}", file=sys.stderr)
        return None


def _round_floats(values, ndigits: int) -> list:
    return [round(float(v), ndigits) for v in values]


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(prog="censorcut.analyze",
                                     description="Audio heuristic + YAMNet analyzer")
    parser.add_argument("--input",  required=True, help="Path to the source video")
    parser.add_argument("--out",    required=True, help="Path to write result JSON")
    parser.add_argument("--profile", help="Path to age-profile JSON (advisory)")
    parser.add_argument("--categories",
                        help="Override path to a categories JSON; defaults to "
                             "data/default_categories.json")
    parser.add_argument("--no-yamnet", action="store_true",
                        help="Skip the YAMNet pass even if it's installed")
    parser.add_argument("--no-clip", action="store_true",
                        help="Skip the CLIP vision pass even if installed")
    parser.add_argument("--no-whisper", action="store_true",
                        help="Skip the Whisper dialogue pass even if installed")
    parser.add_argument("--threshold-mul", type=float, default=1.0,
                        help="Multiplier on every category's threshold; "
                             "values <1.0 are more sensitive (more suggestions), "
                             ">1.0 are stricter. Default 1.0.")
    args = parser.parse_args(argv)

    input_path = args.input
    if not Path(input_path).is_file():
        print(f"censorcut.analyze: input not found: {input_path}", file=sys.stderr)
        return 2

    emit_progress(0.0, "probe")
    try:
        duration_ms = probe_duration_ms(input_path)
    except FileNotFoundError as e:
        print(f"censorcut.analyze: ffprobe not found ({e}). Install ffmpeg.",
              file=sys.stderr)
        return 3
    except Exception as e:
        print(f"censorcut.analyze: ffprobe failed: {e}", file=sys.stderr)
        return 3
    if duration_ms <= 0:
        print("censorcut.analyze: source duration is zero or unknown",
              file=sys.stderr)
        return 4

    # Load category recipes.
    try:
        cats_path = Path(args.categories) if args.categories else None
        categories = cat_mod.load_categories(cats_path)
    except Exception as e:
        print(f"censorcut.analyze: could not load categories: {e}",
              file=sys.stderr)
        return 5

    # Apply the global sensitivity multiplier to each category's threshold.
    # Clamp the multiplier to a sane band; the UI offers 0.5..2.0.
    threshold_mul = max(0.25, min(4.0, float(args.threshold_mul)))
    if threshold_mul != 1.0:
        for cat in categories:
            t = float(cat.get("threshold", 0.5)) * threshold_mul
            cat["threshold"] = max(0.0, min(1.0, t))

    # 1) Loudness pass — always.
    emit_progress(0.05, "loudness")
    try:
        loud = audio_loudness.run(input_path, duration_ms, progress=emit_progress)
    except FileNotFoundError as e:
        print(f"censorcut.analyze: ffmpeg not found ({e}). Install ffmpeg.",
              file=sys.stderr)
        return 3
    except Exception as e:
        print(f"censorcut.analyze: loudness pass failed: {e}", file=sys.stderr)
        return 5

    series_by_key: Dict[str, Series] = dict(loud)

    # 2) YAMNet pass — if available.
    yamnet_labels = cat_mod.required_labels(categories, detector_id="audio.yamnet")
    yamnet_used = False
    if yamnet_labels and not args.no_yamnet:
        emit_progress(0.30, "yamnet")
        out = _try_run_yamnet(input_path, duration_ms, yamnet_labels,
                              progress=emit_progress)
        if out:
            series_by_key.update(out)
            yamnet_used = True

    # 3) CLIP vision pass — if available. Prompts are 'labels' under the
    #    'vision.clip' detector id in each category recipe.
    clip_prompts = cat_mod.required_labels(categories, detector_id="vision.clip")
    # 'vision.clip' uses params.prompts (not labels) by convention; tolerate
    # either spelling.
    clip_prompts.extend([p for p in cat_mod.required_prompts(categories,
                                                              detector_id="vision.clip")
                         if p not in clip_prompts])
    clip_used = False
    if clip_prompts and not args.no_clip:
        emit_progress(0.42, "clip")
        out = _try_run_clip(input_path, duration_ms, clip_prompts,
                            progress=emit_progress)
        if out:
            series_by_key.update(out)
            clip_used = True

    # 4) Whisper dialogue pass — if available.
    dialogue_keywords = cat_mod.required_labels(categories,
                                                 detector_id="dialogue.whisper")
    dialogue_keywords.extend([k for k in cat_mod.required_prompts(categories,
                                                                    detector_id="dialogue.whisper")
                              if k not in dialogue_keywords])
    whisper_used = False
    if dialogue_keywords and not args.no_whisper:
        emit_progress(0.55, "whisper")
        out = _try_run_whisper(input_path, duration_ms, dialogue_keywords,
                               progress=emit_progress)
        if out:
            series_by_key.update(out)
            whisper_used = True

    # 5) Fuse + emit suggestions per category.
    emit_progress(0.95, "fuse")
    all_suggestions: List[dict] = []
    for cat in categories:
        if not cat.get("enabled", True):
            continue
        fused = cat_mod.fuse_category(cat, series_by_key, duration_ms=duration_ms)
        suggestions = cat_mod.fused_to_suggestions(cat, fused,
                                                   duration_ms=duration_ms)
        all_suggestions.extend(suggestions)

    # Merge any overlapping ranges within each category (cheap).
    all_suggestions = cat_mod.merge_overlapping_in_category(all_suggestions,
                                                             merge_gap_ms=200)

    # 4) Write output JSON.
    raw_scores_out = {}
    for key, s in series_by_key.items():
        raw_scores_out[key] = {
            "samplePeriodMs": s.get("period_ms", 100),
            "values": _round_floats(s.get("values", []), 3),
        }

    result = {
        "schemaVersion": 1,
        "sourceFile":   str(Path(input_path).resolve()),
        "durationMs":   duration_ms,
        "yamnetUsed":   yamnet_used,
        "clipUsed":     clip_used,
        "whisperUsed":  whisper_used,
        "rawScores":    raw_scores_out,
        "suggestions":  all_suggestions,
    }
    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps(result, indent=2), encoding="utf-8")

    emit_progress(1.0, "done")
    return 0


if __name__ == "__main__":
    sys.exit(main())
