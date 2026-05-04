from __future__ import annotations

from dataclasses import asdict, dataclass
from typing import Dict, List


@dataclass
class TrackFeatures:
    name: str
    path: str
    sample_rate: int
    duration_sec: float
    peak_dbfs: float
    rms_dbfs: float
    lufs_integrated: float
    crest_db: float
    clipping_ratio: float
    spectral_centroid_hz: float
    low_band_ratio: float
    presence_band_ratio: float
    air_band_ratio: float


@dataclass
class Recommendation:
    scope: str
    target: str
    category: str
    action: str
    confidence: float
    rationale: str


@dataclass
class AnalysisResult:
    tracks: List[TrackFeatures]
    recommendations: List[Recommendation]

    def to_dict(self) -> Dict:
        return {
            "tracks": [asdict(t) for t in self.tracks],
            "recommendations": [asdict(r) for r in self.recommendations],
        }
