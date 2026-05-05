"""Audio loudness detector — drives ffmpeg's ebur128 filter.

Returns three series sampled every 100 ms:
  - audio.lufs.momentary   — 400 ms window momentary LUFS
  - audio.lufs.shortTerm   — 3 s window short-term LUFS
  - audio.lufs.jump        — derived 0..1 jump-scare score

The jump-scare score is the M3 heuristic: a sudden +12 LU rise within 1 s
after at least 1 s averaging below -30 LUFS. Score is clamped to 1.0 at
+24 LU.
"""

from __future__ import annotations

import re
import shutil
import subprocess
from typing import List, Tuple

from .base import DetectorOutput, ProgressFn

_EBUR128_LINE_RE = re.compile(
    r"t:\s*(?P<t>[-\d.]+)\s+"
    r"(?:TARGET:\s*[-\d.]+\s*LUFS\s+)?"
    r"M:\s*(?P<m>[-\d.+]+|nan|-inf)\s+"
    r"S:\s*(?P<s>[-\d.+]+|nan|-inf)"
)


def _ffmpeg_exe() -> str:
    return shutil.which("ffmpeg") or "ffmpeg"


def _parse_lufs(value: str) -> float:
    try:
        v = float(value)
    except (TypeError, ValueError):
        return -120.0
    if v != v:  # NaN
        return -120.0
    if v == float("-inf") or v < -120.0:
        return -120.0
    return v


def _stream_ebur128(input_path: str,
                    duration_ms: int,
                    progress: ProgressFn) -> Tuple[List[float], List[float]]:
    """Return (momentary, short_term) LUFS lists at the filter's native rate."""
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
    momentary: List[float] = []
    short_term: List[float] = []
    last_progress = 0.0
    try:
        assert proc.stderr is not None
        for line in proc.stderr:
            m = _EBUR128_LINE_RE.search(line)
            if not m:
                continue
            t_sec = float(m.group("t"))
            momentary.append(_parse_lufs(m.group("m")))
            short_term.append(_parse_lufs(m.group("s")))
            if progress and duration_ms > 0:
                frac = min(0.95, t_sec * 1000.0 / duration_ms)
                if frac - last_progress > 0.01:
                    last_progress = frac
                    progress(frac, "loudness")
    finally:
        proc.wait()
        if proc.returncode not in (0, None):
            raise RuntimeError(f"ffmpeg exited {proc.returncode}")
    return momentary, short_term


def compute_jump_score(momentary: List[float],
                       *,
                       sample_period_ms: int = 100,
                       rise_threshold_lu: float = 12.0,
                       quiet_threshold_lufs: float = -30.0,
                       rise_window_sec: float = 1.0,
                       quiet_window_sec: float = 1.0,
                       pre_pad_sec: float = 1.0,
                       post_pad_sec: float = 1.5) -> List[float]:
    """Per-sample 0..1 jump-scare scores, aligned with the input series.

    A score is non-zero in the window around any frame where momentary LUFS
    rose by >= rise_threshold_lu within rise_window_sec, after the previous
    quiet_window_sec averaged below quiet_threshold_lufs.
    """
    samples_per_sec = 1000 // sample_period_ms
    rise_n  = max(1, int(rise_window_sec  * samples_per_sec))
    quiet_n = max(1, int(quiet_window_sec * samples_per_sec))
    pre_n   = max(0, int(pre_pad_sec      * samples_per_sec))
    post_n  = max(0, int(post_pad_sec     * samples_per_sec))

    n = len(momentary)
    scores = [0.0] * n
    last_trigger = -10**9
    cooldown = rise_n + quiet_n

    for i in range(rise_n + quiet_n, n):
        if i - last_trigger < cooldown:
            continue
        rise = momentary[i] - momentary[i - rise_n]
        if rise < rise_threshold_lu:
            continue
        quiet_slice = momentary[i - rise_n - quiet_n : i - rise_n]
        if not quiet_slice:
            continue
        quiet_avg = sum(quiet_slice) / len(quiet_slice)
        if quiet_avg > quiet_threshold_lufs:
            continue

        score = min(1.0, rise / 24.0)
        lo = max(0, i - rise_n - pre_n)
        hi = min(n, i + post_n)
        for k in range(lo, hi):
            if score > scores[k]:
                scores[k] = score
        last_trigger = i

    return scores


def run(input_path: str,
        duration_ms: int,
        progress: ProgressFn = None) -> DetectorOutput:
    momentary, short_term = _stream_ebur128(input_path, duration_ms, progress)
    jump = compute_jump_score(momentary)
    return {
        "audio.lufs.momentary": {"period_ms": 100, "values": momentary},
        "audio.lufs.shortTerm": {"period_ms": 100, "values": short_term},
        "audio.lufs.jump":      {"period_ms": 100, "values": jump},
    }
