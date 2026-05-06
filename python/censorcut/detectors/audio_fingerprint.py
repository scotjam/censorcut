"""4-anchor audio fingerprint built from loud non-voice sounds.

Picks 2 anchors near the start and 2 near the end of the source — but
NOT in the first 5 minutes (skips intro logos / studio idents) or in the
last 5 minutes (skips end credits, where dialogue and music make
identification fragile). Each anchor is a loud LUFS spike where the
YAMNet "Speech" / "Conversation" / "Narration" probabilities are below
threshold (so we get sound effects, music stings, gunshots, glass-break
etc. rather than dialogue peaks).

Each anchor stores:
  tMs       — peak time in milliseconds from the start of the file
  peakLufs  — momentary LUFS at the peak
  sig       — 64-bit hex string: 16 log-frequency FFT bands of the
              ±0.5 s waveform around the peak, each quantized to 4 bits

The 4-tuple of signatures gives a content-derived identifier that
*does not reveal the title*. Two files match (same film, possibly
different intro/outro lengths) if at least 3 of 4 signatures pass a
Hamming-distance threshold AND the relative offsets of the matching
anchors are consistent (same affine mapping). Matching + alignment
logic lives on the C++ side (M8.3).
"""

from __future__ import annotations

import hashlib
import shutil
import subprocess
import tempfile
import wave
from pathlib import Path
from typing import Dict, List, Optional, Tuple

from .audio_loudness import _stream_ebur128
from .base import DetectorOutput, ProgressFn

# Time windows (relative to source duration) in which to look for anchors.
START_WINDOW_BEGIN_MS = 5  * 60 * 1000   #  5 min
START_WINDOW_END_MS   = 30 * 60 * 1000   # 30 min
END_TAIL_SKIP_MS      = 5  * 60 * 1000   # last 5 min skipped
END_WINDOW_BACK_MS    = 30 * 60 * 1000   # last 30 min

ANCHORS_PER_WINDOW = 2

# Speech filter — peaks dominated by voice are useless for matching
# (different dubs, different deliveries). YAMNet labels we treat as
# "speech-like".
SPEECH_LABELS = (
    "Speech",
    "Conversation",
    "Narration, monologue",
    "Male speech, man speaking",
    "Female speech, woman speaking",
    "Child speech, kid speaking",
    "Whispering",
)
# Threshold above which a frame is considered speech-dominated.
SPEECH_PROB_BLOCK = 0.20

# Spacing constraints so the two start/end anchors aren't right on top of
# each other. Helps the alignment math have something to constrain on.
MIN_ANCHOR_SPACING_MS = 60 * 1000  # 60 s

# Spectral-signature window around each peak.
SIG_WINDOW_SEC = 1.0


# --------------------------------------------------------------------------
# Helpers
# --------------------------------------------------------------------------

def _ffmpeg_exe() -> str:
    p = shutil.which("ffmpeg")
    if not p:
        raise RuntimeError("ffmpeg not found on PATH")
    return p


def _extract_audio_to_wav(input_path: str, dst_wav: str, sample_rate: int) -> None:
    """Decode the source's audio to a mono PCM 16-bit WAV at the given rate."""
    cmd = [_ffmpeg_exe(), "-y", "-hide_banner", "-loglevel", "error",
           "-i", input_path, "-vn", "-ac", "1", "-ar", str(sample_rate),
           "-f", "wav", dst_wav]
    subprocess.check_call(cmd)


def _read_wav_floats(path: str, np):
    with wave.open(path, "rb") as w:
        if w.getnchannels() != 1:
            raise RuntimeError("expected mono WAV")
        sr = w.getframerate()
        sw = w.getsampwidth()
        n  = w.getnframes()
        raw = w.readframes(n)
    if sw == 2:
        ints = np.frombuffer(raw, dtype="<i2")
        samples = ints.astype(np.float32) / 32768.0
    elif sw == 1:
        ints = np.frombuffer(raw, dtype=np.uint8)
        samples = (ints.astype(np.float32) - 128.0) / 128.0
    else:
        raise RuntimeError(f"unsupported sampwidth {sw}")
    return samples, sr


