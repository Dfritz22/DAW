#include <gtest/gtest.h>

#include <cmath>

#include "vm/timeline_zoom.h"

using namespace daw::vm;

TEST(TimelineZoom, ZoomVisible_KeyZoomInShrinks) {
    EXPECT_NEAR(ZoomVisible(32.0f, kKeyZoomInFactor), 32.0f * 0.85f, 1e-4f);
}

TEST(TimelineZoom, ZoomVisible_KeyZoomOutGrows) {
    EXPECT_NEAR(ZoomVisible(32.0f, kKeyZoomOutFactor), 32.0f * 1.15f, 1e-4f);
}

TEST(TimelineZoom, ZoomVisible_ClampsAtMin) {
    // Repeatedly zoom in from a small value; should bottom out at kMinViewBeats.
    float v = 8.0f;
    for (int i = 0; i < 20; ++i) v = ZoomVisible(v, kKeyZoomInFactor);
    EXPECT_FLOAT_EQ(v, kMinViewBeats);
}

TEST(TimelineZoom, ZoomVisible_ClampsAtMax) {
    float v = 32.0f;
    for (int i = 0; i < 50; ++i) v = ZoomVisible(v, kKeyZoomOutFactor);
    EXPECT_FLOAT_EQ(v, kMaxViewBeats);
}

TEST(TimelineZoom, ZoomVisible_NaNFallsBackToDefault) {
    EXPECT_FLOAT_EQ(ZoomVisible(std::nanf(""), 0.85f), kDefaultViewBeats);
}

TEST(TimelineZoom, ZoomVisibleAround_PinsFocusBeatScreenPosition) {
    // viewStart=0, visible=32, focus=8 → focus is at 25% across viewport.
    // After zoom-in by 0.5: visible=16; focus must still be at 25% across
    // → newStart = 8 - 0.25*16 = 4.
    const auto r = ZoomVisibleAround(0.0f, 32.0f, 8.0f, 0.5f);
    EXPECT_FLOAT_EQ(r.viewBeatsVisible, 16.0f);
    EXPECT_FLOAT_EQ(r.viewStartBeat, 4.0f);
}

TEST(TimelineZoom, ZoomVisibleAround_ClampsStartToZero) {
    // Focus at beat 1 with viewStart=0 — zooming out would push start
    // negative; must clamp to 0.
    const auto r = ZoomVisibleAround(0.0f, 32.0f, 1.0f, 2.0f);
    EXPECT_GE(r.viewStartBeat, 0.0f);
}

TEST(TimelineZoom, ZoomVisibleAround_RespectsMaxClamp) {
    const auto r = ZoomVisibleAround(0.0f, 100.0f, 50.0f, 10.0f);
    EXPECT_FLOAT_EQ(r.viewBeatsVisible, kMaxViewBeats);
}

TEST(TimelineZoom, FitVisibleToContent_EmptyProjectGetsMin) {
    EXPECT_FLOAT_EQ(FitVisibleToContent(0.0f), kFitMinViewBeats);
}

TEST(TimelineZoom, FitVisibleToContent_AddsTailPadding) {
    EXPECT_FLOAT_EQ(FitVisibleToContent(20.0f), 24.0f);
}

TEST(TimelineZoom, FitVisibleToContent_ClampsLong) {
    EXPECT_FLOAT_EQ(FitVisibleToContent(1000.0f), kMaxViewBeats);
}

TEST(TimelineZoom, PlayheadIsPastViewRight_True) {
    // viewStart=0, visible=10 → viewRight=10; threshold = 10 - 1 = 9.
    EXPECT_TRUE(PlayheadIsPastViewRight(0.0f, 10.0f, 9.5f));
}

TEST(TimelineZoom, PlayheadIsPastViewRight_FalseAtThreshold) {
    EXPECT_FALSE(PlayheadIsPastViewRight(0.0f, 10.0f, 9.0f));
}

TEST(TimelineZoom, PlayheadIsPastViewRight_FalseInside) {
    EXPECT_FALSE(PlayheadIsPastViewRight(0.0f, 10.0f, 5.0f));
}

TEST(TimelineZoom, AutoScrollViewStart_Places75PercentLeft) {
    // visible=20, playhead=30 → newStart = 30 - 15 = 15;
    // → playhead now sits at start + 0.75*visible = 15 + 15 = 30. ✓
    EXPECT_FLOAT_EQ(AutoScrollViewStart(20.0f, 30.0f), 15.0f);
}

TEST(TimelineZoom, AutoScrollViewStart_ClampsToZero) {
    EXPECT_FLOAT_EQ(AutoScrollViewStart(20.0f, 5.0f), 0.0f);
}

TEST(TimelineZoom, ResetView_ReturnsDefaults) {
    const auto r = ResetView();
    EXPECT_FLOAT_EQ(r.viewStartBeat,    0.0f);
    EXPECT_FLOAT_EQ(r.viewBeatsVisible, kDefaultViewBeats);
}

TEST(TimelineZoom, PanViewStartBeat_AddsDelta) {
    EXPECT_FLOAT_EQ(PanViewStartBeat(10.0f, 4.0f), 14.0f);
    EXPECT_FLOAT_EQ(PanViewStartBeat(10.0f, -3.0f), 7.0f);
}

TEST(TimelineZoom, PanViewStartBeat_ClampsAtZero) {
    EXPECT_FLOAT_EQ(PanViewStartBeat(2.0f, -10.0f), 0.0f);
}

// Cross-check: chained zoom+pan operations should be deterministic and
// reversible-ish within float epsilon. This guards against future "ratio"
// formula refactors silently breaking the pin-focus invariant.
TEST(TimelineZoom, ZoomVisibleAround_ZoomInThenOutNearlyIdentity) {
    const float start0 = 12.0f;
    const float vis0   = 40.0f;
    const float focus  = 25.0f;
    const auto in  = ZoomVisibleAround(start0, vis0, focus, 0.5f);
    const auto out = ZoomVisibleAround(in.viewStartBeat, in.viewBeatsVisible, focus, 2.0f);
    EXPECT_NEAR(out.viewBeatsVisible, vis0,   1e-3f);
    EXPECT_NEAR(out.viewStartBeat,    start0, 1e-3f);
}
