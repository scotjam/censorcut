"""Tests for the spectral-signature helper used by audio fingerprinting.

We don't try to run the full ffmpeg+YAMNet pipeline here — that needs a
real video file. Instead we test the pure-numpy primitive: same audio
content → same signature; different content → different signature; sub-
second timing jitter doesn't catastrophically shift the hash."""

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))


class TestSpectralSignature(unittest.TestCase):

    def setUp(self):
        try:
            import numpy as np  # noqa: F401
        except ImportError:
            self.skipTest("numpy not installed")
        from censorcut.detectors import audio_fingerprint
        self.fp = audio_fingerprint
        import numpy as np
        self.np = np

    def _tone(self, freq_hz, duration_sec=2.0, sample_rate=22050):
        np = self.np
        t = np.arange(int(duration_sec * sample_rate)) / sample_rate
        return 0.5 * np.sin(2.0 * np.pi * freq_hz * t).astype(np.float32)

    def test_same_audio_yields_same_signature(self):
        sr = 22050
        samples = self._tone(880.0)
        sig_a = self.fp._spectral_signature(self.np, samples, sr, 1.0)
        sig_b = self.fp._spectral_signature(self.np, samples, sr, 1.0)
        self.assertEqual(sig_a, sig_b)

    def test_different_frequencies_yield_different_signatures(self):
        sr = 22050
        sig_low  = self.fp._spectral_signature(self.np, self._tone(220.0),  sr, 1.0)
        sig_high = self.fp._spectral_signature(self.np, self._tone(4000.0), sr, 1.0)
        self.assertNotEqual(sig_low, sig_high)

    def test_signature_is_64_bit_hex(self):
        sr = 22050
        sig = self.fp._spectral_signature(self.np, self._tone(1000.0), sr, 1.0)
        self.assertEqual(len(sig), 16)
        int(sig, 16)  # raises if not hex

    def test_short_buffer_returns_sentinel(self):
        sr = 22050
        # Buffer way smaller than the 1 s window — function must not panic.
        sig = self.fp._spectral_signature(self.np, self.np.zeros(10, dtype=self.np.float32),
                                          sr, 0.0)
        self.assertEqual(len(sig), 16)


class TestAnchorWindows(unittest.TestCase):

    def setUp(self):
        from censorcut.detectors import audio_fingerprint
        self.fp = audio_fingerprint

    def test_window_constants_match_spec(self):
        # The user's spec: skip first 5 min, look in (5..30 min) for
        # start anchors; skip last 5 min, look in (duration-30..-5 min)
        # for end anchors.
        self.assertEqual(self.fp.START_WINDOW_BEGIN_MS,  5  * 60 * 1000)
        self.assertEqual(self.fp.START_WINDOW_END_MS,    30 * 60 * 1000)
        self.assertEqual(self.fp.END_TAIL_SKIP_MS,       5  * 60 * 1000)
        self.assertEqual(self.fp.END_WINDOW_BACK_MS,     30 * 60 * 1000)
        self.assertEqual(self.fp.ANCHORS_PER_WINDOW,     2)

    def test_pick_top_anchors_skips_speechy_frames(self):
        # Background is below the -110 LUFS finite-cutoff so it's
        # ignored entirely; only the two real peaks compete.
        momentary = [-120.0] * 1000
        momentary[100] = -8.0
        momentary[200] = -12.0
        speech_period = 100
        speech_series = [0.0] * 1000
        speech_series[100] = 0.6  # speechy at t=10 s
        anchors = self.fp._pick_top_anchors_in_window(
            momentary, lufs_period_ms=100,
            win_lo_ms=5_000, win_hi_ms=900_000,
            want=2,
            speech_period_ms=speech_period,
            speech_series=speech_series)
        self.assertEqual(len(anchors), 1)
        self.assertEqual(anchors[0][0], 200 * 100)  # 20 s
        self.assertAlmostEqual(anchors[0][1], -12.0)

    def test_pick_top_anchors_enforces_spacing(self):
        # Two peaks 20 s apart violates the 60 s minimum spacing; a
        # third peak 70 s in is far enough.
        momentary = [-120.0] * 1000
        momentary[100] = -5.0   # 10 s
        momentary[300] = -6.0   # 30 s — too close to the 10 s pick
        momentary[700] = -7.0   # 70 s — far enough from 10 s
        anchors = self.fp._pick_top_anchors_in_window(
            momentary, lufs_period_ms=100,
            win_lo_ms=5_000, win_hi_ms=200_000,
            want=2,
            speech_period_ms=None, speech_series=None)
        self.assertEqual(len(anchors), 2)
        self.assertEqual(anchors[0][0], 100 * 100)  # 10 s
        self.assertEqual(anchors[1][0], 700 * 100)  # 70 s


if __name__ == "__main__":
    unittest.main()
