#pragma once
#include "core/state.h"

// ── Inspector panel layout constants (used by draw.cpp and WndProc) ──
constexpr int kUiDrawInspHeaderH = 26;
constexpr int kUiDrawInspCtrlH   = 26;
constexpr int kUiDrawInspSlotH   = 22;
constexpr int kUiDrawInspPadX    = 4;
constexpr int kUiDrawInspW       = kLeftPanelWidth - kUiDrawInspPadX * 2;
constexpr int kUiDrawInspParamH  = 52;

RECT UiDrawGetInspectorPanelRect(const RECT& client, const UiState& state);

// ── Draw functions (defined in draw.cpp) ──
void UiDrawTopBar(HDC hdc, const RECT& client, UiState& state);
void UiDrawInsertInspector(HDC hdc, const RECT& client, const UiState& state);
void UiDrawLeftTrackPanel(HDC hdc, const RECT& rect, const UiState& state);
void UiDrawRuler(HDC hdc, const RECT& rect, const UiState& state);
void UiDrawArrangeLanes(HDC hdc, const RECT& rect, const UiState& state);
