#include "timeline_edit.h"
#include "core/timeline.h"

#include <algorithm>

static constexpr int kMaxUndoLevels = 50;

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

void PushUndo(CoreState& state) {
    std::vector<float> gainSnap;
    gainSnap.reserve(state.project.tracks.size());
    for (const auto& t : state.project.tracks) gainSnap.push_back(t.gainDb);
    state.undoStack.push_back({state.project.clips, gainSnap});
    state.redoStack.clear();
    if (static_cast<int>(state.undoStack.size()) > kMaxUndoLevels) {
        state.undoStack.erase(state.undoStack.begin());
    }
}

bool ApplyUndo(CoreState& state) {
    if (state.undoStack.empty()) return false;
    std::vector<float> gainSnap;
    gainSnap.reserve(state.project.tracks.size());
    for (const auto& t : state.project.tracks) gainSnap.push_back(t.gainDb);
    state.redoStack.push_back({state.project.clips, gainSnap});
    state.project.clips = state.undoStack.back().clips;
    const auto& savedGain = state.undoStack.back().trackGainDb;
    for (size_t i = 0; i < state.project.tracks.size() && i < savedGain.size(); ++i) {
        state.project.tracks[i].gainDb = savedGain[i];
    }
    state.undoStack.pop_back();
    state.projectModified = true;
    return true;
}

bool ApplyRedo(CoreState& state) {
    if (state.redoStack.empty()) return false;
    std::vector<float> gainSnap;
    gainSnap.reserve(state.project.tracks.size());
    for (const auto& t : state.project.tracks) gainSnap.push_back(t.gainDb);
    state.undoStack.push_back({state.project.clips, gainSnap});
    state.project.clips = state.redoStack.back().clips;
    const auto& savedGain = state.redoStack.back().trackGainDb;
    for (size_t i = 0; i < state.project.tracks.size() && i < savedGain.size(); ++i) {
        state.project.tracks[i].gainDb = savedGain[i];
    }
    state.redoStack.pop_back();
    state.projectModified = true;
    return true;
}

bool SplitSelectedClip(CoreState& state, int selectedClipIndex, float playheadBeat) {
    if (selectedClipIndex < 0 ||
        selectedClipIndex >= static_cast<int>(state.project.clips.size())) return false;
    const float splitBeat = playheadBeat;
    ClipItem& clip = state.project.clips[static_cast<size_t>(selectedClipIndex)];
    if (splitBeat <= clip.startBeat + 0.01f ||
        splitBeat >= clip.startBeat + clip.lengthBeats - 0.01f) return false;

    PushUndo(state);
    ClipItem& orig = state.project.clips[static_cast<size_t>(selectedClipIndex)];
    ClipItem right = orig;
    const float splitDelta = splitBeat - orig.startBeat;
    const float spb = TimelineSamplesPerBeat(state);
    right.startBeat         = splitBeat;
    right.lengthBeats       = orig.lengthBeats - splitDelta;
    right.sourceOffsetFrames = orig.sourceOffsetFrames + static_cast<std::uint64_t>(splitDelta * spb);
    orig.lengthBeats        = splitDelta;
    state.project.clips.push_back(right);
    state.projectModified = true;
    return true;
}

int DuplicateSelectedClip(CoreState& state, int selectedClipIndex) {
    if (selectedClipIndex < 0 ||
        selectedClipIndex >= static_cast<int>(state.project.clips.size())) return -1;
    PushUndo(state);
    ClipItem dup = state.project.clips[static_cast<size_t>(selectedClipIndex)];
    dup.startBeat += dup.lengthBeats;
    state.project.clips.push_back(dup);
    state.projectModified = true;
    return static_cast<int>(state.project.clips.size()) - 1;
}

bool NudgeSelectedClip(CoreState& state, int selectedClipIndex, float deltaBeats) {
    if (selectedClipIndex < 0 ||
        selectedClipIndex >= static_cast<int>(state.project.clips.size())) return false;
    PushUndo(state);
    ClipItem& clip = state.project.clips[static_cast<size_t>(selectedClipIndex)];
    clip.startBeat = (std::max)(0.0f, clip.startBeat + deltaBeats);
    state.projectModified = true;
    return true;
}

bool DeleteSelectedClip(CoreState& state, int selectedClipIndex) {
    if (selectedClipIndex < 0 ||
        selectedClipIndex >= static_cast<int>(state.project.clips.size())) {
        return false;
    }
    PushUndo(state);
    state.project.clips.erase(state.project.clips.begin() + selectedClipIndex);
    state.projectModified = true;
    return true;
}

int AddNewTrack(CoreState& state) {
    const int index = static_cast<int>(state.project.tracks.size());
    TrackData newTrack{};
    newTrack.name = L"Track " + std::to_wstring(index + 1);
    newTrack.busIndex = 1;
    newTrack.insertEffects = DefaultInsertEffectsLocal();
    newTrack.insertBypass  = DefaultInsertBypassLocal();
    newTrack.insertConfig  = DefaultInsertConfigLocal();
    state.project.tracks.push_back(std::move(newTrack));
    state.projectModified = true;
    return index;
}

bool DeleteTrackAt(CoreState& state, int trackIndex) {
    if (trackIndex < 0 || trackIndex >= static_cast<int>(state.project.tracks.size())) {
        return false;
    }

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

    state.projectModified = true;
    return true;
}
