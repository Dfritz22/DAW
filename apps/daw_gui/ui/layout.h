#pragma once

#ifdef DAW_PUBLIC_API
#error "This header is internal-only."
#endif

#include "core/state.h"
#include "daw_automation.h"
#include "daw_timeline.h"

// ── Layout / coordinate math ─────────────────────────────────────────────────
// Defined in ui/layout.cpp; used by draw.cpp and WndProc in main.cpp.

void  UiLayoutGetTrackFaderRects(const RECT& leftPanel, int trackIndex, RECT* rail, RECT* knob);
void  UiLayoutGetTrackButtonRects(const RECT& leftPanel, int trackIndex, RECT* muteRect, RECT* soloRect, RECT* recRect);
void  UiLayoutGetTrackRoutingRects(const RECT& leftPanel, int trackIndex, RECT* busRect, RECT* panKnobRect, RECT* panValRect, RECT* fxRect);
int   UiLayoutBusPanelTop(const RECT& leftPanel, const UiState& state);
void  UiLayoutGetBusControlRects(const RECT& leftPanel, const UiState& state, int busIndex,
                         RECT* rowRect, RECT* muteRect, RECT* gainDownRect, RECT* gainUpRect,
                         RECT* panKnobRect, RECT* panValRect, RECT* fxRect);

int   UiLayoutFaderKnobTopFromGain(const RECT& rail, float gainDb);
float UiLayoutGainFromFaderY(const RECT& rail, int mouseY);

LayoutRects UiLayoutComputeLayout(const RECT& client);
float UiLayoutSnapBeat(float beat);
float UiLayoutXToBeat(const RECT& arrange, const UiState& state, int x);
int   UiLayoutBeatToX(const RECT& area, const UiState& state, float beat);
int   UiLayoutTrackIndexFromY(const RECT& arrange, const UiState& state, int y);
bool  UiLayoutClipRectForDraw(const RECT& arrange, const UiState& state, const ClipItem& clip, RECT* outRect);
