"""Pre-fetch CLIP weights into the local cache.

open_clip can download on first use, but pre-fetching lets the user
verify the model works before triggering a long analysis.

    python -m censorcut.fetch_clip
    python -m censorcut.fetch_clip --model ViT-L-14 --pretrained openai
    python -m censorcut.fetch_clip --model SigLIP-Large

Models live under python/censorcut/models/clip/. That directory is
gitignored alongside yamnet.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path
from typing import Optional

PACKAGE_DIR = Path(__file__).parent
CACHE_DIR = PACKAGE_DIR / "models" / "clip"


def main(argv: Optional[list] = None) -> int:
    parser = argparse.ArgumentParser(prog="censorcut.fetch_clip",
                                     description="Pre-download CLIP weights")
    parser.add_argument("--model", default="ViT-L-14",
                        help="open_clip model name (default ViT-L-14)")
    parser.add_argument("--pretrained", default="openai",
                        help="open_clip pretrained tag (default openai)")
    args = parser.parse_args(argv)

    try:
        import open_clip  # type: ignore
    except ImportError:
        print("open_clip_torch not installed. Try: pip install open_clip_torch",
              file=sys.stderr)
        return 2

    CACHE_DIR.mkdir(parents=True, exist_ok=True)
    print(f"Downloading {args.model} / {args.pretrained} into {CACHE_DIR}")
    try:
        model, _, _ = open_clip.create_model_and_transforms(
            args.model, pretrained=args.pretrained, cache_dir=str(CACHE_DIR))
        del model
    except Exception as e:
        print(f"Download failed: {e}", file=sys.stderr)
        return 3
    print("Done. The vision_clip detector should now activate on the next run.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
