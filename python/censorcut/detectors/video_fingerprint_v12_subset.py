"""v12 prototype: keyframe-time subset matching.

The simplest and most direct alternative to v11. Don't pick a top-K,
don't extract pHashes, don't anchor anything. Just store the full
keyframe time list and match via subset/alignment check.

Rationale:
  Two encodes of the same source content always share the major
  scene-cut keyframes (scene-cut-aware encoders agree on real scene
  boundaries). Differences are:
    - One encode may have ADDITIONAL keyframes for periodic refresh
      or aggressive scenecut sensitivity (e.g., the CENSORED file
      has 1016 keyframes vs the original's 791).
    - All keyframes shift by a constant offset = trim amount.

  So the shorter list should be (approximately) a subset of the
  longer list after applying the right offset. Match via:

    1. Sort both lists.
    2. Estimate offset via pairwise-difference histogram.
    3. Apply offset, count nearest-neighbour matches.
    4. If ≥70% of the shorter list has near-matches in the longer,
       declare same content.

Properties:
  Cross-encode      ✓  same content events → same time pattern
  Cross-resolution  ✓  irrelevant (keyframes are time events)
  Cross-codec       ✓  works for any codec where keyframes are
                       exposed via ffprobe
  Intro trim        ✓  trim shifts everything; offset estimation
                       absorbs it
  Outro trim        ✓  same — trim shifts entries that survive,
                       missing entries reduce subset-match rate
                       but NMR threshold tolerates partial loss
  Different films   ✗ different fingerprint  (random patterns
                                              don't align)
  Different cuts    ⚠  caveat: an extended edition has all the
                       original's keyframes PLUS new ones, so
                       subset check passes. v12 cannot reliably
                       distinguish "original" from "extended
                       edition that contains all the original
                       scenes". For full cut-sensitivity, layer
                       a pHash check on top.

Output format:
  Just the keyframe times list. ~3-4 KB per film. The fingerprint
  envelope is intentionally minimal.
"""

from __future__ import annotations

import bisect
import sys
from typing import Dict, List, Optional

# Reuse the keyframe extraction code from v11 (3-path fallback handles
# MP4, MKV, AVI).
from . import video_fingerprint_v11_keyframe as v11


# ---------------------------------------------------------------------
# Tunables
# ---------------------------------------------------------------------

# Min keyframes to bother fingerprinting at all.
MIN_KEYFRAMES                 = 30

# Match algorithm
DEFAULT_TIME_TOL_MS           = 2000   # ±2 sec per matched timestamp
DEFAULT_MIN_SUBSET_FRACTION   = 0.70   # 70% of shorter list must match
DEFAULT_OFFSET_PROBE_K        = 50     # how many entries to sample for
                                         # offset estimation
DEFAULT_OFFSET_PROBE_RANGE_MS = 10 * 60 * 1000   # ±10 min plausible trim


# ---------------------------------------------------------------------
# Fingerprint
# ---------------------------------------------------------------------

def run(input_path: str, duration_ms: int, progress=None) -> Dict[str, object]:
    if duration_ms <= 0:
        return {"version": 12, "keyframeTimesMs": [],
                "error": "no duration"}

    if progress: progress(0.0, "v12:keyframes")
    times = v11._extract_keyframe_times_ms(input_path)
    if times is None:
        return {"version": 12, "keyframeTimesMs": [],
                "error": "ffprobe failed"}
    if len(times) < MIN_KEYFRAMES:
        return {"version": 12, "keyframeTimesMs": [],
                "error": f"too few keyframes ({len(times)})"}

    if progress: progress(1.0, "v12:done")
    return {
        "version":         12,
        "type":            "keyframe_subset",
        "durationMs":      duration_ms,
        "approxDurationMin": int(round(duration_ms / 60_000.0)),
        "keyframeCount":   len(times),
        "keyframeTimesMs": times,
    }


# ---------------------------------------------------------------------
# Match
# ---------------------------------------------------------------------

