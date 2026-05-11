"""v9 prototype: AUDIO-PEAK GAP fingerprint + frame pHash at peak times.

The fingerprint is built from the PATTERN of inter-peak gaps. Each
audio peak is a content event (loud non-voice sound — gunshot, music
sting, glass break, dramatic chord) that lives at a stable point
inside the source content, regardless of how the file is trimmed.

Trim-tolerance argument:
  Original  audio peaks at content-times: P1, P2, P3, ..., PK.
  Trimmed   audio peaks at content-times: P1, P2, P3, ..., PK
            (same events; their FILE-times shift by the trim
            amount, but the events themselves are content).

  Inter-peak gaps in original:  P2-P1, P3-P2, ..., PK-P(K-1)
  Inter-peak gaps in trimmed:   IDENTICAL (every Pi shifted by
                                 the same trim, gaps cancel).

So the gap sequence is invariant to head OR tail trim. If the trim
removes a peak entirely (e.g., a peak that lived in the trimmed
region), the trimmed file is missing one peak — its gap sequence
will be one shorter, and the digest differs. We mitigate this by
picking peaks from the BODY only (skipping the first/last 5% of
the file), under the assumption that 5% trim is the realistic
upper bound for intro/outro adjustments.

Pipeline:
  1. ffmpeg → 8 kHz mono Side-channel PCM (M&E-leaning, dub-resilient).
     Falls back to plain mono for mono sources.
  2. RMS at 100 ms windows, 5-second rolling mean for smoothing.
  3. Within the body region (5%–95% of the file), greedy-pick the
     top-K loudest peaks with ≥30 sec spacing.
  4. Sort peaks by time.
  5. For each peak, seek the video to that timestamp, decode 1
     frame at 32×32 grayscale, compute pHash.
  6. Compute consecutive inter-peak gaps (K-1 gaps).
  7. Digest = sha256("v9|<bucketed-gaps>|<pHash sequence>").

Cross-encode invariance: same audio bytes → same RMS → same peaks
→ same gaps + same pHashes. ✓
Trim tolerance (intro and/or outro): peaks in surviving body
content stay at the same content times → gaps preserved. ✓
Different cuts: removed body scenes drop their peaks AND change
the surrounding gap pattern → different digest. ✓
Different films: different peaks → different digest. ✓
"""

from __future__ import annotations

import shutil
import subprocess
import sys
from typing import Dict, List, Optional, Tuple


# ---------------------------------------------------------------------
# Tunables
# ---------------------------------------------------------------------

TOP_K_PEAKS         = 25        # number of audio peaks to anchor on
                                  # (was 15; more redundancy means a few
                                  # ranking-borderline peaks shuffling
                                  # in/out across encodes affect a smaller
                                  # fraction of the fingerprint)
MIN_PEAK_SPACING_MS = 30 * 1000 # min 30 sec between picked peaks
GAP_BUCKET_MS       = 1000      # 1-sec gap quantization

# Per-peak pHash robustness: instead of single-frame pHash at the peak
# time, decode N frames evenly spread over a short window centered on
# the peak and average them before DCT/median thresholding. Averaging
# smooths motion-blur jitter and encoder quantization noise — same
# content under different encoders produces much closer pHashes.
PHASH_FRAMES_PER_PEAK   = 5
PHASH_FRAME_WINDOW_MS   = 1000  # ±500 ms around the peak time

# Body region for peak search — proportional, leaves 5% on each side
# for intro/credits.
PEAK_SEARCH_LO_FRAC = 0.05
PEAK_SEARCH_HI_FRAC = 0.95

# Audio decode
AUDIO_SAMPLE_RATE      = 8000
RMS_WINDOW_SAMPLES     = 800   # 100 ms at 8 kHz
ROLLING_WINDOW_SAMPLES = 50    # 5 sec rolling mean

# Video pHash
PHASH_RES         = 32
PHASH_DCT_KEEP    = 8
PHASH_HEX_CHARS   = 16


# ---------------------------------------------------------------------
# ffmpeg + pHash helpers
# ---------------------------------------------------------------------

def _ffmpeg_exe() -> str:
    p = shutil.which("ffmpeg")
    if not p:
        raise RuntimeError("ffmpeg not found")
    return p


AUDIO_SAMPLE_WINDOWS = 5         # number of audio windows to extract
AUDIO_SAMPLE_WINDOW_SEC = 300    # 5 min each → 25 min total audio
AUDIO_PER_WINDOW_TIMEOUT = 300   # 5 min per ffmpeg subprocess


