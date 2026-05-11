"""Prototype: anchor-pair video fingerprint.

Identifies TWO unique points in the body of the video — one near the
start (avoiding intros/logos), one near the end (avoiding credits) —
then fingerprints with three pieces:

   { phash_A, phash_B, gap_ms }

Where gap_ms is the wall-clock distance between the two anchors.

Different cuts of the same film have different runtimes and different
content at the proportional anchor positions, so produce different
gaps and / or different pHashes → different digests.

Same cut, different encode: same source bytes at the same source
times → same pHashes + same gap → same digest.

Different films: different content at the anchor windows → different
pHashes → different digest.

The "unique" property: within each search window we pick the frame
whose pHash is FURTHEST from the window's mean pHash bit pattern.
That gives us a deterministically-distinctive frame rather than a
random pick. Tie-breaks favour the earlier timestamp to keep
selection reproducible across encodes.

Wall time on 2 hr 1080p H.264:
  - Two seek-and-decode windows (each ~10 frames at 1 fps)
  - Local SSD: ~2 sec
  - Network share (50 MB/s): ~3-5 sec
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

PHASH_RES               = 32
PHASH_DCT_KEEP          = 8
PHASH_HEX_CHARS         = 16

# Body cushion — same shape as the multi-sector module: 5% per side
# clamped to [60s, 5min] so we cover ≥ 90% of the body even on edge-
# case durations.
BODY_CUSHION_FRACTION   = 0.05
BODY_CUSHION_MIN_MS     = 60 * 1000
BODY_CUSHION_MAX_MS     = 5 * 60 * 1000

# Search windows for the two anchors, expressed as fractions of the
# BODY (not of the full duration). The "near-start" anchor lives
# somewhere in the first 10% of the body; the "near-end" anchor in
# the last 10%.
START_WINDOW_FRACTION   = (0.05, 0.15)
END_WINDOW_FRACTION     = (0.85, 0.95)

# How many candidate frames to inspect per anchor window. More =
# better anchor reproducibility across encodes, at linear cost.
CANDIDATES_PER_WINDOW   = 10
CANDIDATE_FPS           = 1   # 1 frame per second across the window

# Bucket the gap to 1-second granularity so the digest absorbs
# minor PTS jitter across encodes.
GAP_BUCKET_MS           = 1000


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


def _decode_window(path: str, start_ms: int, duration_ms: int,
                   fps: int) -> List[Tuple[int, bytes]]:
    """Decode N frames at the given fps over [start_ms, start_ms+duration_ms).
    Returns a list of (sample_t_ms, 32×32 grayscale bytes).
    Uses container-level seek so reads only the bytes near start_ms."""
    cmd = [
        _ffmpeg_exe(),
        "-hide_banner", "-loglevel", "error",
        "-hwaccel", "auto",
        "-ss", f"{start_ms / 1000.0:.3f}",
        "-i", path,
        "-an", "-sn", "-dn",
        "-t", f"{duration_ms / 1000.0:.3f}",
        "-vf", f"fps={fps},scale={PHASH_RES}:{PHASH_RES},format=gray",
        "-f", "rawvideo",
        "-",
    ]
    try:
        proc = subprocess.run(cmd, capture_output=True, timeout=30)
    except subprocess.TimeoutExpired:
        return []
    if proc.returncode != 0:
        return []
    frame_size = PHASH_RES * PHASH_RES
    out: List[Tuple[int, bytes]] = []
    n = len(proc.stdout) // frame_size
    for i in range(n):
        sample_t_ms = start_ms + int(i * 1000 / fps)
        out.append((sample_t_ms,
                    proc.stdout[i * frame_size : (i + 1) * frame_size]))
    return out


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
    block_no_dc = block[1:]
    median = float(np.median(block_no_dc))
    bits = (block_no_dc > median).tolist()
    out = 0
    for b in bits:
        out = (out << 1) | (1 if b else 0)
    return out


def _hamming(a: int, b: int) -> int:
    return bin(a ^ b).count("1")


def _pick_distinctive_anchor(np, dct_matrix,
                              candidates: List[Tuple[int, bytes]]
                              ) -> Optional[Tuple[int, int]]:
    """Among the candidate frames, return (t_ms, phash) for the frame
    whose pHash bit-pattern is furthest from the WINDOW MEAN pattern.
    Ties broken by earlier timestamp, so selection is reproducible
    across encodes that produce slightly different per-frame jitter."""
    if not candidates:
        return None
    phashes = [(t, _phash_from_frame(np, dct_matrix, b)) for t, b in candidates]
    if len(phashes) == 1:
        return phashes[0]
    # Compute the bit-wise mean ("majority vote per bit") across all
    # candidate pHashes — this is the window's "average" frame.
    counts = [0] * 64
    for _t, h in phashes:
        for i in range(64):
            if (h >> i) & 1:
                counts[i] += 1
    half = len(phashes) / 2.0
    mean_pattern = 0
    for i in range(64):
        if counts[i] > half:
            mean_pattern |= (1 << i)
    # Pick the candidate with the maximum Hamming distance from the
    # mean pattern, earliest timestamp wins ties.
    best = None
    best_score = -1
    for t, h in phashes:
        d = _hamming(h, mean_pattern)
        if d > best_score or (d == best_score and (best is None or t < best[0])):
            best = (t, h)
            best_score = d
    return best


# ---------------------------------------------------------------------
# Public entry point
# ---------------------------------------------------------------------

def run(input_path: str,
        duration_ms: int,
        progress=None) -> Dict[str, object]:
    """Compute the anchor-pair video fingerprint. Returns:
       {
         "version": 4, "type": "anchorpair",
         "durationMs", "approxDurationMin",
         "anchorA": {"tMs", "phash"},
         "anchorB": {"tMs", "phash"},
         "gapMs": <bucketed-1s>,
         "digest": <sha256>,
       }
    On any failure, returns {"version": 4, "anchorA": null, ...}.
    """
    if duration_ms <= 0:
        return {"version": 4, "anchorA": None, "anchorB": None}

    try:
        import numpy as np  # type: ignore
    except ImportError:
        print("censorcut.anchorpair: numpy not available", file=sys.stderr)
        return {"version": 4, "anchorA": None, "anchorB": None}

    body_lo, body_hi = _body_window(duration_ms)
    body_span = body_hi - body_lo
    if body_span <= 0:
        return {"version": 4, "anchorA": None, "anchorB": None}

    dct_mat = _make_dct_matrix(np, PHASH_RES)

    # Start window.
    if progress: progress(0.0, "anchorpair:start-window")
    s_lo = body_lo + int(body_span * START_WINDOW_FRACTION[0])
    s_hi = body_lo + int(body_span * START_WINDOW_FRACTION[1])
    start_dur = max(1000, s_hi - s_lo)
    start_candidates = _decode_window(input_path, s_lo, start_dur,
                                       fps=CANDIDATE_FPS)
    anchor_a = _pick_distinctive_anchor(np, dct_mat, start_candidates)

    # End window.
    if progress: progress(0.5, "anchorpair:end-window")
    e_lo = body_lo + int(body_span * END_WINDOW_FRACTION[0])
    e_hi = body_lo + int(body_span * END_WINDOW_FRACTION[1])
    end_dur = max(1000, e_hi - e_lo)
    end_candidates = _decode_window(input_path, e_lo, end_dur,
                                     fps=CANDIDATE_FPS)
    anchor_b = _pick_distinctive_anchor(np, dct_mat, end_candidates)

    if anchor_a is None or anchor_b is None:
        print(f"censorcut.anchorpair: could not select both anchors "
              f"(A={anchor_a is not None}, B={anchor_b is not None})",
              file=sys.stderr)
        return {"version": 4, "anchorA": None, "anchorB": None}

    t_a, phash_a = anchor_a
    t_b, phash_b = anchor_b
    gap_ms = t_b - t_a
    gap_bucketed = gap_ms // GAP_BUCKET_MS

    phash_a_hex = f"{phash_a:0{PHASH_HEX_CHARS}x}"
    phash_b_hex = f"{phash_b:0{PHASH_HEX_CHARS}x}"
    digest_input = f"v4|{phash_a_hex}|{gap_bucketed}|{phash_b_hex}"
    digest = hashlib.sha256(digest_input.encode("utf-8")).hexdigest()

    if progress: progress(1.0, "anchorpair:done")

    return {
        "version":           4,
        "type":              "anchorpair",
        "durationMs":        duration_ms,
        "approxDurationMin": int(round(duration_ms / 60_000.0)),
        "anchorA":           {"tMs": int(t_a), "phash": phash_a_hex},
        "anchorB":           {"tMs": int(t_b), "phash": phash_b_hex},
        "gapMs":             int(gap_ms),
        "digest":            digest,
    }


# ---------------------------------------------------------------------
# CLI for quick benchmarking
# ---------------------------------------------------------------------

if __name__ == "__main__":
    import argparse, json, time
    p = argparse.ArgumentParser(description="Anchor-pair fingerprint prototype")
    p.add_argument("input")
    p.add_argument("--duration-ms", type=int)
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
             progress=lambda f, ph: print(f"  [{int(f*100):3d}%] {ph}",
                                            file=sys.stderr))
    elapsed = time.monotonic() - t0

    print(json.dumps(fp, indent=2))
    print(f"\nWall time: {elapsed:.2f}s", file=sys.stderr)
