#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace daw::core {

// ── Pure value types: audio source + timeline clip ───────────────────────────
// `LoadedAudio` is the in-memory representation of a single decoded audio
// source (one WAV file or one recorded take). `ClipItem` is a placement of
// such a source on the timeline. Both are POD-shaped — no methods that
// allocate beyond the obvious vector/string members, no dependencies on
// app or engine state.

constexpr std::uint32_t Rgb(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    return static_cast<std::uint32_t>(r)
         | (static_cast<std::uint32_t>(g) << 8)
         | (static_cast<std::uint32_t>(b) << 16);
}

struct LoadedAudio {
    std::wstring sourcePath;
    std::wstring displayName;
    int sampleRate {0};
    std::uint32_t frames {0};
    std::vector<float> stereo;  // interleaved L,R,L,R,...

    // Runtime-only cache: per-bucket abs-peak summary used by the waveform
    // renderer to avoid scanning the full PCM on every repaint. Populated
    // lazily on first draw and after edits that mutate `stereo`. Not
    // serialized.
    static constexpr std::uint32_t kPeakBucketFrames = 256;
    mutable std::vector<float> peakSummary;
};

struct ClipItem {
    int trackIndex {0};
    int audioIndex {-1};
    float startBeat {0.0f};
    float lengthBeats {4.0f};
    std::uint32_t color {Rgb(88, 131, 199)};
    std::wstring name;
    std::uint64_t sourceOffsetFrames {0};
};

} // namespace daw::core
