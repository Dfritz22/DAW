#pragma once

#ifdef DAW_PUBLIC_API
#error "This header is internal-only."
#endif

#define NOMINMAX
#include <windows.h>

#include "AppState.h"

// WM_PAINT — main-window double-buffered paint:
//   1. Allocate a memory DC sized to the client rect.
//   2. Fill window background.
//   3. Top bar.
//   4. Lazily build dockRoot from layout.json (or DockBuildDefault) on
//      first paint; spawn any persisted floating panels.
//   5. Compute per-leaf rects, render dock leaves + tab strip + splitters
//      (populates state.ui.dockTabs for WM_LBUTTONDOWN hit-test).
//   6. Tab-drag drop overlay.
//   7. Insert/FX inspector floating layer.
//   8. Status bar.
//   9. BitBlt backbuffer to screen.
LRESULT WndProcOnPaint(HWND hwnd, AppState& state);