def _decode_audio_window(input_path: str,
                          start_ms: int,
                          dur_ms: int) -> Optional[bytes]:
    """Decode a single contiguous audio window via container-level seek.

    Container seek (`-ss BEFORE -i`) jumps to a byte offset in the
    file's index, so ffmpeg only reads the bytes covering the
    requested window — not the whole file. Order-of-magnitude less
    I/O than full audio decode for multi-GB sources on network drives.
    """
    cmd_side = [
        _ffmpeg_exe(), "-hide_banner", "-loglevel", "error",
        "-ss", f"{start_ms / 1000.0:.3f}",
        "-vn", "-sn", "-dn",
        "-i", input_path,
        "-t", f"{dur_ms / 1000.0:.3f}",
        "-af", "pan=mono|c0=0.5*c0-0.5*c1",
        "-ar", str(AUDIO_SAMPLE_RATE), "-ac", "1",
        "-f", "s16le", "-",
    ]
    try:
        proc = subprocess.run(cmd_side, capture_output=True,
                              timeout=AUDIO_PER_WINDOW_TIMEOUT)
    except subprocess.TimeoutExpired:
        return None
    # Need at least 30 sec of audio data (8000 Hz * 30 sec * 2 bytes)
    # for the rolling-mean smoothing to be meaningful in this window.
    min_useful = AUDIO_SAMPLE_RATE * 30 * 2
    if proc.returncode == 0 and len(proc.stdout) >= min_useful:
        return proc.stdout

    # Side channel rejected (mono / unusual layout). Fall back to mono.
    cmd_mono = [
        _ffmpeg_exe(), "-hide_banner", "-loglevel", "error",
        "-ss", f"{start_ms / 1000.0:.3f}",
        "-vn", "-sn", "-dn",
        "-i", input_path,
        "-t", f"{dur_ms / 1000.0:.3f}",
        "-ac", "1", "-ar", str(AUDIO_SAMPLE_RATE),
        "-f", "s16le", "-",
    ]
    try:
        proc = subprocess.run(cmd_mono, capture_output=True,
                              timeout=AUDIO_PER_WINDOW_TIMEOUT)
    except subprocess.TimeoutExpired:
        return None
    if proc.returncode != 0 or len(proc.stdout) < min_useful:
        err = (proc.stderr or b"").decode("utf-8", "replace").strip()
        if err:
            print(f"censorcut.v9: window decode failed at {start_ms}ms "
                  f"({len(proc.stdout)} bytes, rc={proc.returncode}): "
                  f"{err[:200]}", file=sys.stderr)
        return None
    return proc.stdout


def _decode_audio(input_path: str) -> Optional[bytes]:
    """LEGACY full-audio decode. Kept for API back-compat with calls
    that may still expect the whole audio. Most paths should use
    _decode_audio_window via the sampled flow in run().

    Timeout is generous (10 min) because reading multi-GB files over
    SMB shares can take 60-120 sec just for I/O, and the demuxer for
    interleaved MKV/MP4 has to traverse most of the file to find all
    audio packets — independent of how small the resampled output is.
    """
    cmd_side = [
        _ffmpeg_exe(), "-hide_banner", "-loglevel", "error",
        "-vn", "-sn", "-dn",
        "-i", input_path,
        "-af", "pan=mono|c0=0.5*c0-0.5*c1",
        "-ar", str(AUDIO_SAMPLE_RATE), "-ac", "1",
        "-f", "s16le", "-",
    ]
    try:
        proc = subprocess.run(cmd_side, capture_output=True, timeout=1800)
    except subprocess.TimeoutExpired:
        print(f"censorcut.v9: side-channel audio decode timed out (>30 min) "
              f"on {input_path}", file=sys.stderr)
        return None
    if proc.returncode == 0 and len(proc.stdout) >= AUDIO_SAMPLE_RATE * 60 * 2:
        return proc.stdout
    # Side-channel decode failed or produced too little output (mono
    # source, exotic channel layout, codec mismatch with the pan
    # filter). Surface stderr so we can see the actual reason rather
    # than guessing.
    side_err = (proc.stderr or b"").decode("utf-8", "replace").strip()
    if side_err:
        print(f"censorcut.v9: side-channel decode rejected "
              f"({len(proc.stdout)} bytes, rc={proc.returncode}): "
              f"{side_err[:300]}", file=sys.stderr)

    cmd_mono = [
        _ffmpeg_exe(), "-hide_banner", "-loglevel", "error",
        "-vn", "-sn", "-dn",
        "-i", input_path,
        "-ac", "1", "-ar", str(AUDIO_SAMPLE_RATE),
        "-f", "s16le", "-",
    ]
    try:
        proc = subprocess.run(cmd_mono, capture_output=True, timeout=1800)
    except subprocess.TimeoutExpired:
        print(f"censorcut.v9: mono fallback audio decode timed out (>30 min) "
              f"on {input_path}", file=sys.stderr)
        return None
    if proc.returncode != 0 or len(proc.stdout) == 0:
        mono_err = (proc.stderr or b"").decode("utf-8", "replace").strip()
        print(f"censorcut.v9: mono fallback failed "
              f"({len(proc.stdout)} bytes, rc={proc.returncode}): "
              f"{mono_err[:300]}", file=sys.stderr)
        return None
    return proc.stdout


