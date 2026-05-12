#pragma once

#ifdef DAW_PUBLIC_API
#error "This header is internal-only."
#endif

#define NOMINMAX
#include <windows.h>

#include "AppState.h"

// Per-message handlers extracted from apps/daw_gui/main.cpp WindowProc.
// Each handler returns the LRESULT the WndProc should return (always 0 for
// these — they don't fall through to DefWindowProc). Caller checks `state`
// for nullptr first; handlers assume it is non-null.

// WM_MOUSEWHEEL — wheel over left panel scrolls tracks; Ctrl+wheel zooms
// timeline around the cursor; Shift+wheel scrolls tracks; otherwise pans
// the timeline horizontally. All view math goes through daw::vm.
LRESULT WndProcOnMouseWheel(HWND hwnd, WPARAM wParam, LPARAM lParam, AppState& state);

// WM_MOUSEHWHEEL — horizontal wheel pans the timeline view start.
LRESULT WndProcOnMouseHWheel(HWND hwnd, WPARAM wParam, AppState& state);

// WM_CAPTURECHANGED — clear all in-progress drag state when capture is
// lost (user hit Escape, app lost focus, etc.).
LRESULT WndProcOnCaptureChanged(AppState& state);
