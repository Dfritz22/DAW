#include <gtest/gtest.h>

#include "core/timeline_math.h"

#include <cmath>

using daw::core::SamplesPerBeat;
using daw::core::FramesFromBeats;
using daw::core::BeatsFromFrames;

TEST(TimelineMath, SamplesPerBeatAt120Bpm) {
    // 48000 Hz, 120 BPM -> 0.5 sec/beat -> 24000 samples/beat.
    EXPECT_FLOAT_EQ(SamplesPerBeat(48000, 120.0f), 24000.0f);
}

TEST(TimelineMath, SamplesPerBeatAt60Bpm) {
    EXPECT_FLOAT_EQ(SamplesPerBeat(44100, 60.0f), 44100.0f);
}

TEST(TimelineMath, FramesFromBeatsRoundsToNearest) {
    EXPECT_EQ(FramesFromBeats(48000, 120.0f, 1.0f), 24000ULL);
    EXPECT_EQ(FramesFromBeats(48000, 120.0f, 2.0f), 48000ULL);
    EXPECT_EQ(FramesFromBeats(48000, 120.0f, 0.5f), 12000ULL);
}

TEST(TimelineMath, BeatsFromFramesIsInverseOfFramesFromBeats) {
    const std::uint64_t f = FramesFromBeats(48000, 120.0f, 4.0f);
    EXPECT_FLOAT_EQ(BeatsFromFrames(48000, 120.0f, f), 4.0f);
}

TEST(TimelineMath, NegativeBeatClampsToZeroFrames) {
    EXPECT_EQ(FramesFromBeats(48000, 120.0f, -1.0f), 0ULL);
}

TEST(TimelineMath, ZeroSampleRateDoesNotDivideByZero) {
    const float spb = SamplesPerBeat(0, 120.0f);
    EXPECT_TRUE(std::isfinite(spb));
    EXPECT_GT(spb, 0.0f);
    EXPECT_TRUE(std::isfinite(BeatsFromFrames(0, 120.0f, 1000)));
}

TEST(TimelineMath, ZeroBpmDoesNotDivideByZero) {
    const float spb = SamplesPerBeat(48000, 0.0f);
    EXPECT_TRUE(std::isfinite(spb));
    EXPECT_GT(spb, 0.0f);
}
