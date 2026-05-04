#pragma once
#include "state.h"

// ── Layout helpers (defined in main.cpp, used by both draw.cpp and WndProc) ──
// Inspector panel layout constants (also used by WndProc hit-testing)
constexpr int kInspHeaderH = 26;
constexpr int kInspCtrlH   = 26;
constexpr int kInspSlotH   = 22;
constexpr int kInspPadX    = 4;
constexpr int kInspW       = kLeftPanelWidth - kInspPadX * 2;
constexpr int kInspParamH  = 52;

RECT GetInspectorPanelRect(const RECT& client, const UiState& state);

float TrackGainDbAt(const UiState& state, int trackIndex);

int  BusPanelTop(const RECT& leftPanel, const UiState& state);
void GetTrackFaderRects(const RECT& leftPanel, int trackIndex, RECT* rail, RECT* knob);
void GetTrackButtonRects(const RECT& leftPanel, int trackIndex, RECT* muteRect, RECT* soloRect, RECT* recRect);
void GetTrackRoutingRects(const RECT& leftPanel, int trackIndex, RECT* busRect, RECT* panKnobRect, RECT* panValRect, RECT* fxRect);
void GetBusControlRects(const RECT& leftPanel, const UiState& state, int busIndex,
                        RECT* rowRect, RECT* muteRect, RECT* gainDownRect, RECT* gainUpRect,
                        RECT* panKnobRect, RECT* panValRect, RECT* fxRect);

bool  IsTrackAudible(const UiState& state, int trackIndex);
int   FaderKnobTopFromGain(const RECT& rail, float gainDb);
float GainFromFaderY(const RECT& rail, int mouseY);

LayoutRects ComputeLayout(const RECT& client);
float SnapBeat(float beat);
float SamplesPerBeat(const UiState& state);
float XToBeat(const RECT& arrange, const UiState& state, int x);
int   BeatToX(const RECT& area, const UiState& state, float beat);
int   TrackIndexFromY(const RECT& arrange, const UiState& state, int y);
bool  ClipRectForDraw(const RECT& arrange, const UiState& state, const ClipItem& clip, RECT* outRect);

int   TrackBusIndexAt(const UiState& state, int trackIndex);
float TrackPanAt(const UiState& state, int trackIndex);

// ── Draw primitives (defined in draw.cpp) ──
void Fill(HDC hdc, const RECT& rect, COLORREF color);
void StrokeRect(HDC hdc, const RECT& rect, COLORREF color);
void DrawButton(HDC hdc, const RECT& rect, const wchar_t* label, bool active);
void DrawPanKnob(HDC hdc, const RECT& rect, float pan, bool active);
void DrawMenuTab(HDC hdc, const RECT& rect, const wchar_t* label);

// ── Draw functions (defined in draw.cpp) ──
void DrawTopBar(HDC hdc, const RECT& client, UiState& state);
void DrawInsertInspector(HDC hdc, const RECT& client, const UiState& state);
void DrawLeftTrackPanel(HDC hdc, const RECT& rect, const UiState& state);
void DrawRuler(HDC hdc, const RECT& rect, const UiState& state);
void DrawClipWaveform(HDC hdc, const RECT& clipRect, const LoadedAudio& audio,
                      std::uint64_t sourceStartFrame, std::uint64_t sourceEndFrame);
void DrawArrangeLanes(HDC hdc, const RECT& rect, const UiState& state);
