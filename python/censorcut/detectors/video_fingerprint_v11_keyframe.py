"""v11 prototype: keyframe-based fingerprint via container index.

Skips audio entirely. Reads keyframe timestamps from the container's
index (Cues for MKV, stss for MP4) via ffprobe — fast even on slow
network shares because it's metadata-only or close to it. For
scene-cut-aware encoders (x264/x265 default), keyframes ARE scene
cut times, giving us a content-derived anchor pattern at almost
zero I/O cost.

Pipeline:
  1. ffprobe -skip_frame nokey -show_frames=pts_time
       → list of keyframe times in seconds
  2. Filter to body region (5%-95% of duration)
  3. Detect fixed-GOP encodes (uniform keyframe spacing) and reject —
     such encodes have keyframes at fixed intervals with no relation
     to scene structure.
  4. Pick top-K=25 keyframes evenly distributed across the body
     with ≥30 sec min spacing.
  5. For each picked keyframe, decode 5 frames over a 1-sec window
     and compute the averaged pHash (same as v9, for cross-encode
     robustness).
  6. Output: peaks (tMs, phash) + gaps + innerSpanMs.

Output shape matches v9 so v9.match_fingerprints() works on v11
fingerprints without modification.

Trim-tolerance: BOTH ends — keyframes are content events that shift
uniformly under any trim, so inter-keyframe gaps are preserved.
Same robustness argument as v9's audio peaks.

Speed: ~5-30 sec per file (most of it the per-keyframe pHash decode,
which is bounded by 25 small container-level seeks). The ffprobe
keyframe extraction itself is typically 1-5 sec.

Failure modes:
  - Fixed-GOP encodes (some XViD, some streaming): keyframes are
    at uniform intervals, gap pattern is degenerate. Detected and
    rejected; caller should fall back to v9.
  - MKV without Cues: ffprobe falls back to scanning the file,
    losing the speed advantage. Bounded by a 10-min timeout.
"""

from __future__ import annotations

import shutil
import subprocess
import sys
from typing import Dict, List, Optional


TOP_K_PEAKS               = 25
MIN_PEAK_SPACING_MS       = 30 * 1000
PHASH_RES                 = 32
PHASH_DCT_KEEP            = 8
PHASH_HEX_CHARS           = 16
PHASH_FRAMES_PER_PEAK     = 5
PHASH_FRAME_WINDOW_MS     = 1000

PEAK_SEARCH_LO_FRAC       = 0.05
PEAK_SEARCH_HI_FRAC       = 0.95

# End-anchored target offsets. For intro-trim cases, file-end is a
# content-stable reference (the trimmed and untrimmed copies share
# the same final content event). Targeting fixed time offsets from
# the end gives identical CONTENT-time targets in both copies, so
# nearest-keyframe picks land on the same content positions.
#
# Layout: skip the last 5 min (end credits), then put TARGET_COUNT
# anchors spaced TARGET_STEP_MS apart going backwards into the body.
TARGET_END_SKIP_MS  = 5 * 60 * 1000     # 5 min off the end
TARGET_STEP_MS      = 3 * 60 * 1000     # 3 min between targets
TARGET_COUNT        = 25

# Fixed-GOP detection: coefficient of variation (std/mean) of
# inter-keyframe gaps below this means keyframes are at near-uniform
# intervals — useless for content discrimination.
FIXED_GOP_CV_THRESHOLD    = 0.20

FFPROBE_TIMEOUT           = 600    # 10 min cap


# ---------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------

def _ffmpeg_exe() -> str:
    p = shutil.which("ffmpeg")
    if not p:
        raise RuntimeError("ffmpeg not found")
    return p


def _ffprobe_exe() -> str:
    p = shutil.which("ffprobe")
    if not p:
        raise RuntimeError("ffprobe not found")
    return p


def _extract_keyframe_times_fast(input_path: str) -> Optional[List[int]]:
    """Fast path: `-skip_frame nokey` filters at the codec layer.
    Works for MP4 (moov.stss) and MKV (Cues + scene-cut-aware
    encoders). Often returns 0 keyframes for AVI because AVI stores
    keyframe info at the container layer (idx1 chunk) and the codec
    layer doesn't see it."""
    cmd = [
        _ffprobe_exe(),
        "-v", "error",
        "-select_streams", "v:0",
        "-skip_frame", "nokey",
        "-show_entries", "frame=pts_time",
        "-of", "csv=p=0",
        input_path,
    ]
    try:
        proc = subprocess.run(cmd, capture_output=True, timeout=FFPROBE_TIMEOUT)
    except subprocess.TimeoutExpired:
        return None
    if proc.returncode != 0:
        return None
    times_ms: List[int] = []
    for line in proc.stdout.decode("utf-8", "replace").splitlines():
        s = line.strip().rstrip(",")
        if not s:
            continue
        try:
            t = float(s)
        except ValueError:
            continue
        if t < 0:
            continue
        times_ms.append(int(t * 1000))
    times_ms.sort()
    return times_ms


