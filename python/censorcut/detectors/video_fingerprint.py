"""Production video-content fingerprint: keyframe-time list + MAD match.

Algorithm F: extract the full keyframe-time list from the container
index (Cues/stss/idx1) via fast_keyframes, store it as the fingerprint.
Match by order-preserving subset alignment with a single time offset,
verdict = match_fraction >= 55% AND offset-residual MAD <= 250 ms.

Properties (validated on D:\\censorcut-test corpus + 238-film library
bench, 0 false positives across 28k pairs):

  cross-encode invariance     yes — scene-cut keyframes are content events
  cross-codec invariance      yes — container-level keyframe positions
  cross-resolution            yes — keyframe times don't depend on res
  intro/outro trim            yes — single offset absorbs uniform shift,
                                    MAD stays at encoder-jitter levels
  mid-film cuts (different    no  — by design. Multi-cut content produces
    cut of the same film)           multi-modal time-diffs, MAD blows up,
                                    verdict = DIFFER. This is correct:
                                    cuts at different content positions
                                    aren't applicable to the original.

Failure mode: when the container has no keyframe index (broken AVI,
some streaming muxes, fixed-GOP encodes after the dedupe step), we
fall back to the v9 audio-peak-gap fingerprint. Audio peak gaps are
content-derived and trim-tolerant, so the same matching semantics
apply, but they require reading the audio stream — much slower than
the container-metadata-only F path.

Output shape:

  F (primary):
    {
      "version":            <int>,           # 1 currently
      "type":               "keyframes",
      "durationMs":         <int>,
      "approxDurationMin":  <int>,
      "keyframeCount":      <int>,
      "keyframeTimesMs":    [<int>, ...],
    }

  v9 (fallback):
    {
      "version":            <int>,           # 1 currently
      "type":               "audio_peak_gaps",
      "durationMs":         <int>,
      "approxDurationMin":  <int>,
      "innerSpanMs":        <int>,
      "peakCount":          <int>,
      "gapsMs":             [<int>, ...],
      "peaks":              [{"tMs": int, "phash": "<16-hex>"}, ...],
    }

Caller is expected to look at the `type` field and dispatch to the
right matcher. match_fingerprints() in this module does that
automatically.
"""

from __future__ import annotations

import sys
from typing import Dict, List, Optional, Tuple

from . import fast_keyframes
from . import video_fingerprint_v9_peakgaps as v9
from . import video_fingerprint_v11_keyframe as v11

# Schema version. Bump if the on-disk shape changes in a way readers
# need to know about. The C++ side and the Rust edits server check
# this field.
FINGERPRINT_VERSION = 1

# F: minimum surviving keyframes for the F path to be viable. Below
# this we fall back to v9. Same threshold as fast_keyframes / v11.
MIN_KEYFRAMES = 30

# F: match thresholds (validated on D:\censorcut-test corpus).
GAP_TOL_MS         = 2000
MIN_MATCH_FRAC     = 0.55
MAX_MAD_MS         = 250


# ---------------------------------------------------------------------
# Fingerprint compute
# ---------------------------------------------------------------------

