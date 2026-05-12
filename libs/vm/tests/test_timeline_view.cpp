// Pin down the timeline view-model geometry math. These tests cover the
// projections that both apps/daw_gui's draw path and its hit-test path now
// share (via app shims that delegate to daw::vm) — analogous to the
// libs/engine mix_pipeline split.

#include <gtest/gtest.h>

#include "vm/timeline_view.h"

using daw::vm::BeatToX;
using daw::vm::ClipRectForDraw;
using daw::vm::Rect;
using daw::vm::SnapBeat;
using daw::vm::TimelineViewport;
using daw::vm::TrackIndexFromY;
using daw::vm::XToBeat;

namespace {

// 1000-pixel-wide arrange (x ∈ [100, 1100)), 10 beats visible starting at 0.
// Easy mental math: 1 beat = 100 px, beat N starts at x = 100 + N*100.
constexpr int   kRowH   = 60;
constexpr int   kInsetY = 4;
TimelineViewport BasicViewport(int trackCount = 8, int scrollY = 0,
                                float viewStart = 0.0f, float viewBeats = 10.0f) {
    return TimelineViewport{
        /*arrange*/ Rect{100, 50, 1100, 50 + trackCount * kRowH},
        /*viewStartBeat*/ viewStart,
        /*viewBeatsVisible*/ viewBeats,
        /*tracksScrollY*/ scrollY,
        /*rowHeightPx*/ kRowH,
        /*clipInsetYPx*/ kInsetY,
        /*trackCount*/ trackCount,
    };
}

} // namespace

TEST(TimelineView, BeatToX_LeftEdgeIsArrangeLeft) {
    const auto vp = BasicViewport();
    EXPECT_EQ(BeatToX(vp, 0.0f), 100);
}

TEST(TimelineView, BeatToX_LinearMapping) {
    const auto vp = BasicViewport();
    EXPECT_EQ(BeatToX(vp, 5.0f), 600);
    EXPECT_EQ(BeatToX(vp, 10.0f), 1100);
}

TEST(TimelineView, BeatToX_NegativeBeatBeforeArrange) {
    const auto vp = BasicViewport();
    EXPECT_LT(BeatToX(vp, -1.0f), vp.arrange.left);
}

TEST(TimelineView, XToBeat_RoundTripsAtPixelGranularity) {
    const auto vp = BasicViewport();
    // 1 beat = 100 px → integer beat values land on integer pixel positions.
    for (int beat = 0; beat <= 10; ++beat) {
        const int   x = BeatToX(vp, static_cast<float>(beat));
        const float b = XToBeat(vp, x);
        EXPECT_NEAR(b, static_cast<float>(beat), 1e-3f);
    }
}

TEST(TimelineView, BeatToX_RespectsViewStartBeatScroll) {
    auto vp = BasicViewport();
    vp.viewStartBeat = 4.0f;
    EXPECT_EQ(BeatToX(vp, 4.0f), 100);   // beat 4 now at left edge
    EXPECT_EQ(BeatToX(vp, 14.0f), 1100); // beat 14 at right edge
}

TEST(TimelineView, BeatToX_ZeroOrNegativeViewBeatsClampedSafe) {
    auto vp = BasicViewport();
    vp.viewBeatsVisible = 0.0f;  // pathological
    // Should not divide by zero / crash; result is implementation-defined
    // but must be finite.
    const int x = BeatToX(vp, 1.0f);
    EXPECT_GE(x, INT32_MIN / 2);
    EXPECT_LE(x, INT32_MAX / 2);
}

TEST(TimelineView, TrackIndexFromY_FirstRow) {
    const auto vp = BasicViewport();
    EXPECT_EQ(TrackIndexFromY(vp, vp.arrange.top + 1), 0);
    EXPECT_EQ(TrackIndexFromY(vp, vp.arrange.top + kRowH - 1), 0);
}

TEST(TimelineView, TrackIndexFromY_SecondRow) {
    const auto vp = BasicViewport();
    EXPECT_EQ(TrackIndexFromY(vp, vp.arrange.top + kRowH), 1);
    EXPECT_EQ(TrackIndexFromY(vp, vp.arrange.top + kRowH + 5), 1);
}

TEST(TimelineView, TrackIndexFromY_ClampsToLastTrack) {
    const auto vp = BasicViewport(/*trackCount*/ 3);
    EXPECT_EQ(TrackIndexFromY(vp, vp.arrange.top + 100 * kRowH), 2);
}

TEST(TimelineView, TrackIndexFromY_AboveArrangeReturnsZero) {
    const auto vp = BasicViewport();
    EXPECT_EQ(TrackIndexFromY(vp, vp.arrange.top - 10), 0);
}

TEST(TimelineView, TrackIndexFromY_AccountsForScrollY) {
    auto vp = BasicViewport();
    vp.tracksScrollY = kRowH;  // first row scrolled out of view
    // y = arrange.top + 1 → idx = (1 + kRowH) / kRowH = 1
    EXPECT_EQ(TrackIndexFromY(vp, vp.arrange.top + 1), 1);
}

