#pragma once

#include "core/audio_clip.h"

#include <cstdint>
#include <vector>

namespace daw::engine {

// Compute the end frame (in project sample-rate frames) of the rightmost
// valid clip in `clips`. A clip is "valid" when its `audioIndex` is in
// [0, audioCount). Clips with negative startBeat are clamped to 0.
//
//   clips           Project clip list.
//   audioCount      Number of entries in the project's audio pool.
//   samplesPerBeat  Project samples-per-beat (must be > 0; otherwise 0 is
//                   returned).
//
// Returns 0 when no valid clip exists or `samplesPerBeat <= 0`.
std::uint64_t ComputeProjectEndFrame(
    const std::vector<daw::core::ClipItem>& clips,
    int audioCount,
    float samplesPerBeat);

} // namespace daw::engine
