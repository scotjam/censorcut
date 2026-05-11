"""Tests for the production video fingerprint (F + v9 fallback).

Synthesizes small test videos via ffmpeg's lavfi sources to validate
the keyframe-extraction path and the match algorithm without depending
on real movies. Tests skip when ffmpeg or numpy are unavailable.
"""

from __future__ import annotations

import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))


def _have_ffmpeg() -> bool:
    return shutil.which("ffmpeg") is not None and shutil.which("ffprobe") is not None


def _have_numpy() -> bool:
    try:
        import numpy  # noqa: F401
        return True
    except ImportError:
        return False


def _make_irregular_video(path: str, durations: list) -> int:
    """Concat colored clips with per-clip durations from `durations`.
    Returns total duration in milliseconds. Used to produce a non-
    uniform keyframe pattern so the matcher has something interesting
    to align."""
    with tempfile.TemporaryDirectory(prefix="censorcut_test_") as td:
        tdp = Path(td)
        clip_paths = []
        colors = ["red", "green", "blue", "yellow", "magenta",
                  "cyan", "white", "black"]
        for i, dur_sec in enumerate(durations):
            color = colors[i % len(colors)]
            clip = str(tdp / f"clip{i}.mp4")
            cmd = [
                "ffmpeg", "-y", "-hide_banner", "-loglevel", "error",
                "-f", "lavfi",
                "-i", f"color=c={color}:s=320x240:r=24:d={dur_sec}",
                "-c:v", "libx264", "-preset", "ultrafast",
                "-pix_fmt", "yuv420p", "-g", "30",
                clip,
            ]
            subprocess.check_call(cmd)
            clip_paths.append(clip)
        list_file = tdp / "list.txt"
        list_file.write_text(
            "\n".join(f"file '{p}'" for p in clip_paths),
            encoding="utf-8")
        cmd = [
            "ffmpeg", "-y", "-hide_banner", "-loglevel", "error",
            "-f", "concat", "-safe", "0",
            "-i", str(list_file),
            "-c", "copy",
            path,
        ]
        subprocess.check_call(cmd)
    return sum(durations) * 1000


def _make_video_with_cuts(path: str, clip_count: int,
                            seconds_per_clip: int) -> int:
    """Concat several different-color clips so the encoder inserts a
    keyframe at each cut. Returns total duration in milliseconds."""
    with tempfile.TemporaryDirectory(prefix="censorcut_test_") as td:
        tdp = Path(td)
        clip_paths = []
        # Eight distinct colors gives us enough variety for >30 cuts.
        colors = ["red", "green", "blue", "yellow", "magenta",
                  "cyan", "white", "black"]
        for i in range(clip_count):
            color = colors[i % len(colors)]
            clip = str(tdp / f"clip{i}.mp4")
            cmd = [
                "ffmpeg", "-y", "-hide_banner", "-loglevel", "error",
                "-f", "lavfi",
                "-i", f"color=c={color}:s=320x240:r=24:d={seconds_per_clip}",
                "-c:v", "libx264", "-preset", "ultrafast",
                "-pix_fmt", "yuv420p",
                "-g", "30",
                clip,
            ]
            subprocess.check_call(cmd)
            clip_paths.append(clip)
        list_file = tdp / "list.txt"
        list_file.write_text(
            "\n".join(f"file '{p}'" for p in clip_paths),
            encoding="utf-8")
        cmd = [
            "ffmpeg", "-y", "-hide_banner", "-loglevel", "error",
            "-f", "concat", "-safe", "0",
            "-i", str(list_file),
            "-c", "copy",
            path,
        ]
        subprocess.check_call(cmd)
    return clip_count * seconds_per_clip * 1000


@unittest.skipUnless(_have_ffmpeg() and _have_numpy(),
                     "ffmpeg + numpy required")
