"""v8 prototype: AUDIO-ANCHORED body + SCENE-CUT picking within each
sample window.

Combines v7's content-anchored body endpoints with v6's scene-cut
picking inside each per-fraction candidate window. Two unrelated
sources of robustness composed:

  - Trim-tolerance from the audio-anchored body (v7).
  - Frame selection that prefers content boundaries over peer-
    relative outliers, hopefully more reproducible across encodes
    than v3/v4 even at the per-anchor level (v6).

For each of 11 body fractions, decode 5 candidate frames at ~2.5 fps
covering ±1 sec around the target time. Pick the one with the
largest pHash Hamming distance from its predecessor (scene-cut-like).
"""

from __future__ import annotations

import hashlib
import shutil
import subprocess
import sys
from typing import Dict, List, Optional, Tuple


N_ANCHORS_VIDEO = 11
PHASH_RES = 32
PHASH_DCT_KEEP = 8
PHASH_HEX_CHARS = 16
TAU_BUCKET = 0.0005
SPAN_BUCKET_MS = 1000

CANDIDATES_PER_FRACTION = 5
CANDIDATE_WINDOW_MS = 2000

AUDIO_SAMPLE_RATE = 8000
RMS_WINDOW_SAMPLES = 800
ROLLING_WINDOW_SAMPLES = 50

START_SKIP_MS = 3 * 60 * 1000
START_HORIZON_MS = 15 * 60 * 1000
END_SKIP_MS = 5 * 60 * 1000
END_HORIZON_MS = 15 * 60 * 1000
SHORT_CONTENT_THRESHOLD_MS = 30 * 60 * 1000


def _ffmpeg_exe() -> str:
    p = shutil.which("ffmpeg")
    if not p:
        raise RuntimeError("ffmpeg not found")
    return p


def _decode_audio(input_path: str) -> Optional[bytes]:
    cmd_side = [
        _ffmpeg_exe(), "-hide_banner", "-loglevel", "error",
        "-i", input_path,
        "-af", "pan=mono|c0=0.5*c0-0.5*c1",
        "-ar", str(AUDIO_SAMPLE_RATE), "-ac", "1",
        "-f", "s16le", "-",
    ]
    try:
        proc = subprocess.run(cmd_side, capture_output=True, timeout=120)
    except subprocess.TimeoutExpired:
        return None
    if proc.returncode == 0 and len(proc.stdout) >= AUDIO_SAMPLE_RATE * 60 * 2:
        return proc.stdout
    cmd_mono = [
        _ffmpeg_exe(), "-hide_banner", "-loglevel", "error",
        "-i", input_path,
        "-ac", "1", "-ar", str(AUDIO_SAMPLE_RATE),
        "-f", "s16le", "-",
    ]
    try:
        proc = subprocess.run(cmd_mono, capture_output=True, timeout=120)
    except subprocess.TimeoutExpired:
        return None
    if proc.returncode != 0 or len(proc.stdout) == 0:
        return None
    return proc.stdout


def _smoothed_rms(np, raw_pcm: bytes):
    samples = np.frombuffer(raw_pcm, dtype="<i2").astype(np.float32)
    n_windows = len(samples) // RMS_WINDOW_SAMPLES
    if n_windows < 60:
        return None
    samples = samples[:n_windows * RMS_WINDOW_SAMPLES]
    framed = samples.reshape(n_windows, RMS_WINDOW_SAMPLES)
    rms = np.sqrt(np.mean(framed * framed, axis=1))
    if len(rms) < ROLLING_WINDOW_SAMPLES:
        return rms
    kernel = np.ones(ROLLING_WINDOW_SAMPLES, dtype=np.float32) / ROLLING_WINDOW_SAMPLES
    return np.convolve(rms, kernel, mode='same')


def _audio_anchor_times(np, smoothed, duration_ms: int):
    if smoothed is None or len(smoothed) < 60:
        return None, None
    n = len(smoothed)
    def ms_to_idx(ms): return max(0, min(n, int(ms / 100)))

    if duration_ms < SHORT_CONTENT_THRESHOLD_MS:
        s_lo = ms_to_idx(int(duration_ms * 0.05))
        s_hi = ms_to_idx(int(duration_ms * 0.20))
        e_lo = ms_to_idx(int(duration_ms * 0.80))
        e_hi = ms_to_idx(int(duration_ms * 0.95))
    else:
        s_lo = ms_to_idx(START_SKIP_MS)
        s_hi = ms_to_idx(START_SKIP_MS + START_HORIZON_MS)
        e_lo = ms_to_idx(duration_ms - END_SKIP_MS - END_HORIZON_MS)
        e_hi = ms_to_idx(duration_ms - END_SKIP_MS)

    s_hi = min(s_hi, n); e_hi = min(e_hi, n)
    if s_lo >= s_hi or e_lo >= e_hi:
        return None, None
    s_idx = s_lo + int(np.argmax(smoothed[s_lo:s_hi]))
    e_lo_eff = max(e_lo, s_idx + 600)
    if e_lo_eff >= e_hi:
        return None, None
    e_idx = e_lo_eff + int(np.argmax(smoothed[e_lo_eff:e_hi]))
    return s_idx * 100, e_idx * 100


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
        return {"version": 8, "anchors": []}
    try:
        import numpy as np
    except ImportError:
        return {"version": 8, "anchors": []}

    if progress: progress(0.0, "v8:audio-decode")
    raw_pcm = _decode_audio(input_path)
    if raw_pcm is None:
        print("censorcut.v8: audio decode failed", file=sys.stderr)
        return {"version": 8, "anchors": []}

    if progress: progress(0.30, "v8:rms")
    smoothed = _smoothed_rms(np, raw_pcm)
    if smoothed is None:
        return {"version": 8, "anchors": []}

    if progress: progress(0.40, "v8:audio-anchors")
    body_lo, body_hi = _audio_anchor_times(np, smoothed, duration_ms)
    if body_lo is None or body_hi is None or body_hi <= body_lo:
        print(f"censorcut.v8: could not find audio anchors", file=sys.stderr)
        return {"version": 8, "anchors": []}
    body_span = body_hi - body_lo

    dct = _make_dct_matrix(np, PHASH_RES)
    fractions = [i / (N_ANCHORS_VIDEO - 1) for i in range(N_ANCHORS_VIDEO)]

    anchors = []
    for idx, f in enumerate(fractions):
        if progress: progress(0.5 + 0.5 * idx / N_ANCHORS_VIDEO,
                              f"v8:anchor {idx + 1}/{N_ANCHORS_VIDEO}")
        t_target = int(body_lo + f * body_span)
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
        return {"version": 8, "anchors": []}

    digest_parts = [f"v8|span={body_span // SPAN_BUCKET_MS}"]
    for a in anchors:
        digest_parts.append(f"{int(round(float(a['tau']) / TAU_BUCKET)):05d}:{a['phash']}")
    digest = hashlib.sha256("|".join(digest_parts).encode()).hexdigest()

    if progress: progress(1.0, "v8:done")
    return {
        "version": 8, "type": "audio_anchored_scenecut",
        "durationMs": duration_ms,
        "approxDurationMin": int(round(duration_ms / 60000)),
        "bodyStartMs": body_lo,
        "bodyEndMs":   body_hi,
        "bodySpanMs":  body_span,
        "anchors": anchors,
        "digest": digest,
    }
