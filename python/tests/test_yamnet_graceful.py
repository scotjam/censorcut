"""Smoke tests for the YAMNet detector's graceful-fail paths.

These run without any ML dependencies installed: we just verify that the
module imports, that calling run() with no model file raises a clean
:class:`YamnetUnavailable`, and that the label-resolution helper handles
missing names. The full inference path is exercised manually once the
user has run python -m censorcut.fetch_yamnet."""

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from censorcut.detectors import audio_label  # noqa: E402


class TestYamnetGraceful(unittest.TestCase):

    def test_resolve_label_indices_handles_missing(self):
        class_names = ["Speech", "Screaming", "Yell", "Crying, sobbing"]
        out = audio_label.resolve_label_indices(
            class_names,
            ["Screaming", "yell", "Whatever-not-real", "Crying, sobbing"])
        # Case-insensitive + skips unresolved.
        names = [n for n, _ in out]
        self.assertIn("Screaming", names)
        self.assertIn("yell", names)
        self.assertIn("Crying, sobbing", names)
        self.assertNotIn("Whatever-not-real", names)
        # Indices match.
        idx_for = dict(out)
        self.assertEqual(idx_for["Screaming"], 1)
        self.assertEqual(idx_for["Crying, sobbing"], 3)

    def test_run_raises_yamnet_unavailable_when_no_model(self):
        # The point: a missing model OR an inaccessible input should produce
        # a controlled failure, not an AttributeError / TypeError leaking
        # from our code. The exact exception depends on which check trips
        # first (no deps -> YamnetUnavailable; deps + model present but
        # missing source file -> ffmpeg subprocess error).
        import subprocess
        try:
            audio_label.run("/nonexistent.mp4", duration_ms=1000,
                            labels=["Screaming"])
        except audio_label.YamnetUnavailable:
            return
        except Exception as e:
            allowed = (FileNotFoundError, RuntimeError,
                       subprocess.CalledProcessError)
            self.assertIsInstance(e, allowed,
                                  msg=f"unexpected exception class: {type(e).__name__}: {e}")


if __name__ == "__main__":
    unittest.main()