def _extract_keyframe_times_packets(input_path: str) -> Optional[List[int]]:
    """Fallback path: read all packets at the container layer and
    filter on the K (keyframe) flag. Works for AVI and any container
    where keyframes are exposed via packet flags. Slower than the
    fast path because it reads packet metadata for ALL frames, not
    just keyframes — but still no decoding."""
    cmd = [
        _ffprobe_exe(),
        "-v", "error",
        "-select_streams", "v:0",
        "-show_entries", "packet=pts_time,flags",
        "-of", "csv=p=0",
        input_path,
    ]
    try:
        proc = subprocess.run(cmd, capture_output=True, timeout=FFPROBE_TIMEOUT)
    except subprocess.TimeoutExpired:
        print(f"censorcut.v11: ffprobe (packet path) timed out (>10 min) "
              f"on {input_path}", file=sys.stderr)
        return None
    if proc.returncode != 0:
        err = (proc.stderr or b"").decode("utf-8", "replace").strip()
        print(f"censorcut.v11: ffprobe (packet path) failed "
              f"(rc={proc.returncode}): {err[:200]}", file=sys.stderr)
        return None
    times_ms: List[int] = []
    for line in proc.stdout.decode("utf-8", "replace").splitlines():
        s = line.strip()
        if not s:
            continue
        # CSV format from `packet=pts_time,flags` is "<pts_time>,<flags>"
        # where <flags> is a 2-char string like "K_" / "__" / "KD" etc.
        # Keyframe → flags contains 'K'.
        parts = s.split(",")
        if len(parts) < 2:
            continue
        try:
            t = float(parts[0])
        except ValueError:
            continue
        if t < 0:
            continue
        flags = parts[1]
        if "K" in flags:
            times_ms.append(int(t * 1000))
    times_ms.sort()
    return times_ms


def _extract_keyframe_times_discard_nonkey(input_path: str) -> Optional[List[int]]:
    """Third fallback: `-discard nonkey` at the demuxer layer drops
    non-keyframe packets BEFORE they reach the codec. For AVI this
    uses the idx1 chunk's AVIIF_KEYFRAME flags directly. Should work
    when the codec-layer pict_type is unreliable but the demuxer
    knows which packets are keyframes."""
    cmd = [
        _ffprobe_exe(),
        "-v", "error",
        "-discard", "nonkey",
        "-select_streams", "v:0",
        "-show_entries", "packet=pts_time",
        "-of", "csv=p=0",
        input_path,
    ]
    try:
        proc = subprocess.run(cmd, capture_output=True, timeout=FFPROBE_TIMEOUT)
    except subprocess.TimeoutExpired:
        print(f"censorcut.v11: ffprobe (discard-nonkey) timed out on "
              f"{input_path}", file=sys.stderr)
        return None
    if proc.returncode != 0:
        err = (proc.stderr or b"").decode("utf-8", "replace").strip()
        print(f"censorcut.v11: ffprobe (discard-nonkey) failed "
              f"(rc={proc.returncode}): {err[:200]}", file=sys.stderr)
        return None
    times_ms: List[int] = []
    for line in proc.stdout.decode("utf-8", "replace").splitlines():
        s = line.strip().rstrip(",")
        if not s:
            continue
        try:
            t = float(s)
        except ValueError:
            continue
        if t < 0:
            continue
        times_ms.append(int(t * 1000))
    times_ms.sort()
    return times_ms


