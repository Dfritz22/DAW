#pragma once

#include "core/audio_clip.h"

#include <cstdint>
#include <vector>

namespace daw::engine {

// Additively renders all clips that belong to `trackIndex` into the stereo
// destination buffer, mixing into existing content (does not zero `dstStereo`).
//
//   clips              Project clip list.
//   audioPool          Project audio pool. Clips with audioIndex outside
//                      [0, audioPool.size()) are skipped.
//   trackIndex         Only clips whose `trackIndex` matches are rendered.
//   projectSampleRate  Project sample rate (Hz). Forwarded to ReadClipSample.
//   samplesPerBeat     Project samples-per-beat. Used to convert beat-based
//                      clip metadata to frames. Must be > 0; otherwise the
//                      call is a no-op.
//   bufferStartFrame   Absolute project frame corresponding to dstStereo[0].
//                      Allows realtime callbacks (=startCursor) and offline
//                      renders (=0) to share this routine.
//   dstStereo          Interleaved L,R destination, sized at least
//                      `dstFrames * 2` floats. Must not be null.
//   dstFrames          Number of stereo frames in `dstStereo`.
void RenderClipsForTrack(
    const std::vector<daw::core::ClipItem>& clips,
    const std::vector<daw::core::LoadedAudio>& audioPool,
    int trackIndex,
    int projectSampleRate,
    float samplesPerBeat,
    std::uint64_t bufferStartFrame,
    float* dstStereo,
    std::uint64_t dstFrames);

} // namespace daw::engine