def _count_ordered_matches(times_short: List[int],
                             times_long: List[int],
                             offset_ms: int,
                             tolerance_ms: int) -> int:
    """Order-preserving 1-to-1 subset count.

    For each timestamp t in `times_short`, advance through
    `times_long` looking for an entry within `tolerance_ms` of
    (t + offset_ms). Each entry in `times_long` is consumed at
    most once — once a match is taken, the cursor advances past
    it. This enforces that matched entries appear IN THE SAME
    ORDER in both lists, with the longer list permitted to have
    extras between matches.

    Returns the number of `times_short` entries that found a
    match in `times_long`.
    """
    matched = 0
    j = 0
    n_long = len(times_long)
    for t in times_short:
        target = t + offset_ms
        # Advance the cursor while long[j] is too early to match.
        while j < n_long and times_long[j] < target - tolerance_ms:
            j += 1
        if j >= n_long:
            break
        if abs(times_long[j] - target) <= tolerance_ms:
            matched += 1
            j += 1   # consume this entry; can't be reused
    return matched


def _best_offset(times_short: List[int],
                  times_long: List[int],
                  tolerance_ms: int,
                  probe_k: int,
                  probe_range_ms: int) -> tuple:
    """Try offsets that align times_short[0] with each of the first
    `probe_k` entries in times_long (only those within
    probe_range_ms). For each candidate offset, count ordered matches.
    Return (best_offset_ms, best_match_count).
    """
    if not times_short or not times_long:
        return 0, 0
    s0 = times_short[0]
    candidates = []
    for k in range(min(probe_k, len(times_long))):
        cand = times_long[k] - s0
        if abs(cand) <= probe_range_ms:
            candidates.append(cand)
    if not candidates:
        return 0, 0
    # Also try a few candidates near the END of times_long in case
    # the alignment is in the middle (shorter starts well into longer).
    for k in range(min(probe_k, len(times_long))):
        idx = len(times_long) - 1 - k
        if idx < 0:
            break
        cand = times_long[idx] - times_short[-1]
        if abs(cand) <= probe_range_ms and cand not in candidates:
            candidates.append(cand)

    best_offset = candidates[0]
    best_count = -1
    for cand in candidates:
        cnt = _count_ordered_matches(times_short, times_long,
                                       cand, tolerance_ms)
        if cnt > best_count:
            best_count = cnt
            best_offset = cand
    return best_offset, best_count


def match_fingerprints(fp_a: dict, fp_b: dict,
                        time_tol_ms: int = DEFAULT_TIME_TOL_MS,
                        min_subset_frac: float = DEFAULT_MIN_SUBSET_FRACTION,
                        probe_k: int = DEFAULT_OFFSET_PROBE_K,
                        probe_range_ms: int = DEFAULT_OFFSET_PROBE_RANGE_MS
                        ) -> Dict[str, object]:
    """Compare two v12 fingerprints with ORDER-PRESERVING subset matching.

    The shorter keyframe list, after applying a constant time offset,
    must appear in-order inside the longer list. The longer list can
    have extra keyframes interleaved between the matched ones (those
    are the "extras" from a denser re-encode or an extended edition).

    Returns a verdict dict.
    """
    times_a = sorted(fp_a.get("keyframeTimesMs") or [])
    times_b = sorted(fp_b.get("keyframeTimesMs") or [])
    if len(times_a) < MIN_KEYFRAMES or len(times_b) < MIN_KEYFRAMES:
        return {
            "isSameFilm": False, "matched": 0,
            "totalShorter": min(len(times_a), len(times_b)),
            "matchFraction": 0.0, "estimatedTrimMs": 0,
            "reason": f"too few keyframes (a={len(times_a)}, b={len(times_b)})",
        }

    if len(times_a) <= len(times_b):
        times_short, times_long = times_a, times_b
        a_is_short = True
    else:
        times_short, times_long = times_b, times_a
        a_is_short = False

    offset, matched = _best_offset(times_short, times_long,
                                     time_tol_ms, probe_k, probe_range_ms)
    if matched < 0:
        return {
            "isSameFilm": False, "matched": 0,
            "totalShorter": len(times_short),
            "matchFraction": 0.0, "estimatedTrimMs": 0,
            "reason": "no plausible offset found",
        }

    match_frac = matched / len(times_short)
    same = match_frac >= min_subset_frac
    signed_offset = offset if a_is_short else -offset

    return {
        "isSameFilm":      same,
        "matched":         matched,
        "totalShorter":    len(times_short),
        "matchFraction":   match_frac,
        "estimatedTrimMs": signed_offset,
        "reason": (f"matched {matched}/{len(times_short)} "
                    f"({match_frac:.0%} of shorter list, in-order, "
                    f"1-to-1), trim≈{signed_offset} ms"),
    }
