#pragma once

#ifdef DAW_PUBLIC_API
#error "This header is internal-only."
#endif

#define NOMINMAX
#include <windows.h>

#include "AppState.h"

// WM_LBUTTONUP — drop-target finalization. Three drag-end paths:
//   * Dock tab drag commit (reorder / tab-into / split / tear-off into a
//     floating window) when dragTabArmed.
//   * Splitter drag end.
//   * In-arrange drags (playhead, clip trim, clip move, fader, pan,
//     param knob) — clear flags + invalidate.
// Returns 0 always.
LRESULT WndProcOnLButtonUp(HWND hwnd, AppState& state);
