#include "core/CoreState.h"
#include "core/timeline_math.h"

// Thin adapter: pure math lives in libs/core. The app sees CoreState-shaped
// helpers; this file only does the field lookup.

float TimelineSamplesPerBeat(const CoreState& state) {
    return daw::core::SamplesPerBeat(state.project.projectSampleRate, state.project.bpm);
}

std::uint64_t TimelineFramesFromBeats(const CoreState& state, float beat) {
    return daw::core::FramesFromBeats(state.project.projectSampleRate, state.project.bpm, beat);
}

float TimelineBeatsFromFrames(const CoreState& state, std::uint64_t frame) {
    return daw::core::BeatsFromFrames(state.project.projectSampleRate, state.project.bpm, frame);
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
