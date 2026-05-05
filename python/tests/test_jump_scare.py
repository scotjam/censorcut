"""Unit tests for the jump-scare detector — pure-arithmetic, no ffmpeg."""

import sys
import unittest
from pathlib import Path

# Allow `python -m unittest` from the repo root.
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from censorcut.analyze import detect_jump_scares  # noqa: E402


class TestJumpScare(unittest.TestCase):

    def test_clear_jump_scare_fires(self):
        # 2.0 s of quiet (-40 LUFS) at 100 ms samples, then sudden rise to -15.
        samples = [-40.0] * 20 + [-15.0] * 30
        suggestions, scores = detect_jump_scares(samples, duration_ms=5000)
        self.assertGreaterEqual(len(suggestions), 1)
        self.assertEqual(suggestions[0]["category"], "Jump scare")
        self.assertGreater(suggestions[0]["score"], 0.5)
        self.assertGreater(max(scores), 0.0)

    def test_no_trigger_when_already_loud(self):
        # No quiet preceding the loudness — should not fire.
        samples = [-15.0] * 100
        suggestions, scores = detect_jump_scares(samples, duration_ms=10000)
        self.assertEqual(suggestions, [])
        self.assertEqual(max(scores), 0.0)

    def test_no_trigger_in_silence(self):
        samples = [-50.0] * 100
        suggestions, _ = detect_jump_scares(samples, duration_ms=10000)
        self.assertEqual(suggestions, [])

    def test_small_rise_below_threshold(self):
        # Quiet then only 6 LU rise — below default 12 LU threshold.
        samples = [-40.0] * 20 + [-34.0] * 20
        suggestions, _ = detect_jump_scares(samples, duration_ms=4000)
        self.assertEqual(suggestions, [])

    def test_score_caps_at_one(self):
        # 30 LU rise — score should clamp to 1.0.
        samples = [-50.0] * 20 + [-15.0] * 20
        suggestions, _ = detect_jump_scares(samples, duration_ms=4000)
        self.assertGreaterEqual(len(suggestions), 1)
        self.assertLessEqual(suggestions[0]["score"], 1.0)
        self.assertGreaterEqual(suggestions[0]["score"], 1.0 - 1e-9)


if __name__ == "__main__":
    unittest.main()