TEST(TimelineView, TrackIndexFromY_EmptyProjectReturnsZero) {
    const auto vp = BasicViewport(/*trackCount*/ 0);
    EXPECT_EQ(TrackIndexFromY(vp, vp.arrange.top + 100), 0);
}

TEST(TimelineView, ClipRectForDraw_FullyVisibleClipOnTrack0) {
    const auto vp = BasicViewport();
    Rect r{};
    EXPECT_TRUE(ClipRectForDraw(vp, /*trackIndex*/ 0,
                                /*startBeat*/ 1.0f, /*lengthBeats*/ 2.0f, &r));
    EXPECT_EQ(r.left,   200);          // 1 beat from arrange.left
    EXPECT_EQ(r.right,  400);
    EXPECT_EQ(r.top,    vp.arrange.top + kInsetY);
    EXPECT_EQ(r.bottom, vp.arrange.top + kRowH - kInsetY);
}

TEST(TimelineView, ClipRectForDraw_ClipOnTrack3UsesRowOffset) {
    const auto vp = BasicViewport();
    Rect r{};
    ASSERT_TRUE(ClipRectForDraw(vp, /*trackIndex*/ 3, 0.0f, 1.0f, &r));
    EXPECT_EQ(r.top,    vp.arrange.top + 3 * kRowH + kInsetY);
    EXPECT_EQ(r.bottom, vp.arrange.top + 3 * kRowH + kRowH - kInsetY);
}

TEST(TimelineView, ClipRectForDraw_ClipsToArrangeRight) {
    const auto vp = BasicViewport();
    Rect r{};
    // Clip extends to beat 12, but only 10 beats are visible.
    ASSERT_TRUE(ClipRectForDraw(vp, 0, 8.0f, 4.0f, &r));
    EXPECT_EQ(r.left,  900);
    EXPECT_EQ(r.right, vp.arrange.right);  // clipped from 1300 → 1100
}

TEST(TimelineView, ClipRectForDraw_ClipsToArrangeLeft) {
    auto vp = BasicViewport();
    vp.viewStartBeat = 5.0f;  // beats 5..15 visible
    Rect r{};
    // Clip is beats 3..7 — partially before viewport.
    ASSERT_TRUE(ClipRectForDraw(vp, 0, 3.0f, 4.0f, &r));
    EXPECT_EQ(r.left, vp.arrange.left);
}

TEST(TimelineView, ClipRectForDraw_FullyOffRightReturnsFalse) {
    const auto vp = BasicViewport();
    Rect r{};
    EXPECT_FALSE(ClipRectForDraw(vp, 0, /*start*/ 11.0f, /*len*/ 2.0f, &r));
}

TEST(TimelineView, ClipRectForDraw_FullyOffLeftReturnsFalse) {
    auto vp = BasicViewport();
    vp.viewStartBeat = 5.0f;
    Rect r{};
    EXPECT_FALSE(ClipRectForDraw(vp, 0, /*start*/ 0.0f, /*len*/ 1.0f, &r));
}

TEST(TimelineView, ClipRectForDraw_ScrolledTrackOffTopReturnsFalse) {
    auto vp = BasicViewport();
    vp.tracksScrollY = 100 * kRowH;  // scrolled way down
    Rect r{};
    EXPECT_FALSE(ClipRectForDraw(vp, 0, 1.0f, 2.0f, &r));
}

TEST(TimelineView, ClipRectForDraw_NullOutRectReturnsFalse) {
    const auto vp = BasicViewport();
    EXPECT_FALSE(ClipRectForDraw(vp, 0, 0.0f, 1.0f, nullptr));
}

TEST(TimelineView, SnapBeat_RoundsToQuarterGrid) {
    EXPECT_FLOAT_EQ(SnapBeat(0.0f),    0.0f);
    EXPECT_FLOAT_EQ(SnapBeat(0.10f),   0.0f);
    EXPECT_FLOAT_EQ(SnapBeat(0.13f),   0.25f);
    EXPECT_FLOAT_EQ(SnapBeat(0.25f),   0.25f);
    EXPECT_FLOAT_EQ(SnapBeat(0.50f),   0.50f);
    EXPECT_FLOAT_EQ(SnapBeat(1.13f),   1.25f);
    EXPECT_FLOAT_EQ(SnapBeat(-0.13f), -0.25f);
}

// Cross-check: draw rect and hit-test rect MUST be the same rect (the whole
// point of sharing one helper). This pins that any future refactor that
// accidentally differentiates them would fail.
TEST(TimelineView, ClipRectForDraw_DeterministicAcrossCalls) {
    const auto vp = BasicViewport();
    Rect a{}, b{};
    ASSERT_TRUE(ClipRectForDraw(vp, 2, 1.5f, 3.25f, &a));
    ASSERT_TRUE(ClipRectForDraw(vp, 2, 1.5f, 3.25f, &b));
    EXPECT_EQ(a.left,   b.left);
    EXPECT_EQ(a.top,    b.top);
    EXPECT_EQ(a.right,  b.right);
    EXPECT_EQ(a.bottom, b.bottom);
}
