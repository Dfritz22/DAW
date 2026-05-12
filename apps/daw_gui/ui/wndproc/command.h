#pragma once

#ifdef DAW_PUBLIC_API
#error "This header is internal-only."
#endif

#define NOMINMAX
#include <windows.h>

#include "AppState.h"

// WM_COMMAND — dispatch a menu-bar / accelerator command id. Handles the full
// File / View / Window / Audio / Track command space (kCmd* in
// ui/UiRuntimeState.h), then issues a single InvalidateRect at the end.
void WndProcOnMenuCommand(HWND hwnd, AppState& state, UINT cmd);
