#include "ui/panel.h"
#include "ui/draw.h"
#include "AppState.h"

// ── Panel registry implementation ────────────────────────────────────────────
// Each PanelKind shims to a self-contained UiDraw* function. The registry
// gives the dock walker a single uniform "draw this panel into this rect"
// API; all per-panel layout math lives inside the draw function itself.

namespace daw::ui {

namespace {

void DrawTransportShim(HDC hdc, const RECT& rect, AppState& state) {
    UiDrawTransport(hdc, rect, state);
}

void DrawToolsShim(HDC hdc, const RECT& rect, AppState& state) {
    UiDrawTools(hdc, rect, state);
}

void DrawRulerShim(HDC hdc, const RECT& rect, AppState& state) {
    UiDrawRuler(hdc, rect, state);
}

void DrawTracksShim(HDC hdc, const RECT& rect, AppState& state) {
    UiDrawLeftTrackPanel(hdc, rect, state);
}

void DrawBusesShim(HDC hdc, const RECT& rect, AppState& state) {
    UiDrawBusesPanel(hdc, rect, state);
}

void DrawArrangeShim(HDC hdc, const RECT& rect, AppState& state) {
    UiDrawArrangeLanes(hdc, rect, state);
}

constexpr int kCount = static_cast<int>(PanelKind::Count_);

const PanelDef kPanels[kCount] = {
    /* Transport */ {L"Transport", L"transport", &DrawTransportShim, false},
    /* Tools     */ {L"Tools",     L"tools",     &DrawToolsShim,     false},
    /* Ruler     */ {L"Ruler",     L"ruler",     &DrawRulerShim,     true },
    /* Tracks    */ {L"Tracks",    L"tracks",    &DrawTracksShim,    true },
    /* Buses     */ {L"Buses",     L"buses",     &DrawBusesShim,     false},
    /* Arrange   */ {L"Arrange",   L"arrange",   &DrawArrangeShim,   true },
};

} // namespace

const PanelDef& PanelGet(PanelKind kind) {
    const int idx = static_cast<int>(kind);
    // Defensive bounds check; out-of-range kinds return Arrange so we never
    // dereference a null draw function.
    if (idx < 0 || idx >= kCount) return kPanels[static_cast<int>(PanelKind::Arrange)];
    return kPanels[idx];
}

int PanelCount() {
    return kCount;
}

PanelKind PanelKindAt(int index) {
    if (index < 0 || index >= kCount) return PanelKind::Arrange;
    return static_cast<PanelKind>(index);
}

} // namespace daw::ui
