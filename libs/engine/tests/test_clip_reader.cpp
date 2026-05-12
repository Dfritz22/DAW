#include <gtest/gtest.h>

#include "engine/clip_reader.h"

#include <cstdint>

using daw::core::LoadedAudio;
using daw::engine::ReadClipSample;

namespace {

LoadedAudio MakeSource(int sampleRate, std::initializer_list<float> interleavedStereo) {
    LoadedAudio a;
    a.sampleRate = sampleRate;
    a.stereo.assign(interleavedStereo.begin(), interleavedStereo.end());
    a.frames = static_cast<std::uint32_t>(a.stereo.size() / 2);
    return a;
}

} // namespace

TEST(ClipReader, FastPathReturnsExactSampleAtMatchingRate) {
    LoadedAudio a = MakeSource(48000, {0.1f, -0.1f, 0.2f, -0.2f, 0.3f, -0.3f});
    float l = 0.0f, r = 0.0f;
    EXPECT_TRUE(ReadClipSample(a, 1, 48000, 0, &l, &r));
    EXPECT_FLOAT_EQ(l, 0.2f);
    EXPECT_FLOAT_EQ(r, -0.2f);
}

TEST(ClipReader, SourceOffsetSkipsForward) {
    LoadedAudio a = MakeSource(48000, {0.0f, 0.0f, 1.0f, -1.0f, 0.5f, -0.5f});
    float l = 0.0f, r = 0.0f;
    // Skip first frame; clipFrame 0 should land on the second source frame.
    EXPECT_TRUE(ReadClipSample(a, 0, 48000, 1, &l, &r));
    EXPECT_FLOAT_EQ(l, 1.0f);
    EXPECT_FLOAT_EQ(r, -1.0f);
}

TEST(ClipReader, OutOfRangeReturnsFalse) {
    LoadedAudio a = MakeSource(48000, {1.0f, 1.0f, 2.0f, 2.0f});
    float l = 0.0f, r = 0.0f;
    EXPECT_FALSE(ReadClipSample(a, 5, 48000, 0, &l, &r));
}

TEST(ClipReader, EmptySourceReturnsFalse) {
    LoadedAudio a;
    a.sampleRate = 48000;
    float l = 0.0f, r = 0.0f;
    EXPECT_FALSE(ReadClipSample(a, 0, 48000, 0, &l, &r));
}

TEST(ClipReader, InvalidProjectRateReturnsFalse) {
    LoadedAudio a = MakeSource(48000, {1.0f, 1.0f});
    float l = 0.0f, r = 0.0f;
    EXPECT_FALSE(ReadClipSample(a, 0, 0, 0, &l, &r));
}

TEST(ClipReader, ResamplesWhenSourceRateDiffersFromProjectRate) {
    // Source @ 24 kHz, project @ 48 kHz → ratio 0.5; project frame 1 should
    // map to source position 0.5 → linear interp between frames 0 and 1.
    LoadedAudio a = MakeSource(24000, {0.0f, 0.0f, 1.0f, -1.0f});
    float l = 0.0f, r = 0.0f;
    EXPECT_TRUE(ReadClipSample(a, 1, 48000, 0, &l, &r));
    EXPECT_NEAR(l, 0.5f, 1e-5f);
    EXPECT_NEAR(r, -0.5f, 1e-5f);
}
