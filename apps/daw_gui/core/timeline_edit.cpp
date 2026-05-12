#include "timeline_edit.h"
#include "core/timeline.h"
#include "core/timeline_edit_ops.h"

#include <vector>

// CoreState-aware shim. Pure operations live in `daw::core::*`; this file
// owns only the undo-stack snapshotting and `projectModified` bookkeeping.

static constexpr int kMaxUndoLevels = 50;

namespace {

CoreState::UndoEntry SnapshotForUndo(const CoreState& state) {
    CoreState::UndoEntry entry;
    entry.clips = state.project.clips;
    entry.trackGainDb.reserve(state.project.tracks.size());
    for (const auto& t : state.project.tracks) entry.trackGainDb.push_back(t.gainDb);
    return entry;
}

void RestoreFromEntry(CoreState& state, const CoreState::UndoEntry& entry) {
    state.project.clips = entry.clips;
    for (size_t i = 0; i < state.project.tracks.size() && i < entry.trackGainDb.size(); ++i) {
        state.project.tracks[i].gainDb = entry.trackGainDb[i];
    }
}

} // namespace

void PushUndo(CoreState& state) {
    state.undoStack.push_back(SnapshotForUndo(state));
    state.redoStack.clear();
    if (static_cast<int>(state.undoStack.size()) > kMaxUndoLevels) {
        state.undoStack.erase(state.undoStack.begin());
    }
}

bool ApplyUndo(CoreState& state) {
    if (state.undoStack.empty()) return false;
    state.redoStack.push_back(SnapshotForUndo(state));
    RestoreFromEntry(state, state.undoStack.back());
    state.undoStack.pop_back();
    state.projectModified = true;
    return true;
}

bool ApplyRedo(CoreState& state) {
    if (state.redoStack.empty()) return false;
    state.undoStack.push_back(SnapshotForUndo(state));
    RestoreFromEntry(state, state.redoStack.back());
    state.redoStack.pop_back();
    state.projectModified = true;
    return true;
}

bool SplitSelectedClip(CoreState& state, int selectedClipIndex, float playheadBeat) {
    // Bounds-check up front so we don't push an undo entry for a no-op.
    if (selectedClipIndex < 0 ||
        selectedClipIndex >= static_cast<int>(state.project.clips.size())) return false;
    const ClipItem& clip = state.project.clips[static_cast<size_t>(selectedClipIndex)];
    if (playheadBeat <= clip.startBeat + 0.01f ||
        playheadBeat >= clip.startBeat + clip.lengthBeats - 0.01f) return false;

    PushUndo(state);
    const float spb = TimelineSamplesPerBeat(state);
    const bool ok = daw::core::SplitClip(state.project, selectedClipIndex, playheadBeat, spb);
    if (ok) state.projectModified = true;
    return ok;
}

int DuplicateSelectedClip(CoreState& state, int selectedClipIndex) {
    if (selectedClipIndex < 0 ||
        selectedClipIndex >= static_cast<int>(state.project.clips.size())) return -1;
    PushUndo(state);
    const int newIdx = daw::core::DuplicateClip(state.project, selectedClipIndex);
    if (newIdx >= 0) state.projectModified = true;
    return newIdx;
}

bool NudgeSelectedClip(CoreState& state, int selectedClipIndex, float deltaBeats) {
    if (selectedClipIndex < 0 ||
        selectedClipIndex >= static_cast<int>(state.project.clips.size())) return false;
    PushUndo(state);
    const bool ok = daw::core::NudgeClip(state.project, selectedClipIndex, deltaBeats);
    if (ok) state.projectModified = true;
    return ok;
}

bool DeleteSelectedClip(CoreState& state, int selectedClipIndex) {
    if (selectedClipIndex < 0 ||
        selectedClipIndex >= static_cast<int>(state.project.clips.size())) return false;
    PushUndo(state);
    const bool ok = daw::core::DeleteClip(state.project, selectedClipIndex);
    if (ok) state.projectModified = true;
    return ok;
}

int AddNewTrack(CoreState& state) {
    const int index = daw::core::AppendDefaultTrack(state.project);
    state.projectModified = true;
    return index;
}

bool DeleteTrackAt(CoreState& state, int trackIndex) {
    const bool ok = daw::core::DeleteTrack(state.project, trackIndex);
    if (ok) state.projectModified = true;
    return ok;
}