def _smoothed_rms(np, raw_pcm: bytes):
    samples = np.frombuffer(raw_pcm, dtype="<i2").astype(np.float32)
    n = len(samples) // RMS_WINDOW_SAMPLES
    if n < 60:
        return None
    framed = samples[:n * RMS_WINDOW_SAMPLES].reshape(n, RMS_WINDOW_SAMPLES)
    rms = np.sqrt(np.mean(framed * framed, axis=1))
    if len(rms) < ROLLING_WINDOW_SAMPLES:
        return rms
    kernel = np.ones(ROLLING_WINDOW_SAMPLES, dtype=np.float32) / ROLLING_WINDOW_SAMPLES
    return np.convolve(rms, kernel, mode='same')


def _pick_top_peaks(np, smoothed, lo_idx: int, hi_idx: int,
                    k: int, spacing_idx: int) -> List[int]:
    """Greedy non-maximum suppression: pick the loudest sample in
    [lo, hi), mask ±spacing around it, repeat until k peaks selected
    or no candidates remain. Returns indices into `smoothed` sorted
    ASC by time."""
    if hi_idx <= lo_idx:
        return []
    work = np.array(smoothed[lo_idx:hi_idx], copy=True)
    picks: List[int] = []
    # Use -inf as sentinel for masked-out regions.
    minus_inf = np.float32(-1.0)
    for _ in range(k):
        if not np.isfinite(work).any() or work.max() <= minus_inf:
            break
        local_idx = int(np.argmax(work))
        global_idx = lo_idx + local_idx
        picks.append(global_idx)
        # Mask ±spacing.
        m_lo = max(0, local_idx - spacing_idx)
        m_hi = min(len(work), local_idx + spacing_idx + 1)
        work[m_lo:m_hi] = minus_inf
    picks.sort()
    return picks


