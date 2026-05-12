#include <gtest/gtest.h>

#include "core/timeline_edit_ops.h"

using daw::core::ProjectData;
using daw::core::ClipItem;
using daw::core::SplitClip;
using daw::core::DuplicateClip;
using daw::core::NudgeClip;
using daw::core::DeleteClip;
using daw::core::AppendDefaultTrack;
using daw::core::DeleteTrack;

namespace {

ClipItem MakeClip(int track, float start, float len, std::uint64_t srcOff = 0) {
    ClipItem c;
    c.trackIndex = track;
    c.startBeat = start;
    c.lengthBeats = len;
    c.sourceOffsetFrames = srcOff;
    return c;
}

} // namespace

TEST(TimelineEdit, SplitClipProducesTwoContiguousPieces) {
    ProjectData p;
    p.clips.push_back(MakeClip(0, 0.0f, 4.0f, 1000));
    ASSERT_TRUE(SplitClip(p, 0, 1.0f, 24000.0f));
    ASSERT_EQ(p.clips.size(), 2u);
    EXPECT_FLOAT_EQ(p.clips[0].startBeat, 0.0f);
    EXPECT_FLOAT_EQ(p.clips[0].lengthBeats, 1.0f);
    EXPECT_FLOAT_EQ(p.clips[1].startBeat, 1.0f);
    EXPECT_FLOAT_EQ(p.clips[1].lengthBeats, 3.0f);
    EXPECT_EQ(p.clips[1].sourceOffsetFrames, 1000ULL + 24000ULL);
}

TEST(TimelineEdit, SplitRejectsTooCloseToEdge) {
    ProjectData p;
    p.clips.push_back(MakeClip(0, 0.0f, 4.0f));
    EXPECT_FALSE(SplitClip(p, 0, 0.001f, 24000.0f));
    EXPECT_FALSE(SplitClip(p, 0, 3.999f, 24000.0f));
    EXPECT_EQ(p.clips.size(), 1u);
}

TEST(TimelineEdit, SplitRejectsBadIndex) {
    ProjectData p;
    EXPECT_FALSE(SplitClip(p, 0, 1.0f, 24000.0f));
    EXPECT_FALSE(SplitClip(p, -1, 1.0f, 24000.0f));
}

TEST(TimelineEdit, DuplicatePlacesCopyAfterOriginal) {
    ProjectData p;
    p.clips.push_back(MakeClip(0, 2.0f, 4.0f));
    const int newIdx = DuplicateClip(p, 0);
    ASSERT_EQ(newIdx, 1);
    EXPECT_FLOAT_EQ(p.clips[1].startBeat, 6.0f);
    EXPECT_FLOAT_EQ(p.clips[1].lengthBeats, 4.0f);
}

TEST(TimelineEdit, NudgeClampsToZero) {
    ProjectData p;
    p.clips.push_back(MakeClip(0, 1.0f, 4.0f));
    ASSERT_TRUE(NudgeClip(p, 0, -5.0f));
    EXPECT_FLOAT_EQ(p.clips[0].startBeat, 0.0f);
}

TEST(TimelineEdit, DeleteClipRemovesEntry) {
    ProjectData p;
    p.clips.push_back(MakeClip(0, 0.0f, 4.0f));
    p.clips.push_back(MakeClip(0, 4.0f, 4.0f));
    ASSERT_TRUE(DeleteClip(p, 0));
    ASSERT_EQ(p.clips.size(), 1u);
    EXPECT_FLOAT_EQ(p.clips[0].startBeat, 4.0f);
}

TEST(TimelineEdit, AppendDefaultTrackIncrementsName) {
    ProjectData p;
    EXPECT_EQ(AppendDefaultTrack(p), 0);
    EXPECT_EQ(AppendDefaultTrack(p), 1);
    EXPECT_EQ(p.tracks[0].name, L"Track 1");
    EXPECT_EQ(p.tracks[1].name, L"Track 2");
    EXPECT_EQ(p.tracks[0].busIndex, 1);
}

TEST(TimelineEdit, DeleteTrackRemovesClipsAndShiftsHigherIndices) {
    ProjectData p;
    AppendDefaultTrack(p);
    AppendDefaultTrack(p);
    AppendDefaultTrack(p);
    p.clips.push_back(MakeClip(0, 0.0f, 4.0f));
    p.clips.push_back(MakeClip(1, 0.0f, 4.0f));
    p.clips.push_back(MakeClip(2, 0.0f, 4.0f));
    p.clips.push_back(MakeClip(2, 4.0f, 4.0f));

    ASSERT_TRUE(DeleteTrack(p, 1));
    EXPECT_EQ(p.tracks.size(), 2u);
    ASSERT_EQ(p.clips.size(), 3u);
    // Track 0 clips unchanged.
    EXPECT_EQ(p.clips[0].trackIndex, 0);
    // Old track 2 clips renumbered to 1.
    EXPECT_EQ(p.clips[1].trackIndex, 1);
    EXPECT_EQ(p.clips[2].trackIndex, 1);
}