def _extract_keyframe_times_packet_index(input_path: str) -> Optional[List[int]]:
    """Fourth fallback: AVI/MS-MPEG4 streams expose keyframe FLAGS at
    the packet level but not pts_time. Derive times from packet INDEX
    multiplied by the constant frame period read from the stream's
    avg_frame_rate. One packet == one frame in AVI, so the i-th
    packet's time is `i / fps` seconds.
    """
    # Probe the frame rate first.
    rate_cmd = [
        _ffprobe_exe(), "-v", "error",
        "-select_streams", "v:0",
        "-show_entries", "stream=avg_frame_rate",
        "-of", "csv=p=0",
        input_path,
    ]
    try:
        proc = subprocess.run(rate_cmd, capture_output=True, timeout=30)
    except subprocess.TimeoutExpired:
        return None
    if proc.returncode != 0:
        return None
    rate_str = proc.stdout.decode("utf-8", "replace").strip().rstrip(",")
    fps: float
    try:
        if "/" in rate_str:
            num, den = rate_str.split("/")
            d = float(den)
            if d == 0.0:
                return None
            fps = float(num) / d
        else:
            fps = float(rate_str)
    except ValueError:
        return None
    if fps <= 0.0 or fps > 1000.0:
        return None
    frame_period_ms = 1000.0 / fps

    # Now stream packet flags and count index per packet.
    cmd = [
        _ffprobe_exe(), "-v", "error",
        "-select_streams", "v:0",
        "-show_entries", "packet=flags",
        "-of", "csv=p=0",
        input_path,
    ]
    try:
        proc = subprocess.run(cmd, capture_output=True, timeout=FFPROBE_TIMEOUT)
    except subprocess.TimeoutExpired:
        return None
    if proc.returncode != 0:
        return None
    times_ms: List[int] = []
    idx = 0
    for line in proc.stdout.decode("utf-8", "replace").splitlines():
        s = line.strip().rstrip(",")
        if not s:
            continue
        if "K" in s:
            times_ms.append(int(idx * frame_period_ms))
        idx += 1
    times_ms.sort()
    return times_ms


def _extract_keyframe_times_ms(input_path: str) -> Optional[List[int]]:
    """Read keyframe timestamps. Tries five paths in order of speed;
    returns the first that produces ≥10 keyframes:

      0. Direct container-metadata parse (MKV Cues / MP4 stss / AVI
         idx1) — ~1000× faster than ffprobe per MB on network shares
         because it reads only the container index, never the
         bitstream. See censorcut.detectors.fast_keyframes.

      1. ffprobe fast: `-skip_frame nokey -show_frames=pts_time`
         Works for MP4 (moov.stss), MKV with Cues, scene-cut-aware
         encoders.

      2. ffprobe packet flags: `-show_packets=pts_time,flags`
         filtered on K. Works for any container that exposes
         keyframe info at the packet level.

      3. ffprobe demuxer discard: `-discard nonkey
         -show_packets=pts_time`. Drops non-keyframe packets before
         the codec layer; uses AVI's idx1 chunk directly.

      4. ffprobe packet index × frame period: for AVI/MS-MPEG4 where
         packet flags expose K but pts_time is N/A. Time derived
         from packet index * (1000 / avg_frame_rate).
    """
    from . import fast_keyframes  # local import — keeps v11 importable
                                  # even if fast_keyframes has a bug.
    n_direct = 0
    n_fast = 0
    n_pkt = 0
    n_disc = 0
    n_pidx = 0

    direct = fast_keyframes.extract_keyframe_times_ms(input_path)
    if direct is not None:
        n_direct = len(direct)
        if n_direct >= 10:
            return direct

    times = _extract_keyframe_times_fast(input_path)
    if times is not None:
        n_fast = len(times)
        if n_fast >= 10:
            return times

    pkt = _extract_keyframe_times_packets(input_path)
    if pkt is not None:
        n_pkt = len(pkt)
        if n_pkt >= 10:
            return pkt

    disc = _extract_keyframe_times_discard_nonkey(input_path)
    if disc is not None:
        n_disc = len(disc)
        if n_disc >= 10:
            return disc

    pidx = _extract_keyframe_times_packet_index(input_path)
    if pidx is not None:
        n_pidx = len(pidx)
        if n_pidx >= 10:
            return pidx

    # All paths failed. Log diagnostic and return the longest non-None
    # list (might still be empty).
    print(f"censorcut.v11: keyframe extraction underperformed "
          f"(direct={n_direct}, fast={n_fast}, packet-flags={n_pkt}, "
          f"discard-nonkey={n_disc}, packet-index={n_pidx}) "
          f"on {input_path}", file=sys.stderr)
    candidates = [t for t in (direct, times, pkt, disc, pidx) if t is not None]
    if not candidates:
        return None
    return max(candidates, key=len)


def _is_fixed_gop(np, times_ms: List[int]) -> bool:
    """Detect roughly-uniform keyframe spacing. Returns True if the
    coefficient of variation of consecutive gaps is below the
    FIXED_GOP_CV_THRESHOLD — i.e., gaps are too uniform to be content-
    derived."""
    if len(times_ms) < 10:
        return False
    arr = np.array(times_ms, dtype=np.float64)
    gaps = np.diff(arr)
    if len(gaps) == 0:
        return False
    mean_gap = float(gaps.mean())
    if mean_gap <= 0:
        return False
    std_gap = float(gaps.std())
    cv = std_gap / mean_gap
    return cv < FIXED_GOP_CV_THRESHOLD


