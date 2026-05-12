#pragma once

// ── Panel registry ───────────────────────────────────────────────────────────
// A "panel" is one self-contained dockable UI region (the track list, the
// arrange grid, the bus mixer, the transport strip, etc.). Each panel knows
// only how to draw / hit-test itself given a RECT and the AppState.
//
// The registry decouples *what to draw* (PanelKind) from *where to draw it*
// (the dock layout walker). This is the seam the upcoming Unity-style dock
// tree (split-H / split-V / tabbed-leaf) will plug into: a leaf node holds
// one or more PanelKind values, and the walker calls `PanelGet(kind).draw`
// on the leaf's rect.
//
// During Phase 1 the layout is still computed by UiLayoutComputeLayout(); the
// draw entry points below are thin shims that forward to the existing
// UiDraw* functions. Once the dock tree lands they become the *only* draw
// surface and UiDrawTopBar / UiDrawLeftTrackPanel / etc. are deleted.

#define NOMINMAX
#include <windows.h>

struct AppState;

namespace daw::ui {

enum class PanelKind {
    Transport,    // Play / Stop / Record / BPM / Count-In / position
    Tools,        // Import / AutoMix / Vocal Check / Auto Master / Met / Monitor
    Ruler,        // Beat ruler above the arrange grid
    Tracks,       // Left-panel track strip (faders, mute/solo/rec, routing)
    Buses,        // Bus mixer (Drums / Music / Vocals / Master)
    Arrange,      // Clip lanes / playhead / grid

    Count_,       // sentinel
};

using PanelDrawFn = void (*)(HDC hdc, const RECT& rect, AppState& state);

struct PanelDef {
    const wchar_t* title;        // Shown on tab strips and Window menu
    const wchar_t* persistKey;   // Stable ID for layout.json (never rename)
    PanelDrawFn    draw;
    // Primary panels (Ruler, Arrange) are pinned: they cannot be dragged,
    // closed, tabbed, or torn off into a separate window. Other panels may
    // dock to their edges, but the primary panel is always the centerpiece
    // everything else snaps around. The dock chrome skips its tab strip.
    bool           primary;
};

// Look up a panel's metadata + draw function by kind.
const PanelDef& PanelGet(PanelKind kind);

// Iterate every registered panel (used to build the Window menu, layout.json
// validators, etc.).
int PanelCount();
PanelKind PanelKindAt(int index);

} // namespace daw::ui
