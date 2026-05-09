#include "core/CoreState.h"
#include <algorithm>
#include <cmath>

float TimelineSamplesPerBeat(const CoreState& state) {
    int sr = state.project.projectSampleRate;
    if (sr <= 0) sr = 1;
    return static_cast<float>(sr) * 60.0f / state.project.bpm;
}

std::uint64_t TimelineFramesFromBeats(const CoreState& state, float beat) {
    return static_cast<std::uint64_t>(
    std::llround(static_cast<double>(beat) * static_cast<double>(TimelineSamplesPerBeat(state))));
}

float TimelineBeatsFromFrames(const CoreState& state, std::uint64_t frame) {
    const float spb = std::max(1.0f, TimelineSamplesPerBeat(state));
    return static_cast<float>(frame) / spb;
}

float SamplesPerBeat(const CoreState& state) {
    return TimelineSamplesPerBeat(state);
}

std::uint64_t FramesFromBeats(const CoreState& state, float beat) {
    return TimelineFramesFromBeats(state, beat);
}

float BeatsFromFrames(const CoreState& state, std::uint64_t frame) {
    return TimelineBeatsFromFrames(state, frame);
}
