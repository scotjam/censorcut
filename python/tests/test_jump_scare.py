"""Unit tests for the loudness detector's jump-scare scoring logic.

Pure arithmetic — no ffmpeg required to run."""

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from censorcut.detectors.audio_loudness import compute_jump_score  # noqa: E402


class TestJumpScore(unittest.TestCase):

    def test_clear_jump_scare_fires(self):
        # 2.0 s of quiet (-40 LUFS) at 100 ms samples, then sudden rise to -15.
        samples = [-40.0] * 20 + [-15.0] * 30
        scores = compute_jump_score(samples)
        self.assertGreater(max(scores), 0.5)

    def test_no_trigger_when_already_loud(self):
        samples = [-15.0] * 100
        scores = compute_jump_score(samples)
        self.assertEqual(max(scores), 0.0)

    def test_no_trigger_in_silence(self):
        samples = [-50.0] * 100
        scores = compute_jump_score(samples)
        self.assertEqual(max(scores), 0.0)

    def test_small_rise_below_threshold(self):
        # Only 6 LU rise — below default 12 LU threshold.
        samples = [-40.0] * 20 + [-34.0] * 20
        scores = compute_jump_score(samples)
        self.assertEqual(max(scores), 0.0)

    def test_score_caps_at_one(self):
        samples = [-50.0] * 20 + [-15.0] * 20
        scores = compute_jump_score(samples)
        self.assertGreater(max(scores), 0.95)
        self.assertLessEqual(max(scores), 1.0)


if __name__ == "__main__":
    unittest.main()
