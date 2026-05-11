"""v13 prototype: keyframe-subset alignment + pHash content verification.

Combines v12's order-preserving keyframe-time subset matching (for
finding the time offset between two files of different lengths) with
v9/v11-style averaged pHash extraction at picked keyframes (for
content discrimination).

Fingerprint structure:

  {
    "version": 13,
    "type": "keyframe_subset_phash",
    "durationMs":  <int>,
    "keyframeTimesMs": [t0, t1, ..., tN],   # FULL list — for alignment
    "anchors": [                             # K picked + averaged pHash
        {"tMs": ..., "phash": "<16-hex>"},
        ...
    ],
  }

Picking strategy for the K pHash anchors: the K keyframes with the
LARGEST PRECEDING GAPS — i.e., the keyframes that follow the longest
periods of "encoder calm". In scene-cut-aware encoding, those are the
keyframes right after the longest static / calm scenes, which tend
to be the most prominent scene transitions. Different encoders are
most likely to AGREE on these (the major scene cuts) even when they
disagree on borderline ones. Cross-encode robust by construction.

Match algorithm:

  1. Run v12-style ordered subset alignment on the full
     keyframeTimesMs lists. Get `offset` (= trim) and
     `timing_match_frac`.

  2. If timing_match_frac is too low (< 50%), declare DIFFER
     immediately — the films don't even share their timing skeleton.

  3. For each anchor in the SHORTER fingerprint, shift its tMs by
     the offset and find the anchor in the LONGER fingerprint
     whose tMs is closest. If close enough (≤ 5 sec apart), check
     whether the pHashes agree (Hamming ≤ 20).

  4. Verdict: same content if timing_match_frac >= 0.55 AND
     phash_match_frac >= 0.40 (or similar permissive thresholds).

Properties carried forward from v12 + v11:

  cross-encode invariance     ✓ pHash + averaged 5-frame
  intro-trim tolerance        ✓ subset alignment finds offset
  outro-trim tolerance        ✓ same — outro just truncates the
                                  alignable region; threshold is
                                  set on FRACTION matched
  cross-codec invariance      ✓ pHash + container-index keyframes
  different-cut detection     ✓ pHash matches catch different
                                  content even when timing aligns
                                  (different cuts insert/remove
                                  scenes which alter the pHash
                                  pattern at corresponding aligned
                                  positions)
  cross-resolution            ✓ pHash works at any res
"""

from __future__ import annotations

import shutil
import subprocess
import sys
from typing import Dict, List, Optional, Tuple

# Reuse v11's keyframe extraction (3-path AVI/MKV/MP4 fallback) and
# v12's subset alignment.
from . import video_fingerprint_v11_keyframe as v11
from . import video_fingerprint_v12_subset    as v12


TOP_K_ANCHORS              = 25
PHASH_RES                  = 32
PHASH_DCT_KEEP             = 8
PHASH_HEX_CHARS            = 16
PHASH_FRAMES_PER_PEAK      = 5
PHASH_FRAME_WINDOW_MS      = 1000

# Match thresholds
DEFAULT_GAP_TOL_MS         = 5000
DEFAULT_PHASH_HAMMING_MAX  = 20
DEFAULT_MIN_TIMING_FRAC    = 0.55
DEFAULT_MIN_PHASH_FRAC     = 0.40
DEFAULT_ANCHOR_TIME_TOL_MS = 5000


# ---------------------------------------------------------------------
# Anchor picking: largest preceding gap = most prominent scene cut
# ---------------------------------------------------------------------

def _pick_largest_gap_anchors(keyframe_times: List[int],
                                k: int) -> List[int]:
    """Pick K keyframes with the LARGEST PRECEDING GAPS. The gap-
    before-a-keyframe is how long the scene before it lasted in
    encoder terms; large gaps mark calm-to-action transitions, which
    different encoders tend to agree on.

    Returns sorted list of picked keyframe times."""
    if len(keyframe_times) < 2:
        return list(keyframe_times)
    # Score each keyframe by its preceding gap (skip index 0 which has
    # no preceding gap).
    scored: List[Tuple[int, int]] = []  # (preceding_gap_ms, t_ms)
    for i in range(1, len(keyframe_times)):
        gap = keyframe_times[i] - keyframe_times[i - 1]
        scored.append((gap, keyframe_times[i]))
    # Sort by score descending, take top k, sort the result by time.
    scored.sort(key=lambda r: r[0], reverse=True)
    picked = sorted({t for _, t in scored[:k]})
    return picked


