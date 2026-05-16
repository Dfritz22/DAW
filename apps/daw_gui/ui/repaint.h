#pragma once

#ifdef DAW_PUBLIC_API
#error "This header is internal-only."
#endif

struct AppState;

namespace daw::ui {

// Phase 19 / Step F — Repaint policy.
//
// Single chokepoint for "model state changed, repaint everything". Walks
// `state.hwnd` (main window) and every `state.ui.dock.floatingPanels[*].hwnd`,
// calling InvalidateRect(..., nullptr, FALSE) on each. No-op for null/dead
// HWNDs.
//
// Replaces the legacy pattern `InvalidateRect(state.hwnd, nullptr, FALSE)`
// which only repaints the main window — leaving floating Mixer / meter
// panels stale. Required before any live-meter or floating-automation work.
void RequestRepaintAll(AppState& state);

}  // namespace daw::ui