def _seek_decode_n_frames(path: str, center_ms: int, window_ms: int,
                           n_frames: int) -> List[bytes]:
    """Decode N evenly-spaced frames over [center - window/2, center +
    window/2). Used to build an averaged pHash that's robust to motion
    blur and codec quantization at the peak's exact moment."""
    start_ms = max(0, center_ms - window_ms // 2)
    fps = max(1, int(round(n_frames * 1000 / window_ms)))
    cmd = [
        _ffmpeg_exe(), "-hide_banner", "-loglevel", "error",
        "-hwaccel", "auto",
        "-ss", f"{start_ms / 1000.0:.3f}",
        "-i", path, "-an", "-sn", "-dn",
        "-t", f"{window_ms / 1000.0:.3f}",
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
    for i in range(min(n_frames, len(proc.stdout) // fs)):
        out.append(proc.stdout[i * fs:(i + 1) * fs])
    return out


def _averaged_phash(np, dct, frame_bytes_list: List[bytes]) -> Optional[int]:
    """Compute a 64-bit pHash from the per-pixel mean of multiple frames.
    Averaging in pixel space before the DCT / median threshold smooths
    motion-blur noise and codec quantization differences that hit single-
    frame pHashes hard at audio peaks (which often coincide with motion
    / action moments)."""
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


# ---------------------------------------------------------------------
# Public entry point
# ---------------------------------------------------------------------

def run(input_path: str, duration_ms: int, progress=None) -> Dict[str, object]:
    if duration_ms <= 0:
        return {"version": 9, "anchors": []}
    try:
        import numpy as np
    except ImportError:
        return {"version": 9, "anchors": []}

    if progress: progress(0.0, "v9:audio-decode")
    raw_pcm = _decode_audio(input_path)
    if raw_pcm is None:
        print("censorcut.v9: audio decode failed", file=sys.stderr)
        return {"version": 9, "anchors": []}

    if progress: progress(0.30, "v9:rms+smooth")
    smoothed = _smoothed_rms(np, raw_pcm)
    if smoothed is None or len(smoothed) < 30:
        return {"version": 9, "anchors": []}

    n = len(smoothed)
    def ms_to_idx(ms): return max(0, min(n, int(ms / 100)))
    spacing_idx = max(1, MIN_PEAK_SPACING_MS // 100)
    lo = ms_to_idx(int(duration_ms * PEAK_SEARCH_LO_FRAC))
    hi = ms_to_idx(int(duration_ms * PEAK_SEARCH_HI_FRAC))

    if progress: progress(0.40, "v9:pick-peaks")
    peak_indices = _pick_top_peaks(np, smoothed, lo, hi,
                                     TOP_K_PEAKS, spacing_idx)
    if len(peak_indices) < 2:
        print(f"censorcut.v9: only {len(peak_indices)} peak(s) found",
              file=sys.stderr)
        return {"version": 9, "anchors": []}
    peak_times_ms = [int(idx * 100) for idx in peak_indices]

    # Decode an averaged pHash at each peak time. Averaging across
    # PHASH_FRAMES_PER_PEAK frames in PHASH_FRAME_WINDOW_MS smooths
    # motion-blur and encoder quantization differences.
    dct = _make_dct_matrix(np, PHASH_RES)
    anchors: List[Dict[str, object]] = []
    for i, t_ms in enumerate(peak_times_ms):
        if progress: progress(0.5 + 0.4 * i / len(peak_times_ms),
                              f"v9:phash {i + 1}/{len(peak_times_ms)}")
        frames = _seek_decode_n_frames(input_path, t_ms,
                                        PHASH_FRAME_WINDOW_MS,
                                        PHASH_FRAMES_PER_PEAK)
        ph = _averaged_phash(np, dct, frames)
        if ph is None:
            anchors.append({"tMs": t_ms, "phash": None})
            continue
        anchors.append({"tMs": t_ms, "phash": f"{ph:0{PHASH_HEX_CHARS}x}"})

    # Inter-peak gaps. The PRIMARY discriminator.
    gaps_ms = [peak_times_ms[i + 1] - peak_times_ms[i]
               for i in range(len(peak_times_ms) - 1)]
    if progress: progress(1.0, "v9:done")
    return {
        "version": 9, "type": "peak_gaps",
        "durationMs": duration_ms,
        # No exact digest. Approximate matching uses the raw peaks
        # + pHashes (see match_fingerprints below). The server-side
        # index of choice is `inner_span_ms` — the gap between the
        # 2nd and 2nd-to-last peaks, which survives losing one or
        # two peaks at either edge from intro/outro trim.
        "innerSpanMs": (peak_times_ms[-2] - peak_times_ms[1]
                        if len(peak_times_ms) >= 4 else 0),
        "peakCount": len(peak_times_ms),
        "gapsMs": gaps_ms,
        "peaks": [{"tMs": pt, "phash": (a.get("phash"))}
                  for pt, a in zip(peak_times_ms, anchors)],
    }


# ---------------------------------------------------------------------
# Approximate matching (replaces exact-digest lookup)
# ---------------------------------------------------------------------

# Permissive defaults. Sub-second peak-time jitter from audio re-encoding
# is normal; 2-3 sec tolerance per gap easily absorbs that. pHash Hamming
# distances of 8-16 are typical for the same content under different
# encoders; 16 is permissive but well below the ~32-bit "completely
# different content" baseline.
DEFAULT_GAP_TOL_MS               = 5000  # ±5 sec per gap
DEFAULT_PHASH_HAMMING_MAX        = 20    # out of 64 bits
DEFAULT_MIN_GAP_MATCH_FRACTION   = 0.55
DEFAULT_MIN_PHASH_MATCH_FRACTION = 0.4
DEFAULT_PEAK_COUNT_TOLERANCE     = 4      # tolerate ±4 missing peaks
                                            # (was 2; with K=25 we have
                                            # more room for ranking
                                            # shuffles at the bottom)
DEFAULT_INNER_SPAN_TOL_MS        = 5000   # ±5 sec for the indexable inner span


def _hex_hamming(a: Optional[str], b: Optional[str]) -> int:
    if not a or not b or len(a) != len(b):
        return 64
    try:
        return bin(int(a, 16) ^ int(b, 16)).count("1")
    except ValueError:
        return 64


def match_fingerprints(fp_a: dict, fp_b: dict,
                        gap_tol_ms: int = DEFAULT_GAP_TOL_MS,
                        phash_hamming_max: int = DEFAULT_PHASH_HAMMING_MAX,
                        min_gap_frac: float = DEFAULT_MIN_GAP_MATCH_FRACTION,
                        min_phash_frac: float = DEFAULT_MIN_PHASH_MATCH_FRACTION,
                        peak_count_tol: int = DEFAULT_PEAK_COUNT_TOLERANCE
                        ) -> Dict[str, object]:
    """Compare two v9 fingerprints with permissive tolerances.

    Returns a verdict dict:
      {
        "isSameFilm":      bool,
        "matchedGaps":     int,  out of total compared
        "totalGaps":       int,
        "matchedPHashes":  int,  out of compared
        "totalPHashes":    int,
        "estimatedTrimMs": int,  median (peak_b.tMs - peak_a.tMs) over
                                  matched pairs — useful for transferring
                                  cut times from one file's timeline to
                                  the other.
        "reason":          str,  human-readable diagnostic
      }

    Pairing strategy:
      Both files are assumed to have selected the same K peaks (audio
      content events). With intro/outro trim the peaks shift uniformly,
      so 1-to-1 index pairing works. If peak counts differ by more
      than `peak_count_tol`, declare not-same-film.
    """
    peaks_a = fp_a.get("peaks") or []
    peaks_b = fp_b.get("peaks") or []
    n_a, n_b = len(peaks_a), len(peaks_b)

    if n_a < 4 or n_b < 4:
        return {
            "isSameFilm": False, "matchedGaps": 0, "totalGaps": 0,
            "matchedPHashes": 0, "totalPHashes": 0, "estimatedTrimMs": 0,
            "reason": f"insufficient peaks (a={n_a}, b={n_b})",
        }

    if abs(n_a - n_b) > peak_count_tol:
        return {
            "isSameFilm": False, "matchedGaps": 0, "totalGaps": 0,
            "matchedPHashes": 0, "totalPHashes": 0, "estimatedTrimMs": 0,
            "reason": f"peak counts too different (a={n_a}, b={n_b})",
        }

    # 1-to-1 pair by index, comparing only the overlapping prefix.
    n = min(n_a, n_b)
    pa = peaks_a[:n]
    pb = peaks_b[:n]

    # Estimate trim offset = median(b - a) across paired peaks.
    diffs = [pb[i]["tMs"] - pa[i]["tMs"] for i in range(n)]
    diffs_sorted = sorted(diffs)
    estimated_trim = diffs_sorted[n // 2]

    # pHash agreement.
    matched_phashes = 0
    total_phashes = 0
    for i in range(n):
        ha = pa[i].get("phash")
        hb = pb[i].get("phash")
        if not ha or not hb:
            continue
        total_phashes += 1
        if _hex_hamming(ha, hb) <= phash_hamming_max:
            matched_phashes += 1

    # Gap agreement: a's gaps vs b's gaps.
    gaps_a = [pa[i + 1]["tMs"] - pa[i]["tMs"] for i in range(n - 1)]
    gaps_b = [pb[i + 1]["tMs"] - pb[i]["tMs"] for i in range(n - 1)]
    matched_gaps = sum(1 for ga, gb in zip(gaps_a, gaps_b)
                       if abs(ga - gb) <= gap_tol_ms)
    total_gaps = len(gaps_a)

    gap_frac = matched_gaps / total_gaps if total_gaps else 0.0
    phash_frac = matched_phashes / total_phashes if total_phashes else 0.0
    same = (gap_frac >= min_gap_frac) and (phash_frac >= min_phash_frac)

    return {
        "isSameFilm":      same,
        "matchedGaps":     matched_gaps,
        "totalGaps":       total_gaps,
        "matchedPHashes":  matched_phashes,
        "totalPHashes":    total_phashes,
        "estimatedTrimMs": estimated_trim,
        "reason": (f"gaps {matched_gaps}/{total_gaps} ({gap_frac:.0%}), "
                    f"phashes {matched_phashes}/{total_phashes} "
                    f"({phash_frac:.0%}), "
                    f"trim≈{estimated_trim} ms"),
    }