# ---------------------------------------------------------------------
# pHash helpers (reuse v11's logic)
# ---------------------------------------------------------------------

def _ffmpeg_exe() -> str:
    p = shutil.which("ffmpeg")
    if not p:
        raise RuntimeError("ffmpeg not found")
    return p


def _seek_decode_n_frames(input_path: str, center_ms: int,
                           window_ms: int, n_frames: int) -> List[bytes]:
    return v11._seek_decode_n_frames(input_path, center_ms,
                                       window_ms, n_frames)


def _make_dct_matrix(np, n: int):
    return v11._make_dct_matrix(np, n)


def _averaged_phash(np, dct, frame_bytes_list: List[bytes]) -> Optional[int]:
    if not frame_bytes_list:
        return None
    arrs = []
    for fb in frame_bytes_list:
        if len(fb) < PHASH_RES * PHASH_RES:
            continue
        arrs.append(np.frombuffer(fb, dtype=np.uint8) \
                      .reshape(PHASH_RES, PHASH_RES).astype(np.float32))
    if not arrs:
        return None
    averaged = np.mean(arrs, axis=0)
    coeffs = dct @ averaged @ dct.T
    block = coeffs[:PHASH_DCT_KEEP, :PHASH_DCT_KEEP].flatten()[1:]
    median = float(np.median(block))
    bits = (block > median).tolist()
    out = 0
    for b in bits:
        out = (out << 1) | (1 if b else 0)
    return out


# ---------------------------------------------------------------------
# Public entry point
# ---------------------------------------------------------------------

def run(input_path: str, duration_ms: int, progress=None) -> Dict[str, object]:
    if duration_ms <= 0:
        return {"version": 13, "anchors": []}
    try:
        import numpy as np
    except ImportError:
        return {"version": 13, "anchors": []}

    if progress: progress(0.0, "v13:keyframes")
    times = v11._extract_keyframe_times_ms(input_path)
    if times is None or len(times) < 30:
        return {"version": 13, "anchors": [],
                "error": (f"too few keyframes "
                          f"({len(times) if times is not None else 0})")}

    if progress: progress(0.10, "v13:pick-anchors")
    anchor_times = _pick_largest_gap_anchors(times, TOP_K_ANCHORS)
    if len(anchor_times) < 5:
        return {"version": 13, "anchors": [],
                "error": f"only {len(anchor_times)} anchors picked"}

    if progress: progress(0.20, "v13:phash")
    dct = _make_dct_matrix(np, PHASH_RES)
    anchors: List[Dict[str, object]] = []
    for i, t_ms in enumerate(anchor_times):
        if progress:
            progress(0.20 + 0.75 * i / len(anchor_times),
                     f"v13:phash {i + 1}/{len(anchor_times)}")
        frames = _seek_decode_n_frames(input_path, t_ms,
                                        PHASH_FRAME_WINDOW_MS,
                                        PHASH_FRAMES_PER_PEAK)
        ph = _averaged_phash(np, dct, frames)
        if ph is None:
            anchors.append({"tMs": t_ms, "phash": None})
            continue
        anchors.append({"tMs": t_ms, "phash": f"{ph:0{PHASH_HEX_CHARS}x}"})

    if progress: progress(1.0, "v13:done")
    return {
        "version":         13,
        "type":            "keyframe_subset_phash",
        "durationMs":      duration_ms,
        "approxDurationMin": int(round(duration_ms / 60_000.0)),
        "keyframeCount":   len(times),
        "keyframeTimesMs": times,
        "anchors":         anchors,
    }


# ---------------------------------------------------------------------
# Match (subset alignment + pHash content verification)
# ---------------------------------------------------------------------

