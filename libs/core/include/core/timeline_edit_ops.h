#pragma once

#include "core/project_data.h"

namespace daw::core {

// ── Pure timeline edits ──────────────────────────────────────────────────────
// Operations on `ProjectData` that don't depend on CoreState/AppState. Undo
// snapshotting is the app's responsibility — these helpers mutate in place
// and report success/failure. All inputs are validated; out-of-range indices
// return false (or -1 for AddTrack callers can rely on size growth).

/// Split clip at `clipIndex` at absolute beat `splitBeat`. Requires the
/// timeline `samplesPerBeat` so the right-hand piece's source-frame offset
/// stays sample-accurate. Returns false if the index is out of range or the
/// split point is too close to either edge (within 0.01 beats).
bool SplitClip(ProjectData& project, int clipIndex, float splitBeat, float samplesPerBeat);

/// Duplicate clip at `clipIndex` placing the copy immediately after it on
/// the timeline. Returns the new clip index, or -1 on failure.
int DuplicateClip(ProjectData& project, int clipIndex);

/// Move clip's start beat by `deltaBeats`, clamping to >= 0. Returns false
/// if the index is out of range.
bool NudgeClip(ProjectData& project, int clipIndex, float deltaBeats);

/// Erase clip at index. Returns false if the index is out of range.
bool DeleteClip(ProjectData& project, int clipIndex);

/// Append a new default-constructed track. Returns the new track's index.
int AppendDefaultTrack(ProjectData& project);

/// Delete the track at `trackIndex`. Erases all clips on that track and
/// shifts higher trackIndex values on remaining clips down by one. Returns
/// false if the index is out of range.
bool DeleteTrack(ProjectData& project, int trackIndex);

} // namespace daw::core