def _make_dct_matrix(np, n: int):
    k = np.arange(n)[:, None]; i = np.arange(n)[None, :]
    m = np.cos(np.pi * (i + 0.5) * k / n)
    m[0, :] *= 1.0 / np.sqrt(2.0); m *= np.sqrt(2.0 / n)
    return m.astype(np.float32)


def _seek_decode_n_frames(input_path: str, center_ms: int,
                           window_ms: int, n_frames: int) -> List[bytes]:
    start_ms = max(0, center_ms - window_ms // 2)
    fps = max(1, int(round(n_frames * 1000 / window_ms)))
    cmd = [
        _ffmpeg_exe(),
        "-hide_banner", "-loglevel", "error",
        "-hwaccel", "auto",
        "-ss", f"{start_ms / 1000.0:.3f}",
        "-i", input_path, "-an", "-sn", "-dn",
        "-t", f"{window_ms / 1000.0:.3f}",
        "-vf", f"fps={fps},scale={PHASH_RES}:{PHASH_RES},format=gray",
        "-f", "rawvideo", "-",
    ]
    try:
        proc = subprocess.run(cmd, capture_output=True, timeout=60)
    except subprocess.TimeoutExpired:
        return []
    if proc.returncode != 0:
        return []
    fs = PHASH_RES * PHASH_RES
    out = []
    for i in range(min(n_frames, len(proc.stdout) // fs)):
        out.append(proc.stdout[i * fs:(i + 1) * fs])
    return out


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


def _evenly_sample(picks: List[int], k: int) -> List[int]:
    """If picks > k, return k evenly-distributed entries by index.
    NOTE: index-based sampling is fragile across encodes — different
    encoders place different numbers of keyframes, so the i-th
    keyframe by index is NOT at the same content time in two encodes
    of the same source. Use _pick_nearest_to_fractions for cross-
    encode robustness."""
    if len(picks) <= k:
        return list(picks)
    step = len(picks) / k
    return [picks[int(i * step)] for i in range(k)]


def _pick_nearest_to_fractions(body_keys: List[int],
                                 body_lo: int, body_hi: int,
                                 k: int) -> List[int]:
    """LEGACY body-fraction picking. Drifts under trim because
    body_span itself differs between trimmed/untrimmed copies. Use
    _pick_nearest_to_end_offsets for cross-encode + intro-trim
    robustness."""
    body_span = max(1, body_hi - body_lo)
    targets = [body_lo + (i + 0.5) / k * body_span for i in range(k)]
    if not body_keys:
        return []
    import bisect
    picked: set = set()
    for t in targets:
        idx = bisect.bisect_left(body_keys, t)
        cands = []
        if idx > 0:
            cands.append(body_keys[idx - 1])
        if idx < len(body_keys):
            cands.append(body_keys[idx])
        if not cands:
            continue
        nearest = min(cands, key=lambda k: abs(k - t))
        picked.add(nearest)
    return sorted(picked)


def _pick_nearest_to_end_offsets(body_keys: List[int],
                                   duration_ms: int) -> List[int]:
    """Pick keyframes nearest to fixed offsets-from-end-of-file.

    For intro-trim cases, the file end is the same content event in
    original and trimmed versions. Anchoring targets to fixed offsets
    from end gives identical CONTENT-time targets in both — so the
    nearest-keyframe picks land on the same content positions even
    though body_span differs by the trim amount.

    Layout: skip last TARGET_END_SKIP_MS, then place TARGET_COUNT
    anchors spaced TARGET_STEP_MS apart going backwards. Anchors
    that would fall before body_lo are dropped (short content)."""
    if not body_keys:
        return []
    import bisect

    # Build target list in chronological (ascending) order.
    targets_asc: List[int] = []
    for i in range(TARGET_COUNT):
        # Index 0 = furthest from end, last index = closest to end.
        offset_from_end = TARGET_END_SKIP_MS + (TARGET_COUNT - 1 - i) * TARGET_STEP_MS
        t = duration_ms - offset_from_end
        if t < 0:
            continue
        targets_asc.append(t)
    if not targets_asc:
        return []

    picked: set = set()
    for t in targets_asc:
        idx = bisect.bisect_left(body_keys, t)
        cands = []
        if idx > 0:
            cands.append(body_keys[idx - 1])
        if idx < len(body_keys):
            cands.append(body_keys[idx])
        if not cands:
            continue
        nearest = min(cands, key=lambda k: abs(k - t))
        picked.add(nearest)
    return sorted(picked)


def _enforce_min_spacing(times_ms: List[int], min_spacing_ms: int) -> List[int]:
    """Greedy left-to-right NMS by time spacing."""
    out = []
    for t in times_ms:
        if not out or t - out[-1] >= min_spacing_ms:
            out.append(t)
    return out


# ---------------------------------------------------------------------
# Public entry point
# ---------------------------------------------------------------------

def run(input_path: str, duration_ms: int, progress=None) -> Dict[str, object]:
    if duration_ms <= 0:
        return {"version": 11, "anchors": []}
    try:
        import numpy as np
    except ImportError:
        return {"version": 11, "anchors": []}

    if progress: progress(0.0, "v11:keyframes")
    keyframe_times = _extract_keyframe_times_ms(input_path)
    if keyframe_times is None:
        return {"version": 11, "anchors": [], "error": "ffprobe failed"}
    total_keyframes = len(keyframe_times)
    if total_keyframes < 10:
        print(f"censorcut.v11: only {total_keyframes} keyframes in container",
              file=sys.stderr)
        return {"version": 11, "anchors": [],
                "error": f"too few keyframes ({total_keyframes})"}

    # Filter to body region.
    body_lo = int(duration_ms * PEAK_SEARCH_LO_FRAC)
    body_hi = int(duration_ms * PEAK_SEARCH_HI_FRAC)
    body_keys = [t for t in keyframe_times if body_lo <= t <= body_hi]
    if len(body_keys) < 10:
        print(f"censorcut.v11: only {len(body_keys)} body keyframes",
              file=sys.stderr)
        return {"version": 11, "anchors": [],
                "error": f"only {len(body_keys)} body keyframes"}

    # Reject fixed-GOP encodes (keyframes at uniform intervals).
    if _is_fixed_gop(np, body_keys):
        gaps = np.diff(np.array(body_keys, dtype=np.float64))
        cv = float(gaps.std() / max(1.0, gaps.mean()))
        print(f"censorcut.v11: fixed-GOP detected — gaps cv={cv:.3f}, "
              f"mean={gaps.mean():.0f} ms; falling back required",
              file=sys.stderr)
        return {"version": 11, "anchors": [],
                "isFixedGop": True,
                "totalKeyframes": total_keyframes,
                "bodyKeyframes": len(body_keys),
                "gapCv": cv,
                "error": "fixed_gop"}

    if progress: progress(0.10, "v11:pick-peaks")
    # END-anchored target offsets: for each fixed offset-from-end,
    # find the keyframe nearest in time. Content-stable for intro-
    # trim cases (file end is the same content event in trimmed
    # and untrimmed copies). Cross-encode robust because both copies
    # gravitate to the same content positions even when their
    # underlying keyframe sets differ in size.
    spaced = _pick_nearest_to_end_offsets(body_keys, duration_ms)
    spaced = _enforce_min_spacing(spaced, MIN_PEAK_SPACING_MS)
    if len(spaced) < 5:
        return {"version": 11, "anchors": [],
                "error": f"only {len(spaced)} peaks after end-offset picking"}

    if progress: progress(0.20, "v11:phash")
    dct = _make_dct_matrix(np, PHASH_RES)
    anchors: List[Dict[str, object]] = []
    for i, t_ms in enumerate(spaced):
        if progress:
            progress(0.20 + 0.75 * i / len(spaced),
                     f"v11:phash {i + 1}/{len(spaced)}")
        frames = _seek_decode_n_frames(input_path, t_ms,
                                        PHASH_FRAME_WINDOW_MS,
                                        PHASH_FRAMES_PER_PEAK)
        ph = _averaged_phash(np, dct, frames)
        if ph is None:
            anchors.append({"tMs": t_ms, "phash": None})
            continue
        anchors.append({"tMs": t_ms, "phash": f"{ph:0{PHASH_HEX_CHARS}x}"})

    peak_times_ms = [int(a["tMs"]) for a in anchors]
    gaps_ms = [peak_times_ms[i + 1] - peak_times_ms[i]
               for i in range(len(peak_times_ms) - 1)]
    inner_span = (peak_times_ms[-2] - peak_times_ms[1]
                  if len(peak_times_ms) >= 4 else 0)

    if progress: progress(1.0, "v11:done")
    return {
        "version":           11,
        "type":              "keyframe",
        "durationMs":        duration_ms,
        "approxDurationMin": int(round(duration_ms / 60000)),
        "totalKeyframes":    total_keyframes,
        "bodyKeyframes":     len(body_keys),
        "peakCount":         len(peak_times_ms),
        "innerSpanMs":       inner_span,
        "gapsMs":            gaps_ms,
        "peaks":             anchors,
    }
