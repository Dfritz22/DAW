#include "dsp/metronome.h"

#include <algorithm>
#include <cmath>

namespace daw::dsp {

namespace {
constexpr float kPi = 3.14159265358979323846f;
} // namespace

void RenderMetronomeClicks(
    float* outStereo,
    int frames,
    float sampleRate,
    float samplesPerBeat,
    std::uint64_t baseFrame,
    float gain)
{
    if (outStereo == nullptr || frames <= 0 || sampleRate <= 0.0f) {
        return;
    }
    const float spb = std::max(1.0f, samplesPerBeat);
    const int clickSamples = std::max(1, static_cast<int>(sampleRate * 0.04f));

    for (int i = 0; i < frames; ++i) {
        const std::uint64_t gf = baseFrame + static_cast<std::uint64_t>(i);
        const double beatPos = static_cast<double>(gf) / static_cast<double>(spb);
        const int beatIdx = static_cast<int>(std::floor(beatPos));
        const std::uint64_t beatStart = static_cast<std::uint64_t>(
            std::llround(static_cast<double>(beatIdx) * static_cast<double>(spb)));
        const int since = static_cast<int>(gf - beatStart);
        if (since < 0 || since >= clickSamples) continue;

        const bool accent = (beatIdx % 4) == 0;
        const float freq  = accent ? 1800.0f : 1200.0f;
        const float amp   = (accent ? 0.22f : 0.14f) * gain;
        const float env   = std::exp(-4.0f * static_cast<float>(since) / static_cast<float>(clickSamples));
        const float t     = static_cast<float>(since) / sampleRate;
        const float s     = std::sin(2.0f * kPi * freq * t) * env * amp;

        outStereo[static_cast<size_t>(i) * 2]     += s;
        outStereo[static_cast<size_t>(i) * 2 + 1] += s;
    }
}

} // namespace daw::dsp
