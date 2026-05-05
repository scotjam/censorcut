"""Tests for the category fusion + suggestion-emission logic."""

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from censorcut.categories import (  # noqa: E402
    fuse_category,
    fused_to_suggestions,
    merge_overlapping_in_category,
    required_labels,
)


def jump_series(values):
    return {"period_ms": 100, "values": values}


class TestFuseCategory(unittest.TestCase):

    def test_direct_series_passes_through_with_weight(self):
        cat = {
            "name": "Test",
            "enabled": True,
            "threshold": 0.5,
            "detectors": [{"id": "audio.lufs.jump", "weight": 1.0}],
        }
        series = {"audio.lufs.jump": jump_series([0.0, 0.6, 0.9, 0.3])}
        fused = fuse_category(cat, series, duration_ms=400)
        self.assertEqual(len(fused), 4)
        self.assertAlmostEqual(fused[1], 0.6)
        self.assertAlmostEqual(fused[2], 0.9)

    def test_label_family_takes_max_across_labels(self):
        cat = {
            "name": "Cry",
            "enabled": True,
            "threshold": 0.5,
            "detectors": [
                {"id": "audio.yamnet", "weight": 1.0,
                 "params": {"labels": ["Crying, sobbing", "Whimper"]}}
            ],
        }
        series = {
            "audio.yamnet.Crying, sobbing": jump_series([0.1, 0.4, 0.2]),
            "audio.yamnet.Whimper":         jump_series([0.0, 0.6, 0.0]),
        }
        fused = fuse_category(cat, series, duration_ms=300)
        # Frame 0: max(0.1, 0.0) = 0.1 ; Frame 1: max(0.4, 0.6) = 0.6 ; Frame 2: 0.2
        self.assertAlmostEqual(fused[0], 0.1)
        self.assertAlmostEqual(fused[1], 0.6)
        self.assertAlmostEqual(fused[2], 0.2)

    def test_clamps_to_one_when_summed_detectors_exceed(self):
        cat = {
            "name": "Big",
            "enabled": True,
            "threshold": 0.5,
            "detectors": [
                {"id": "audio.lufs.jump", "weight": 1.0},
                {"id": "audio.yamnet", "weight": 1.0,
                 "params": {"labels": ["Scream"]}},
            ],
        }
        series = {
            "audio.lufs.jump":      jump_series([0.7, 0.7]),
            "audio.yamnet.Scream":  jump_series([0.7, 0.7]),
        }
        fused = fuse_category(cat, series, duration_ms=200)
        for v in fused:
            self.assertLessEqual(v, 1.0)
            self.assertGreater(v, 0.99)

    def test_disabled_category_returns_empty(self):
        cat = {"name": "Off", "enabled": False, "detectors": []}
        fused = fuse_category(cat, {"x": jump_series([1.0])}, duration_ms=100)
        self.assertEqual(fused, [])


class TestFusedToSuggestions(unittest.TestCase):

    def test_one_run_above_threshold(self):
        cat = {"name": "X", "threshold": 0.5,
               "padBeforeMs": 0, "padAfterMs": 0}
        # Five frames at 100ms: 0.1, 0.6, 0.7, 0.6, 0.1  → run of 3 above.
        sug = fused_to_suggestions(cat, [0.1, 0.6, 0.7, 0.6, 0.1],
                                   duration_ms=500)
        self.assertEqual(len(sug), 1)
        self.assertEqual(sug[0]["startMs"], 100)
        self.assertEqual(sug[0]["endMs"], 400)
        self.assertAlmostEqual(sug[0]["score"], 0.7)
        self.assertEqual(sug[0]["category"], "X")

    def test_padding_extends_range(self):
        cat = {"name": "X", "threshold": 0.5,
               "padBeforeMs": 200, "padAfterMs": 100}
        sug = fused_to_suggestions(cat, [0.1, 0.6, 0.6, 0.1],
                                   duration_ms=400)
        self.assertEqual(len(sug), 1)
        # Run is frames 1..2 (100..300ms). Pad: -200ms before, +100ms after.
        # start clamped to 0; end clamped to duration.
        self.assertEqual(sug[0]["startMs"], 0)
        self.assertEqual(sug[0]["endMs"], 400)

    def test_no_runs_below_threshold(self):
        cat = {"name": "X", "threshold": 0.9}
        sug = fused_to_suggestions(cat, [0.1, 0.2, 0.3], duration_ms=300)
        self.assertEqual(sug, [])


class TestRequiredLabels(unittest.TestCase):

    def test_collects_unique_labels_across_categories(self):
        cats = [
            {"name": "A", "enabled": True, "detectors": [
                {"id": "audio.yamnet", "params": {"labels": ["Scream", "Yell"]}}]},
            {"name": "B", "enabled": True, "detectors": [
                {"id": "audio.yamnet", "params": {"labels": ["Yell", "Bang"]}}]},
            {"name": "C", "enabled": False, "detectors": [
                {"id": "audio.yamnet", "params": {"labels": ["Should-be-skipped"]}}]},
        ]
        labels = required_labels(cats)
        self.assertEqual(set(labels), {"Scream", "Yell", "Bang"})


class TestMergeOverlapping(unittest.TestCase):

    def test_merges_overlap_within_same_category(self):
        sugs = [
            {"category": "A", "startMs": 0,    "endMs": 1000, "score": 0.7, "reasons": ["a"]},
            {"category": "A", "startMs": 800,  "endMs": 1500, "score": 0.8, "reasons": ["b"]},
        ]
        merged = merge_overlapping_in_category(sugs, merge_gap_ms=0)
        self.assertEqual(len(merged), 1)
        self.assertEqual(merged[0]["startMs"], 0)
        self.assertEqual(merged[0]["endMs"], 1500)
        self.assertAlmostEqual(merged[0]["score"], 0.8)

    def test_does_not_merge_different_categories(self):
        sugs = [
            {"category": "A", "startMs": 0,   "endMs": 500, "score": 0.7, "reasons": []},
            {"category": "B", "startMs": 100, "endMs": 600, "score": 0.7, "reasons": []},
        ]
        merged = merge_overlapping_in_category(sugs, merge_gap_ms=0)
        self.assertEqual(len(merged), 2)


if __name__ == "__main__":
    unittest.main()