def run(input_path: str, duration_ms: int,
        progress=None) -> Dict[str, object]:
    """Compute a video fingerprint for `input_path`. Tries the F path
    first (container-index-only, ~1-2 sec on a network share); falls
    back to v9 audio peak gaps when F can't get enough keyframes.

    Always returns a dict. On total failure the dict has
    `type='unknown'` and `error=<reason>`, with no keyframeTimesMs or
    peaks — callers can detect this via `type` and skip matching.
    """
    if duration_ms <= 0:
        return {
            "version": FINGERPRINT_VERSION,
            "type":    "unknown",
            "error":   "no duration",
        }
    if progress: progress(0.0, "fingerprint:keyframes")
    # F path: pull keyframe times via fast_keyframes (direct MKV/MP4/
    # AVI container-index parse), falling back through v11's ffprobe
    # ladder for files where the direct parse doesn't recognise the
    # container or the index is missing.
    times = v11._extract_keyframe_times_ms(input_path)
    if times is not None and len(times) >= MIN_KEYFRAMES:
        if progress: progress(1.0, "fingerprint:done")
        return {
            "version":           FINGERPRINT_VERSION,
            "type":              "keyframes",
            "durationMs":        duration_ms,
            "approxDurationMin": int(round(duration_ms / 60_000.0)),
            "keyframeCount":     len(times),
            "keyframeTimesMs":   times,
        }

    # Fall back to v9 audio peak gaps. This is dramatically slower
    # because it reads the audio stream end-to-end (the audio demux
    # still requires walking most of the file), but it works for
    # fixed-GOP / no-Cues / broken-AVI cases that defeat F.
    print("censorcut.video_fingerprint: F path got "
          f"{len(times) if times else 0} keyframe(s); falling back to v9",
          file=sys.stderr)
    if progress: progress(0.05, "fingerprint:v9-audio")
    fp9 = v9.run(input_path, duration_ms=duration_ms, progress=progress)
    if not fp9.get("peaks"):
        return {
            "version":          FINGERPRINT_VERSION,
            "type":             "unknown",
            "durationMs":       duration_ms,
            "approxDurationMin": int(round(duration_ms / 60_000.0)),
            "error":            "no keyframes and v9 fallback also failed",
        }
    return {
        "version":           FINGERPRINT_VERSION,
        "type":              "audio_peak_gaps",
        "durationMs":        duration_ms,
        "approxDurationMin": int(round(duration_ms / 60_000.0)),
        "innerSpanMs":       fp9.get("innerSpanMs", 0),
        "peakCount":         fp9.get("peakCount", 0),
        "gapsMs":            fp9.get("gapsMs", []),
        "peaks":             fp9.get("peaks", []),
    }


# ---------------------------------------------------------------------
# Match: subset alignment + offset-residual MAD
# ---------------------------------------------------------------------

def _ordered_pairs(short: List[int], long: List[int],
                    offset_ms: int, tol_ms: int) -> List[Tuple[int, int]]:
    """Order-preserving 1-to-1 subset matching with cursor advance.
    Returns the list of (short_t, long_t) pairs whose times agree to
    within +-tol_ms after `offset_ms` is added to short side."""
    pairs: List[Tuple[int, int]] = []
    j = 0
    n_long = len(long)
    for t in short:
        target = t + offset_ms
        while j < n_long and long[j] < target - tol_ms:
            j += 1
        if j >= n_long:
            break
        if abs(long[j] - target) <= tol_ms:
            pairs.append((t, long[j]))
            j += 1
    return pairs


def _best_offset_pairs(short: List[int], long: List[int],
                         tol_ms: int, probe_k: int = 50,
                         probe_range_ms: int = 10 * 60 * 1000
                         ) -> Tuple[int, List[Tuple[int, int]]]:
    """Try a handful of plausible offsets (aligning short[0] against
    each of the first probe_k entries in long, plus probe_k entries
    near the end of long), pick the one that pairs the most short
    entries. Returns (best_offset, matched_pairs).
    """
    if not short or not long:
        return 0, []
    candidates: List[int] = []
    s0 = short[0]
    for k in range(min(probe_k, len(long))):
        cand = long[k] - s0
        if abs(cand) <= probe_range_ms:
            candidates.append(cand)
    s_last = short[-1]
    for k in range(min(probe_k, len(long))):
        idx = len(long) - 1 - k
        if idx < 0:
            break
        cand = long[idx] - s_last
        if abs(cand) <= probe_range_ms and cand not in candidates:
            candidates.append(cand)
    if not candidates:
        return 0, []
    best_offset = candidates[0]
    best_pairs: List[Tuple[int, int]] = []
    for cand in candidates:
        pairs = _ordered_pairs(short, long, cand, tol_ms)
        if len(pairs) > len(best_pairs):
            best_pairs = pairs
            best_offset = cand
    return best_offset, best_pairs


