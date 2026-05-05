"""CLIP/SigLIP vision detector (M5).

Samples 1 fps frames from the source video via ffmpeg, then scores each
frame against the union of text prompts referenced by enabled categories.
Returns one float series per prompt at a 1 s sampling period.

Optional dependency: lazy-imports torch / open_clip_torch / Pillow.
Raises :class:`ClipUnavailable` if any of those, the model, or ffmpeg
are missing — analyze.py catches it and emits the audio-only result with
a stderr note.

CUDA is used automatically when a CUDA-capable GPU is present (e.g. an
RTX 5070 Ti running PyTorch 2.5+ on CUDA 12.4+); otherwise CPU.
"""

from __future__ import annotations

import io
import os
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Dict, List, Optional, Tuple

from .base import DetectorOutput, ProgressFn

# 1 fps sampling — produces ~5400 frames for a 90 min movie.
SAMPLE_FPS = 1
FRAME_PERIOD_MS = 1000

# Model defaults. open_clip_torch supports many architectures; this one
# is the sweet spot for accuracy + speed on a recent GPU.
DEFAULT_MODEL = "ViT-L-14"
DEFAULT_PRETRAINED = "openai"

_PACKAGE_DIR = Path(__file__).parent.parent
_MODELS_DIR = _PACKAGE_DIR / "models" / "clip"


class ClipUnavailable(RuntimeError):
    """Raised when the CLIP detector cannot run for an environmental
    reason. Caller should log it and skip the vision pass."""


# --------------------------------------------------------------------------
# Lazy imports
# --------------------------------------------------------------------------

def _import_torch_and_clip():
    """Return (torch, open_clip) or raise ClipUnavailable."""
    try:
        import torch  # type: ignore
    except ImportError as e:
        raise ClipUnavailable(
            "PyTorch not installed. For an NVIDIA GPU, try:\n"
            "  pip install torch --index-url https://download.pytorch.org/whl/cu124\n"
            "For CPU-only:\n"
            "  pip install torch") from e
    try:
        import open_clip  # type: ignore
    except ImportError as e:
        raise ClipUnavailable(
            "open_clip_torch not installed. Try: pip install open_clip_torch") from e
    return torch, open_clip


def _import_pil():
    try:
        from PIL import Image  # type: ignore
        return Image
    except ImportError as e:
        raise ClipUnavailable(
            "Pillow not installed. Try: pip install Pillow") from e


def _ffmpeg_exe() -> str:
    p = shutil.which("ffmpeg")
    if not p:
        raise ClipUnavailable("ffmpeg not found on PATH")
    return p


# --------------------------------------------------------------------------
# Model loader
# --------------------------------------------------------------------------

def _load_model(torch, open_clip):
    """Load the CLIP model + tokenizer + preprocess. Looks in the local
    cache dir first; if not present, allows open_clip to download to the
    same cache directory."""
    model_name = os.getenv("CENSORCUT_CLIP_MODEL", DEFAULT_MODEL)
    pretrained = os.getenv("CENSORCUT_CLIP_PRETRAINED", DEFAULT_PRETRAINED)

    cache_dir = Path(os.getenv("CENSORCUT_CLIP_CACHE", str(_MODELS_DIR)))
    cache_dir.mkdir(parents=True, exist_ok=True)

    device = "cuda" if torch.cuda.is_available() else "cpu"

    try:
        model, _, preprocess = open_clip.create_model_and_transforms(
            model_name, pretrained=pretrained, cache_dir=str(cache_dir))
    except Exception as e:
        raise ClipUnavailable(
            f"could not load CLIP model {model_name}/{pretrained}. "
            f"Run: python -m censorcut.fetch_clip\n({e})") from None

    tokenizer = open_clip.get_tokenizer(model_name)
    model.to(device).eval()
    return model, preprocess, tokenizer, device


# --------------------------------------------------------------------------
# Frame extraction
# --------------------------------------------------------------------------

