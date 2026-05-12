#pragma once

#ifdef DAW_PUBLIC_API
#error "This header is internal-only."
#endif

#include "AppState.h"

// ── Inspector panel layout constants (used by draw.cpp and WndProc) ──
constexpr int kUiDrawInspHeaderH = 26;
constexpr int kUiDrawInspCtrlH   = 26;
constexpr int kUiDrawInspSlotH   = 22;
constexpr int kUiDrawInspPadX    = 4;
constexpr int kUiDrawInspW       = kLeftPanelWidth - kUiDrawInspPadX * 2;
constexpr int kUiDrawInspParamH  = 52;

RECT UiDrawGetInspectorPanelRect(const RECT& client, const AppState& state);

// ── Draw functions (defined in draw.cpp) ──
void UiDrawTopBar(HDC hdc, const RECT& client, AppState& state);
void UiDrawTransport(HDC hdc, const RECT& rect, AppState& state);
void UiDrawTools(HDC hdc, const RECT& rect, AppState& state);
void UiDrawStatusBar(HDC hdc, const RECT& rect, const AppState& state);
void UiDrawInsertInspector(HDC hdc, const RECT& client, const AppState& state);
void UiDrawLeftTrackPanel(HDC hdc, const RECT& rect, const AppState& state);
void UiDrawBusesPanel(HDC hdc, const RECT& rect, const AppState& state);
void UiDrawRuler(HDC hdc, const RECT& rect, const AppState& state);
void UiDrawArrangeLanes(HDC hdc, const RECT& rect, const AppState& state);

// Renders the tab-drag drop preview overlay (translucent fill + 2 px accent
// border around dropPreviewRect, plus the 5-square compass centered on the
// target leaf). No-ops when state.ui.dragTabActive is false or no drop
// target has been resolved yet. Pure GDI; lives in draw.cpp so the WM_PAINT
// handler in main.cpp stays focused on dock walking + composition.
void UiDrawDockDropOverlay(HDC hdc, const AppState& state);
