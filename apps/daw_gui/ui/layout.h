#pragma once

#ifdef DAW_PUBLIC_API
#error "This header is internal-only."
#endif

#include "AppState.h"

// ── Layout / coordinate math ─────────────────────────────────────────────────
// Defined in ui/layout.cpp; used by draw.cpp and WndProc in main.cpp.

void  UiLayoutGetTrackFaderRects(const RECT& leftPanel, int trackIndex, RECT* rail, RECT* knob);
void  UiLayoutGetTrackButtonRects(const RECT& leftPanel, int trackIndex, RECT* muteRect, RECT* soloRect, RECT* recRect);
void  UiLayoutGetTrackRoutingRects(const RECT& leftPanel, int trackIndex, RECT* busRect, RECT* panKnobRect, RECT* panValRect, RECT* fxRect);
int   UiLayoutBusPanelTop(const RECT& leftPanel, const AppState& state);
void  UiLayoutGetBusControlRects(const RECT& leftPanel, const AppState& state, int busIndex,
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
