"""Category recipes + fusion.

A category is a JSON dict like::

    {
      "name": "Jump scare",
      "enabled": true,
      "threshold": 0.5,
      "padBeforeMs": 1000,
      "padAfterMs": 1000,
      "detectors": [
        {"id": "audio.lufs.jump", "weight": 0.7},
        {"id": "audio.yamnet", "weight": 0.4,
         "params": {"labels": ["Scream", "Bang", "Crash"]}}
      ]
    }

For each detector entry, we take the *max* score across the labels it
references (so multiple labels in one entry behave as an OR), multiply by
the entry's weight, then sum across detectors and clamp to 1.0 to get the
category's per-frame fused score.
"""

from __future__ import annotations

import json
from pathlib import Path
from typing import Dict, List, Optional

from .detectors.base import DetectorOutput, Series

DEFAULT_CATEGORIES_PATH = Path(__file__).parent / "data" / "default_categories.json"

DEFAULT_FUSE_PERIOD_MS = 100


def load_categories(path: Optional[Path] = None) -> List[dict]:
    p = Path(path) if path else DEFAULT_CATEGORIES_PATH
    with open(p, "r", encoding="utf-8") as f:
        data = json.load(f)
    cats = data.get("categories") if isinstance(data, dict) else data
    if not isinstance(cats, list):
        raise ValueError(f"categories file is malformed: {p}")
    return cats


def required_labels(categories: List[dict], detector_id: str = "audio.yamnet") -> List[str]:
    """Union of all label strings any enabled category needs from the named
    detector. Used to pre-prune YAMNet's 521 outputs to just the labels we
    actually care about."""
    seen: List[str] = []
    out: List[str] = []
    for cat in categories:
        if not cat.get("enabled", True):
            continue
        for det in cat.get("detectors", []):
            if det.get("id") != detector_id:
                continue
            labels = det.get("params", {}).get("labels", []) or []
            for lbl in labels:
                if lbl not in seen:
                    seen.append(lbl)
                    out.append(lbl)
    return out


def _resample(values: List[float], src_period_ms: int, dst_period_ms: int,
              dst_n: int) -> List[float]:
    """Nearest-sample resample of ``values`` to a series of length ``dst_n``
    at ``dst_period_ms``."""
    if src_period_ms == dst_period_ms and len(values) >= dst_n:
        return values[:dst_n]
    if not values:
        return [0.0] * dst_n
    out: List[float] = [0.0] * dst_n
    src_n = len(values)
    for i in range(dst_n):
        t_ms = i * dst_period_ms
        src_idx = int(t_ms / src_period_ms)
        if src_idx >= src_n:
            src_idx = src_n - 1
        out[i] = values[src_idx]
    return out


def fuse_category(category: dict,
                  series_by_key: Dict[str, Series],
                  *,
                  fuse_period_ms: int = DEFAULT_FUSE_PERIOD_MS,
                  duration_ms: int = 0) -> List[float]:
    """Compute the per-frame fused score for one category, on the
    ``fuse_period_ms`` grid."""
    if not category.get("enabled", True):
        return []

    # Determine the output length: prefer duration; else longest input series.
    if duration_ms > 0:
        n = duration_ms // fuse_period_ms
    else:
        n = 0
        for s in series_by_key.values():
            sn = len(s.get("values", []))
            sp = s.get("period_ms", fuse_period_ms)
            n = max(n, sn * sp // fuse_period_ms)
    if n <= 0:
        return []

    fused: List[float] = [0.0] * n

    for det in category.get("detectors", []):
        det_id  = det.get("id")
        weight  = float(det.get("weight", 1.0))
        labels  = det.get("params", {}).get("labels", []) or []

        # Two flavors:
        #   1. A direct series whose key IS det_id (e.g. "audio.lufs.jump").
        #   2. A label-keyed family like "audio.yamnet" + labels=[...].
        contribution: Optional[List[float]] = None
        if det_id in series_by_key:
            s = series_by_key[det_id]
            contribution = _resample(s["values"], s["period_ms"], fuse_period_ms, n)
        elif labels:
            # Take the max across the requested labels at each frame.
            for lbl in labels:
                key = f"{det_id}.{lbl}"
                s = series_by_key.get(key)
                if not s:
                    continue
                resampled = _resample(s["values"], s["period_ms"], fuse_period_ms, n)
                if contribution is None:
                    contribution = list(resampled)
                else:
                    for i in range(n):
                        if resampled[i] > contribution[i]:
                            contribution[i] = resampled[i]
        if contribution is None:
            continue

        for i in range(n):
            fused[i] += weight * contribution[i]

    # Clamp to 1.0
    for i in range(n):
        if fused[i] > 1.0:
            fused[i] = 1.0

    return fused


def fused_to_suggestions(category: dict,
                         fused: List[float],
                         *,
                         fuse_period_ms: int = DEFAULT_FUSE_PERIOD_MS,
                         duration_ms: int = 0) -> List[dict]:
    """Find contiguous runs above the category's threshold, pad before/after,
    and emit one suggestion per run."""
    if not fused:
        return []
    threshold   = float(category.get("threshold", 0.5))
    pad_before  = int(category.get("padBeforeMs", 0)) // fuse_period_ms
    pad_after   = int(category.get("padAfterMs", 0))  // fuse_period_ms
    name        = category.get("name", "Unnamed")

    out: List[dict] = []
    n = len(fused)
    i = 0
    while i < n:
        if fused[i] < threshold:
            i += 1
            continue
        j = i
        while j < n and fused[j] >= threshold:
            j += 1
        peak = max(fused[i:j])
        start_idx = max(0, i - pad_before)
        end_idx   = min(n, j + pad_after)
        start_ms  = start_idx * fuse_period_ms
        end_ms    = end_idx   * fuse_period_ms
        if duration_ms > 0:
            end_ms = min(end_ms, duration_ms)
        if end_ms > start_ms:
            out.append({
                "category": name,
                "startMs":  start_ms,
                "endMs":    end_ms,
                "score":    round(peak, 3),
                "reasons":  [f"fused score peaked at {peak:.2f}"],
            })
        i = j
    return out


def merge_overlapping_in_category(suggestions: List[dict],
                                  *,
                                  merge_gap_ms: int = 0) -> List[dict]:
    """Sort and merge overlapping/adjacent suggestions of the *same category*."""
    if not suggestions:
        return []
    by_cat: Dict[str, List[dict]] = {}
    for s in suggestions:
        by_cat.setdefault(s["category"], []).append(s)
    out: List[dict] = []
    for cat, lst in by_cat.items():
        lst.sort(key=lambda s: (s["startMs"], s["endMs"]))
        cur = dict(lst[0])
        for s in lst[1:]:
            if s["startMs"] - cur["endMs"] <= merge_gap_ms:
                cur["endMs"] = max(cur["endMs"], s["endMs"])
                cur["score"] = round(max(cur["score"], s["score"]), 3)
                cur["reasons"] = (cur["reasons"] or []) + (s["reasons"] or [])
            else:
                out.append(cur)
                cur = dict(s)
        out.append(cur)
    out.sort(key=lambda s: (s["startMs"], s["endMs"]))
    return out
