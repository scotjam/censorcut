"""YAMNet audio-label detector (M4).

Runs Google's YAMNet model on 16 kHz mono audio extracted from the source
video, scoring each ~0.48 s frame against AudioSet's 521 classes. Returns
a dict of per-label score series.

Optional dependency: this module imports lazily and raises
:class:`YamnetUnavailable` (a clean error) if any of the following are
missing:

  - tflite_runtime (preferred) or tensorflow (heavier fallback)
  - numpy
  - the yamnet.tflite model and yamnet_class_map.csv (run
    ``python -m censorcut.fetch_yamnet`` once)
  - ffmpeg on PATH

The host (analyze.py) catches :class:`YamnetUnavailable` and reports a
human-readable warning while still emitting M3-only suggestions.
"""

from __future__ import annotations

import csv
import os
import shutil
import struct
import subprocess
import tempfile
import wave
from pathlib import Path
from typing import Dict, List, Optional, Tuple

from .base import DetectorOutput, ProgressFn

# YAMNet's hop and window. The model expects waveform float32 in [-1, 1] at
# 16 kHz; outputs scores at frames every 0.48 s.
YAMNET_SR = 16000
YAMNET_FRAME_HOP_SEC = 0.48

_PACKAGE_DIR = Path(__file__).parent.parent
_MODEL_PATH = _PACKAGE_DIR / "models" / "yamnet.tflite"
_CLASSMAP_PATH = _PACKAGE_DIR / "models" / "yamnet_class_map.csv"


class YamnetUnavailable(RuntimeError):
    """Raised when YAMNet can't run for an environmental reason (missing
    dependency, missing model file, missing ffmpeg). Caller should log it
    and continue without YAMNet rather than treat as fatal."""


# --------------------------------------------------------------------------
# Model loader
# --------------------------------------------------------------------------

def _import_tflite():
    """Return (Interpreter, source_label) or raise YamnetUnavailable."""
    # Preferred: lightweight runtime (~10 MB).
    try:
        from tflite_runtime.interpreter import Interpreter  # type: ignore
        return Interpreter, "tflite_runtime"
    except ImportError:
        pass
    # Fallback: full TensorFlow's bundled lite interpreter (~600 MB install).
    try:
        import tensorflow as tf  # type: ignore
        return tf.lite.Interpreter, "tensorflow"
    except ImportError:
        pass
    raise YamnetUnavailable(
        "neither tflite-runtime nor tensorflow is installed. "
        "Try: pip install tflite-runtime numpy")


def _import_numpy():
    try:
        import numpy as np  # type: ignore
        return np
    except ImportError as e:
        raise YamnetUnavailable(f"numpy not installed: {e}") from None


def _model_path() -> Path:
    override = os.getenv("CENSORCUT_YAMNET_MODEL")
    if override and Path(override).is_file():
        return Path(override)
    if _MODEL_PATH.is_file():
        return _MODEL_PATH
    raise YamnetUnavailable(
        f"yamnet.tflite not found at {_MODEL_PATH}. "
        f"Run: python -m censorcut.fetch_yamnet")


def _classmap_path() -> Path:
    override = os.getenv("CENSORCUT_YAMNET_CLASSMAP")
    if override and Path(override).is_file():
        return Path(override)
    if _CLASSMAP_PATH.is_file():
        return _CLASSMAP_PATH
    raise YamnetUnavailable(
        f"yamnet_class_map.csv not found at {_CLASSMAP_PATH}. "
        f"Run: python -m censorcut.fetch_yamnet")


def load_class_map() -> List[str]:
    """Return the 521 display names indexed by class id."""
    rows: Dict[int, str] = {}
    with open(_classmap_path(), "r", encoding="utf-8") as f:
        reader = csv.DictReader(f)
        for r in reader:
            try:
                idx = int(r.get("index", -1))
            except (TypeError, ValueError):
                continue
            name = (r.get("display_name") or r.get("displayName") or "").strip()
            if idx >= 0 and name:
                rows[idx] = name
    if not rows:
        raise YamnetUnavailable("yamnet class map is empty or unreadable")
    n = max(rows) + 1
    out = [""] * n
    for k, v in rows.items():
        out[k] = v
    return out


def resolve_label_indices(class_names: List[str], wanted: List[str]) -> List[Tuple[str, int]]:
    """Map each user-supplied display name to its class index.

    Matching is case-insensitive on the first character, then exact. Returns
    pairs of (input-name, index). Inputs that don't resolve are silently
    dropped — this is a soft mismatch (e.g. someone typed 'Scream' but the
    model uses 'Screaming') rather than a hard error.
    """
    by_lower = {name.lower(): i for i, name in enumerate(class_names) if name}
    out: List[Tuple[str, int]] = []
    for w in wanted:
        if not w:
            continue
        idx = by_lower.get(w.lower())
        if idx is not None:
            out.append((w, idx))
    return out