def _stream_frames(input_path: str, duration_ms: int):
    """Yield (t_ms, JPEG bytes) tuples at SAMPLE_FPS."""
    # Pipe individual JPEG frames out of ffmpeg via image2pipe so we never
    # write to disk. Quality 4 keeps them sharp without ballooning IO.
    cmd = [_ffmpeg_exe(), "-nostats", "-hide_banner", "-loglevel", "error",
           "-i", input_path,
           "-an",
           "-vf", f"fps={SAMPLE_FPS}",
           "-q:v", "4",
           "-f", "image2pipe", "-vcodec", "mjpeg", "-"]
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    try:
        assert proc.stdout is not None
        buf = b""
        idx = 0
        # JPEG SOI/EOI scan — frames in image2pipe stream are concatenated
        # JPEGs back-to-back.
        SOI = b"\xff\xd8"
        EOI = b"\xff\xd9"
        while True:
            chunk = proc.stdout.read(64 * 1024)
            if not chunk and not buf:
                break
            buf += chunk
            while True:
                start = buf.find(SOI)
                if start < 0:
                    if not chunk:
                        return
                    break
                end = buf.find(EOI, start + 2)
                if end < 0:
                    if not chunk:
                        return
                    break
                jpg = buf[start : end + 2]
                buf = buf[end + 2:]
                yield (idx * FRAME_PERIOD_MS, jpg)
                idx += 1
    finally:
        proc.kill()
        proc.wait(timeout=5)


# --------------------------------------------------------------------------
# Public entry point
# --------------------------------------------------------------------------

def run(input_path: str,
        duration_ms: int,
        prompts: List[str],
        progress: ProgressFn = None,
        batch_size: int = 16) -> DetectorOutput:
    """Score 1 fps frames against each prompt. Returns one series per
    prompt at FRAME_PERIOD_MS, keyed 'vision.clip.<prompt>'."""
    if not prompts:
        return {}

    torch, open_clip = _import_torch_and_clip()
    Image = _import_pil()
    model, preprocess, tokenizer, device = _load_model(torch, open_clip)

    if progress:
        progress(0.42, f"clip:encode-text ({device})")

    # Pre-encode the prompts once.
    with torch.no_grad():
        text_tokens = tokenizer(prompts).to(device)
        text_feats = model.encode_text(text_tokens)
        text_feats = text_feats / text_feats.norm(dim=-1, keepdim=True)

    if progress:
        progress(0.45, "clip:scoring-frames")

    series: Dict[str, List[float]] = {p: [] for p in prompts}
    expected_n = max(1, duration_ms // FRAME_PERIOD_MS)

    batch_imgs = []
    for i, (t_ms, jpg) in enumerate(_stream_frames(input_path, duration_ms)):
        try:
            img = Image.open(io.BytesIO(jpg)).convert("RGB")
        except Exception:
            for p in prompts: series[p].append(0.0)
            continue
        batch_imgs.append(preprocess(img))

        if len(batch_imgs) >= batch_size:
            _flush_batch(torch, model, batch_imgs, text_feats, prompts, series, device)
            batch_imgs = []
            if progress:
                progress(0.45 + min(0.5, 0.5 * i / max(1, expected_n)),
                         "clip:scoring-frames")

    if batch_imgs:
        _flush_batch(torch, model, batch_imgs, text_feats, prompts, series, device)

    if progress:
        progress(0.95, "clip:done")

    out: DetectorOutput = {}
    for p, vals in series.items():
        out[f"vision.clip.{p}"] = {"period_ms": FRAME_PERIOD_MS, "values": vals}
    return out


def _flush_batch(torch, model, batch_imgs, text_feats, prompts, series, device):
    """Encode a batch of images and append cosine-similarity scores."""
    with torch.no_grad():
        imgs = torch.stack(batch_imgs).to(device)
        img_feats = model.encode_image(imgs)
        img_feats = img_feats / img_feats.norm(dim=-1, keepdim=True)
        # Cosine similarity → [-1, 1]; map to [0, 1] so it's intuitive
        # against thresholds (0.5 = "kinda matches", 0.8 = "clearly").
        sims = (img_feats @ text_feats.T).cpu().numpy()
        sims = (sims + 1.0) * 0.5  # rescale
    for row in sims:
        for i, p in enumerate(prompts):
            series[p].append(float(row[i]))
