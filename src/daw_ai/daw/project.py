from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path


@dataclass(slots=True)
class TrackClip:
    track_name: str
    source_file: Path
    start_bar: int = 1
    start_beat: int = 1
    gain_db: float = 0.0
    pan: float = 0.0
    muted: bool = False
    solo: bool = False


@dataclass(slots=True)
class DawProject:
    name: str
    sample_rate: int = 48000
    tempo_bpm: float = 120.0
    time_signature_num: int = 4
    time_signature_den: int = 4
    clips: list[TrackClip] = field(default_factory=list)

    def add_clip(self, clip: TrackClip) -> None:
        self.clips.append(clip)

    def active_clips(self) -> list[TrackClip]:
        solo_clips = [clip for clip in self.clips if clip.solo]
        if solo_clips:
            return [clip for clip in solo_clips if not clip.muted]
        return [clip for clip in self.clips if not clip.muted]
