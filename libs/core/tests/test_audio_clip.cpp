#include <gtest/gtest.h>

#include "core/audio_clip.h"

using daw::core::Rgb;
using daw::core::LoadedAudio;
using daw::core::ClipItem;

TEST(AudioClip, RgbPacksBytesAsBgrLayout) {
    // Channel order matches the historical CoreRgb layout: r in low byte,
    // g in middle, b in high — i.e. stored as 0x00BBGGRR.
    EXPECT_EQ(Rgb(0xFF, 0x00, 0x00), 0x000000FFu);
    EXPECT_EQ(Rgb(0x00, 0xFF, 0x00), 0x0000FF00u);
    EXPECT_EQ(Rgb(0x00, 0x00, 0xFF), 0x00FF0000u);
}

TEST(AudioClip, LoadedAudioDefaultsAreEmpty) {
    LoadedAudio a;
    EXPECT_EQ(a.sampleRate, 0);
    EXPECT_EQ(a.frames, 0u);
    EXPECT_TRUE(a.stereo.empty());
    EXPECT_TRUE(a.peakSummary.empty());
}

TEST(AudioClip, ClipItemDefaultsMatchHistoricalValues) {
    ClipItem c;
    EXPECT_EQ(c.trackIndex, 0);
    EXPECT_EQ(c.audioIndex, -1);
    EXPECT_FLOAT_EQ(c.startBeat, 0.0f);
    EXPECT_FLOAT_EQ(c.lengthBeats, 4.0f);
    EXPECT_EQ(c.sourceOffsetFrames, 0ULL);
    EXPECT_EQ(c.color, Rgb(88, 131, 199));
}

TEST(AudioClip, PeakBucketFramesIsStableConstant) {
    // Renderer + cache invalidation logic both depend on this exact value.
    EXPECT_EQ(LoadedAudio::kPeakBucketFrames, 256u);
}