class TestVideoFingerprintF(unittest.TestCase):
    def setUp(self):
        self.tmpdir = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmpdir.cleanup)

    def _fp(self, path: str, duration_ms: int):
        from censorcut.detectors import video_fingerprint as vfp
        return vfp.run(path, duration_ms=duration_ms)

    def test_fingerprint_returns_keyframe_type(self):
        path = str(Path(self.tmpdir.name) / "many_cuts.mp4")
        # 40 cuts, each 2 sec long → 80 sec total. libx264 inserts a
        # keyframe at every scene cut, so we should see ~40 keyframes
        # in the Cues / stss table — over the MIN_KEYFRAMES=30 floor
        # that the F path requires.
        dur_ms = _make_video_with_cuts(path, clip_count=40,
                                          seconds_per_clip=2)
        fp = self._fp(path, dur_ms)
        self.assertEqual(fp.get("type"), "keyframes")
        self.assertEqual(fp.get("version"), 1)
        self.assertEqual(fp.get("durationMs"), dur_ms)
        self.assertGreaterEqual(fp.get("keyframeCount", 0), 30)
        self.assertIsInstance(fp.get("keyframeTimesMs"), list)

    def test_match_is_deterministic(self):
        path = str(Path(self.tmpdir.name) / "deterministic.mp4")
        dur_ms = _make_video_with_cuts(path, clip_count=40,
                                          seconds_per_clip=2)
        from censorcut.detectors import video_fingerprint as vfp
        fp1 = self._fp(path, dur_ms)
        fp2 = self._fp(path, dur_ms)
        self.assertEqual(fp1.get("keyframeTimesMs"),
                         fp2.get("keyframeTimesMs"))
        verdict = vfp.match_fingerprints(fp1, fp2)
        self.assertTrue(verdict["isSameFilm"], msg=verdict["reason"])
        self.assertEqual(verdict["estimatedTrimMs"], 0)
        self.assertLessEqual(verdict["madMs"], vfp.MAX_MAD_MS)

    def test_different_films_differ(self):
        """Two films with different keyframe-time PATTERNS must DIFFER.

        Note: libx264 on monochrome lavfi clips produces near-uniformly
        spaced keyframes — so we deliberately use IRREGULAR clip
        durations so the keyframe pattern is non-trivial. Otherwise the
        match algorithm finds a perfect uniform-grid alignment for any
        two such videos."""
        path_a = str(Path(self.tmpdir.name) / "a.mp4")
        path_b = str(Path(self.tmpdir.name) / "b.mp4")
        # A: irregular pattern via per-clip duration variation.
        dur_a = _make_irregular_video(path_a,
                                          durations=[3, 1, 4, 1, 5, 9, 2, 6,
                                                     5, 3, 5, 8, 9, 7, 9,
                                                     3, 2, 3, 8, 4, 6, 2,
                                                     6, 4, 3, 3, 8, 3, 2,
                                                     7, 5])
        # B: completely different pattern.
        dur_b = _make_irregular_video(path_b,
                                          durations=[1, 1, 2, 3, 5, 8, 13, 21,
                                                     1, 2, 5, 10, 7, 3, 1,
                                                     6, 2, 8, 4, 1, 9, 3,
                                                     7, 2, 5, 3, 8, 1, 4,
                                                     6, 2])
        from censorcut.detectors import video_fingerprint as vfp
        fp_a = self._fp(path_a, dur_a)
        fp_b = self._fp(path_b, dur_b)
        verdict = vfp.match_fingerprints(fp_a, fp_b)
        self.assertFalse(verdict["isSameFilm"], msg=verdict["reason"])

    def test_incompatible_types_differ(self):
        """A keyframe-typed fp and an audio_peak_gaps fp can't be
        compared and must return isSameFilm=False with an explanatory
        reason."""
        from censorcut.detectors import video_fingerprint as vfp
        fp_kf = {"version": 1, "type": "keyframes",
                  "durationMs": 60_000,
                  "keyframeTimesMs": [0, 1000, 2000, 3000, 4000]}
        fp_v9 = {"version": 1, "type": "audio_peak_gaps",
                  "durationMs": 60_000,
                  "peaks": [{"tMs": 1000, "phash": "0"*16}]*5,
                  "gapsMs": [1000]*4, "peakCount": 5}
        verdict = vfp.match_fingerprints(fp_kf, fp_v9)
        self.assertFalse(verdict["isSameFilm"])
        self.assertIn("incompatible", verdict.get("reason", "").lower())


class TestMadAndPairing(unittest.TestCase):
    """Pure-Python unit tests for the matcher's pair-finding and MAD
    computation — no ffmpeg required."""

    def test_mad_zero_for_identical_offsets(self):
        from censorcut.detectors import video_fingerprint as vfp
        diffs = [1234] * 50
        self.assertEqual(vfp._mad_ms(diffs), 0.0)

    def test_mad_picks_up_drift(self):
        from censorcut.detectors import video_fingerprint as vfp
        # Half the pairs have offset 0, half have offset 10000. MAD
        # picks up the spread.
        diffs = [0] * 25 + [10_000] * 25
        mad = vfp._mad_ms(diffs)
        self.assertGreater(mad, 1000.0)

    def test_ordered_pairs_basic(self):
        from censorcut.detectors import video_fingerprint as vfp
        short = [1000, 2000, 3000]
        long  = [500, 1100, 1900, 3050, 4000]
        # Offset = 0, tol = 200 ms.
        pairs = vfp._ordered_pairs(short, long, offset_ms=0, tol_ms=200)
        self.assertEqual(len(pairs), 3)
        # Each short entry consumed exactly one long entry.
        self.assertEqual([p[1] for p in pairs], [1100, 1900, 3050])

    def test_ordered_pairs_one_to_one(self):
        from censorcut.detectors import video_fingerprint as vfp
        # Two short entries both within tol of the same long entry —
        # only one of them consumes it; the other must miss.
        short = [1000, 1010]
        long  = [1005]
        pairs = vfp._ordered_pairs(short, long, offset_ms=0, tol_ms=100)
        self.assertEqual(len(pairs), 1)


if __name__ == "__main__":
    unittest.main()
