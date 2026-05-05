"""Common types for detectors.

A detector is a callable that consumes the source video and returns a dict
of named score series. Series keys are namespaced strings like
``audio.lufs.momentary`` or ``audio.yamnet.Scream``. Values are float lists
sampled at a fixed period (in milliseconds).

The dict shape is plain JSON-friendly so the same structure flows from
detector → fusion → output JSON without any conversion.
"""

from __future__ import annotations

from typing import Callable, Dict, List, Optional, TypedDict


class Series(TypedDict):
    """One time-series of detector scores."""
    period_ms: int
    values:    List[float]


# Map of series key -> Series dict.
DetectorOutput = Dict[str, Series]

# Optional progress callback: fraction in [0, 1] and a phase label.
ProgressFn = Optional[Callable[[float, str], None]]


def empty_series(period_ms: int = 100) -> Series:
    return {"period_ms": period_ms, "values": []}