def _hex_hamming(a: Optional[str], b: Optional[str]) -> int:
    if not a or not b or len(a) != len(b):
        return 64
    try:
        return bin(int(a, 16) ^ int(b, 16)).count("1")
    except ValueError:
        return 64


def match_fingerprints(fp_a: dict, fp_b: dict,
                        timing_tol_ms: int = v12.DEFAULT_TIME_TOL_MS,
                        anchor_tol_ms: int = DEFAULT_ANCHOR_TIME_TOL_MS,
                        phash_hamming_max: int = DEFAULT_PHASH_HAMMING_MAX,
                        min_timing_frac: float = DEFAULT_MIN_TIMING_FRAC,
                        min_phash_frac: float = DEFAULT_MIN_PHASH_FRAC
                        ) -> Dict[str, object]:
    """Hybrid v13 match: subset-of-keyframes for offset estimation,
    then pHash agreement at offset-aligned anchors for content
    verification."""
    times_a = sorted(fp_a.get("keyframeTimesMs") or [])
    times_b = sorted(fp_b.get("keyframeTimesMs") or [])
    anchors_a = fp_a.get("anchors") or []
    anchors_b = fp_b.get("anchors") or []

    if (len(times_a) < 30 or len(times_b) < 30
            or not anchors_a or not anchors_b):
        return {
            "isSameFilm": False, "matched": 0, "totalShorter": 0,
            "timingFraction": 0.0, "phashMatched": 0, "phashCompared": 0,
            "phashFraction": 0.0, "estimatedTrimMs": 0,
            "reason": "insufficient fingerprint data",
        }

    # Phase 1: subset timing alignment.
    timing = v12.match_fingerprints(fp_a, fp_b, time_tol_ms=timing_tol_ms,
                                      min_subset_frac=min_timing_frac)
    timing_frac = timing.get("matchFraction", 0.0)
    matched = timing.get("matched", 0)
    total_short = timing.get("totalShorter", 0)
    offset = timing.get("estimatedTrimMs", 0)

    if timing_frac < min_timing_frac:
        return {
            "isSameFilm":     False,
            "matched":        matched,
            "totalShorter":   total_short,
            "timingFraction": timing_frac,
            "phashMatched":   0,
            "phashCompared":  0,
            "phashFraction":  0.0,
            "estimatedTrimMs": offset,
            "reason": (f"timing failed: {matched}/{total_short} "
                        f"({timing_frac:.0%}); skipped pHash check"),
        }

    # Phase 2: pHash agreement at offset-aligned anchors.
    # offset is signed: estimatedTrimMs = (b_anchor.tMs - a_anchor.tMs)
    # for a same-content pair. So shift a's anchors by `offset` to land
    # in b's timeline.
    phash_compared = 0
    phash_matched = 0
    a_anchors_with_phash = [a for a in anchors_a if a.get("phash")]
    b_anchors_with_phash = [a for a in anchors_b if a.get("phash")]
    for a in a_anchors_with_phash:
        target_b_t = int(a["tMs"]) + offset
        # Find b anchor whose tMs is nearest to target.
        nearest = min(b_anchors_with_phash,
                      key=lambda x: abs(int(x["tMs"]) - target_b_t),
                      default=None)
        if nearest is None:
            continue
        if abs(int(nearest["tMs"]) - target_b_t) > anchor_tol_ms:
            continue
        phash_compared += 1
        if _hex_hamming(a["phash"], nearest["phash"]) <= phash_hamming_max:
            phash_matched += 1

    phash_frac = phash_matched / phash_compared if phash_compared else 0.0
    same = (timing_frac >= min_timing_frac) and (phash_frac >= min_phash_frac)

    return {
        "isSameFilm":      same,
        "matched":         matched,
        "totalShorter":    total_short,
        "timingFraction":  timing_frac,
        "phashMatched":    phash_matched,
        "phashCompared":   phash_compared,
        "phashFraction":   phash_frac,
        "estimatedTrimMs": offset,
        "reason": (f"timing {matched}/{total_short} ({timing_frac:.0%}), "
                    f"pHash {phash_matched}/{phash_compared} "
                    f"({phash_frac:.0%}), trim≈{offset} ms"),
    }
