"""v14 prototype: dedupe-on-N>=4 + slide-align keyframe matching.

Refines v13 (M9.fingerprint design discussion):

  - Drop the explicit `_is_fixed_gop` cv-threshold classifier from v11/v13.
  - Replace it with a per-file dedupe step: any inter-keyframe gap value
    that appears >= GAP_DEDUPE_THRESHOLD times in this file is treated
    as a fixed-cadence period (encoder periodic refresh), and the
    keyframes that follow such gaps are dropped from the fingerprint.
  - If too few keyframes survive the dedupe, the file is fixed-GOP; the
    caller should fall back to the v9 audio-peak-gap fingerprint.
  - Match uses slide-and-search alignment: take the shorter gap
    sequence, try aligning its first element with each element of the
    longer, keep the alignment with the most pairwise gap matches
    (within +/- GAP_TOL_MS).
  - Verdict combines gap agreement at the best alignment with pHash
    Hamming agreement at the offset-aligned anchors.

Why GAP_DEDUPE_THRESHOLD = 4:

  Birthday-problem math at 24 fps frame quantization (~42 ms):
  a typical 90-min film with ~200 content keyframes has ~700 plausible
  integer-ms gap values, so coincidental same-value pairs number ~28.
  Almost no single gap value appears >= 3 times by chance. Fixed-GOP
  encoders, by contrast, produce hundreds of identical gaps. N>=4
  cleanly separates the two without false-dropping content keyframes.

Output shape (success):

  {
      "version":           14,
      "type":              "keyframe_dedupe_phash",
      "durationMs":        int,
      "approxDurationMin": int,
      "totalKeyframes":    int,
      "survivedKeyframes": int,
      "droppedAsCadence":  int,
      "cadenceGapsMs":     [int, ...],   # diagnostic: which gap
                                          # values were classified
                                          # as fixed-cadence
      "keyframeTimesMs":   [int, ...],   # SURVIVORS only, sorted asc
      "anchors":           [{"tMs": int, "phash": "<16-hex>"}, ...],
  }

Output shape (degenerate / fixed-GOP):

  {
      "version":           14,
      "type":              "fixed_gop",
      "totalKeyframes":    int,
      "survivedKeyframes": int,    # < MIN_SURVIVED
      "error":             "fixed_gop",
  }

The caller (analyze.py / match dialog) interprets "fixed_gop" as the
signal to invoke v9 audio-peak-gap fingerprinting on this file.
"""

from __future__ import annotations

import collections
import sys
from typing import Dict, List, Optional, Tuple

# Reuse v11's three-path keyframe extraction (works for MP4 moov.stss,
# MKV Cues, and AVI idx1 paths).
from . import video_fingerprint_v11_keyframe as v11


# ---------------------------------------------------------------------
# Tunables
# ---------------------------------------------------------------------

# Dedupe: a gap value appearing in this file at or above this count is
# classified as a fixed-cadence period. See file docstring for rationale.
GAP_DEDUPE_THRESHOLD = 4

# Minimum keyframes that must survive dedupe for the file to be useful.
# Below this we declare fixed_gop and the caller falls back to v9.
MIN_SURVIVED_KEYFRAMES = 30

# pHash anchor selection (same as v13)
TOP_K_ANCHORS         = 25
PHASH_RES             = 32
PHASH_DCT_KEEP        = 8
PHASH_HEX_CHARS       = 16
PHASH_FRAMES_PER_PEAK = 5
PHASH_FRAME_WINDOW_MS = 1000

# Match thresholds
DEFAULT_GAP_TOL_MS         = 5000
DEFAULT_PHASH_HAMMING_MAX  = 20
DEFAULT_MIN_GAP_FRAC       = 0.55
DEFAULT_MIN_PHASH_FRAC     = 0.40
DEFAULT_ANCHOR_TIME_TOL_MS = 5000


