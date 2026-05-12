#pragma once

#ifdef DAW_PUBLIC_API
#error "This header is internal-only."
#endif

#define NOMINMAX
#include <windows.h>

#include "AppState.h"

// WM_MOUSEMOVE — drag-update dispatcher. Resolves whichever drag flag is
// active (in priority order):
//   * Dock tab drag (promote arm→active past threshold; ResolveDropTarget).
//   * Dock splitter drag (recompute parent extent + clamp ratio).
//   * Playhead, clip trim (left/right edge), clip move.
//   * Track fader, track/bus pan knob (Shift = fine).
//   * Effect slot param knob (Shift = fine; per-paramId range table).
// Returns 0 always.
LRESULT WndProcOnMouseMove(HWND hwnd, LPARAM lParam, AppState& state);
