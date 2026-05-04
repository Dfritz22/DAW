from __future__ import annotations

from dataclasses import dataclass


@dataclass(slots=True)
class Timeline:
    tempo_bpm: float
    sample_rate: int
    beats_per_bar: int = 4

    @property
    def seconds_per_beat(self) -> float:
        return 60.0 / self.tempo_bpm

    @property
    def samples_per_beat(self) -> int:
        return int(self.seconds_per_beat * self.sample_rate)

    def bars_beats_to_sample(self, bar: int, beat: int = 1) -> int:
        # DAW timelines are one-based for user-facing musical positions.
        total_beats = (bar - 1) * self.beats_per_bar + (beat - 1)
        return total_beats * self.samples_per_beat
