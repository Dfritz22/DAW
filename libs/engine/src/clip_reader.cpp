#include "engine/clip_reader.h"

#include <algorithm>

namespace daw::engine {

bool ReadClipSample(
    const daw::core::LoadedAudio& audio,
    std::uint64_t clipFrameInProjectRate,
    int projectSampleRate,
    std::uint64_t sourceOffsetFrames,
    float* outL,
    float* outR)
{
    if (outL == nullptr || outR == nullptr || audio.frames == 0 ||
        audio.stereo.empty() || projectSampleRate <= 0) {
        return false;
    }

    const int srcRate = (audio.sampleRate > 0) ? audio.sampleRate : projectSampleRate;

    // Fast path: source SR matches project SR. Skip interpolation entirely;
    // just look up the integer frame.
    if (srcRate == projectSampleRate) {
        const std::uint64_t i = sourceOffsetFrames + clipFrameInProjectRate;
        if (i >= audio.frames) {
            return false;
        }
        const size_t b = static_cast<size_t>(i) * 2;
        if (b + 1 >= audio.stereo.size()) {
            return false;
        }
        *outL = audio.stereo[b];
        *outR = audio.stereo[b + 1];
        return true;
    }

    const double ratio = static_cast<double>(srcRate) / static_cast<double>(projectSampleRate);
    const double srcPos = static_cast<double>(sourceOffsetFrames) +
                          static_cast<double>(clipFrameInProjectRate) * ratio;
    if (srcPos < 0.0) {
        return false;
    }

    const double maxSrc = static_cast<double>(audio.frames - 1);
    if (srcPos > maxSrc) {
        return false;
    }

    const std::uint64_t i0 = static_cast<std::uint64_t>(srcPos);
    const std::uint64_t i1 = std::min<std::uint64_t>(i0 + 1, audio.frames - 1);
    const float frac = static_cast<float>(srcPos - static_cast<double>(i0));

    const size_t b0 = static_cast<size_t>(i0) * 2;
    const size_t b1 = static_cast<size_t>(i1) * 2;
    if (b0 + 1 >= audio.stereo.size() || b1 + 1 >= audio.stereo.size()) {
        return false;
    }

    const float l0 = audio.stereo[b0];
    const float r0 = audio.stereo[b0 + 1];
    const float l1 = audio.stereo[b1];
    const float r1 = audio.stereo[b1 + 1];
    *outL = l0 + (l1 - l0) * frac;
    *outR = r0 + (r1 - r0) * frac;
    return true;
}

} // namespace daw::engine
