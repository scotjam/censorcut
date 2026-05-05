"""Whisper dialogue detector (M6).

Transcribes the source's audio with `faster-whisper` (CTranslate2 backend
— ~4x speed of openai-whisper at the same accuracy) and emits per-keyword
score series: 1.0 during a transcript segment whose text matches the
keyword (case-insensitive substring or word-boundary), 0.0 elsewhere.

Optional dependency: lazy-imports faster_whisper. Raises
:class:`WhisperUnavailable` if the package or model are missing —
analyze.py catches it and emits an audio-only result with a stderr note.

CUDA is used automatically when available. On a 5070 Ti running
PyTorch 2.5+ on CUDA 12.4+, large-v3 transcribes a 90-min movie in
~3-5 min. CPU fallback works but is slow with the larger models —
prefer 'small' or 'base' on CPU.
"""

from __future__ import annotations

import os
import re
import shutil
import subprocess
import tempfile
from pathlib import Path
from typing import Dict, Iterable, List, Optional

from .base import DetectorOutput, ProgressFn

# Output series period — 100 ms aligns with the audio detectors.
SERIES_PERIOD_MS = 100

DEFAULT_MODEL = "small"  # 470 MB; balanced for CPU and GPU
_PACKAGE_DIR = Path(__file__).parent.parent
_MODELS_DIR = _PACKAGE_DIR / "models" / "whisper"


class WhisperUnavailable(RuntimeError):
    """Raised when the dialogue detector can't run for an environmental
    reason (missing dep / model / ffmpeg)."""


# --------------------------------------------------------------------------
# Lazy imports
# --------------------------------------------------------------------------

def _import_faster_whisper():
    try:
        from faster_whisper import WhisperModel  # type: ignore
        return WhisperModel
    except ImportError as e:
        raise WhisperUnavailable(
            "faster-whisper not installed. Try: pip install faster-whisper") from e


def _ffmpeg_exe() -> str:
    p = shutil.which("ffmpeg")
    if not p:
        raise WhisperUnavailable("ffmpeg not found on PATH")
    return p


# --------------------------------------------------------------------------
# Model loader
# --------------------------------------------------------------------------

def _load_model(WhisperModel):
    """Load the configured Whisper model. faster-whisper auto-downloads
    from HuggingFace into its own cache; we override the cache dir so the
    files live under our repo and can be cleaned up alongside CLIP."""
    name = os.getenv("CENSORCUT_WHISPER_MODEL", DEFAULT_MODEL)
    cache_dir = Path(os.getenv("CENSORCUT_WHISPER_CACHE", str(_MODELS_DIR)))
    cache_dir.mkdir(parents=True, exist_ok=True)

    # Pick the best precision available. CTranslate2's int8_float16 on
    # CUDA gives near-fp16 quality at ~half the VRAM. CPU stays int8.
    device = "cuda"
    compute_type = "int8_float16"
    try:
        return WhisperModel(name, device=device, compute_type=compute_type,
                            download_root=str(cache_dir))
    except Exception:
        pass
    try:
        return WhisperModel(name, device="cuda", compute_type="float16",
                            download_root=str(cache_dir))
    except Exception:
        pass
    try:
        return WhisperModel(name, device="cpu", compute_type="int8",
                            download_root=str(cache_dir))
    except Exception as e:
        raise WhisperUnavailable(
            f"could not load Whisper model '{name}': {e}\n"
            f"Run: python -m censorcut.fetch_whisper") from None


# --------------------------------------------------------------------------
# Audio extraction
# --------------------------------------------------------------------------

def _extract_audio(input_path: str, dst_wav: str) -> None:
    """Pull audio out as 16 kHz mono PCM, which is what Whisper expects."""
    cmd = [_ffmpeg_exe(), "-y", "-hide_banner", "-loglevel", "error",
           "-i", input_path,
           "-vn",
           "-ac", "1",
           "-ar", "16000",
           "-f", "wav", dst_wav]
    subprocess.check_call(cmd)


# --------------------------------------------------------------------------
# Keyword matching
# --------------------------------------------------------------------------

def _compile_keyword_regex(keyword: str) -> re.Pattern:
    """Build a case-insensitive regex that matches the keyword as a word
    (or short phrase) inside transcript text. Phrases like 'I'll kill you'
    are matched as substrings; bare single words are matched on word
    boundaries to avoid 'kill' firing inside 'killer'."""
    is_phrase = " " in keyword.strip() or "'" in keyword
    pat = re.escape(keyword.strip())
    if not is_phrase:
        pat = rf"\b{pat}\b"
    return re.compile(pat, flags=re.IGNORECASE)


def _segments_to_keyword_series(segments: Iterable, keywords: List[str],
                                 duration_ms: int) -> Dict[str, List[float]]:
    """Walk Whisper segments and project keyword hits onto a per-100 ms
    grid spanning [0, duration_ms)."""
    n = max(1, duration_ms // SERIES_PERIOD_MS)
    series = {kw: [0.0] * n for kw in keywords}
    patterns = {kw: _compile_keyword_regex(kw) for kw in keywords}

    for seg in segments:
        start_ms = int((seg.start or 0.0) * 1000)
        end_ms   = int((seg.end   or 0.0) * 1000)
        text = seg.text or ""
        if end_ms <= start_ms:
            continue
        lo = max(0, start_ms // SERIES_PERIOD_MS)
        hi = min(n, end_ms   // SERIES_PERIOD_MS)
        if hi <= lo:
            continue

        for kw, pat in patterns.items():
            if not pat.search(text):
                continue
            for k in range(lo, hi):
                if 1.0 > series[kw][k]:
                    series[kw][k] = 1.0

    return series


# --------------------------------------------------------------------------
# Public entry point
# --------------------------------------------------------------------------

def run(input_path: str,
        duration_ms: int,
        keywords: List[str],
        progress: ProgressFn = None,
        language: Optional[str] = None) -> DetectorOutput:
    """Transcribe audio and emit one per-keyword series. Series keys are
    'dialogue.whisper.<keyword>'."""
    if not keywords:
        return {}

    WhisperModel = _import_faster_whisper()
    model = _load_model(WhisperModel)

    if progress:
        progress(0.50, "whisper:extract")

    with tempfile.TemporaryDirectory(prefix="censorcut_whisper_") as td:
        wav_path = str(Path(td) / "audio.wav")
        _extract_audio(input_path, wav_path)

        if progress:
            progress(0.55, "whisper:transcribe")

        # vad_filter trims silence so we transcribe less; word_timestamps
        # are nice-to-have for finer-grained matches but not used yet.
        segments, _info = model.transcribe(
            wav_path,
            language=language,
            vad_filter=True,
            beam_size=1,  # greedy; fastest, only minor quality loss
        )
        # The generator is consumed once — collect into a list so we can
        # emit progress as we go and still pass to the keyword pass.
        seg_list = []
        last_t = 0.0
        for seg in segments:
            seg_list.append(seg)
            if progress and duration_ms > 0:
                t_sec = (seg.end or 0.0)
                if t_sec - last_t > 5.0:
                    last_t = t_sec
                    frac = 0.55 + 0.40 * min(1.0, t_sec * 1000.0 / duration_ms)
                    progress(min(0.95, frac), "whisper:transcribe")

    if progress:
        progress(0.96, "whisper:keywords")

    series = _segments_to_keyword_series(seg_list, keywords, duration_ms)

    out: DetectorOutput = {}
    for kw, vals in series.items():
        out[f"dialogue.whisper.{kw}"] = {"period_ms": SERIES_PERIOD_MS,
                                          "values": vals}
    return out
