from __future__ import annotations

from pathlib import Path
from typing import Iterable, List

AUDIO_EXTENSIONS = {".wav", ".flac", ".aiff", ".aif"}


def find_audio_files(input_dir: Path) -> List[Path]:
    files = [
        p
        for p in input_dir.rglob("*")
        if p.is_file() and p.suffix.lower() in AUDIO_EXTENSIONS
    ]
    return sorted(files)


def filter_selected(files: Iterable[Path], selected_names: list[str] | None) -> List[Path]:
    if not selected_names:
        return list(files)
    selected = {name.lower() for name in selected_names}
    return [f for f in files if f.name.lower() in selected]


def parse_groups(group_args: list[str] | None) -> dict[str, list[str]]:
    groups: dict[str, list[str]] = {}
    if not group_args:
        return groups

    for raw in group_args:
        if ":" not in raw:
            continue
        name, members = raw.split(":", 1)
        group_name = name.strip()
        track_names = [m.strip() for m in members.split(",") if m.strip()]
        if group_name and track_names:
            groups[group_name] = track_names
    return groups
