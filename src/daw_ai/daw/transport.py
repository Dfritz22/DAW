from __future__ import annotations

from dataclasses import dataclass


@dataclass(slots=True)
class TransportState:
    is_playing: bool = False
    is_recording: bool = False
    playhead_sample: int = 0

    def play(self) -> None:
        self.is_playing = True

    def stop(self) -> None:
        self.is_playing = False
        self.is_recording = False

    def record(self) -> None:
        self.is_playing = True
        self.is_recording = True

    def rewind(self) -> None:
        self.playhead_sample = 0
