#pragma once
#include "../state.h"

// ── Layout / coordinate math ─────────────────────────────────────────────────
// Defined in ui/layout.cpp; used by draw.cpp and WndProc in main.cpp.

void  GetTrackFaderRects(const RECT& leftPanel, int trackIndex, RECT* rail, RECT* knob);
void  GetTrackButtonRects(const RECT& leftPanel, int trackIndex, RECT* muteRect, RECT* soloRect, RECT* recRect);
void  GetTrackRoutingRects(const RECT& leftPanel, int trackIndex, RECT* busRect, RECT* panKnobRect, RECT* panValRect, RECT* fxRect);
int   BusPanelTop(const RECT& leftPanel, const UiState& state);
void  GetBusControlRects(const RECT& leftPanel, const UiState& state, int busIndex,
                         RECT* rowRect, RECT* muteRect, RECT* gainDownRect, RECT* gainUpRect,
                         RECT* panKnobRect, RECT* panValRect, RECT* fxRect);

int   FaderKnobTopFromGain(const RECT& rail, float gainDb);
float GainFromFaderY(const RECT& rail, int mouseY);

LayoutRects ComputeLayout(const RECT& client);
float SnapBeat(float beat);
float XToBeat(const RECT& arrange, const UiState& state, int x);
int   BeatToX(const RECT& area, const UiState& state, float beat);
int   TrackIndexFromY(const RECT& arrange, const UiState& state, int y);
bool  ClipRectForDraw(const RECT& arrange, const UiState& state, const ClipItem& clip, RECT* outRect);
