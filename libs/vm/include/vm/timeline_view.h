#pragma once

// Phase 13: libs/vm — view-model layer between core (project data) and ui
// (Win32 draw/input). Holds pure projections from project + UI scroll/zoom
// state to screen-space geometry. No Win32, no AppState, no allocations on
// the per-frame path; all callers can use these in either draw or hit-test
// paths so the two stay in sync structurally (analogous to the libs/engine
// mix_pipeline split that pinned realtime + offline together).
//
// daw::vm types use a plain int rect (Rect) rather than Win32 RECT so the
// layer stays platform-free. App shims convert at the boundary.

#include <cstdint>

namespace daw::vm {

// Plain int rect [left, right) × [top, bottom). Replaces Win32 RECT inside
// libs/vm so the lib stays platform-free.
struct Rect {
    int left;
    int top;
    int right;
    int bottom;
};

// Inputs the timeline view-model needs to project beat/track coordinates to
// screen pixels. All "Px" fields are already DPI-scaled by the caller —
// libs/vm has no opinion on DPI policy.
//
//   arrange         Pixel rect of the timeline arrangement region.
//   viewStartBeat   Beat at arrange.left.
//   viewBeatsVisible Beats that fit between arrange.left and arrange.right.
//                   Must be > 0; callers clamp.
//   tracksScrollY   Vertical scroll offset (px) into the track stack.
//   rowHeightPx     Height of one track row, DPI-scaled.
//   clipInsetYPx    Top/bottom inset of a clip within its row, DPI-scaled.
//   trackCount      Number of tracks in the project; clamps Y→track-index
//                   results into [0, trackCount).
struct TimelineViewport {
    Rect  arrange;
    float viewStartBeat;
    float viewBeatsVisible;
    int   tracksScrollY;
    int   rowHeightPx;
    int   clipInsetYPx;
    int   trackCount;
};

// Pixel x coordinate within `area` corresponding to project beat `beat`.
// `area` defaults to the viewport's arrange rect; supplying a different rect
// lets callers project into the ruler strip (which has the same horizontal
// scale as the arrange).
int   BeatToX(const TimelineViewport& vp, float beat);
int   BeatToXIn(const Rect& area, const TimelineViewport& vp, float beat);

// Project beat coordinate at pixel x within the arrange rect.
float XToBeat(const TimelineViewport& vp, int x);

// Track index for a vertical pixel coordinate within the arrange rect.
// Returns 0 when trackCount == 0 or y is above arrange.top; otherwise clamps
// into [0, trackCount).
int   TrackIndexFromY(const TimelineViewport& vp, int y);

// Computes the screen-space rect for a clip on `trackIndex` covering
// [startBeat, startBeat + lengthBeats). Returns false (and leaves *outRect
// unchanged) when the clip is fully outside the arrange rect; otherwise
// fills *outRect with the visible portion (clipped to arrange.left/right).
//
// Vertical extent: row top + clipInsetY .. row top + (rowHeight - clipInsetY).
// Horizontal extent: BeatToX(startBeat) .. BeatToX(startBeat + lengthBeats).
//
// Mirrors the legacy UiLayoutClipRectForDraw exactly; both draw and
// hit-test paths in apps/daw_gui delegate here so they cannot drift.
bool  ClipRectForDraw(const TimelineViewport& vp,
                      int trackIndex,
                      float startBeat,
                      float lengthBeats,
                      Rect* outRect);

// Snap a beat to the project grid (¼-beat resolution). Pure helper; no
// viewport state needed.
float SnapBeat(float beat);

} // namespace daw::vm
