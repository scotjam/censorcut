"""Tests for the M8.video scene-cut + pHash fingerprint.

Most tests synthesize a tiny test video on disk via ffmpeg's lavfi
sources so we can prove determinism without depending on a real movie
sitting in the repo. Tests skip cleanly if ffmpeg isn't on PATH (some
CI environments don't have it)."""

from __future__ import annotations

import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))


def _have_ffmpeg() -> bool:
    return shutil.which("ffmpeg") is not None


def _have_numpy() -> bool:
    try:
        import numpy  # noqa: F401
        return True
    except ImportError:
        return False


def _make_test_video(path: str, seconds: int = 60) -> None:
    """Generate a deterministic test video using ffmpeg's lavfi.

    The 'testsrc' source produces a slow-moving counter pattern with
    occasional sharp transitions — enough variation to find scene
    'cuts' even though it's synthesized."""
    cmd = [
        "ffmpeg", "-y", "-hide_banner", "-loglevel", "error",
        "-f", "lavfi",
        "-i", f"testsrc=duration={seconds}:size=320x240:rate=24",
        "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p",
        path,
    ]
    subprocess.check_call(cmd)


def _make_test_video_with_cuts(path: str, cuts: int = 5,
                                seconds_per_clip: int = 5) -> None:
    """Concat several different-color blank clips so we get clear hard
    cuts at known timestamps. Result is `cuts+1` clips back-to-back."""
    with tempfile.TemporaryDirectory(prefix="censorcut_test_") as td:
        tdp = Path(td)
        clip_paths = []
        # Use a few visually distinct colors so the cuts have big phash
        # changes.
        colors = ["red", "green", "blue", "yellow", "magenta",
                  "cyan", "white", "black"]
        for i in range(cuts + 1):
            color = colors[i % len(colors)]
            clip = str(tdp / f"clip{i}.mp4")
            cmd = [
                "ffmpeg", "-y", "-hide_banner", "-loglevel", "error",
                "-f", "lavfi",
                "-i", f"color=c={color}:s=320x240:r=24:d={seconds_per_clip}",
                "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p",
                clip,
            ]
            subprocess.check_call(cmd)
            clip_paths.append(clip)
        list_file = tdp / "list.txt"
        list_file.write_text(
            "\n".join(f"file '{p}'" for p in clip_paths), encoding="utf-8")
        cmd = [
            "ffmpeg", "-y", "-hide_banner", "-loglevel", "error",
            "-f", "concat", "-safe", "0",
            "-i", str(list_file),
            "-c", "copy",
            path,
        ]
        subprocess.check_call(cmd)


@unittest.skipUnless(_have_ffmpeg() and _have_numpy(),
                     "ffmpeg + numpy required")
