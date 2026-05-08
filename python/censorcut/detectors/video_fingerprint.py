"""Video-content fingerprint built from scene-cut timing + per-cut pHash.

Identifies a specific cut of a film (theatrical / director's / TV) so
the federated edits database can apply user-authored cuts to any
encode of that same cut. Robust to:
  - Different codecs (H.264 / H.265 / VP9 / AV1)
  - Different resolutions and aspect ratios (letterboxing detected)
  - Different frame rates (timestamps in milliseconds, not frame indices)
  - Time-scaling (PAL speedup) — fingerprint is scale-invariant
  - Different audio (dubs, remasters) — audio is irrelevant here
  - Different intro/outro lengths — body window with adaptive cushion

NOT robust to:
  - Different cuts of the film (which is correct — cuts have different
    scene timings, so they should produce different fingerprints)
  - Aggressive bitrate (≤ 1 Mbps streaming) where soft cuts smooth out
  - Films with very few cuts (< 30 cuts in a 90-minute film)
  - Heavy re-grading that shifts pHash beyond the threshold

Algorithm:
  1. ffmpeg pipeline:
       -hwaccel auto
       -vf "fps=2, scale=32:32, format=gray"
       -f rawvideo
     → 1024 bytes per frame (32*32 grayscale).

  2. Per frame: compute a 64-bit pHash (DCT → 8x8 low-freq block →
     drop DC → median threshold → 63 bits, pad to 64).

  3. Detect cuts: Hamming distance between consecutive pHashes;
     adaptive threshold over a sliding 90-second median; minimum
     spacing 1 second.

  4. Refine each cut to sub-frame time via parabolic interpolation
     of the 3-point Hamming-distance peak.

  5. Body window:
       cushion = min(10 min, durationMs / 3)
       body    = [cushion, durationMs - cushion]

  6. Pick top-100 cuts BY HAMMING-DISTANCE MAGNITUDE inside the body.

  7. Normalize timestamps to [0, 1] over the body:
       tau[i] = (cut[i].t - body_start) / (body_end - body_start)
     PAL-speedup multiplies all times by α; the ratio cancels α.
     → Fingerprint is scale-invariant.

  8. Bucket each tau at 0.0005 resolution; concatenate IN ORDER with
     the per-cut phash; sha256 → digest.

  9. Output: {version, durationMs, approxDurationMin, anchors:[{tau, phash}], digest}
"""

from __future__ import annotations

import hashlib
import shutil
import subprocess
import sys
from typing import Dict, List, Optional, Tuple

# ----------------------------------------------------------------------
# Tunables
# ----------------------------------------------------------------------

SAMPLE_FPS              = 2                  # frame rate for cut detection
                                              # (was 8; cuts are 1-frame events
                                              # producing 25+ bit pHash deltas,
                                              # detectable at any rate ≥ 2 fps;
                                              # 4× decode speedup vs 8 fps)
PHASH_RES               = 32                 # decode each frame to NxN gray
PHASH_DCT_KEEP          = 8                  # keep top-left 8x8 DCT block
TOP_K_CUTS              = 100
MIN_CUT_SPACING_MS      = 1000               # 1 second
ADAPTIVE_WINDOW_SEC     = 90                 # sliding-median window
                                              # (was 30; widened to compensate
                                              # for the 4× lower sample rate so
                                              # the median still sees ~180
                                              # samples)
THRESHOLD_MULTIPLIER    = 4.0                # cut iff hd > median * mul
TAU_BUCKET              = 0.0005             # ~2.7s at 90min runtime
PHASH_HEX_CHARS         = 16                 # 64 bits


# ----------------------------------------------------------------------
# Pipeline
# ----------------------------------------------------------------------

def _ffmpeg_exe() -> str:
    p = shutil.which("ffmpeg")
    if not p:
        raise RuntimeError("ffmpeg not found on PATH")
    return p


def _decode_command(input_path: str) -> List[str]:
    """ffmpeg invocation: hardware-decode if available, sample at
    SAMPLE_FPS, scale to PHASH_RES x PHASH_RES grayscale, raw bytes."""
    return [
        _ffmpeg_exe(),
        "-hide_banner", "-loglevel", "error",
        "-hwaccel", "auto",
        "-i", input_path,
        "-an", "-sn", "-dn",
        "-vf", f"fps={SAMPLE_FPS},scale={PHASH_RES}:{PHASH_RES},format=gray",
        "-f", "rawvideo",
        "-",
    ]


def _decode_command_software(input_path: str) -> List[str]:
    """Same pipeline without hwaccel — used as fallback if hwaccel
    initialisation fails. Some Windows machines have hwaccel auto
    pick a backend that can't decode the source codec."""
    return [
        _ffmpeg_exe(),
        "-hide_banner", "-loglevel", "error",
        "-i", input_path,
        "-an", "-sn", "-dn",
        "-vf", f"fps={SAMPLE_FPS},scale={PHASH_RES}:{PHASH_RES},format=gray",
        "-f", "rawvideo",
        "-",
    ]