# ---------------------------------------------------------------------
# Dedupe step
# ---------------------------------------------------------------------

def _dedupe_keyframes(times_ms: List[int],
                       threshold: int = GAP_DEDUPE_THRESHOLD
                       ) -> Tuple[List[int], List[int]]:
    """Drop keyframes whose preceding-gap value appears >= threshold
    times in the file. Returns (survivors, dropped_gap_values).

    The first keyframe (no preceding gap) is always kept. For each
    subsequent keyframe, look at its preceding gap; if that gap value
    is in the high-frequency set, drop the keyframe."""
    if len(times_ms) < 2:
        return list(times_ms), []
    gaps = [times_ms[i] - times_ms[i - 1] for i in range(1, len(times_ms))]
    counter = collections.Counter(gaps)
    high_freq = {g for g, c in counter.items() if c >= threshold}
    if not high_freq:
        return list(times_ms), []
    survivors = [times_ms[0]]
    for i in range(1, len(times_ms)):
        if gaps[i - 1] in high_freq:
            continue
        survivors.append(times_ms[i])
    return survivors, sorted(high_freq)


# ---------------------------------------------------------------------
# Anchor picking and pHash
# ---------------------------------------------------------------------

def _pick_largest_gap_anchors(times: List[int], k: int) -> List[int]:
    """Pick K keyframes with the LARGEST preceding gaps among survivors.
    Same intuition as v13: large gaps mark calm-to-action transitions,
    which encoders agree on most reliably."""
    if len(times) <= k:
        return list(times)
    scored: List[Tuple[int, int]] = []
    for i in range(1, len(times)):
        scored.append((times[i] - times[i - 1], times[i]))
    scored.sort(key=lambda r: r[0], reverse=True)
    picked = sorted({t for _, t in scored[:k]})
    return picked


# ---------------------------------------------------------------------
# Public entry point
# ---------------------------------------------------------------------

def run(input_path: str, duration_ms: int, progress=None) -> Dict[str, object]:
    if duration_ms <= 0:
        return {"version": 14, "anchors": [], "error": "no duration"}
    try:
        import numpy as np
    except ImportError:
        return {"version": 14, "anchors": [], "error": "numpy not available"}

    if progress: progress(0.0, "v14:keyframes")
    times = v11._extract_keyframe_times_ms(input_path)
    if times is None or len(times) < MIN_SURVIVED_KEYFRAMES:
        return {
            "version":        14,
            "type":           "fixed_gop",
            "totalKeyframes": len(times) if times is not None else 0,
            "survivedKeyframes": 0,
            "anchors":        [],
            "error":          "too_few_keyframes_in_container",
        }

    if progress: progress(0.05, "v14:dedupe")
    survivors, cadence_gaps = _dedupe_keyframes(times)
    if len(survivors) < MIN_SURVIVED_KEYFRAMES:
        return {
            "version":           14,
            "type":              "fixed_gop",
            "totalKeyframes":    len(times),
            "survivedKeyframes": len(survivors),
            "droppedAsCadence":  len(times) - len(survivors),
            "cadenceGapsMs":     cadence_gaps,
            "anchors":           [],
            "error":             "fixed_gop",
        }

    if progress: progress(0.10, "v14:pick-anchors")
    anchor_times = _pick_largest_gap_anchors(survivors, TOP_K_ANCHORS)
    if len(anchor_times) < 5:
        return {
            "version":           14,
            "type":              "fixed_gop",
            "totalKeyframes":    len(times),
            "survivedKeyframes": len(survivors),
            "anchors":           [],
            "error":             "too_few_anchors",
        }

    if progress: progress(0.20, "v14:phash")
    dct = v11._make_dct_matrix(np, PHASH_RES)
    anchors: List[Dict[str, object]] = []
    for i, t_ms in enumerate(anchor_times):
        if progress:
            progress(0.20 + 0.75 * i / len(anchor_times),
                     f"v14:phash {i + 1}/{len(anchor_times)}")
        frames = v11._seek_decode_n_frames(input_path, t_ms,
                                            PHASH_FRAME_WINDOW_MS,
                                            PHASH_FRAMES_PER_PEAK)
        ph = v11._averaged_phash(np, dct, frames)
        if ph is None:
            anchors.append({"tMs": t_ms, "phash": None})
            continue
        anchors.append({"tMs": t_ms, "phash": f"{ph:0{PHASH_HEX_CHARS}x}"})

    if progress: progress(1.0, "v14:done")
    return {
        "version":           14,
        "type":              "keyframe_dedupe_phash",
        "durationMs":        duration_ms,
        "approxDurationMin": int(round(duration_ms / 60_000.0)),
        "totalKeyframes":    len(times),
        "survivedKeyframes": len(survivors),
        "droppedAsCadence":  len(times) - len(survivors),
        "cadenceGapsMs":     cadence_gaps,
        "keyframeTimesMs":   survivors,
        "anchors":           anchors,
    }


