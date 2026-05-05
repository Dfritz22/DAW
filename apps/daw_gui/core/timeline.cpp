#include "timeline.h"

float TrackGainDbAt(const UiState& state, int trackIndex) {
    if (trackIndex < 0 || trackIndex >= static_cast<int>(state.project.tracks.size())) {
        return 0.0f;
    }
    return state.project.tracks[static_cast<size_t>(trackIndex)].gainDb;
}

int TrackBusIndexAt(const UiState& state, int trackIndex) {
    if (trackIndex < 0 || trackIndex >= static_cast<int>(state.project.tracks.size())) {
        return 1;  // default to Music bus
    }
    return std::clamp(state.project.tracks[static_cast<size_t>(trackIndex)].busIndex, 0, kBusCount - 1);
}

float TrackPanAt(const UiState& state, int trackIndex) {
    if (trackIndex < 0 || trackIndex >= static_cast<int>(state.project.tracks.size())) {
        return 0.0f;
    }
    return std::clamp(state.project.tracks[static_cast<size_t>(trackIndex)].pan, -1.0f, 1.0f);
}

float SamplesPerBeat(const UiState& state) {
    int sr = state.project.projectSampleRate;
    if (sr <= 0) sr = state.activeDeviceSampleRate;
    if (sr <= 0) sr = state.preferredSampleRate;
    if (sr <= 0) sr = 1;
    return static_cast<float>(sr) * 60.0f / state.project.bpm;
}