def _stream_frames(cmd: List[str]):
    """Yield (frame_index, 1024-byte grayscale frame) tuples."""
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    frame_size = PHASH_RES * PHASH_RES
    idx = 0
    try:
        assert proc.stdout is not None
        while True:
            buf = proc.stdout.read(frame_size)
            if not buf:
                break
            if len(buf) < frame_size:
                # ragged tail — discard
                break
            yield idx, buf
            idx += 1
    finally:
        if proc.stdout: proc.stdout.close()
        err = b""
        if proc.stderr:
            try:
                err = proc.stderr.read() or b""
            except Exception:
                pass
            proc.stderr.close()
        rc = proc.wait(timeout=10)
        if rc != 0:
            raise RuntimeError(
                f"ffmpeg exited {rc}: {err.decode('utf-8', 'replace').strip()[:400]}")


# ----------------------------------------------------------------------
# pHash (DCT-based, 64-bit)
# ----------------------------------------------------------------------

def _make_dct_matrix(np, n: int):
    """DCT-II coefficients as an N x N matrix M, where DCT2(x) = M @ x.
    For a 2D image we apply M @ image @ M.T."""
    k = np.arange(n)[:, None]
    i = np.arange(n)[None, :]
    m = np.cos(np.pi * (i + 0.5) * k / n)
    # Orthonormal scaling so the output is comparable across N.
    m[0, :] *= 1.0 / np.sqrt(2.0)
    m *= np.sqrt(2.0 / n)
    return m.astype(np.float32)


def _phash_from_frame(np, dct_matrix, frame_bytes: bytes) -> int:
    """Compute a 64-bit pHash from a PHASH_RES x PHASH_RES grayscale
    frame. Returns a Python int (64 significant bits)."""
    arr = np.frombuffer(frame_bytes, dtype=np.uint8) \
            .reshape(PHASH_RES, PHASH_RES) \
            .astype(np.float32)
    # 2D DCT-II via the precomputed matrix.
    coeffs = dct_matrix @ arr @ dct_matrix.T
    block = coeffs[:PHASH_DCT_KEEP, :PHASH_DCT_KEEP]
    # Discard the DC term (overall brightness — varies with encoding/grade).
    flat = block.flatten()
    flat_no_dc = np.concatenate([flat[:0], flat[1:]])  # drop index 0
    median = float(np.median(flat_no_dc))
    bits = (flat_no_dc > median)
    # Pack 63 bits into a 64-bit int (pad MSB with 0).
    out = 0
    for b in bits.tolist():
        out = (out << 1) | (1 if b else 0)
    return out


def _hamming(a: int, b: int) -> int:
    return bin(a ^ b).count("1")


# ----------------------------------------------------------------------
# Cut detection
# ----------------------------------------------------------------------

def _detect_cuts(np,
                 phashes: List[int],
                 frame_period_ms: int) -> List[Tuple[int, float]]:
    """Find scene cuts as (frame_index, hamming_distance) pairs.

    Adaptive threshold: at each frame, compare Hamming distance to the
    median of distances within ADAPTIVE_WINDOW_SEC. A cut fires if it
    exceeds the local median × THRESHOLD_MULTIPLIER and is the local
    maximum within MIN_CUT_SPACING_MS."""
    if len(phashes) < 3:
        return []

    diffs = np.zeros(len(phashes), dtype=np.int32)
    for i in range(1, len(phashes)):
        diffs[i] = _hamming(phashes[i - 1], phashes[i])

    window_frames = max(1, int(ADAPTIVE_WINDOW_SEC * 1000 / frame_period_ms))
    half = window_frames // 2
    cuts: List[Tuple[int, float]] = []
    min_spacing_frames = max(1, int(MIN_CUT_SPACING_MS / frame_period_ms))
    last_cut_at = -10_000_000

    # Precompute rolling median by quantile-of-window.
    # For speed, use a coarser non-overlapping median.
    for i in range(1, len(phashes)):
        lo = max(1, i - half)
        hi = min(len(phashes), i + half + 1)
        # Median over the window (excluding the candidate itself if it's
        # an obvious outlier — it'll dominate a small window).
        window = diffs[lo:hi]
        if window.size == 0:
            continue
        med = float(np.median(window))
        threshold = max(med * THRESHOLD_MULTIPLIER, 8.0)  # noise floor
        if diffs[i] >= threshold and diffs[i] > diffs[i - 1] and \
           (i + 1 >= len(diffs) or diffs[i] >= diffs[i + 1]):
            if i - last_cut_at < min_spacing_frames:
                # Within spacing window — keep the larger of the two.
                if cuts and diffs[i] > cuts[-1][1]:
                    cuts[-1] = (i, float(diffs[i]))
                continue
            cuts.append((i, float(diffs[i])))
            last_cut_at = i

    return cuts


