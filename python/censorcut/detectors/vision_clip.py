"""CLIP/SigLIP vision detector (M5) with M7 feedback loop.

Samples 1 fps frames from the source video via ffmpeg, then scores each
frame against the union of text prompts referenced by enabled categories.
Returns one float series per prompt at a 1 s sampling period.

M7 feedback: on every run we load ~/.censorcut/feedback.jsonl and, for
each frame, compute cosine similarity between its CLIP embedding and the
embeddings of past accept/reject decisions. Frames close to a past
*reject* (cosine >= NEAR_THRESHOLD) get a multiplicative penalty applied
to all their per-prompt scores; frames close to a past *accept* get a
small boost. This steers future suggestions away from the user's known
no's and toward known yes's without changing the model.

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

# Feedback k-NN bands. The cosine threshold has to be high enough that
# "looks similar" really means semantically near-identical content;
# anything looser starts wiping out unrelated suggestions.
FEEDBACK_NEAR_THRESHOLD = 0.85
FEEDBACK_REJECT_PENALTY = 0.5   # multiply scores by this when near a reject
FEEDBACK_ACCEPT_BOOST   = 1.15  # multiply by this when near an accept (clamped to 1)


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


def _feedback_file_path() -> Path:
    """Where FeedbackStore writes — must agree with the C++ side."""
    home = os.path.expanduser("~")
    return Path(home) / ".censorcut" / "feedback.jsonl"


def _load_feedback_vectors(torch, np):
    """Load past accept/reject embeddings as a dict of stacked tensors.
    Returns ({"reject": tensor[N1, D], "accept": tensor[N2, D]}, count)
    on the same device CLIP is using (caller passes them through).
    Empty dict if no feedback file."""
    import json
    path = _feedback_file_path()
    if not path.exists():
        return {"reject": None, "accept": None}, 0
    by_decision: Dict[str, List[List[float]]] = {"reject": [], "accept": []}
    try:
        with open(path, "r", encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                try:
                    row = json.loads(line)
                except Exception:
                    continue
                vec = row.get("vec")
                dec = row.get("decision")
                if not vec or dec not in ("accept", "reject"):
                    continue
                by_decision[dec].append(vec)
    except OSError:
        return {"reject": None, "accept": None}, 0

    out: Dict[str, object] = {}
    total = 0
    for k in ("reject", "accept"):
        rows = by_decision[k]
        if not rows:
            out[k] = None
            continue
        arr = np.asarray(rows, dtype=np.float32)
        # Re-normalize to be safe — older entries may differ slightly.
        norms = np.linalg.norm(arr, axis=1, keepdims=True)
        norms[norms == 0] = 1.0
        arr = arr / norms
        out[k] = torch.from_numpy(arr)
        total += arr.shape[0]
    return out, total


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
    prompt at FRAME_PERIOD_MS, keyed 'vision.clip.<prompt>'.

    A side-effect: every per-frame image embedding (after L2-normalize) is
    stashed in ``run.last_image_embeddings`` as a list of (t_ms, [768
    floats]) tuples. analyze.py reads it and persists only those that fall
    inside emitted suggestion ranges, so the feedback loop has access to
    the semantic vector that produced each suggestion. Embeddings are NOT
    written into the per-prompt series (those stay numeric scores)."""
    run.last_image_embeddings = []  # type: ignore[attr-defined]

    if not prompts:
        return {}

    torch, open_clip = _import_torch_and_clip()
    np = _import_numpy()
    Image = _import_pil()
    model, preprocess, tokenizer, device = _load_model(torch, open_clip)

    if progress:
        progress(0.42, f"clip:encode-text ({device})")

    with torch.no_grad():
        text_tokens = tokenizer(prompts).to(device)
        text_feats = model.encode_text(text_tokens)
        text_feats = text_feats / text_feats.norm(dim=-1, keepdim=True)

    feedback, fb_count = _load_feedback_vectors(torch, np)
    if fb_count > 0:
        for k in ("reject", "accept"):
            if feedback.get(k) is not None:
                feedback[k] = feedback[k].to(device)
        import sys as _sys
        print(f"censorcut.vision_clip: loaded {fb_count} feedback vector(s) "
              f"(reject={0 if feedback.get('reject') is None else feedback['reject'].shape[0]}, "
              f"accept={0 if feedback.get('accept') is None else feedback['accept'].shape[0]})",
              file=_sys.stderr)

    if progress:
        progress(0.45, "clip:scoring-frames")

    series: Dict[str, List[float]] = {p: [] for p in prompts}
    expected_n = max(1, duration_ms // FRAME_PERIOD_MS)

    batch_imgs: List = []
    batch_times_ms: List[int] = []
    for i, (t_ms, jpg) in enumerate(_stream_frames(input_path, duration_ms)):
        try:
            img = Image.open(io.BytesIO(jpg)).convert("RGB")
        except Exception:
            for p in prompts: series[p].append(0.0)
            run.last_image_embeddings.append((t_ms, None))  # type: ignore[attr-defined]
            continue
        batch_imgs.append(preprocess(img))
        batch_times_ms.append(t_ms)

        if len(batch_imgs) >= batch_size:
            _flush_batch(torch, model, batch_imgs, batch_times_ms,
                         text_feats, prompts, series,
                         run.last_image_embeddings,  # type: ignore[attr-defined]
                         feedback, device)
            batch_imgs = []
            batch_times_ms = []
            if progress:
                progress(0.45 + min(0.5, 0.5 * i / max(1, expected_n)),
                         "clip:scoring-frames")

    if batch_imgs:
        _flush_batch(torch, model, batch_imgs, batch_times_ms,
                     text_feats, prompts, series,
                     run.last_image_embeddings,  # type: ignore[attr-defined]
                     feedback, device)

    if progress:
        progress(0.95, "clip:done")

    out: DetectorOutput = {}
    for p, vals in series.items():
        out[f"vision.clip.{p}"] = {"period_ms": FRAME_PERIOD_MS, "values": vals}
    return out


def _flush_batch(torch, model, batch_imgs, batch_times_ms, text_feats,
                 prompts, series, embeddings_out, feedback, device):
    """Encode a batch of images, append rescaled-similarity scores, and
    stash the L2-normalized image embeddings (one per frame) so the
    feedback loop can compare them later.

    Raw cosine similarity for normalized CLIP features lives roughly in
    [0.10, 0.40] — non-matches cluster around 0.15-0.20, strong matches
    reach 0.30-0.40. We map [0.15, 0.35] linearly onto [0, 1] (clamped),
    so a non-match is near 0 and a clear match is above 0.7."""
    with torch.no_grad():
        imgs = torch.stack(batch_imgs).to(device)
        img_feats = model.encode_image(imgs)
        img_feats = img_feats / img_feats.norm(dim=-1, keepdim=True)
        raw = img_feats @ text_feats.T
        sims = torch.clamp((raw - 0.15) / 0.20, 0.0, 1.0)

        # Per-frame multiplicative scale from feedback. Default 1.0; reject
        # neighbours bring it down, accept neighbours bring it up. Single
        # max-cosine match is enough — feedback is sparse, we don't need
        # nearest-K.
        scale = torch.ones(sims.shape[0], 1, device=device)
        if feedback.get("reject") is not None:
            sim_to_reject = (img_feats @ feedback["reject"].T).max(dim=1).values
            scale = torch.where(
                sim_to_reject.unsqueeze(1) >= FEEDBACK_NEAR_THRESHOLD,
                scale * FEEDBACK_REJECT_PENALTY, scale)
        if feedback.get("accept") is not None:
            sim_to_accept = (img_feats @ feedback["accept"].T).max(dim=1).values
            scale = torch.where(
                sim_to_accept.unsqueeze(1) >= FEEDBACK_NEAR_THRESHOLD,
                scale * FEEDBACK_ACCEPT_BOOST, scale)
        sims = torch.clamp(sims * scale, 0.0, 1.0)

        sims_np = sims.cpu().numpy()
        embeds = img_feats.cpu().numpy()
    for k, row in enumerate(sims_np):
        for i, p in enumerate(prompts):
            series[p].append(float(row[i]))
        embeddings_out.append((batch_times_ms[k],
                               [round(float(x), 5) for x in embeds[k].tolist()]))