def _median(xs: List[float]) -> float:
    if not xs:
        return 0.0
    s = sorted(xs)
    n = len(s)
    if n % 2 == 1:
        return float(s[n // 2])
    return float((s[n // 2 - 1] + s[n // 2]) / 2)


def _mad_ms(diffs: List[int]) -> float:
    if not diffs:
        return 0.0
    med = _median([float(d) for d in diffs])
    return _median([abs(float(d) - med) for d in diffs])


def _match_f(fp_a: dict, fp_b: dict) -> Dict[str, object]:
    """F-path match: timing-only, no pHash. Caller has already
    confirmed both sides are type=='keyframes'."""
    times_a = sorted(fp_a.get("keyframeTimesMs") or [])
    times_b = sorted(fp_b.get("keyframeTimesMs") or [])
    if len(times_a) < MIN_KEYFRAMES or len(times_b) < MIN_KEYFRAMES:
        return {
            "isSameFilm":    False,
            "matched":       0,
            "totalShorter":  min(len(times_a), len(times_b)),
            "matchFraction": 0.0,
            "madMs":         0.0,
            "estimatedTrimMs": 0,
            "reason": (f"insufficient keyframes "
                        f"(a={len(times_a)}, b={len(times_b)})"),
        }
    if len(times_a) <= len(times_b):
        short, long_ = times_a, times_b
        a_is_short = True
    else:
        short, long_ = times_b, times_a
        a_is_short = False
    _offset, pairs = _best_offset_pairs(short, long_, GAP_TOL_MS)
    matched = len(pairs)
    match_frac = matched / len(short) if short else 0.0
    diffs = [b - a for a, b in pairs]
    mad = _mad_ms(diffs)
    median_diff = int(_median([float(d) for d in diffs])) if diffs else 0
    signed = (median_diff if not a_is_short else -median_diff)
    same = (match_frac >= MIN_MATCH_FRAC) and (mad <= MAX_MAD_MS)
    return {
        "isSameFilm":      same,
        "matched":         matched,
        "totalShorter":    len(short),
        "matchFraction":   match_frac,
        "madMs":           mad,
        "estimatedTrimMs": signed,
        "reason": (f"timing {matched}/{len(short)} ({match_frac:.0%}), "
                    f"MAD={mad:.0f}ms (max {MAX_MAD_MS}ms), "
                    f"trim~={signed}ms"),
    }


def _match_v9(fp_a: dict, fp_b: dict) -> Dict[str, object]:
    """Fallback: v9's peak-gap matcher. Caller has confirmed both
    sides are type=='audio_peak_gaps'."""
    raw = v9.match_fingerprints(fp_a, fp_b)
    # Normalize the keys to F's style for caller convenience.
    return {
        "isSameFilm":      raw.get("isSameFilm", False),
        "matched":         raw.get("matchedGaps", 0),
        "totalShorter":    raw.get("totalGaps", 0),
        "matchFraction":   (raw.get("matchedGaps", 0)
                            / max(1, raw.get("totalGaps", 1))),
        "phashMatched":    raw.get("matchedPHashes", 0),
        "phashCompared":   raw.get("totalPHashes", 0),
        "estimatedTrimMs": raw.get("estimatedTrimMs", 0),
        "reason":          raw.get("reason", ""),
    }


def match_fingerprints(fp_a: dict, fp_b: dict) -> Dict[str, object]:
    """Dispatch by fingerprint type. Same-type pairs use their native
    matcher; cross-type pairs (one F, one v9) are declared NOT same
    film because we don't yet have a way to compare apples-and-oranges
    fingerprints. The caller can resolve cross-type cases by
    re-fingerprinting one side with the other's algorithm.
    """
    type_a = fp_a.get("type")
    type_b = fp_b.get("type")
    if type_a == "keyframes" and type_b == "keyframes":
        return _match_f(fp_a, fp_b)
    if type_a == "audio_peak_gaps" and type_b == "audio_peak_gaps":
        return _match_v9(fp_a, fp_b)
    return {
        "isSameFilm":      False,
        "matched":         0,
        "totalShorter":    0,
        "matchFraction":   0.0,
        "madMs":           0.0,
        "estimatedTrimMs": 0,
        "reason": (f"incompatible fingerprint types: "
                    f"{type_a!r} vs {type_b!r}"),
    }
