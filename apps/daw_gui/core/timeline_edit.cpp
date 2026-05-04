#include "timeline_edit.h"
#include "../state.h"
#include "../ui/layout.h"
#include <algorithm>

static constexpr int kMaxUndoLevels = 50;

// Forward declaration of UpdateWindowTitle from main.cpp
void UpdateWindowTitle(HWND hwnd, const UiState& state);

static InsertEffectArray DefaultInsertEffectsLocal() {
    InsertEffectArray effects{};
    for (int i = 0; i < kMaxInsertSlots; ++i) {
        effects[static_cast<size_t>(i)] = static_cast<std::uint8_t>(i % kInsertEffectTypeCount);
    }
    return effects;
}

static InsertBypassArray DefaultInsertBypassLocal() {
    InsertBypassArray bypass{};
    for (int i = 0; i < kMaxInsertSlots; ++i) {
        bypass[static_cast<size_t>(i)] = false;
    }
    return bypass;
}

static InsertParamsArray DefaultInsertParamsLocal() {
    InsertParamsArray arr;
    return arr;
}

void PushUndo(UiState& state) {
    state.undoStack.push_back({state.clips, state.trackGainDb});
    state.redoStack.clear();
    if (static_cast<int>(state.undoStack.size()) > kMaxUndoLevels) {
        state.undoStack.erase(state.undoStack.begin());
    }
}

void ApplyUndo(HWND hwnd, UiState& state) {
    if (state.undoStack.empty()) return;
    state.redoStack.push_back({state.clips, state.trackGainDb});
    state.clips       = state.undoStack.back().clips;
    state.trackGainDb = state.undoStack.back().trackGainDb;
    state.undoStack.pop_back();
    state.selectedClipIndex = -1;
    state.projectModified = true;
    UpdateWindowTitle(hwnd, state);
}

void ApplyRedo(HWND hwnd, UiState& state) {
    if (state.redoStack.empty()) return;
    state.undoStack.push_back({state.clips, state.trackGainDb});
    state.clips       = state.redoStack.back().clips;
    state.trackGainDb = state.redoStack.back().trackGainDb;
    state.redoStack.pop_back();
    state.selectedClipIndex = -1;
    state.projectModified = true;
    UpdateWindowTitle(hwnd, state);
}

void SplitSelectedClip(UiState& state) {
    if (state.selectedClipIndex < 0 ||
        state.selectedClipIndex >= static_cast<int>(state.clips.size())) return;
    const float splitBeat = state.playheadBeat;
    ClipItem& clip = state.clips[static_cast<size_t>(state.selectedClipIndex)];
    if (splitBeat <= clip.startBeat + 0.01f ||
        splitBeat >= clip.startBeat + clip.lengthBeats - 0.01f) return;

    PushUndo(state);
    // Re-acquire reference after PushUndo (it copies to undoStack but does not reallocate state.clips)
    ClipItem& orig = state.clips[static_cast<size_t>(state.selectedClipIndex)];
    ClipItem right = orig;
    const float splitDelta = splitBeat - orig.startBeat;
    const float spb = SamplesPerBeat(state);
    right.startBeat         = splitBeat;
    right.lengthBeats       = orig.lengthBeats - splitDelta;
    right.sourceOffsetFrames = orig.sourceOffsetFrames + static_cast<std::uint64_t>(splitDelta * spb);
    orig.lengthBeats        = splitDelta;
    state.clips.push_back(right);
    state.selectedClipIndex = -1;
    state.projectModified = true;
}

void DuplicateSelectedClip(UiState& state) {
    if (state.selectedClipIndex < 0 ||
        state.selectedClipIndex >= static_cast<int>(state.clips.size())) return;
    PushUndo(state);
    ClipItem dup = state.clips[static_cast<size_t>(state.selectedClipIndex)];
    dup.startBeat += dup.lengthBeats;
    state.clips.push_back(dup);
    state.selectedClipIndex = static_cast<int>(state.clips.size()) - 1;
    state.projectModified = true;
}

void NudgeSelectedClip(UiState& state, float deltaBeats) {
    if (state.selectedClipIndex < 0 ||
        state.selectedClipIndex >= static_cast<int>(state.clips.size())) return;
    PushUndo(state);
    ClipItem& clip = state.clips[static_cast<size_t>(state.selectedClipIndex)];
    clip.startBeat = (std::max)(0.0f, clip.startBeat + deltaBeats);
    state.projectModified = true;
}

