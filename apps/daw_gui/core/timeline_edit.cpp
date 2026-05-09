#include "timeline_edit.h"
#include "core/state.h"
#include "core/timeline.h"
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

static InsertConfigArray DefaultInsertConfigLocal() {
    InsertConfigArray arr;
    return arr;
}

void PushUndo(UiState& state) {
    std::vector<float> gainSnap;
    gainSnap.reserve(state.project.tracks.size());
    for (const auto& t : state.project.tracks) gainSnap.push_back(t.gainDb);
    state.undoStack.push_back({state.project.clips, gainSnap});
    state.redoStack.clear();
    if (static_cast<int>(state.undoStack.size()) > kMaxUndoLevels) {
        state.undoStack.erase(state.undoStack.begin());
    }
}

void ApplyUndo(HWND hwnd, UiState& state) {
    if (state.undoStack.empty()) return;
    std::vector<float> gainSnap;
    gainSnap.reserve(state.project.tracks.size());
    for (const auto& t : state.project.tracks) gainSnap.push_back(t.gainDb);
    state.redoStack.push_back({state.project.clips, gainSnap});
    state.project.clips = state.undoStack.back().clips;
    const auto& savedGain = state.undoStack.back().trackGainDb;
    for (size_t i = 0; i < state.project.tracks.size() && i < savedGain.size(); ++i)
        state.project.tracks[i].gainDb = savedGain[i];
    state.undoStack.pop_back();
    state.selectedClipIndex = -1;
    state.projectModified = true;
    UpdateWindowTitle(hwnd, state);
}

void ApplyRedo(HWND hwnd, UiState& state) {
    if (state.redoStack.empty()) return;
    std::vector<float> gainSnap;
    gainSnap.reserve(state.project.tracks.size());
    for (const auto& t : state.project.tracks) gainSnap.push_back(t.gainDb);
    state.undoStack.push_back({state.project.clips, gainSnap});
    state.project.clips = state.redoStack.back().clips;
    const auto& savedGain = state.redoStack.back().trackGainDb;
    for (size_t i = 0; i < state.project.tracks.size() && i < savedGain.size(); ++i)
        state.project.tracks[i].gainDb = savedGain[i];
    state.redoStack.pop_back();
    state.selectedClipIndex = -1;
    state.projectModified = true;
    UpdateWindowTitle(hwnd, state);
}

void SplitSelectedClip(UiState& state) {
    if (state.selectedClipIndex < 0 ||
        state.selectedClipIndex >= static_cast<int>(state.project.clips.size())) return;
    const float splitBeat = state.playheadBeat;
    ClipItem& clip = state.project.clips[static_cast<size_t>(state.selectedClipIndex)];
    if (splitBeat <= clip.startBeat + 0.01f ||
        splitBeat >= clip.startBeat + clip.lengthBeats - 0.01f) return;

    PushUndo(state);
    // Re-acquire reference after PushUndo (it copies to undoStack but does not reallocate state.project.clips)
    ClipItem& orig = state.project.clips[static_cast<size_t>(state.selectedClipIndex)];
    ClipItem right = orig;
    const float splitDelta = splitBeat - orig.startBeat;
    const float spb = SamplesPerBeat(state);
    right.startBeat         = splitBeat;
    right.lengthBeats       = orig.lengthBeats - splitDelta;
    right.sourceOffsetFrames = orig.sourceOffsetFrames + static_cast<std::uint64_t>(splitDelta * spb);
    orig.lengthBeats        = splitDelta;
    state.project.clips.push_back(right);
    state.selectedClipIndex = -1;
    state.projectModified = true;
}

void DuplicateSelectedClip(UiState& state) {
    if (state.selectedClipIndex < 0 ||
        state.selectedClipIndex >= static_cast<int>(state.project.clips.size())) return;
    PushUndo(state);
    ClipItem dup = state.project.clips[static_cast<size_t>(state.selectedClipIndex)];
    dup.startBeat += dup.lengthBeats;
    state.project.clips.push_back(dup);
    state.selectedClipIndex = static_cast<int>(state.project.clips.size()) - 1;
    state.projectModified = true;
}

void NudgeSelectedClip(UiState& state, float deltaBeats) {
    if (state.selectedClipIndex < 0 ||
        state.selectedClipIndex >= static_cast<int>(state.project.clips.size())) return;
    PushUndo(state);
    ClipItem& clip = state.project.clips[static_cast<size_t>(state.selectedClipIndex)];
    clip.startBeat = (std::max)(0.0f, clip.startBeat + deltaBeats);
    state.projectModified = true;
}

void DeleteSelectedClip(UiState& state) {
    if (state.selectedClipIndex < 0 ||
        state.selectedClipIndex >= static_cast<int>(state.project.clips.size())) {
        return;
    }
    PushUndo(state);
    state.project.clips.erase(state.project.clips.begin() + state.selectedClipIndex);
    state.selectedClipIndex = -1;
    state.projectModified = true;
}

int AddNewTrack(UiState& state) {
    const int index = static_cast<int>(state.project.tracks.size());
    TrackData newTrack{};
    newTrack.name = L"Track " + std::to_wstring(index + 1);
    newTrack.busIndex = 1;
    newTrack.insertEffects = DefaultInsertEffectsLocal();
    newTrack.insertBypass  = DefaultInsertBypassLocal();
    newTrack.insertConfig  = DefaultInsertConfigLocal();
    state.project.tracks.push_back(std::move(newTrack));
    return index;
}

void DeleteTrackAt(UiState& state, int trackIndex) {
    if (trackIndex < 0 || trackIndex >= static_cast<int>(state.project.tracks.size())) {
        return;
    }

    EnterCriticalSection(&state.audioStateLock);

    state.project.tracks.erase(state.project.tracks.begin() + trackIndex);

    for (int i = static_cast<int>(state.project.clips.size()) - 1; i >= 0; --i) {
        ClipItem& clip = state.project.clips[static_cast<size_t>(i)];
        if (clip.trackIndex == trackIndex) {
            state.project.clips.erase(state.project.clips.begin() + i);
            continue;
        }
        if (clip.trackIndex > trackIndex) {
            clip.trackIndex -= 1;
        }
    }

    if (state.selectedClipIndex >= static_cast<int>(state.project.clips.size())) {
        state.selectedClipIndex = -1;
    }

    LeaveCriticalSection(&state.audioStateLock);

    if (state.selectedTrackIndex >= static_cast<int>(state.project.tracks.size())) {
        state.selectedTrackIndex = static_cast<int>(state.project.tracks.size()) - 1;
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