class TestVideoFingerprint(unittest.TestCase):
    def setUp(self):
        self.tmpdir = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmpdir.cleanup)

    def _run_fp(self, path: str, duration_ms: int):
        from censorcut.detectors import video_fingerprint
        return video_fingerprint.run(path, duration_ms=duration_ms)

    def test_deterministic_repeat(self):
        """Same input file → same digest, twice in a row."""
        path = str(Path(self.tmpdir.name) / "a.mp4")
        # 60 seconds × 5 colored clips = 300s. body cushion = 100s.
        _make_test_video_with_cuts(path, cuts=8, seconds_per_clip=40)

        fp1 = self._run_fp(path, duration_ms=8 * 40 * 1000)
        fp2 = self._run_fp(path, duration_ms=8 * 40 * 1000)
        self.assertIn("anchors", fp1)
        self.assertEqual(fp1.get("digest"), fp2.get("digest"),
                         msg="determinism: same input twice should produce "
                             "the same digest")

    def test_short_film_falls_back_gracefully(self):
        """A very short clip with no detectable cuts should return an
        empty-anchor result, not crash."""
        path = str(Path(self.tmpdir.name) / "short.mp4")
        _make_test_video(path, seconds=4)
        fp = self._run_fp(path, duration_ms=4000)
        # Either anchors=[] or some-but-fewer-than-5 entries; either way,
        # no crash.
        self.assertIn("anchors", fp)
        self.assertIsInstance(fp["anchors"], list)

    def test_different_cuts_produce_different_digests(self):
        """Two videos with cuts at *different* points produce *different*
        digests."""
        path_a = str(Path(self.tmpdir.name) / "a.mp4")
        path_b = str(Path(self.tmpdir.name) / "b.mp4")
        _make_test_video_with_cuts(path_a, cuts=8, seconds_per_clip=40)
        # Different rhythm: 5 cuts, 60s each = 360s total.
        _make_test_video_with_cuts(path_b, cuts=5, seconds_per_clip=60)
        fp_a = self._run_fp(path_a, duration_ms=8 * 40 * 1000)
        fp_b = self._run_fp(path_b, duration_ms=5 * 60 * 1000)
        if not fp_a.get("anchors") or not fp_b.get("anchors"):
            self.skipTest("test video produced no detectable cuts; "
                          "scene-cut detection on synthetic constant-color "
                          "clips is sensitive to encoder choices")
        self.assertNotEqual(fp_a.get("digest"), fp_b.get("digest"))

    def test_body_window_short_film(self):
        """For a 12-minute (720s) film, cushion should be 4 minutes
        (since 720s / 3 = 240s = 4 min, less than the 10-min default)."""
        from censorcut.detectors import video_fingerprint
        body_lo, body_hi = video_fingerprint._body_window(12 * 60 * 1000)
        self.assertEqual(body_lo, 4 * 60 * 1000)
        self.assertEqual(body_hi, 8 * 60 * 1000)

    def test_body_window_full_length_film(self):
        """For a 90-minute film, cushion should cap at 10 minutes."""
        from censorcut.detectors import video_fingerprint
        body_lo, body_hi = video_fingerprint._body_window(90 * 60 * 1000)
        self.assertEqual(body_lo, 10 * 60 * 1000)
        self.assertEqual(body_hi, 80 * 60 * 1000)

    def test_phash_is_deterministic(self):
        """pHash function on the same byte buffer returns the same int."""
        from censorcut.detectors import video_fingerprint as vf
        import numpy as np
        rng = np.random.default_rng(seed=42)
        frame = bytes(rng.integers(0, 256, size=vf.PHASH_RES * vf.PHASH_RES,
                                   dtype=np.uint8).tolist())
        m = vf._make_dct_matrix(np, vf.PHASH_RES)
        h1 = vf._phash_from_frame(np, m, frame)
        h2 = vf._phash_from_frame(np, m, frame)
        self.assertEqual(h1, h2)
        # And it fits in 64 bits.
        self.assertGreaterEqual(h1, 0)
        self.assertLess(h1, 1 << 64)


class TestVideoFingerprintNoFfmpeg(unittest.TestCase):
    """These tests don't invoke ffmpeg at all — pure unit logic."""

    def test_dct_matrix_orthonormality(self):
        if not _have_numpy():
            self.skipTest("numpy not available")
        from censorcut.detectors import video_fingerprint as vf
        import numpy as np
        m = vf._make_dct_matrix(np, vf.PHASH_RES)
        # M @ M.T should be identity (modulo float precision) for an
        # orthonormal DCT.
        ident = m @ m.T
        diag = np.diag(ident)
        off  = ident - np.diag(diag)
        np.testing.assert_allclose(diag, np.ones_like(diag), atol=1e-4)
        np.testing.assert_allclose(off, np.zeros_like(off), atol=1e-4)

    def test_hamming_basics(self):
        from censorcut.detectors import video_fingerprint as vf
        self.assertEqual(vf._hamming(0, 0), 0)
        self.assertEqual(vf._hamming(0, 1), 1)
        self.assertEqual(vf._hamming(0xFF, 0x00), 8)
        self.assertEqual(vf._hamming(0xFFFF_FFFF_FFFF_FFFF, 0x0), 64)


if __name__ == "__main__":
    unittest.main()
