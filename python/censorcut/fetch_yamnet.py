"""Download the YAMNet model + class map into censorcut/models/.

Run once before the YAMNet detector can be used::

    python -m censorcut.fetch_yamnet

The destination is ~4 MB. Falls back to a list of mirror URLs and lets the
user override via --model-url / --classmap-url if any host stops serving
the canonical files.
"""

from __future__ import annotations

import argparse
import hashlib
import sys
import urllib.request
from pathlib import Path
from typing import Iterable, Optional

PACKAGE_DIR = Path(__file__).parent
MODELS_DIR = PACKAGE_DIR / "models"

# Class map: the official AudioSet display-name table that ships in the
# tensorflow/models repo. ~16 KB, stable URL.
DEFAULT_CLASSMAP_URLS = [
    "https://raw.githubusercontent.com/tensorflow/models/master/research/audioset/yamnet/yamnet_class_map.csv",
]

# Model: the standard YAMNet TFLite (~4 MB). The Kaggle URL is the
# canonical landing page; we try the storage.googleapis.com mirror first
# because it doesn't require login.
DEFAULT_MODEL_URLS = [
    "https://storage.googleapis.com/mediapipe-models/audio_classifier/yamnet/float32/1/yamnet.tflite",
    "https://storage.googleapis.com/audioset/yamnet.tflite",
]


def _download(urls: Iterable[str], dst: Path, label: str) -> None:
    last_err: Optional[Exception] = None
    for url in urls:
        try:
            print(f"Fetching {label}: {url}")
            with urllib.request.urlopen(url, timeout=60) as resp:
                data = resp.read()
            dst.parent.mkdir(parents=True, exist_ok=True)
            tmp = dst.with_suffix(dst.suffix + ".part")
            tmp.write_bytes(data)
            tmp.replace(dst)
            digest = hashlib.sha256(data).hexdigest()[:12]
            print(f"  → wrote {dst}  ({len(data):,} bytes, sha256={digest})")
            return
        except Exception as e:  # network, HTTP, IO
            print(f"  ! {e}", file=sys.stderr)
            last_err = e
    raise SystemExit(f"could not fetch {label}; last error: {last_err}")


def main(argv: Optional[list] = None) -> int:
    parser = argparse.ArgumentParser(prog="censorcut.fetch_yamnet",
                                     description="Download YAMNet model + class map")
    parser.add_argument("--model-url", action="append", default=None,
                        help="Override URL for yamnet.tflite (repeatable)")
    parser.add_argument("--classmap-url", action="append", default=None,
                        help="Override URL for yamnet_class_map.csv (repeatable)")
    parser.add_argument("--force", action="store_true",
                        help="Re-download even if the file already exists")
    args = parser.parse_args(argv)

    model_path = MODELS_DIR / "yamnet.tflite"
    classmap_path = MODELS_DIR / "yamnet_class_map.csv"

    if not args.force and model_path.exists() and classmap_path.exists():
        print(f"Already present: {model_path} and {classmap_path}")
        print("Pass --force to re-download.")
        return 0

    classmap_urls = args.classmap_url or DEFAULT_CLASSMAP_URLS
    model_urls    = args.model_url    or DEFAULT_MODEL_URLS

    if args.force or not classmap_path.exists():
        _download(classmap_urls, classmap_path, "class map")
    if args.force or not model_path.exists():
        _download(model_urls, model_path, "yamnet model")

    print("Done. The YAMNet detector should now activate on the next analysis run.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
