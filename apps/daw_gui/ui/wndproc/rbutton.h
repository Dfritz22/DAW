#pragma once

#ifdef DAW_PUBLIC_API
#error "This header is internal-only."
#endif

#define NOMINMAX
#include <windows.h>

#include "AppState.h"

// WM_RBUTTONUP — track-area context menu (New Track + per-track Arm/Disarm).
// Hit-tests left panel + arrange; bails if neither. Builds a popup menu,
// dispatches by returned command id. Returns 0 always.
LRESULT WndProcOnRButtonUp(HWND hwnd, LPARAM lParam, AppState& state);
