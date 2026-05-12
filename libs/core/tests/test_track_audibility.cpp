#include <gtest/gtest.h>

#include "core/track_audibility.h"

using daw::core::AnyTrackSoloed;
using daw::core::IsTrackAudible;
using daw::core::TrackData;

namespace {

TrackData Make(bool mute, bool solo) {
    TrackData t;
    t.mute = mute;
    t.solo = solo;
    return t;
}

} // namespace

TEST(TrackAudibility, EmptyListYieldsNoSolo) {
    std::vector<TrackData> tracks;
    EXPECT_FALSE(AnyTrackSoloed(tracks));
}

TEST(TrackAudibility, AnyTrackSoloedDetectsSingleSolo) {
    std::vector<TrackData> tracks{ Make(false,false), Make(false,true), Make(false,false) };
    EXPECT_TRUE(AnyTrackSoloed(tracks));
}

TEST(TrackAudibility, OutOfRangeIsInaudible) {
    std::vector<TrackData> tracks{ Make(false,false) };
    EXPECT_FALSE(IsTrackAudible(tracks, -1));
    EXPECT_FALSE(IsTrackAudible(tracks, 1));
    EXPECT_FALSE(IsTrackAudible(tracks, 999));
}

TEST(TrackAudibility, MutedTrackIsInaudible) {
    std::vector<TrackData> tracks{ Make(true,false) };
    EXPECT_FALSE(IsTrackAudible(tracks, 0));
}

TEST(TrackAudibility, NoSoloUnmutedAudible) {
    std::vector<TrackData> tracks{ Make(false,false), Make(false,false) };
    EXPECT_TRUE(IsTrackAudible(tracks, 0));
    EXPECT_TRUE(IsTrackAudible(tracks, 1));
}

TEST(TrackAudibility, SoloOnOtherTrackSilencesUnsoloed) {
    std::vector<TrackData> tracks{ Make(false,false), Make(false,true) };
    EXPECT_FALSE(IsTrackAudible(tracks, 0));
    EXPECT_TRUE(IsTrackAudible(tracks, 1));
}

TEST(TrackAudibility, MutedSoloIsInaudibleButStillSilencesOthers) {
    // Pro Tools-style: mute beats solo on the same track. Other unsoloed
    // tracks remain silenced because solo flag still implies "solo mode".
    std::vector<TrackData> tracks{ Make(false,false), Make(true,true) };
    EXPECT_FALSE(IsTrackAudible(tracks, 0));
    EXPECT_FALSE(IsTrackAudible(tracks, 1));
}
