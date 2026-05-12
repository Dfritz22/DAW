#include "engine/timeline_layout.h"

#include <algorithm>
#include <cmath>

namespace daw::engine {

std::uint64_t ComputeProjectEndFrame(
    const std::vector<daw::core::ClipItem>& clips,
    int audioCount,
    float samplesPerBeat)
{
    if (samplesPerBeat <= 0.0f) {
        return 0;
    }
    std::uint64_t maxFrame = 0;
    for (const auto& clip : clips) {
        if (clip.audioIndex < 0 || clip.audioIndex >= audioCount) {
            continue;
        }
        const std::uint64_t clipStart = static_cast<std::uint64_t>(
            std::llround(std::max(0.0f, clip.startBeat) * samplesPerBeat));
        const std::uint64_t clipLen = static_cast<std::uint64_t>(
            std::llround(static_cast<double>(clip.lengthBeats) * static_cast<double>(samplesPerBeat)));
        maxFrame = std::max(maxFrame, clipStart + clipLen);
    }
    return maxFrame;
}

} // namespace daw::engine
