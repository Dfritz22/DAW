#pragma once

#include "core/audio_clip.h"

#include <cstdint>

namespace daw::engine {

// Read a single stereo sample from a `LoadedAudio` source at a given clip-
// relative project frame. Performs sample-rate conversion (linear) when the
// source SR differs from the project SR. Pure: depends only on the source
// audio buffer.
//
//   audio                    Source audio (interleaved float stereo).
//   clipFrameInProjectRate   Frame offset within the clip, expressed in
//                            *project* sample rate.
//   projectSampleRate        Project SR in Hz (must be > 0).
//   sourceOffsetFrames       In-source offset (in *source* SR frames) where
//                            the clip starts reading from.
//   outL, outR               Output sample (must be non-null).
//
// Returns true on success; false if inputs are invalid or the read falls
// outside the source's available range.
bool ReadClipSample(
    const daw::core::LoadedAudio& audio,
    std::uint64_t clipFrameInProjectRate,
    int projectSampleRate,
    std::uint64_t sourceOffsetFrames,
    float* outL,
    float* outR);

} // namespace daw::engine
