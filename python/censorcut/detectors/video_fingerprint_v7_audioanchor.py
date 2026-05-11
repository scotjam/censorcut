"""v7 prototype: AUDIO-ANCHORED body, fixed-fraction sampling.

Body endpoints are derived from loud audio peaks, NOT from time
fractions of the file. Trim-tolerant because audio peaks are bit-
identical content events: a 22 sec intro trim shifts BOTH peaks by
22 sec, but body_span = end - start stays the same and the source
content at body fraction f is identical in both files.

Pipeline:
  1. ffmpeg → 8 kHz mono Side-channel (L−R) PCM
       -af "pan=mono|c0=0.5*c0-0.5*c1"
     For mono sources, fall back to plain mono (still OK for content
     anchoring, just less dub-resilient at the anchor moments).
  2. RMS at 100 ms windows → 5-second rolling mean → smoothed series.
  3. Loudest peak in [3 min, 15 min] of file       → body_start
     Loudest peak in [duration - 15, duration - 5] → body_end
     For very short content (< 30 min) scale these proportionally.
  4. Sample at 11 fractions (0.0, 0.1, ..., 1.0) within
     [body_start, body_end].
  5. For each fraction: seek + decode 1 frame, pHash.
  6. Digest = sha256("v7|body_span|<sequence>").

Speed: audio decode ~1-2 sec, then 11 video seeks at ~0.5 sec each.
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

# Audio
AUDIO_SAMPLE_RATE = 8000  # Hz
RMS_WINDOW_SAMPLES = 800  # 100 ms at 8 kHz
ROLLING_WINDOW_SAMPLES = 50  # 50 × 100 ms = 5 sec rolling mean

# Audio search windows.
START_SKIP_MS = 3 * 60 * 1000
START_HORIZON_MS = 15 * 60 * 1000
END_SKIP_MS = 5 * 60 * 1000
END_HORIZON_MS = 15 * 60 * 1000

# For short content, switch to proportional cushions.
SHORT_CONTENT_THRESHOLD_MS = 30 * 60 * 1000


def _ffmpeg_exe() -> str:
    p = shutil.which("ffmpeg")
    if not p:
        raise RuntimeError("ffmpeg not found")
    return p


def _decode_audio(input_path: str) -> Optional[bytes]:
    """Decode the input's audio to mono 8 kHz s16le. Tries the Side
    channel (L − R) first to suppress center-panned dialogue; falls
    back to plain mono if the Side channel produces silence (mono
    source) or the filter graph fails."""
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
    # Fallback: plain mono mix.
    cmd_mono = [
        _ffmpeg_exe(), "-hide_banner", "-loglevel", "error",
        "-i", input_path,
        "-ac", "1",
        "-ar", str(AUDIO_SAMPLE_RATE),
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
    """100 ms RMS, then 5-sec rolling mean. Returns 1D float32 array
    of length n_windows. Each entry corresponds to ms_offset =
    i * 100."""
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
    smoothed = np.convolve(rms, kernel, mode='same')
    return smoothed


def _audio_anchor_times(np, smoothed, duration_ms: int) -> Tuple[Optional[int], Optional[int]]:
    """Find the loudest peak (smoothed) in start and end search
    windows. Returns (start_t_ms, end_t_ms) or (None, None) if
    either window is empty / too short."""
    if smoothed is None or len(smoothed) < 60:
        return None, None
    n = len(smoothed)
    # Convert ms to 100-ms-bin indices.
    def ms_to_idx(ms): return max(0, min(n, int(ms / 100)))

    # Decide search ranges.
    if duration_ms < SHORT_CONTENT_THRESHOLD_MS:
        # Proportional skip + horizon.
        s_lo = ms_to_idx(int(duration_ms * 0.05))
        s_hi = ms_to_idx(int(duration_ms * 0.20))
        e_lo = ms_to_idx(int(duration_ms * 0.80))
        e_hi = ms_to_idx(int(duration_ms * 0.95))
    else:
        s_lo = ms_to_idx(START_SKIP_MS)
        s_hi = ms_to_idx(START_SKIP_MS + START_HORIZON_MS)
        e_lo = ms_to_idx(duration_ms - END_SKIP_MS - END_HORIZON_MS)
        e_hi = ms_to_idx(duration_ms - END_SKIP_MS)

    s_hi = min(s_hi, n)
    e_hi = min(e_hi, n)
    if s_lo >= s_hi or e_lo >= e_hi:
        return None, None

    s_idx = s_lo + int(np.argmax(smoothed[s_lo:s_hi]))
    # Ensure end window doesn't overlap start anchor.
    e_lo_eff = max(e_lo, s_idx + 600)  # at least 60 sec apart
    if e_lo_eff >= e_hi:
        return None, None
    e_idx = e_lo_eff + int(np.argmax(smoothed[e_lo_eff:e_hi]))

    return s_idx * 100, e_idx * 100


def _seek_decode_one_frame(path: str, t_ms: int) -> Optional[bytes]:
    cmd = [
        _ffmpeg_exe(), "-hide_banner", "-loglevel", "error",
        "-hwaccel", "auto",
        "-ss", f"{t_ms / 1000.0:.3f}",
        "-i", path, "-an", "-sn", "-dn",
        "-frames:v", "1",
        "-vf", f"scale={PHASH_RES}:{PHASH_RES},format=gray",
        "-f", "rawvideo", "-",
    ]
    try:
        proc = subprocess.run(cmd, capture_output=True, timeout=30)
    except subprocess.TimeoutExpired:
        return None
    if proc.returncode != 0:
        return None
    fs = PHASH_RES * PHASH_RES
    if len(proc.stdout) < fs:
        return None
    return proc.stdout[:fs]


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


def run(input_path: str, duration_ms: int, progress=None) -> Dict[str, object]:
    if duration_ms <= 0:
        return {"version": 7, "anchors": []}
    try:
        import numpy as np
    except ImportError:
        return {"version": 7, "anchors": []}

    if progress: progress(0.0, "v7:audio-decode")
    raw_pcm = _decode_audio(input_path)
    if raw_pcm is None:
        print("censorcut.v7: audio decode failed", file=sys.stderr)
        return {"version": 7, "anchors": []}

    if progress: progress(0.30, "v7:rms")
    smoothed = _smoothed_rms(np, raw_pcm)
    if smoothed is None:
        return {"version": 7, "anchors": []}

    if progress: progress(0.40, "v7:audio-anchors")
    body_lo, body_hi = _audio_anchor_times(np, smoothed, duration_ms)
    if body_lo is None or body_hi is None or body_hi <= body_lo:
        print(f"censorcut.v7: could not find audio anchors "
              f"(dur={duration_ms} ms)", file=sys.stderr)
        return {"version": 7, "anchors": []}

    body_span = body_hi - body_lo
    dct = _make_dct_matrix(np, PHASH_RES)
    fractions = [i / (N_ANCHORS_VIDEO - 1) for i in range(N_ANCHORS_VIDEO)]

    anchors = []
    for idx, f in enumerate(fractions):
        if progress: progress(0.5 + 0.5 * idx / N_ANCHORS_VIDEO,
                              f"v7:anchor {idx + 1}/{N_ANCHORS_VIDEO}")
        t_ms = int(body_lo + f * body_span)
        fb = _seek_decode_one_frame(input_path, t_ms)
        if fb is None:
            continue
        ph = _phash(np, dct, fb)
        anchors.append({
            "tau": round(f, 6),
            "phash": f"{ph:0{PHASH_HEX_CHARS}x}",
            "tMs": t_ms,
        })

    if len(anchors) < 2:
        return {"version": 7, "anchors": []}

    # Bucket body span (rounded to second) into the digest. This captures
    # the content-anchored body length, which is invariant to trim/encode.
    digest_parts = [f"v7|span={body_span // SPAN_BUCKET_MS}"]
    for a in anchors:
        digest_parts.append(f"{int(round(float(a['tau']) / TAU_BUCKET)):05d}:{a['phash']}")
    digest = hashlib.sha256("|".join(digest_parts).encode()).hexdigest()

    if progress: progress(1.0, "v7:done")
    return {
        "version": 7, "type": "audio_anchored",
        "durationMs": duration_ms,
        "approxDurationMin": int(round(duration_ms / 60000)),
        "bodyStartMs": body_lo,
        "bodyEndMs":   body_hi,
        "bodySpanMs":  body_span,
        "anchors": anchors,
        "digest": digest,
    }
