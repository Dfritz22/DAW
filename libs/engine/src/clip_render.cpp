#include "engine/clip_render.h"

#include "engine/clip_reader.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace daw::engine {

namespace {

// Templated body so both the mutable-shared_ptr (UI/offline) and
// const-shared_ptr (audio-thread snapshot) overloads can share one
// implementation without forcing the caller to convert vector element
// types (which would require a per-callback allocation on the realtime
// path).
template <typename AudioPoolT>
void RenderClipsForTrackImpl(
    const std::vector<daw::core::ClipItem>& clips,
    const AudioPoolT& audioPool,
    int trackIndex,
    int projectSampleRate,
    float samplesPerBeat,
    std::uint64_t bufferStartFrame,
    float* dstStereo,
    std::uint64_t dstFrames)
{
    if (dstStereo == nullptr || dstFrames == 0 || samplesPerBeat <= 0.0f) {
        return;
    }
    const std::uint64_t bufferEnd = bufferStartFrame + dstFrames;
    const int audioCount = static_cast<int>(audioPool.size());

    for (const auto& clip : clips) {
        if (clip.trackIndex != trackIndex) continue;
        if (clip.audioIndex < 0 || clip.audioIndex >= audioCount) continue;
        const auto& sourcePtr = audioPool[static_cast<std::size_t>(clip.audioIndex)];
        if (!sourcePtr) continue;
        const auto& source = *sourcePtr;

        const std::uint64_t clipStart = static_cast<std::uint64_t>(
            std::llround(std::max(0.0f, clip.startBeat) * samplesPerBeat));
        const std::uint64_t clipFrames = static_cast<std::uint64_t>(
            std::llround(static_cast<double>(clip.lengthBeats) * static_cast<double>(samplesPerBeat)));
        const std::uint64_t clipEnd = clipStart + clipFrames;

        const std::uint64_t f0 = std::max(clipStart, bufferStartFrame);
        const std::uint64_t f1 = std::min(clipEnd, bufferEnd);
        if (f0 >= f1) continue;

        for (std::uint64_t f = f0; f < f1; ++f) {
            float l = 0.0f;
            float r = 0.0f;
            if (!ReadClipSample(source, f - clipStart, projectSampleRate,
                                clip.sourceOffsetFrames, &l, &r)) {
                continue;
            }
            const std::size_t dst = static_cast<std::size_t>(f - bufferStartFrame) * 2;
            dstStereo[dst]     += l;
            dstStereo[dst + 1] += r;
        }
    }
}

} // namespace

void RenderClipsForTrack(
    const std::vector<daw::core::ClipItem>& clips,
    const std::vector<std::shared_ptr<daw::core::LoadedAudio>>& audioPool,
    int trackIndex,
    int projectSampleRate,
    float samplesPerBeat,
    std::uint64_t bufferStartFrame,
    float* dstStereo,
    std::uint64_t dstFrames)
{
    RenderClipsForTrackImpl(clips, audioPool, trackIndex, projectSampleRate,
                            samplesPerBeat, bufferStartFrame, dstStereo, dstFrames);
}

void RenderClipsForTrack(
    const std::vector<daw::core::ClipItem>& clips,
    const std::vector<std::shared_ptr<const daw::core::LoadedAudio>>& audioPool,
    int trackIndex,
    int projectSampleRate,
    float samplesPerBeat,
    std::uint64_t bufferStartFrame,
    float* dstStereo,
    std::uint64_t dstFrames)
{
    RenderClipsForTrackImpl(clips, audioPool, trackIndex, projectSampleRate,
                            samplesPerBeat, bufferStartFrame, dstStereo, dstFrames);
}

} // namespace daw::engine
