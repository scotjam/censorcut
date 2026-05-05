"""CensorCut M3 audio heuristic analyzer.

Drives ffmpeg's ebur128 loudness filter, parses the per-window momentary
and short-term LUFS samples it prints to stderr, and detects "jump scare"
windows: a sudden loudness rise after a quiet stretch.

The result and the raw per-sample scores are written to JSON so the host
app can re-threshold (or pick a different age profile) without re-running.

Output format (schemaVersion 1):

    {
      "schemaVersion": 1,
      "sourceFile":   "...",
      "durationMs":   5400000,
      "rawScores": {
        "audio.lufs.momentary":  {"samplePeriodMs": 100, "values": [...]},
        "audio.lufs.shortTerm":  {"samplePeriodMs": 100, "values": [...]},
        "audio.jumpScore":       {"samplePeriodMs": 100, "values": [...]}
      },
      "suggestions": [
        { "category":  "Jump scare",
          "startMs":   12000,
          "endMs":     14500,
          "score":     0.85,
          "reasons":   ["momentary LUFS rose +14.0 LU after 1.0s avg -33.2 LUFS"] }
      ]
    }

Stdout emits "PROGRESS <0..1> phase=<name>" lines suitable for parsing
by the host UI. All other diagnostics go to stderr.

Usage:
    python -m censorcut.analyze --input MOVIE --out RESULT.json
"""

from __future__ import annotations

import argparse
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Iterable, List, Tuple

PROGRESS_PREFIX = "PROGRESS"

# ebur128 stderr line, e.g.:
#   [Parsed_ebur128_0 @ ...] t: 1.20         M: -23.0 S:-120.7     I: -22.4 LUFS ...
# The TARGET prefix is sometimes inserted; we tolerate either form.
_EBUR128_LINE_RE = re.compile(
    r"t:\s*(?P<t>[-\d.]+)\s+"
    r"(?:TARGET:\s*[-\d.]+\s*LUFS\s+)?"
    r"M:\s*(?P<m>[-\d.+]+|nan|-inf)\s+"
    r"S:\s*(?P<s>[-\d.+]+|nan|-inf)"
)


def emit_progress(frac: float, phase: str) -> None:
    """Print a host-parseable progress line to stdout."""
    frac = max(0.0, min(1.0, float(frac)))
    print(f"{PROGRESS_PREFIX} {frac:.3f} phase={phase}", flush=True)


def _ffmpeg_exe() -> str:
    return shutil.which("ffmpeg") or "ffmpeg"


def _ffprobe_exe() -> str:
    return shutil.which("ffprobe") or "ffprobe"


def probe_duration_ms(input_path: str) -> int:
    """Return source duration in milliseconds via ffprobe."""
    cmd = [_ffprobe_exe(), "-v", "error", "-print_format", "json",
           "-show_entries", "format=duration", input_path]
    out = subprocess.check_output(cmd, encoding="utf-8")
    duration = float(json.loads(out)["format"]["duration"])
    return int(duration * 1000)


def _parse_lufs(value: str) -> float:
    """Convert ebur128's -inf / nan / numeric string to a float, with a sane
    floor for 'silence' so callers can do arithmetic without special cases."""
    try:
        v = float(value)
    except (TypeError, ValueError):
        return -120.0
    if v != v:  # NaN
        return -120.0
    if v == float("-inf") or v < -120.0:
        return -120.0
    return v


def run_ebur128(input_path: str, duration_ms: int) -> Iterable[Tuple[float, float, float]]:
    """Yield (t_sec, momentary_LUFS, short_term_LUFS) tuples from ffmpeg's
    ebur128 filter as it processes the file."""
    cmd = [_ffmpeg_exe(), "-nostats", "-hide_banner",
           "-i", input_path,
           "-vn",
           "-af", "ebur128",
           "-f", "null", "-"]
    proc = subprocess.Popen(cmd,
                            stderr=subprocess.PIPE,
                            stdout=subprocess.DEVNULL,
                            encoding="utf-8",
                            errors="replace")
    last_progress = 0.0
    try:
        assert proc.stderr is not None
        for line in proc.stderr:
            m = _EBUR128_LINE_RE.search(line)
            if not m:
                continue
            t_sec = float(m.group("t"))
            mom   = _parse_lufs(m.group("m"))
            short = _parse_lufs(m.group("s"))
            yield (t_sec, mom, short)
            if duration_ms > 0:
                # ebur128 phase covers up to 0.95 of overall progress.
                frac = min(0.95, t_sec * 1000.0 / duration_ms * 0.95)
                if frac - last_progress > 0.01:
                    last_progress = frac
                    emit_progress(frac, "loudness")
    finally:
        proc.wait()
        if proc.returncode not in (0, None):
            raise RuntimeError(f"ffmpeg exited {proc.returncode}")


