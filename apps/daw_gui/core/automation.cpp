#include "../state.h"
#include "core/timeline.h"
#include <algorithm>
#include <cmath>

float TrackGainDbAt(const UiState& state, int trackIndex, float beat) {
    (void)beat;
    if (trackIndex < 0 || trackIndex >= static_cast<int>(state.project.tracks.size())) {
        return 0.0f;
    }
    return state.project.tracks[static_cast<size_t>(trackIndex)].gainDb;
}

float TrackPanAt(const UiState& state, int trackIndex, float beat) {
    (void)beat;
    if (trackIndex < 0 || trackIndex >= static_cast<int>(state.project.tracks.size())) {
        return 0.0f;
    }
    return std::clamp(state.project.tracks[static_cast<size_t>(trackIndex)].pan, -1.0f, 1.0f);
}

int TrackBusIndexAt(const UiState& state, int trackIndex, float beat) {
    (void)beat;
    if (trackIndex < 0 || trackIndex >= static_cast<int>(state.project.tracks.size())) {
        return 1;  // default to Music bus
    }
    return std::clamp(state.project.tracks[static_cast<size_t>(trackIndex)].busIndex, 0, kBusCount - 1);
}
