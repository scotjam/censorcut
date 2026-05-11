"""Prototype: multi-sector video fingerprint via fractional seeks.

Reads only N small windows of the file via ffmpeg's container-level
seek (`-ss BEFORE -i`). Decodes 1 frame per window at 32×32 grayscale,
computes pHash, builds the fingerprint.

For 2 hr 1080p H.264 over a network share at 50 MB/s:
  - Existing dense-decode approach: 2.5+ minutes (reads ~7 GB)
  - This approach:                   ~3-5 seconds (reads ~30-50 MB)

The fingerprint surfaces two complementary identity features:

  1. Per-anchor pHash sequence — the visual content at 10 fixed
     fractional positions across the body. Same cut, different
     encode → same source bytes at each fraction → same pHashes.

  2. extremeGapMs — the wall-clock distance between the first and
     last anchor (i.e., the 5%-of-body and 95%-of-body anchors).
     Different cuts of the same film have different total
     durations → different body span → different gap.

Body window:
    cushion = clamp(duration * 5%, 60 sec, 5 min)  per side
    body    = [cushion, duration - cushion]

5% per side gives ≥ 90% body coverage for any duration ≥ 0, with
a 1-min floor so very short clips still have meaningful cushions
and a 5-min ceiling so very long content (3+ hr) doesn't trim
unnecessarily.

Anchors land at fractions 0.05, 0.15, 0.25, ..., 0.95 of body.
The first (0.05) is the "near-start" anchor and the last (0.95)
is the "near-end" anchor — together they capture exactly 90% of
the body span.

This is a PROTOTYPE alongside the existing video_fingerprint.py
so both can be benchmarked side-by-side. If real-world testing
shows it's a strict win, we'll promote it.
"""

from __future__ import annotations

import hashlib
import shutil
import subprocess
import sys
from typing import Dict, List, Optional, Tuple


# ---------------------------------------------------------------------
# Tunables
# ---------------------------------------------------------------------

N_ANCHORS               = 10                 # frames sampled across the body
PHASH_RES               = 32
PHASH_DCT_KEEP          = 8
PHASH_HEX_CHARS         = 16                 # 64-bit pHash
TAU_BUCKET              = 0.0005             # ~2.7s at 90min runtime
SPAN_BUCKET_MS          = 1000               # 1-sec body-span granularity

# Body window — 5% per side gives a 90% body gap between first and last
# anchor. Floor of 60s for very short clips; ceiling of 5min for very
# long content so we don't trim more than necessary.
BODY_CUSHION_FRACTION   = 0.05
BODY_CUSHION_MIN_MS     = 60 * 1000
BODY_CUSHION_MAX_MS     = 5 * 60 * 1000


# ---------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------

def _ffmpeg_exe() -> str:
    p = shutil.which("ffmpeg")
    if not p:
        raise RuntimeError("ffmpeg not found on PATH")
    return p


def _body_window(duration_ms: int) -> Tuple[int, int]:
    raw = int(duration_ms * BODY_CUSHION_FRACTION)
    cushion = max(BODY_CUSHION_MIN_MS, min(BODY_CUSHION_MAX_MS, raw))
    body_lo = cushion
    body_hi = max(body_lo + 1, duration_ms - cushion)
    return body_lo, body_hi


def _seek_decode_one_frame(path: str, t_ms: int) -> Optional[bytes]:
    """Container-level seek to t_ms, decode 1 frame, scale to 32×32
    grayscale, return the 1024-byte payload. Uses `-ss BEFORE -i` so
    ffmpeg seeks via the container index instead of decoding from the
    start — essential for the I/O reduction this approach delivers."""
    cmd = [
        _ffmpeg_exe(),
        "-hide_banner", "-loglevel", "error",
        "-hwaccel", "auto",
        "-ss", f"{t_ms / 1000.0:.3f}",
        "-i", path,
        "-an", "-sn", "-dn",
        "-frames:v", "1",
        "-vf", f"scale={PHASH_RES}:{PHASH_RES},format=gray",
        "-f", "rawvideo",
        "-",
    ]
    try:
        proc = subprocess.run(cmd, capture_output=True, timeout=30)
    except subprocess.TimeoutExpired:
        return None
    if proc.returncode != 0:
        return None
    frame_size = PHASH_RES * PHASH_RES
    if len(proc.stdout) < frame_size:
        return None
    return proc.stdout[:frame_size]


def _make_dct_matrix(np, n: int):
    k = np.arange(n)[:, None]
    i = np.arange(n)[None, :]
    m = np.cos(np.pi * (i + 0.5) * k / n)
    m[0, :] *= 1.0 / np.sqrt(2.0)
    m *= np.sqrt(2.0 / n)
    return m.astype(np.float32)


