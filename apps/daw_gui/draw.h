#pragma once
#include "state.h"
#include "ui/layout.h"
#include "core/timeline.h"

// ── Inspector panel layout constants (used by draw.cpp and WndProc) ──
constexpr int kInspHeaderH = 26;
constexpr int kInspCtrlH   = 26;
constexpr int kInspSlotH   = 22;
constexpr int kInspPadX    = 4;
constexpr int kInspW       = kLeftPanelWidth - kInspPadX * 2;
constexpr int kInspParamH  = 52;

RECT GetInspectorPanelRect(const RECT& client, const UiState& state);

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