def _try_yamnet_speech_score(input_path: str, duration_ms: int):
    """Return (period_ms, list of speech-probability per frame) using
    YAMNet, OR (None, None) if YAMNet isn't available. We treat 'speech
    score' as the max over the speech-like labels at each frame."""
    try:
        from . import audio_label  # type: ignore  # noqa: WPS433
        out = audio_label.run(input_path, duration_ms=duration_ms,
                              labels=list(SPEECH_LABELS), progress=None)
    except audio_label.YamnetUnavailable:  # type: ignore[attr-defined]
        return None, None
    except Exception:
        return None, None

    period_ms = None
    n = 0
    series = []
    for label in SPEECH_LABELS:
        s = out.get(f"audio.yamnet.{label}")
        if not s: continue
        period_ms = s.get("period_ms", 480)
        vals = s.get("values", [])
        if not series:
            series = [0.0] * len(vals)
            n = len(vals)
        elif len(vals) != n:
            continue
        for i, v in enumerate(vals):
            if v > series[i]: series[i] = v
    if not series:
        return None, None
    return period_ms or 480, series


def _frame_is_speechy(t_ms: int, period_ms: int, speech: List[float]) -> bool:
    if not speech: return False
    idx = int(t_ms // period_ms)
    if idx < 0 or idx >= len(speech): return False
    return speech[idx] >= SPEECH_PROB_BLOCK


# --------------------------------------------------------------------------
# Anchor selection
# --------------------------------------------------------------------------

def _pick_top_anchors_in_window(momentary: List[float],
                                 lufs_period_ms: int,
                                 win_lo_ms: int, win_hi_ms: int,
                                 want: int,
                                 speech_period_ms: Optional[int],
                                 speech_series: Optional[List[float]]
                                 ) -> List[Tuple[int, float]]:
    """Return up to `want` (t_ms, lufs) anchors from inside the window,
    sorted by descending LUFS. Skips frames flagged as speech-heavy and
    frames within MIN_ANCHOR_SPACING_MS of an already-picked anchor."""
    if win_hi_ms <= win_lo_ms or not momentary:
        return []
    candidates: List[Tuple[int, float]] = []
    for i, lufs in enumerate(momentary):
        t_ms = i * lufs_period_ms
        if t_ms < win_lo_ms or t_ms >= win_hi_ms:
            continue
        if not _is_finite_lufs(lufs):
            continue
        if speech_period_ms is not None and speech_series is not None:
            if _frame_is_speechy(t_ms, speech_period_ms, speech_series):
                continue
        candidates.append((t_ms, lufs))
    candidates.sort(key=lambda p: p[1], reverse=True)

    chosen: List[Tuple[int, float]] = []
    for t_ms, lufs in candidates:
        if any(abs(t_ms - c[0]) < MIN_ANCHOR_SPACING_MS for c in chosen):
            continue
        chosen.append((t_ms, lufs))
        if len(chosen) >= want:
            break
    return chosen


def _is_finite_lufs(v: float) -> bool:
    return v != float("-inf") and v == v and v > -110.0


# --------------------------------------------------------------------------
# Spectral signature
# --------------------------------------------------------------------------

def _spectral_signature(np, samples, sample_rate: int, center_t_sec: float) -> str:
    """Compute a 64-bit signature over a SIG_WINDOW_SEC window centered
    on the given time. 16 logarithmic frequency bands, each quantized
    to 4 bits of band-energy magnitude relative to the band's max."""
    half = int(SIG_WINDOW_SEC * sample_rate * 0.5)
    centre = int(center_t_sec * sample_rate)
    lo = max(0, centre - half)
    hi = min(len(samples), centre + half)
    if hi - lo < 64:
        return "0" * 16  # too short, opaque sentinel

    win = samples[lo:hi]
    # Hann window to suppress spectral leakage.
    n = len(win)
    hann = 0.5 * (1.0 - np.cos(2.0 * np.pi * np.arange(n) / max(1, n - 1)))
    win = win * hann
    spec = np.abs(np.fft.rfft(win))
    if spec.size == 0:
        return "0" * 16
    # Power per logarithmic band (16 bands from ~50 Hz to Nyquist).
    nyq = sample_rate / 2.0
    edges = np.geomspace(50.0, nyq, num=17)
    freqs = np.linspace(0.0, nyq, num=spec.size)
    bands = np.zeros(16, dtype=np.float64)
    for b in range(16):
        mask = (freqs >= edges[b]) & (freqs < edges[b + 1])
        if mask.any():
            bands[b] = float(np.mean(spec[mask] ** 2))
    bmax = bands.max()
    if bmax <= 0:
        return "0" * 16
    norm = bands / bmax
    # 4 bits per band, quantized via log scale so loud bands aren't all
    # saturated. Map norm 0.0..1.0 -> 0..15.
    quant = np.clip(np.round(norm ** 0.5 * 15.0), 0, 15).astype(int)
    out = 0
    for b in range(16):
        out = (out << 4) | int(quant[b])
    return f"{out:016x}"


# --------------------------------------------------------------------------
# Public entry point
# --------------------------------------------------------------------------

def run(input_path: str,
        duration_ms: int,
        progress: ProgressFn = None) -> Dict[str, object]:
    """Compute the 4-anchor fingerprint. Returns a dict (not a
    DetectorOutput; this isn't a per-frame score series). On any
    failure to extract or analyze, returns ``{"anchors": []}`` so the
    caller can record "no fingerprint" without aborting analysis."""
    if duration_ms <= 0:
        return {"durationMs": duration_ms, "anchors": []}

    if progress: progress(0.0, "fingerprint:loudness")
    momentary: List[float] = []
    try:
        for _t, m, _s in _stream_ebur128(input_path, duration_ms, None):
            momentary.append(m)
    except Exception:
        return {"durationMs": duration_ms, "anchors": []}
    lufs_period_ms = 100  # ebur128 default

    if progress: progress(0.4, "fingerprint:speech")
    speech_period, speech_series = _try_yamnet_speech_score(input_path, duration_ms)

    # Anchor windows clamp to the file's duration.
    start_lo = START_WINDOW_BEGIN_MS
    start_hi = min(START_WINDOW_END_MS, duration_ms - END_TAIL_SKIP_MS)
    end_hi   = max(0, duration_ms - END_TAIL_SKIP_MS)
    end_lo   = max(start_hi, duration_ms - END_WINDOW_BACK_MS)

    if progress: progress(0.6, "fingerprint:select-anchors")
    start_anchors = _pick_top_anchors_in_window(
        momentary, lufs_period_ms, start_lo, start_hi,
        ANCHORS_PER_WINDOW, speech_period, speech_series)
    end_anchors = _pick_top_anchors_in_window(
        momentary, lufs_period_ms, end_lo, end_hi,
        ANCHORS_PER_WINDOW, speech_period, speech_series)
    anchor_times: List[Tuple[int, float]] = sorted(start_anchors + end_anchors,
                                                    key=lambda p: p[0])
    if not anchor_times:
        return {"durationMs": duration_ms, "anchors": []}

    if progress: progress(0.8, "fingerprint:signatures")
    try:
        import numpy as np  # type: ignore
    except ImportError:
        # Without numpy we skip the spectral signature but still record
        # the timing-only anchor data — useful as a weak fingerprint.
        anchors_out = [
            {"tMs": int(t), "peakLufs": round(float(lufs), 2), "sig": "0" * 16}
            for t, lufs in anchor_times
        ]
        return {"durationMs": duration_ms, "anchors": anchors_out}

    with tempfile.TemporaryDirectory(prefix="censorcut_fp_") as td:
        wav_path = str(Path(td) / "audio.wav")
        try:
            _extract_audio_to_wav(input_path, wav_path, sample_rate=22050)
            samples, sr = _read_wav_floats(wav_path, np)
        except Exception:
            return {"durationMs": duration_ms,
                    "anchors": [{"tMs": int(t),
                                 "peakLufs": round(float(lufs), 2),
                                 "sig": "0" * 16} for t, lufs in anchor_times]}

        anchors_out = []
        for t_ms, lufs in anchor_times:
            sig = _spectral_signature(np, samples, sr, t_ms / 1000.0)
            anchors_out.append({
                "tMs":      int(t_ms),
                "peakLufs": round(float(lufs), 2),
                "sig":      sig,
            })

    if progress: progress(1.0, "fingerprint:done")

    fp_id = hashlib.sha256(
        ("|".join(a["sig"] for a in anchors_out)).encode("utf-8")
    ).hexdigest()
    return {
        "durationMs":  duration_ms,
        "fingerprint": fp_id,
        "anchors":     anchors_out,
    }
