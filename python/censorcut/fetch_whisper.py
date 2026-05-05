"""Pre-fetch a Whisper model into the local cache.

Usage:
    python -m censorcut.fetch_whisper                      # default 'small'
    python -m censorcut.fetch_whisper --model large-v3
    python -m censorcut.fetch_whisper --model base.en      # English-only

faster-whisper would download on first transcribe anyway; running this
once verifies the install and primes the cache before kicking off a
long analysis. Models live under python/censorcut/models/whisper/."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path
from typing import Optional

PACKAGE_DIR = Path(__file__).parent
CACHE_DIR = PACKAGE_DIR / "models" / "whisper"


def main(argv: Optional[list] = None) -> int:
    parser = argparse.ArgumentParser(prog="censorcut.fetch_whisper",
                                     description="Pre-download a Whisper model")
    parser.add_argument("--model", default="small",
                        help="faster-whisper model name "
                             "(tiny/base/small/medium/large-v3 or .en variants)")
    args = parser.parse_args(argv)

    try:
        from faster_whisper import WhisperModel  # type: ignore
    except ImportError:
        print("faster-whisper not installed. Try: pip install faster-whisper",
              file=sys.stderr)
        return 2

    CACHE_DIR.mkdir(parents=True, exist_ok=True)
    print(f"Downloading whisper '{args.model}' into {CACHE_DIR}")
    try:
        m = WhisperModel(args.model, device="cpu", compute_type="int8",
                         download_root=str(CACHE_DIR))
        del m
    except Exception as e:
        print(f"Download failed: {e}", file=sys.stderr)
        return 3
    print("Done. The dialogue_whisper detector should now activate on the next run.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
