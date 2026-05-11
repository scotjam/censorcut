"""v6 prototype: fixed-fraction body, SCENE-CUT picking within each
candidate window.

For each of 10 body fractions, decode 5 candidate frames at ~2.5 fps
covering ±1 sec around the target time. Pick the candidate whose
pHash is the FURTHEST from its IMMEDIATE PREDECESSOR's pHash —
i.e., the frame that looks most like a scene-cut to a viewer.

Tie-break: largest absolute pHash value (deterministic).

Trim-tolerance: NONE (fixed-fraction body shifts with duration).
This isolates the "scene-cut picking" idea so we can compare it
against v4 (peer outlier) and v5 (variance) under the same body
window.
"""

from __future__ import annotations

import hashlib
import shutil
import subprocess
import sys
from typing import Dict, List, Optional, Tuple


N_ANCHORS = 10
PHASH_RES = 32
PHASH_DCT_KEEP = 8
PHASH_HEX_CHARS = 16
TAU_BUCKET = 0.0005
SPAN_BUCKET_MS = 1000

BODY_CUSHION_FRACTION = 0.05
BODY_CUSHION_MIN_MS = 60 * 1000
BODY_CUSHION_MAX_MS = 5 * 60 * 1000

CANDIDATES_PER_FRACTION = 5
CANDIDATE_WINDOW_MS = 2000


def _ffmpeg_exe() -> str:
    p = shutil.which("ffmpeg")
    if not p:
        raise RuntimeError("ffmpeg not found")
    return p


def _body_window(d: int) -> Tuple[int, int]:
    raw = int(d * BODY_CUSHION_FRACTION)
    c = max(BODY_CUSHION_MIN_MS, min(BODY_CUSHION_MAX_MS, raw))
    return c, max(c + 1, d - c)


def _decode_window(path: str, start_ms: int, dur_ms: int, n: int):
    fps = max(1, int(round(n * 1000 / dur_ms)))
    cmd = [
        _ffmpeg_exe(), "-hide_banner", "-loglevel", "error",
        "-hwaccel", "auto",
        "-ss", f"{start_ms / 1000.0:.3f}",
        "-i", path, "-an", "-sn", "-dn",
        "-t", f"{dur_ms / 1000.0:.3f}",
        "-vf", f"fps={fps},scale={PHASH_RES}:{PHASH_RES},format=gray",
        "-f", "rawvideo", "-",
    ]
    try:
        proc = subprocess.run(cmd, capture_output=True, timeout=30)
    except subprocess.TimeoutExpired:
        return []
    if proc.returncode != 0:
        return []
    fs = PHASH_RES * PHASH_RES
    out = []
    for i in range(min(n, len(proc.stdout) // fs)):
        out.append((start_ms + int(i * 1000 / fps),
                    proc.stdout[i * fs:(i + 1) * fs]))
    return out


def _make_dct_matrix(np, n):
    k = np.arange(n)[:, None]; i = np.arange(n)[None, :]
    m = np.cos(np.pi * (i + 0.5) * k / n)
    m[0, :] *= 1.0 / np.sqrt(2.0); m *= np.sqrt(2.0 / n)
    return m.astype(np.float32)


def _phash(np, dct, frame_bytes: bytes) -> int:
    arr = np.frombuffer(frame_bytes, dtype=np.uint8) \
            .reshape(PHASH_RES, PHASH_RES).astype(np.float32)
    coeffs = dct @ arr @ dct.T
    block = coeffs[:PHASH_DCT_KEEP, :PHASH_DCT_KEEP].flatten()[1:]
    median = float(np.median(block))
    bits = (block > median).tolist()
    out = 0
    for b in bits:
        out = (out << 1) | (1 if b else 0)
    return out


def _hamming(a: int, b: int) -> int:
    return bin(a ^ b).count("1")


def _pick_max_diff_from_prev(np, dct, candidates):
    """Pick the candidate whose pHash has the largest Hamming distance
    from its immediate predecessor in the window — looks like a scene
    cut. Tie-break: largest pHash int (deterministic, encode-stable
    when phashes match exactly)."""
    if not candidates:
        return None
    if len(candidates) == 1:
        return candidates[0]
    phashes = [(t, _phash(np, dct, b), b) for t, b in candidates]
    best = phashes[0]
    best_score = -1
    for i in range(1, len(phashes)):
        prev_h = phashes[i - 1][1]
        cur_t, cur_h, cur_b = phashes[i]
        d = _hamming(prev_h, cur_h)
        if d > best_score or (d == best_score and cur_h > best[1]):
            best = (cur_t, cur_h, cur_b)
            best_score = d
    return (best[0], best[2])


def run(input_path: str, duration_ms: int, progress=None) -> Dict[str, object]:
    if duration_ms <= 0:
        return {"version": 6, "anchors": []}
    try:
        import numpy as np
    except ImportError:
        return {"version": 6, "anchors": []}

    body_lo, body_hi = _body_window(duration_ms)
    span = body_hi - body_lo
    if span <= 0:
        return {"version": 6, "anchors": []}

    dct = _make_dct_matrix(np, PHASH_RES)
    fractions = [(i + 0.5) / N_ANCHORS for i in range(N_ANCHORS)]
    anchors = []
    for idx, f in enumerate(fractions):
        if progress: progress(idx / N_ANCHORS, f"v6:{idx + 1}/{N_ANCHORS}")
        t_target = int(body_lo + f * span)
        win_lo = max(body_lo, t_target - CANDIDATE_WINDOW_MS // 2)
        candidates = _decode_window(input_path, win_lo, CANDIDATE_WINDOW_MS,
                                     CANDIDATES_PER_FRACTION)
        picked = _pick_max_diff_from_prev(np, dct, candidates)
        if picked is None:
            continue
        t_ms, fb = picked
        ph = _phash(np, dct, fb)
        anchors.append({
            "tau": round(f, 6),
            "phash": f"{ph:0{PHASH_HEX_CHARS}x}",
            "tMs": t_ms,
        })
    if len(anchors) < 2:
        return {"version": 6, "anchors": []}

    digest_parts = [f"v6|span={span // SPAN_BUCKET_MS}"]
    for a in anchors:
        digest_parts.append(f"{int(round(float(a['tau']) / TAU_BUCKET)):05d}:{a['phash']}")
    digest = hashlib.sha256("|".join(digest_parts).encode()).hexdigest()
    if progress: progress(1.0, "v6:done")
    return {
        "version": 6, "type": "scene_cut",
        "durationMs": duration_ms,
        "approxDurationMin": int(round(duration_ms / 60000)),
        "bodySpanMs": span, "anchors": anchors, "digest": digest,
    }