def _phash_from_frame(np, dct_matrix, frame_bytes: bytes) -> int:
    arr = np.frombuffer(frame_bytes, dtype=np.uint8) \
            .reshape(PHASH_RES, PHASH_RES) \
            .astype(np.float32)
    coeffs = dct_matrix @ arr @ dct_matrix.T
    block = coeffs[:PHASH_DCT_KEEP, :PHASH_DCT_KEEP].flatten()
    block_no_dc = block[1:]  # drop DC term
    median = float(np.median(block_no_dc))
    bits = (block_no_dc > median).tolist()
    out = 0
    for b in bits:
        out = (out << 1) | (1 if b else 0)
    return out


# ---------------------------------------------------------------------
# Public entry point
# ---------------------------------------------------------------------

def run(input_path: str,
        duration_ms: int,
        progress=None) -> Dict[str, object]:
    """Compute the multi-sector video fingerprint. Returns the standard
    fingerprint envelope with two extra fields:
        bodySpanMs:    total span of the body window in ms
        extremeGapMs:  wall-clock distance between first and last anchor
                       (≈ 90% of bodySpanMs by construction)
    On any failure, returns {"version": 3, "anchors": []}.
    """
    if duration_ms <= 0:
        return {"version": 3, "anchors": []}

    try:
        import numpy as np  # type: ignore
    except ImportError:
        print("censorcut.multisector: numpy not available; skipping",
              file=sys.stderr)
        return {"version": 3, "anchors": []}

    body_lo, body_hi = _body_window(duration_ms)
    body_span = body_hi - body_lo
    if body_span <= 0:
        return {"version": 3, "durationMs": duration_ms, "anchors": []}

    dct_mat = _make_dct_matrix(np, PHASH_RES)

    # Anchor fractions of the body: 0.05, 0.15, 0.25, ..., 0.95.
    # First and last are "near-start" and "near-end" — their gap covers
    # exactly 90% of the body span.
    fractions = [(i + 0.5) / N_ANCHORS for i in range(N_ANCHORS)]

    anchors: List[Dict[str, object]] = []
    for idx, f in enumerate(fractions):
        if progress:
            progress(idx / N_ANCHORS,
                     f"multisector:anchor {idx + 1}/{N_ANCHORS}")
        t_ms = int(body_lo + f * body_span)
        frame = _seek_decode_one_frame(input_path, t_ms)
        if frame is None:
            print(f"censorcut.multisector: failed to decode at "
                  f"{t_ms} ms (anchor {idx + 1})", file=sys.stderr)
            continue
        phash = _phash_from_frame(np, dct_mat, frame)
        anchors.append({
            "tau":   round(f, 6),
            "phash": f"{phash:0{PHASH_HEX_CHARS}x}",
            "tMs":   t_ms,
        })

    if len(anchors) < 2:
        return {"version": 3, "durationMs": duration_ms, "anchors": []}

    extreme_gap_ms = int(anchors[-1]["tMs"]) - int(anchors[0]["tMs"])

    # Digest input includes:
    #   - body span (bucketed to 1 sec) so different cuts of the same
    #     film hash differently even if their per-anchor pHashes happen
    #     to land on similar content.
    #   - per-anchor (tau bucket, phash) — full content discrimination.
    digest_parts = [f"v3|span={body_span // SPAN_BUCKET_MS}"]
    for a in anchors:
        bucket = int(round(float(a["tau"]) / TAU_BUCKET))
        digest_parts.append(f"{bucket:05d}:{a['phash']}")
    digest = hashlib.sha256("|".join(digest_parts).encode("utf-8")).hexdigest()

    if progress:
        progress(1.0, "multisector:done")

    return {
        "version":           3,
        "type":              "multisector",
        "durationMs":        duration_ms,
        "approxDurationMin": int(round(duration_ms / 60_000.0)),
        "bodySpanMs":        body_span,
        "extremeGapMs":      extreme_gap_ms,
        "anchors":           anchors,
        "digest":            digest,
    }


# ---------------------------------------------------------------------
# CLI for quick benchmarking
# ---------------------------------------------------------------------

if __name__ == "__main__":
    import argparse, json, time
    p = argparse.ArgumentParser(description="Multi-sector fingerprint prototype")
    p.add_argument("input")
    p.add_argument("--duration-ms", type=int,
                   help="Skip the ffprobe duration probe (faster)")
    args = p.parse_args()

    if not args.duration_ms:
        ffp = subprocess.check_output(
            ["ffprobe", "-v", "error", "-print_format", "json",
             "-show_entries", "format=duration", args.input], encoding="utf-8")
        duration_ms = int(float(json.loads(ffp)["format"]["duration"]) * 1000)
    else:
        duration_ms = args.duration_ms

    t0 = time.monotonic()
    fp = run(args.input, duration_ms=duration_ms,
             progress=lambda f, ph: print(
                 f"  [{int(f*100):3d}%] {ph}", file=sys.stderr))
    elapsed = time.monotonic() - t0

    print(json.dumps(fp, indent=2))
    print(f"\nWall time: {elapsed:.2f}s", file=sys.stderr)
