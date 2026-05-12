#include <gtest/gtest.h>

#include "engine/timeline_layout.h"

#include <vector>

using daw::core::ClipItem;
using daw::engine::ComputeProjectEndFrame;

namespace {

ClipItem MakeClip(int audioIndex, float startBeat, float lengthBeats) {
    ClipItem c;
    c.audioIndex = audioIndex;
    c.startBeat = startBeat;
    c.lengthBeats = lengthBeats;
    return c;
}

} // namespace

TEST(TimelineLayout, EmptyClipsYieldsZero) {
    std::vector<ClipItem> clips;
    EXPECT_EQ(ComputeProjectEndFrame(clips, 0, 24000.0f), 0u);
}

TEST(TimelineLayout, ZeroSamplesPerBeatYieldsZero) {
    std::vector<ClipItem> clips{ MakeClip(0, 0.0f, 4.0f) };
    EXPECT_EQ(ComputeProjectEndFrame(clips, 1, 0.0f), 0u);
}

TEST(TimelineLayout, ReturnsMaxOfStartPlusLength) {
    std::vector<ClipItem> clips{
        MakeClip(0, 0.0f, 4.0f),    // 0   .. 96000  (24000*4)
        MakeClip(0, 4.0f, 4.0f),    // 96000 .. 192000
        MakeClip(0, 2.0f, 1.0f),    // 48000 .. 72000
    };
    EXPECT_EQ(ComputeProjectEndFrame(clips, 1, 24000.0f), 192000u);
}

TEST(TimelineLayout, IgnoresClipsWithInvalidAudioIndex) {
    std::vector<ClipItem> clips{
        MakeClip(-1, 0.0f, 100.0f),
        MakeClip(99, 0.0f, 100.0f),
        MakeClip(0,  0.0f, 4.0f),
    };
    EXPECT_EQ(ComputeProjectEndFrame(clips, 1, 24000.0f), 96000u);
}

TEST(TimelineLayout, ClampsNegativeStartBeatToZero) {
    std::vector<ClipItem> clips{ MakeClip(0, -3.0f, 4.0f) };
    EXPECT_EQ(ComputeProjectEndFrame(clips, 1, 24000.0f), 96000u);
}
