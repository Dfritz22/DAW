#include "core/state.h"
#include <algorithm>
#include <cmath>

float SamplesPerBeat(const UiState& state) {
    int sr = state.project.projectSampleRate;
    if (sr <= 0) sr = state.activeDeviceSampleRate;
    if (sr <= 0) sr = state.preferredSampleRate;
    if (sr <= 0) sr = 1;
    return static_cast<float>(sr) * 60.0f / state.project.bpm;
}

std::uint64_t FramesFromBeats(const UiState& state, float beat) {
    return static_cast<std::uint64_t>(
    std::llround(static_cast<double>(beat) * static_cast<double>(SamplesPerBeat(state))));
}

float BeatsFromFrames(const UiState& state, std::uint64_t frame) {
    const float spb = std::max(1.0f, SamplesPerBeat(state));
    return static_cast<float>(frame) / spb;
}
