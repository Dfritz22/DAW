#include <gtest/gtest.h>

#include "core/project_data.h"

using daw::core::ProjectData;
using daw::core::TrackData;
using daw::core::BusData;
using daw::core::kProjectBusCount;
using daw::core::kProjectMaxInsertSlots;

TEST(ProjectData, DefaultsAt120BpmAnd48k) {
    ProjectData p;
    EXPECT_FLOAT_EQ(p.bpm, 120.0f);
    EXPECT_EQ(p.projectSampleRate, 48000);
}

TEST(ProjectData, ConstructorPreFillsBusesToBusCount) {
    ProjectData p;
    ASSERT_EQ(static_cast<int>(p.buses.size()), kProjectBusCount);
    for (const BusData& b : p.buses) {
        EXPECT_FLOAT_EQ(b.gainDb, 0.0f);
        EXPECT_FALSE(b.mute);
        EXPECT_EQ(b.insertSlots, 0);
    }
}

TEST(ProjectData, EmptyTracksAndClipsByDefault) {
    ProjectData p;
    EXPECT_TRUE(p.tracks.empty());
    EXPECT_TRUE(p.clips.empty());
    EXPECT_TRUE(p.audio.empty());
}

TEST(ProjectData, TrackDataDefaultsRouteToMusicBus) {
    TrackData t;
    EXPECT_EQ(t.busIndex, 1);  // Music bus by convention
    EXPECT_FLOAT_EQ(t.gainDb, 0.0f);
    EXPECT_FLOAT_EQ(t.pan, 0.0f);
    EXPECT_FALSE(t.mute);
    EXPECT_FALSE(t.solo);
    EXPECT_FALSE(t.recordArm);
    EXPECT_EQ(t.insertSlots, 0);
}

TEST(ProjectData, InsertArraySizeMatchesDspConstant) {
    TrackData t;
    EXPECT_EQ(static_cast<int>(t.insertEffects.size()), kProjectMaxInsertSlots);
    EXPECT_EQ(static_cast<int>(t.insertBypass.size()), kProjectMaxInsertSlots);
    EXPECT_EQ(static_cast<int>(t.insertConfig.size()), kProjectMaxInsertSlots);
}
