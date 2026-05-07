"""Tests for the M7.6.b loader: vision_clip._load_feedback_vectors must
merge ~/.censorcut/feedback.jsonl (your own decisions) with
~/.censorcut/peers.jsonl (peer-derived rows), weighting peer rows by
the trust score from ~/.censorcut/trust.json.

The CLIP detector itself isn't tested here (needs torch + CUDA + a
real model). We mock torch/numpy with thin stand-ins and test the
loader's I/O + weighting logic in isolation."""

from __future__ import annotations

import json
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))


class FakeTensor:
    def __init__(self, data):
        self.data = list(data)
    @property
    def shape(self):
        if not self.data:
            return (0,)
        if isinstance(self.data[0], list):
            return (len(self.data), len(self.data[0]))
        return (len(self.data),)


class FakeTorch:
    def from_numpy(self, arr):
        return FakeTensor(arr.tolist())
    def tensor(self, data, dtype=None):
        return FakeTensor(list(data))
    float32 = "float32"


class FakeNumpy:
    float32 = "float32"
    class linalg:
        @staticmethod
        def norm(arr, axis=None, keepdims=False):
            import math
            class _R:
                def __init__(self, vals):
                    self.vals = vals
                def __getitem__(self, _):
                    return self.vals[0]
                def __setitem__(self, key, value):
                    pass
            rows = arr.tolist() if hasattr(arr, "tolist") else arr
            ns = [math.sqrt(sum(x*x for x in r)) for r in rows]
            class _Wrap:
                def __init__(self, vals):
                    self.vals = vals
                def __eq__(self, other):
                    return [v == other for v in self.vals]
                def __getitem__(self, mask):
                    return self.vals
                def __setitem__(self, mask, value):
                    self.vals = [value if m else v for m, v in zip(mask, self.vals)]
                def tolist(self):
                    return self.vals
            return _Wrap([[n] for n in ns])
    @staticmethod
    def asarray(rows, dtype=None):
        class _A:
            def __init__(self, rs):
                self.rs = rs
            def tolist(self):
                return self.rs
            def __truediv__(self, denom):
                return _A(self.rs)
            @property
            def shape(self):
                return (len(self.rs), len(self.rs[0]) if self.rs else 0)
        return _A(rows)


class TestPeerFeedbackLoader(unittest.TestCase):
    def setUp(self):
        self.tmpdir = tempfile.TemporaryDirectory()
        self.home = Path(self.tmpdir.name)
        (self.home / ".censorcut").mkdir()
        # Patch HOME to redirect ~/.censorcut/* to the tmpdir.
        self._env_patcher = patch.dict(os.environ, {"HOME": str(self.home),
                                                    "USERPROFILE": str(self.home)})
        self._env_patcher.start()

    def tearDown(self):
        self._env_patcher.stop()
        self.tmpdir.cleanup()

    def _write_jsonl(self, path: Path, rows):
        with open(path, "w", encoding="utf-8") as f:
            for r in rows:
                f.write(json.dumps(r) + "\n")

    def _write_json(self, path: Path, obj):
        with open(path, "w", encoding="utf-8") as f:
            json.dump(obj, f)

    def test_local_only(self):
        """When only feedback.jsonl exists, all rows have weight 1.0
        and empty author."""
        from censorcut.detectors import vision_clip
        self._write_jsonl(self.home / ".censorcut" / "feedback.jsonl", [
            {"vec": [1.0, 0.0, 0.0], "decision": "reject"},
            {"vec": [0.0, 1.0, 0.0], "decision": "accept"},
        ])
        state, total = vision_clip._load_feedback_vectors(FakeTorch(), FakeNumpy())
        self.assertEqual(total, 2)
        self.assertIsNotNone(state["reject"])
        self.assertIsNotNone(state["accept"])
        self.assertEqual(list(state["reject_w"].data), [1.0])
        self.assertEqual(list(state["accept_w"].data), [1.0])
        self.assertEqual(state["reject_authors"], [""])
        self.assertEqual(state["accept_authors"], [""])

    def test_peers_weighted_by_trust(self):
        """Peer rows are weighted by trust.json. Unknown peers get 0.1
        floor; below 0.05 they're dropped."""
        from censorcut.detectors import vision_clip
        self._write_jsonl(self.home / ".censorcut" / "feedback.jsonl", [])
        self._write_json(self.home / ".censorcut" / "trust.json", {
            "direct": {
                "alice": {"score": 1.5, "n": 10},
                "carol": {"score": 0.02, "n": 5},  # below drop floor
            },
        })
        self._write_jsonl(self.home / ".censorcut" / "peers.jsonl", [
            {"vec": [1.0, 0.0, 0.0], "decision": "accept", "peer_key": "alice"},
            {"vec": [0.0, 1.0, 0.0], "decision": "accept", "peer_key": "bob"},     # unseen
            {"vec": [0.0, 0.0, 1.0], "decision": "reject", "peer_key": "carol"},  # dropped
        ])
        state, total = vision_clip._load_feedback_vectors(FakeTorch(), FakeNumpy())
        # alice + bob = 2 accept rows; carol dropped.
        self.assertEqual(total, 2)
        accepts = list(state["accept_w"].data)
        self.assertIn(1.5, accepts)   # alice
        self.assertIn(0.1, accepts)   # bob (floor)
        self.assertEqual(state["reject"], None)  # carol was dropped
        self.assertEqual(set(state["accept_authors"]), {"alice", "bob"})

    def test_local_and_peer_merge(self):
        """Both files get merged; authors are local "" + peer pubkey."""
        from censorcut.detectors import vision_clip
        self._write_jsonl(self.home / ".censorcut" / "feedback.jsonl", [
            {"vec": [1.0, 0.0], "decision": "accept"},
        ])
        self._write_jsonl(self.home / ".censorcut" / "peers.jsonl", [
            {"vec": [0.0, 1.0], "decision": "accept", "peer_key": "alice"},
        ])
        # No trust file → alice gets the floor 0.1.
        state, total = vision_clip._load_feedback_vectors(FakeTorch(), FakeNumpy())
        self.assertEqual(total, 2)
        self.assertEqual(list(state["accept_w"].data), [1.0, 0.1])
        self.assertEqual(state["accept_authors"], ["", "alice"])

    def test_trust_load_ignores_zero_interactions(self):
        """A trust.json entry with n=0 must not be applied — those are
        bootstrap-only, never confirmed direct."""
        from censorcut.detectors import vision_clip
        self._write_json(self.home / ".censorcut" / "trust.json", {
            "direct": {"alice": {"score": 1.5, "n": 0}}
        })
        scores = vision_clip._load_trust_scores()
        self.assertNotIn("alice", scores)


if __name__ == "__main__":
    unittest.main()
