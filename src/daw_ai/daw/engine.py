from __future__ import annotations

from dataclasses import dataclass

import numpy as np

from .project import DawProject
from .timeline import Timeline
from .transport import TransportState


@dataclass(slots=True)
class DawEngine:
    project: DawProject
    timeline: Timeline
    transport: TransportState

    @classmethod
    def from_project(cls, project: DawProject) -> "DawEngine":
        timeline = Timeline(
            tempo_bpm=project.tempo_bpm,
            sample_rate=project.sample_rate,
            beats_per_bar=project.time_signature_num,
        )
        return cls(project=project, timeline=timeline, transport=TransportState())

    def render_stub(self, bars: int = 4) -> np.ndarray:
        """
        Placeholder render graph.
        Returns silent stereo so the engine contract is testable now.
        """
        total_beats = max(1, bars) * self.timeline.beats_per_bar
        n_samples = total_beats * self.timeline.samples_per_beat
        return np.zeros((n_samples, 2), dtype=np.float32)