def detect_jump_scares(momentary: List[float],
                       duration_ms: int,
                       sample_period_ms: int = 100,
                       rise_threshold_lu: float = 12.0,
                       quiet_threshold_lufs: float = -30.0,
                       rise_window_sec: float = 1.0,
                       quiet_window_sec: float = 1.0,
                       pre_pad_sec: float = 1.0,
                       post_pad_sec: float = 1.5,
                       merge_gap_sec: float = 2.0
                       ) -> Tuple[List[dict], List[float]]:
    """Find windows where momentary LUFS rose by >= rise_threshold_lu within
    rise_window_sec, after the previous quiet_window_sec averaged below
    quiet_threshold_lufs.

    Returns (suggestions, per-sample jump scores in [0..1])."""
    samples_per_sec = 1000 // sample_period_ms
    rise_n  = max(1, int(rise_window_sec  * samples_per_sec))
    quiet_n = max(1, int(quiet_window_sec * samples_per_sec))
    pre_n   = max(0, int(pre_pad_sec      * samples_per_sec))
    post_n  = max(0, int(post_pad_sec     * samples_per_sec))

    n = len(momentary)
    scores: List[float] = [0.0] * n
    suggestions: List[dict] = []

    last_trigger_idx = -10**9  # cooldown so each rise fires at most once per window
    cooldown_n = rise_n + quiet_n

    for i in range(rise_n + quiet_n, n):
        if i - last_trigger_idx < cooldown_n:
            continue

        before = momentary[i - rise_n]
        now    = momentary[i]
        rise   = now - before
        if rise < rise_threshold_lu:
            continue

        quiet_start = i - rise_n - quiet_n
        quiet_end   = i - rise_n
        quiet_slice = momentary[quiet_start:quiet_end]
        if not quiet_slice:
            continue
        quiet_avg = sum(quiet_slice) / len(quiet_slice)
        if quiet_avg > quiet_threshold_lufs:
            continue

        # Score: how much above the threshold we got, capped at 1.0.
        score = min(1.0, rise / 24.0)

        # Suggested cut window: a beat before the rise to a beat after.
        range_start_ms = max(0, (i - rise_n - pre_n) * sample_period_ms)
        range_end_ms   = min(duration_ms, (i + post_n) * sample_period_ms)

        # Mark the per-sample score.
        score_lo = max(0, i - rise_n - pre_n)
        score_hi = min(n, i + post_n)
        for k in range(score_lo, score_hi):
            if score > scores[k]:
                scores[k] = score

        reason = (f"momentary LUFS rose +{rise:.1f} LU within {rise_window_sec:.1f}s "
                  f"after {quiet_window_sec:.1f}s avg {quiet_avg:.1f} LUFS")

        # Merge with the previous suggestion if they're close.
        if (suggestions
                and range_start_ms - suggestions[-1]["endMs"] < merge_gap_sec * 1000):
            prev = suggestions[-1]
            prev["endMs"] = max(prev["endMs"], range_end_ms)
            prev["score"] = round(max(prev["score"], score), 3)
            prev["reasons"].append(reason)
        else:
            suggestions.append({
                "category": "Jump scare",
                "startMs":  range_start_ms,
                "endMs":    range_end_ms,
                "score":    round(score, 3),
                "reasons":  [reason],
            })
        last_trigger_idx = i

    return suggestions, scores


def main(argv: List[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="censorcut.analyze",
                                     description="Heuristic audio analyzer (M3)")
    parser.add_argument("--input",  required=True, help="Path to the source video")
    parser.add_argument("--out",    required=True, help="Path to write the result JSON")
    parser.add_argument("--profile", help="Path to an age-profile JSON (advisory in M3)")
    parser.add_argument("--rise-threshold-lu", type=float, default=12.0)
    parser.add_argument("--quiet-threshold-lufs", type=float, default=-30.0)
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
        print("censorcut.analyze: source duration is zero or unknown", file=sys.stderr)
        return 4

    emit_progress(0.05, "loudness")
    momentary: List[float] = []
    short_term: List[float] = []
    try:
        for _t_sec, m, s in run_ebur128(input_path, duration_ms):
            momentary.append(m)
            short_term.append(s)
    except FileNotFoundError as e:
        print(f"censorcut.analyze: ffmpeg not found ({e}). Install ffmpeg.",
              file=sys.stderr)
        return 3
    except Exception as e:
        print(f"censorcut.analyze: ffmpeg ebur128 pass failed: {e}", file=sys.stderr)
        return 5

    emit_progress(0.96, "fuse")
    suggestions, jump_scores = detect_jump_scares(
        momentary, duration_ms,
        rise_threshold_lu=args.rise_threshold_lu,
        quiet_threshold_lufs=args.quiet_threshold_lufs,
    )

    result = {
        "schemaVersion": 1,
        "sourceFile":   str(Path(input_path).resolve()),
        "durationMs":   duration_ms,
        "rawScores": {
            "audio.lufs.momentary": {
                "samplePeriodMs": 100,
                "values": [round(v, 2) for v in momentary],
            },
            "audio.lufs.shortTerm": {
                "samplePeriodMs": 100,
                "values": [round(v, 2) for v in short_term],
            },
            "audio.jumpScore": {
                "samplePeriodMs": 100,
                "values": [round(v, 3) for v in jump_scores],
            },
        },
        "suggestions": suggestions,
    }

    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps(result, indent=2), encoding="utf-8")

    emit_progress(1.0, "done")
    return 0


if __name__ == "__main__":
    sys.exit(main())
