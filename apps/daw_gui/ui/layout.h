#pragma once

#ifdef DAW_PUBLIC_API
#error "This header is internal-only."
#endif

#include "AppState.h"

// ── Layout / coordinate math ─────────────────────────────────────────────────
// Defined in ui/layout.cpp; used by draw.cpp and WndProc in main.cpp.

void  UiLayoutGetTrackFaderRects(const RECT& leftPanel, int trackIndex, RECT* rail, RECT* knob, int scrollY = 0);
void  UiLayoutGetTrackButtonRects(const RECT& leftPanel, int trackIndex, RECT* muteRect, RECT* soloRect, RECT* recRect, int scrollY = 0);
void  UiLayoutGetTrackRoutingRects(const RECT& leftPanel, int trackIndex, RECT* busRect, RECT* panKnobRect, RECT* panValRect, RECT* fxRect, int scrollY = 0);
int   UiLayoutBusPanelTop(const RECT& leftPanel, const AppState& state);
int   UiLayoutTracksRegionTop(const RECT& leftPanel);
int   UiLayoutTracksRegionBottom(const RECT& leftPanel);
int   UiLayoutMaxTracksScrollY(const RECT& leftPanel, const AppState& state);
void  UiLayoutGetBusControlRects(const RECT& leftPanel, const AppState& state, int busIndex,
                         RECT* rowRect, RECT* muteRect, RECT* gainDownRect, RECT* gainUpRect,
                         RECT* panKnobRect, RECT* panValRect, RECT* fxRect);

// Same as UiLayoutGetBusControlRects, but `busPanelRect` is the bus panel's
// own rect (top = top of the bus area, bottom = bottom of the bus area).
// Used by the standalone Buses panel draw which doesn't know the leftPanel.
void  UiLayoutGetBusControlRectsInPanel(const RECT& busPanelRect, int busIndex,
                         RECT* rowRect, RECT* muteRect, RECT* gainDownRect, RECT* gainUpRect,
                         RECT* panKnobRect, RECT* panValRect, RECT* fxRect);

int   UiLayoutFaderKnobTopFromGain(const RECT& rail, float gainDb);
float UiLayoutGainFromFaderY(const RECT& rail, int mouseY);

LayoutRects UiLayoutComputeLayout(const RECT& client);
float UiLayoutSnapBeat(float beat);
float UiLayoutXToBeat(const RECT& arrange, const AppState& state, int x);
int   UiLayoutBeatToX(const RECT& area, const AppState& state, float beat);
int   UiLayoutTrackIndexFromY(const RECT& arrange, const AppState& state, int y);
bool  UiLayoutClipRectForDraw(const RECT& arrange, const AppState& state, const ClipItem& clip, RECT* outRect);

// Returns the content-rect (tab strip stripped) of the dock leaf currently
// hosting `kind`, or `fallback` if no leaf hosts it. Used by hit-tests so
// they follow user dock resizes/rearrangements rather than the legacy
// fixed UiLayoutComputeLayout.
RECT UiLayoutFindDockLeafRect(const AppState& state, daw::ui::PanelKind kind, const RECT& fallback);

// Builds a LayoutRects from cached dock leaf rects (Tracks/Ruler/Arrange).
// Falls back to UiLayoutComputeLayout when no dock layout has been built
// yet (first paint hasn't happened). Top bar always uses the fallback
// strip rect.
LayoutRects UiLayoutComputeHitTestLayout(HWND hwnd, const AppState& state);