void DeleteSelectedClip(UiState& state) {
    if (state.selectedClipIndex < 0 ||
        state.selectedClipIndex >= static_cast<int>(state.clips.size())) {
        return;
    }
    PushUndo(state);
    state.clips.erase(state.clips.begin() + state.selectedClipIndex);
    state.selectedClipIndex = -1;
    state.projectModified = true;
}

int AddNewTrack(UiState& state) {
    const int index = static_cast<int>(state.tracks.size());
    state.tracks.push_back(L"Track " + std::to_wstring(index + 1));
    state.trackGainDb.push_back(0.0f);
    state.trackMute.push_back(false);
    state.trackSolo.push_back(false);
    state.trackRecordArm.push_back(false);
    state.trackBusIndex.push_back(1);
    state.trackPan.push_back(0.0f);
    state.trackInsertSlots.push_back(0);
    state.trackInsertEffects.push_back(DefaultInsertEffectsLocal());
    state.trackInsertBypass.push_back(DefaultInsertBypassLocal());
    state.trackInsertParams.push_back(DefaultInsertParamsLocal());
    return index;
}

void DeleteTrackAt(UiState& state, int trackIndex) {
    if (trackIndex < 0 || trackIndex >= static_cast<int>(state.tracks.size())) {
        return;
    }

    EnterCriticalSection(&state.audioStateLock);

    state.tracks.erase(state.tracks.begin() + trackIndex);
    if (trackIndex < static_cast<int>(state.trackGainDb.size())) {
        state.trackGainDb.erase(state.trackGainDb.begin() + trackIndex);
    }
    if (trackIndex < static_cast<int>(state.trackMute.size())) {
        state.trackMute.erase(state.trackMute.begin() + trackIndex);
    }
    if (trackIndex < static_cast<int>(state.trackSolo.size())) {
        state.trackSolo.erase(state.trackSolo.begin() + trackIndex);
    }
    if (trackIndex < static_cast<int>(state.trackRecordArm.size())) {
        state.trackRecordArm.erase(state.trackRecordArm.begin() + trackIndex);
    }
    if (trackIndex < static_cast<int>(state.trackBusIndex.size())) {
        state.trackBusIndex.erase(state.trackBusIndex.begin() + trackIndex);
    }
    if (trackIndex < static_cast<int>(state.trackPan.size())) {
        state.trackPan.erase(state.trackPan.begin() + trackIndex);
    }
    if (trackIndex < static_cast<int>(state.trackInsertSlots.size())) {
        state.trackInsertSlots.erase(state.trackInsertSlots.begin() + trackIndex);
    }
    if (trackIndex < static_cast<int>(state.trackInsertEffects.size())) {
        state.trackInsertEffects.erase(state.trackInsertEffects.begin() + trackIndex);
    }
    if (trackIndex < static_cast<int>(state.trackInsertBypass.size())) {
        state.trackInsertBypass.erase(state.trackInsertBypass.begin() + trackIndex);
    }
    if (trackIndex < static_cast<int>(state.trackInsertParams.size())) {
        state.trackInsertParams.erase(state.trackInsertParams.begin() + trackIndex);
    }

    for (int i = static_cast<int>(state.clips.size()) - 1; i >= 0; --i) {
        ClipItem& clip = state.clips[static_cast<size_t>(i)];
        if (clip.trackIndex == trackIndex) {
            state.clips.erase(state.clips.begin() + i);
            continue;
        }
        if (clip.trackIndex > trackIndex) {
            clip.trackIndex -= 1;
        }
    }

    if (state.selectedClipIndex >= static_cast<int>(state.clips.size())) {
        state.selectedClipIndex = -1;
    }

    LeaveCriticalSection(&state.audioStateLock);

    if (state.selectedTrackIndex >= static_cast<int>(state.tracks.size())) {
        state.selectedTrackIndex = static_cast<int>(state.tracks.size()) - 1;
    }
    if (state.dragFaderTrack == trackIndex) {
        state.dragFaderTrack = -1;
        state.draggingFader = false;
    } else if (state.dragFaderTrack > trackIndex) {
        state.dragFaderTrack -= 1;
    }
    if (!state.dragPanIsBus && state.dragPanIndex == trackIndex) {
        state.dragPanIndex = -1;
        state.draggingPan = false;
    } else if (!state.dragPanIsBus && state.dragPanIndex > trackIndex) {
        state.dragPanIndex -= 1;
    }
}