# ---------------------------------------------------------------------
# Match algorithm: slide-and-search on gap sequences
# ---------------------------------------------------------------------

def _gaps_from_times(times: List[int]) -> List[int]:
    return [times[i] - times[i - 1] for i in range(1, len(times))]


def _slide_align(short_gaps: List[int], long_gaps: List[int],
                  tol_ms: int) -> Tuple[int, int]:
    """Try aligning short_gaps[0] with each position in long_gaps.
    For each candidate offset k, count how many short_gaps[i] match
    long_gaps[k+i] within +/- tol_ms (1-to-1 by index from k onwards).

    Returns (best_offset_k, best_match_count). best_offset_k is the
    starting index in long_gaps where short_gaps[0] aligns. If no
    alignment yields any matches, returns (0, 0).
    """
    if not short_gaps or not long_gaps:
        return 0, 0
    best_k = 0
    best_count = 0
    for k in range(len(long_gaps) - len(short_gaps) + 1):
        cnt = 0
        for i in range(len(short_gaps)):
            if abs(short_gaps[i] - long_gaps[k + i]) <= tol_ms:
                cnt += 1
        if cnt > best_count:
            best_count = cnt
            best_k = k
    return best_k, best_count


def _hex_hamming(a: Optional[str], b: Optional[str]) -> int:
    if not a or not b or len(a) != len(b):
        return 64
    try:
        return bin(int(a, 16) ^ int(b, 16)).count("1")
    except ValueError:
        return 64


