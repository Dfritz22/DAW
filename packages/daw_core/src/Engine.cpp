#include "daw_core/Engine.hpp"

#include <algorithm>
#include <cmath>

namespace daw_core {

Timeline::Timeline(float tempoBpm, std::int32_t sampleRate, std::int32_t beatsPerBar)
    : tempoBpm_(tempoBpm), sampleRate_(sampleRate), beatsPerBar_(beatsPerBar) {}

float Timeline::secondsPerBeat() const {
    return 60.0f / tempoBpm_;
}

std::int64_t Timeline::samplesPerBeat() const {
    return static_cast<std::int64_t>(std::llround(secondsPerBeat() * static_cast<float>(sampleRate_)));
}

std::int64_t Timeline::barsBeatsToSample(std::int32_t bar, std::int32_t beat) const {
    const auto safeBar = std::max(1, bar);
    const auto safeBeat = std::max(1, beat);
    const auto totalBeats = static_cast<std::int64_t>((safeBar - 1) * beatsPerBar_ + (safeBeat - 1));
    return totalBeats * samplesPerBeat();
}

void Transport::play() {
    isPlaying_ = true;
}

void Transport::stop() {
    isPlaying_ = false;
    isRecording_ = false;
}

void Transport::record() {
    isPlaying_ = true;
    isRecording_ = true;
}

void Transport::rewind() {
    playheadSample_ = 0;
}

Engine::Engine(Project project)
    : project_(std::move(project)),
      timeline_(project_.tempoBpm, project_.sampleRate, project_.timeSignatureNum) {}

std::vector<float> Engine::renderStub(int bars) const {
    const auto safeBars = std::max(1, bars);
    const auto totalBeats = static_cast<std::int64_t>(safeBars) * project_.timeSignatureNum;
    const auto frames = totalBeats * timeline_.samplesPerBeat();
    std::vector<float> interleavedStereo(static_cast<std::size_t>(frames) * 2U, 0.0f);
    return interleavedStereo;
}

} // namespace daw_core
