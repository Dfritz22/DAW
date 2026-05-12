#include "core/timeline_math.h"

#include <algorithm>
#include <cmath>

namespace daw::core {

namespace {

inline int SafeSampleRate(int sr) { return sr > 0 ? sr : 1; }
inline float SafeBpm(float bpm) { return bpm > 0.0f ? bpm : 1.0f; }

} // namespace

float SamplesPerBeat(int sampleRate, float bpm) {
    return static_cast<float>(SafeSampleRate(sampleRate)) * 60.0f / SafeBpm(bpm);
}

std::uint64_t FramesFromBeats(int sampleRate, float bpm, float beat) {
    const double spb = static_cast<double>(SamplesPerBeat(sampleRate, bpm));
    const double frames = static_cast<double>(beat) * spb;
    if (!(frames > 0.0)) return 0;  // negative or NaN clamp to 0
    return static_cast<std::uint64_t>(std::llround(frames));
}

float BeatsFromFrames(int sampleRate, float bpm, std::uint64_t frame) {
    const float spb = std::max(1.0f, SamplesPerBeat(sampleRate, bpm));
    return static_cast<float>(frame) / spb;
}

} // namespace daw::core