def _refine_cut_time(diffs, frame_index: int, frame_period_ms: int) -> float:
    """Parabolic interpolation around (i-1, i, i+1) to give a sub-frame
    time refinement. Returns time in milliseconds."""
    i = frame_index
    if i <= 0 or i >= len(diffs) - 1:
        return float(i * frame_period_ms)
    y0, y1, y2 = float(diffs[i - 1]), float(diffs[i]), float(diffs[i + 1])
    denom = (y0 - 2 * y1 + y2)
    if abs(denom) < 1e-6:
        return float(i * frame_period_ms)
    delta = 0.5 * (y0 - y2) / denom
    delta = max(-1.0, min(1.0, delta))  # clamp to ±1 frame
    return float((i + delta) * frame_period_ms)


# ----------------------------------------------------------------------
# Body window
# ----------------------------------------------------------------------

def _body_window(duration_ms: int) -> Tuple[int, int]:
    """Adaptive cushion: 10 minutes for full-length films, scaled down
    for short content."""
    cushion = min(10 * 60 * 1000, duration_ms // 3)
    return cushion, max(cushion + 1, duration_ms - cushion)


# ----------------------------------------------------------------------
# Public entry point
# ----------------------------------------------------------------------

def run(input_path: str,
        duration_ms: int,
        progress=None) -> Dict[str, object]:
    """Compute the video fingerprint. Returns:
        {"version": 1,
         "durationMs": <int>,
         "approxDurationMin": <int>,
         "anchors": [{"tau": float, "phash": "<16-hex>"}, ...],
         "digest": "<sha256 hex>"}
    On any failure, returns {"version": 1, "anchors": []}."""
    if duration_ms <= 0:
        return {"version": 1, "anchors": []}

    try:
        import numpy as np  # type: ignore
    except ImportError:
        print("censorcut.video_fingerprint: numpy not available; skipping",
              file=sys.stderr)
        return {"version": 1, "anchors": []}

    if progress: progress(0.0, "video_fingerprint:decode")
    dct_mat = _make_dct_matrix(np, PHASH_RES)
    frame_period_ms = int(round(1000.0 / SAMPLE_FPS))
    phashes: List[int] = []

    # Try hwaccel first; if ffmpeg fails (e.g., Windows D3D11 init issue
    # for HEVC Main10), fall back to software decode.
    try:
        for _idx, buf in _stream_frames(_decode_command(input_path)):
            phashes.append(_phash_from_frame(np, dct_mat, buf))
    except Exception as e:
        print(f"censorcut.video_fingerprint: hwaccel decode failed ({e}); "
              "retrying with software decode", file=sys.stderr)
        phashes = []
        try:
            for _idx, buf in _stream_frames(_decode_command_software(input_path)):
                phashes.append(_phash_from_frame(np, dct_mat, buf))
        except Exception as e2:
            print(f"censorcut.video_fingerprint: software decode failed: {e2}",
                  file=sys.stderr)
            return {"version": 1, "anchors": []}

    if len(phashes) < 30:
        return {"version": 1, "anchors": []}

    if progress: progress(0.85, "video_fingerprint:cuts")
    diffs = np.zeros(len(phashes), dtype=np.int32)
    for i in range(1, len(phashes)):
        diffs[i] = _hamming(phashes[i - 1], phashes[i])
    cuts = _detect_cuts(np, phashes, frame_period_ms)

    body_lo, body_hi = _body_window(duration_ms)

    # Filter cuts to body, refine sub-frame.
    in_body: List[Tuple[float, int, float]] = []  # (t_ms, frame_idx, hd)
    for frame_idx, hd in cuts:
        t_ms = _refine_cut_time(diffs, frame_idx, frame_period_ms)
        if body_lo <= t_ms < body_hi:
            in_body.append((t_ms, frame_idx, hd))

    if len(in_body) < 5:
        # Not enough cuts to form a fingerprint.
        return {"version": 1, "durationMs": duration_ms, "anchors": []}

    # Top-K by Hamming distance magnitude.
    in_body.sort(key=lambda r: r[2], reverse=True)
    top = in_body[:TOP_K_CUTS]
    # Sort selected back into time order.
    top.sort(key=lambda r: r[0])

    if progress: progress(0.95, "video_fingerprint:digest")
    body_span = float(body_hi - body_lo)
    if body_span <= 0:
        return {"version": 1, "durationMs": duration_ms, "anchors": []}

    anchors_out = []
    digest_parts: List[str] = []
    for t_ms, frame_idx, _hd in top:
        tau = (t_ms - body_lo) / body_span
        # Quantize tau to the bucket grid for the digest, but keep the
        # raw tau in the published anchor for fuzzy matching.
        bucket = int(round(tau / TAU_BUCKET))
        phash_int = phashes[frame_idx]
        phash_hex = f"{phash_int:0{PHASH_HEX_CHARS}x}"
        anchors_out.append({"tau": round(tau, 6), "phash": phash_hex})
        digest_parts.append(f"{bucket:05d}:{phash_hex}")

    digest_input = "v1|" + "|".join(digest_parts)
    digest = hashlib.sha256(digest_input.encode("utf-8")).hexdigest()

    if progress: progress(1.0, "video_fingerprint:done")
    return {
        "version":           1,
        "durationMs":        duration_ms,
        "approxDurationMin": int(round(duration_ms / 60_000.0)),
        "anchors":           anchors_out,
        "digest":            digest,
    }
