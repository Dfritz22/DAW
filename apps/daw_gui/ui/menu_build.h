#pragma once

#ifdef DAW_PUBLIC_API
#error "This header is internal-only."
#endif

#define NOMINMAX
#include <windows.h>

#include "AppState.h"

// Build the application's top-level HMENU bar (File / View / Audio / Track
// / Window). Called once at startup; the returned HMENU is attached to the
// main window via SetMenu(). Top-level popups are refreshed lazily in
// WM_INITMENUPOPUP via UiRefreshTopLevelPopup so dynamic content (device
// list, current sample rate / backend / buffer-size checkmarks) reflects
// current state.
HMENU UiBuildMainMenuBar(AppState& state);

// Repopulate the contents of one of the four top-level popups. Returns true
// if `popup` was one of ours (caller should swallow the message), false if
// it's the system menu or some other popup (caller should DefWindowProc).
bool UiRefreshTopLevelPopup(HMENU popup, AppState& state);
