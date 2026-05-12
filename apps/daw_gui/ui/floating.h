#pragma once

#ifdef DAW_PUBLIC_API
#error "This header is internal-only."
#endif

#define NOMINMAX
#include <windows.h>

#include "AppState.h"
#include "ui/dock.h"

// Floating-panel window infrastructure.
//
// Each floating window hosts a single PanelKind torn off from the main
// dock tree. The window class is registered once in WinMain (under
// kFloatingClassName from ui/UiRuntimeState.h) with FloatingWindowProc
// as its lpfnWndProc.

LRESULT CALLBACK FloatingWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Spawn a floating top-level window hosting `panel` at the given screen
// rect. `restoreOnFail` controls whether to put the panel back into the
// main dock if window creation fails: true for tear-off (caller already
// removed the tab), false for layout-restore (panel was never docked).
HWND SpawnFloatingPanelAt(AppState& state, daw::ui::PanelKind panel,
                          int x, int y, int w, int h,
                          bool restoreOnFail);

// Tear-off entry point: anchor `panel` near `screenPt` (where the user
// dropped the tab). Caller has already removed the panel from the dock.
void SpawnFloatingPanel(AppState& state, daw::ui::PanelKind panel, POINT screenPt);