# --------------------------------------------------------------------------
# Audio extraction
# --------------------------------------------------------------------------

def _ffmpeg_exe() -> str:
    p = shutil.which("ffmpeg")
    if not p:
        raise YamnetUnavailable("ffmpeg not found on PATH")
    return p


def _extract_audio_to_wav(input_path: str, dst_wav: str) -> None:
    """Decode the source's audio to a 16 kHz mono PCM 16-bit WAV file."""
    cmd = [_ffmpeg_exe(), "-y", "-hide_banner", "-loglevel", "error",
           "-i", input_path,
           "-vn",
           "-ac", "1",
           "-ar", str(YAMNET_SR),
           "-f", "wav",
           dst_wav]
    subprocess.check_call(cmd)


def _read_wav_as_floats(path: str, np):
    """Read a 16-bit PCM mono WAV at 16 kHz as a float32 numpy array in
    [-1, 1]. Uses the stdlib wave module so we don't pull in soundfile."""
    with wave.open(path, "rb") as w:
        if w.getnchannels() != 1 or w.getframerate() != YAMNET_SR:
            raise YamnetUnavailable(
                f"WAV must be mono 16 kHz, got "
                f"channels={w.getnchannels()} rate={w.getframerate()}")
        sampwidth = w.getsampwidth()
        n = w.getnframes()
        raw = w.readframes(n)
    if sampwidth == 2:
        ints = np.frombuffer(raw, dtype="<i2")
        return (ints.astype(np.float32) / 32768.0)
    if sampwidth == 1:
        ints = np.frombuffer(raw, dtype=np.uint8)
        return ((ints.astype(np.float32) - 128.0) / 128.0)
    raise YamnetUnavailable(f"unsupported PCM sampwidth: {sampwidth}")


# --------------------------------------------------------------------------
# Inference
# --------------------------------------------------------------------------

def _run_yamnet_inference(np, Interpreter, model_path: Path, audio):
    """Returns (T, 521) float numpy array of per-frame class scores."""
    interpreter = Interpreter(model_path=str(model_path))
    interpreter.resize_tensor_input(
        interpreter.get_input_details()[0]['index'],
        [len(audio)],
        strict=True)
    interpreter.allocate_tensors()
    inp = interpreter.get_input_details()[0]
    interpreter.set_tensor(inp['index'], audio.astype(np.float32))
    interpreter.invoke()
    # The model has three outputs: scores, embeddings, spectrogram.
    # We pick the rank-2 output with width 521 (scores) — the order is not
    # guaranteed across model variants.
    outputs = interpreter.get_output_details()
    scores = None
    for o in outputs:
        t = interpreter.get_tensor(o['index'])
        if t.ndim == 2 and t.shape[1] == 521:
            scores = t
            break
    if scores is None:
        raise YamnetUnavailable(
            "could not find a (T, 521) scores output in yamnet.tflite — "
            "this build of the model may not be the standard YAMNet")
    return scores


# --------------------------------------------------------------------------
# Public entry point
# --------------------------------------------------------------------------

def run(input_path: str,
        duration_ms: int,
        labels: List[str],
        progress: ProgressFn = None) -> DetectorOutput:
    """Run YAMNet over the source audio and return per-label score series.

    Returns one series per requested label (those that resolve to a class).
    Series keys are formatted as "audio.yamnet.<display_name>".
    """
    if not labels:
        return {}

    Interpreter, _flavour = _import_tflite()
    np = _import_numpy()
    class_names = load_class_map()
    label_to_idx = resolve_label_indices(class_names, labels)
    if not label_to_idx:
        return {}

    if progress:
        progress(0.42, "yamnet:extract")

    with tempfile.TemporaryDirectory(prefix="censorcut_yamnet_") as td:
        wav_path = str(Path(td) / "audio.wav")
        _extract_audio_to_wav(input_path, wav_path)
        if progress:
            progress(0.55, "yamnet:infer")
        audio = _read_wav_as_floats(wav_path, np)

    scores = _run_yamnet_inference(np, Interpreter, _model_path(), audio)

    if progress:
        progress(0.92, "yamnet:done")

    period_ms = int(round(YAMNET_FRAME_HOP_SEC * 1000))  # ~480
    out: DetectorOutput = {}
    for name, idx in label_to_idx:
        col = scores[:, idx].astype(np.float32).tolist()
        out[f"audio.yamnet.{name}"] = {"period_ms": period_ms, "values": col}
    return out
