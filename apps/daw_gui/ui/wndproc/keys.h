#pragma once

#ifdef DAW_PUBLIC_API
#error "This header is internal-only."
#endif

#define NOMINMAX
#include <windows.h>

#include "AppState.h"

// WM_KEYDOWN — global hotkey dispatch (Space/Home/R for transport via FSM,
// Ctrl+S/Shift+Ctrl+S/Ctrl+O for project IO, Ctrl+Z/Ctrl+Y/Ctrl+Shift+Z
// for undo/redo, S/Ctrl+D for clip edit, Left/Right for ¼-beat nudge,
// I for import, A/V for AutoMix/quality analyze, Esc to close inspector,
// Delete for selected clip/track, +/- for keyboard zoom). Returns 0 always.
//
// The handler reads many free functions defined in main.cpp
// (ImportWavFiles, UpdateWindowTitle) — those are forward-declared in
// keys.cpp. Other functions come from public headers (daw_project.h,
// automix_bridge.h, transport_adapter.h, vm/timeline_zoom.h).
LRESULT WndProcOnKeyDown(HWND hwnd, WPARAM wParam, AppState& state);