def match_fingerprints(fp_a: dict, fp_b: dict,
                        gap_tol_ms: int = DEFAULT_GAP_TOL_MS,
                        anchor_tol_ms: int = DEFAULT_ANCHOR_TIME_TOL_MS,
                        phash_hamming_max: int = DEFAULT_PHASH_HAMMING_MAX,
                        min_gap_frac: float = DEFAULT_MIN_GAP_FRAC,
                        min_phash_frac: float = DEFAULT_MIN_PHASH_FRAC
                        ) -> Dict[str, object]:
    """Compare two v14 fingerprints with slide-and-search alignment.

    Step 1: pick the shorter and longer keyframe-time lists.
    Step 2: build gap sequences for both.
    Step 3: slide the shorter sequence's first element against each
            position in the longer's, count matches (+/- gap_tol_ms),
            keep the best alignment.
    Step 4: from the best alignment, derive a time offset and check
            pHash agreement at offset-aligned anchors (+/- anchor_tol_ms).
    Step 5: verdict requires both gap-frac and pHash-frac to clear
            their thresholds.
    """
    times_a = sorted(fp_a.get("keyframeTimesMs") or [])
    times_b = sorted(fp_b.get("keyframeTimesMs") or [])
    anchors_a = fp_a.get("anchors") or []
    anchors_b = fp_b.get("anchors") or []

    if (len(times_a) < MIN_SURVIVED_KEYFRAMES
            or len(times_b) < MIN_SURVIVED_KEYFRAMES
            or not anchors_a or not anchors_b):
        return {
            "isSameFilm": False,
            "matched":    0,
            "totalShorter": min(len(times_a), len(times_b)),
            "gapFraction":   0.0,
            "phashMatched":  0,
            "phashCompared": 0,
            "phashFraction": 0.0,
            "estimatedTrimMs": 0,
            "reason": (f"insufficient survivors (a={len(times_a)}, "
                        f"b={len(times_b)})"),
        }

    if len(times_a) <= len(times_b):
        times_short, times_long = times_a, times_b
        a_is_short = True
    else:
        times_short, times_long = times_b, times_a
        a_is_short = False

    short_gaps = _gaps_from_times(times_short)
    long_gaps  = _gaps_from_times(times_long)
    if not short_gaps or not long_gaps:
        return {
            "isSameFilm": False, "matched": 0, "totalShorter": 0,
            "gapFraction": 0.0, "phashMatched": 0, "phashCompared": 0,
            "phashFraction": 0.0, "estimatedTrimMs": 0,
            "reason": "empty gap sequence",
        }

    best_k, best_count = _slide_align(short_gaps, long_gaps, gap_tol_ms)
    gap_frac = best_count / len(short_gaps) if short_gaps else 0.0

    # The alignment k means: times_short[0] aligns with times_long[best_k].
    # Therefore the time offset (long - short) is:
    offset_long_minus_short = times_long[best_k] - times_short[0]
    # Trim, conventionally signed as estimatedTrimMs = b - a:
    signed_offset = (offset_long_minus_short if not a_is_short
                                                else -offset_long_minus_short)

    if gap_frac < min_gap_frac:
        return {
            "isSameFilm":    False,
            "matched":       best_count,
            "totalShorter":  len(short_gaps),
            "gapFraction":   gap_frac,
            "phashMatched":  0,
            "phashCompared": 0,
            "phashFraction": 0.0,
            "estimatedTrimMs": signed_offset,
            "reason": (f"gap match too low: {best_count}/{len(short_gaps)} "
                        f"({gap_frac:.0%}); skipping pHash check"),
        }

    # pHash agreement at offset-aligned anchors. We use the offset in
    # the (a -> b) direction throughout.
    if a_is_short:
        ofs_a_to_b = offset_long_minus_short
    else:
        ofs_a_to_b = -offset_long_minus_short

    a_with_ph = [a for a in anchors_a if a.get("phash")]
    b_with_ph = [a for a in anchors_b if a.get("phash")]
    phash_compared = 0
    phash_matched  = 0
    for a in a_with_ph:
        target_b_t = int(a["tMs"]) + ofs_a_to_b
        if not b_with_ph:
            break
        nearest = min(b_with_ph,
                      key=lambda x: abs(int(x["tMs"]) - target_b_t))
        if abs(int(nearest["tMs"]) - target_b_t) > anchor_tol_ms:
            continue
        phash_compared += 1
        if _hex_hamming(a.get("phash"), nearest.get("phash")) <= phash_hamming_max:
            phash_matched += 1
    phash_frac = phash_matched / phash_compared if phash_compared else 0.0
    same = (gap_frac >= min_gap_frac) and (phash_frac >= min_phash_frac)

    return {
        "isSameFilm":      same,
        "matched":         best_count,
        "totalShorter":    len(short_gaps),
        "gapFraction":     gap_frac,
        "phashMatched":    phash_matched,
        "phashCompared":   phash_compared,
        "phashFraction":   phash_frac,
        "estimatedTrimMs": signed_offset,
        "reason": (f"gaps {best_count}/{len(short_gaps)} ({gap_frac:.0%}), "
                    f"pHash {phash_matched}/{phash_compared} "
                    f"({phash_frac:.0%}), trim~={signed_offset} ms"),
    }
