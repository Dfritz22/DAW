#include "ui/layout.h"
#include "ui/dpi.h"

#include <algorithm>
#include <cmath>

// ── Left panel / track row rects ─────────────────────────────────────────────
// All literal pixel offsets are authored at 96-DPI baseline and scaled through
// `Dpi(...)` so the same code produces correctly-sized rects on any monitor.

void UiLayoutGetTrackFaderRects(const RECT& leftPanel, int trackIndex, RECT* rail, RECT* knob, int scrollY) {
    const int rowTop = leftPanel.top + Dpi(kRulerHeight) + trackIndex * Dpi(kTrackRowHeight) - scrollY;
    const int rowBottom = rowTop + Dpi(kTrackRowHeight);

    const int railLeft = leftPanel.right - Dpi(52);
    const int railTop = rowTop + Dpi(10);
    const int railBottom = rowBottom - Dpi(10);
    *rail = RECT{railLeft, railTop, railLeft + Dpi(kFaderRailWidth), railBottom};

    const int knobOffset = (Dpi(kFaderKnobWidth) - Dpi(kFaderRailWidth)) / 2;
    *knob = RECT{railLeft - knobOffset, railTop,
                 railLeft - knobOffset + Dpi(kFaderKnobWidth),
                 railTop + Dpi(kFaderKnobHeight)};
}

void UiLayoutGetTrackButtonRects(const RECT& leftPanel, int trackIndex, RECT* muteRect, RECT* soloRect, RECT* recRect, int scrollY) {
    const int rowTop = leftPanel.top + Dpi(kRulerHeight) + trackIndex * Dpi(kTrackRowHeight) - scrollY;
    const int buttonTop = rowTop + Dpi(34);
    *muteRect = RECT{leftPanel.left + Dpi(10), buttonTop, leftPanel.left + Dpi(28), buttonTop + Dpi(18)};
    *soloRect = RECT{leftPanel.left + Dpi(30), buttonTop, leftPanel.left + Dpi(48), buttonTop + Dpi(18)};
    *recRect  = RECT{leftPanel.left + Dpi(50), buttonTop, leftPanel.left + Dpi(68), buttonTop + Dpi(18)};
}

void UiLayoutGetTrackRoutingRects(const RECT& leftPanel, int trackIndex, RECT* busRect, RECT* panKnobRect, RECT* panValRect, RECT* fxRect, int scrollY) {
    const int rowTop = leftPanel.top + Dpi(kRulerHeight) + trackIndex * Dpi(kTrackRowHeight) - scrollY;
    *busRect     = RECT{leftPanel.left + Dpi(74), rowTop + Dpi(9),  leftPanel.left + Dpi(156), rowTop + Dpi(27)};
    *panKnobRect = RECT{leftPanel.left + Dpi(85), rowTop + Dpi(29), leftPanel.left + Dpi(113), rowTop + Dpi(57)};
    *panValRect  = RECT{leftPanel.left + Dpi(118), rowTop + Dpi(35), leftPanel.left + Dpi(152), rowTop + Dpi(53)};
    *fxRect      = RECT{leftPanel.left + Dpi(154), rowTop + Dpi(35), leftPanel.left + Dpi(190), rowTop + Dpi(53)};
}

int UiLayoutTracksRegionTop(const RECT& leftPanel) {
    return leftPanel.top + Dpi(kRulerHeight);
}

int UiLayoutTracksRegionBottom(const RECT& leftPanel) {
    // Tracks region fills the entire Tracks panel (the bus mixer is now its
    // own dock leaf, not a pinned strip inside the track panel).
    return leftPanel.bottom;
}

int UiLayoutMaxTracksScrollY(const RECT& leftPanel, const AppState& state) {
    const int regionH = std::max(0, UiLayoutTracksRegionBottom(leftPanel) - UiLayoutTracksRegionTop(leftPanel));
    const int contentH = static_cast<int>(state.core.project.tracks.size()) * Dpi(kTrackRowHeight);
    return std::max(0, contentH - regionH);
}

int UiLayoutBusPanelTop(const RECT& /*leftPanel*/, const AppState& /*state*/) {
    // (legacy signature kept for ABI; bus panel is now pinned.)
    return 0;
}

void UiLayoutGetBusControlRects(const RECT& leftPanel, const AppState& /*state*/, int busIndex,
                        RECT* rowRect, RECT* muteRect, RECT* gainDownRect, RECT* gainUpRect,
                        RECT* panKnobRect, RECT* panValRect, RECT* fxRect) {
    const int panelTop = leftPanel.bottom - Dpi(kBusPanelHeight) + Dpi(kBusPanelTopMargin);
    const int top = panelTop + Dpi(kBusPanelHeaderHeight) + busIndex * Dpi(kBusRowHeight);
    *rowRect      = RECT{leftPanel.left + Dpi(8),   top,           leftPanel.right - Dpi(8), top + Dpi(kBusRowHeight) - Dpi(2)};
    *muteRect     = RECT{leftPanel.left + Dpi(98),  top + Dpi(4),  leftPanel.left + Dpi(116), top + Dpi(22)};
    *gainDownRect = RECT{leftPanel.left + Dpi(120), top + Dpi(4),  leftPanel.left + Dpi(136), top + Dpi(22)};
    *gainUpRect   = RECT{leftPanel.left + Dpi(138), top + Dpi(4),  leftPanel.left + Dpi(154), top + Dpi(22)};
    *panKnobRect  = RECT{leftPanel.left + Dpi(162), top + Dpi(3),  leftPanel.left + Dpi(184), top + Dpi(25)};
    *panValRect   = RECT{leftPanel.left + Dpi(188), top + Dpi(4),  leftPanel.left + Dpi(228), top + Dpi(22)};
    *fxRect       = RECT{leftPanel.left + Dpi(232), top + Dpi(4),  leftPanel.left + Dpi(258), top + Dpi(22)};
}

// Bus panel rect = busPanelRect.top is the top of the bus area, .bottom the
// bottom. This bypasses the legacy "compute from leftPanel.bottom" math so the
// Buses panel can be docked anywhere by the dock walker.
void UiLayoutGetBusControlRectsInPanel(const RECT& busPanelRect, int busIndex,
                        RECT* rowRect, RECT* muteRect, RECT* gainDownRect, RECT* gainUpRect,
                        RECT* panKnobRect, RECT* panValRect, RECT* fxRect) {
    const int panelTop = busPanelRect.top + Dpi(kBusPanelTopMargin);
    const int top = panelTop + Dpi(kBusPanelHeaderHeight) + busIndex * Dpi(kBusRowHeight);
    *rowRect      = RECT{busPanelRect.left + Dpi(8),   top,           busPanelRect.right - Dpi(8), top + Dpi(kBusRowHeight) - Dpi(2)};
    *muteRect     = RECT{busPanelRect.left + Dpi(98),  top + Dpi(4),  busPanelRect.left + Dpi(116), top + Dpi(22)};
    *gainDownRect = RECT{busPanelRect.left + Dpi(120), top + Dpi(4),  busPanelRect.left + Dpi(136), top + Dpi(22)};
    *gainUpRect   = RECT{busPanelRect.left + Dpi(138), top + Dpi(4),  busPanelRect.left + Dpi(154), top + Dpi(22)};
    *panKnobRect  = RECT{busPanelRect.left + Dpi(162), top + Dpi(3),  busPanelRect.left + Dpi(184), top + Dpi(25)};
    *panValRect   = RECT{busPanelRect.left + Dpi(188), top + Dpi(4),  busPanelRect.left + Dpi(228), top + Dpi(22)};
    *fxRect       = RECT{busPanelRect.left + Dpi(232), top + Dpi(4),  busPanelRect.left + Dpi(258), top + Dpi(22)};
}

// ── Track audibility / fader math ────────────────────────────────────────────

int UiLayoutFaderKnobTopFromGain(const RECT& rail, float gainDb) {
    const float t = (std::clamp(gainDb, kFaderMinDb, kFaderMaxDb) - kFaderMinDb) / (kFaderMaxDb - kFaderMinDb);
    const int railHeight = static_cast<int>(rail.bottom - rail.top);
    const int knobH = Dpi(kFaderKnobHeight);
    const int travel = std::max(1, railHeight - knobH);
    return rail.bottom - knobH - static_cast<int>(t * static_cast<float>(travel));
}

float UiLayoutGainFromFaderY(const RECT& rail, int mouseY) {
    const int railTop = static_cast<int>(rail.top);
    const int railHeight = static_cast<int>(rail.bottom - rail.top);
    const int travel = std::max(1, railHeight - Dpi(kFaderKnobHeight));
    const int clamped = std::clamp(mouseY - railTop, 0, travel);
    const float t = 1.0f - (static_cast<float>(clamped) / static_cast<float>(travel));
    return kFaderMinDb + t * (kFaderMaxDb - kFaderMinDb);
}

// ── Window layout ─────────────────────────────────────────────────────────────

LayoutRects UiLayoutComputeLayout(const RECT& client) {
    LayoutRects l{};
    l.topBar    = RECT{client.left, client.top, client.right, client.top + Dpi(kTopBarHeight)};
    l.leftPanel = RECT{client.left, l.topBar.bottom, client.left + Dpi(kLeftPanelWidth), client.bottom};
    l.ruler     = RECT{l.leftPanel.right, l.topBar.bottom, client.right, l.topBar.bottom + Dpi(kRulerHeight)};
    l.arrange   = RECT{l.leftPanel.right, l.ruler.bottom, client.right, client.bottom};
    return l;
}

// ── Beat / coordinate math ────────────────────────────────────────────────────

float UiLayoutSnapBeat(float beat) {
    const float grid = 0.25f;
    return std::round(beat / grid) * grid;
}

float UiLayoutXToBeat(const RECT& arrange, const AppState& state, int x) {
    const int width = std::max(1, static_cast<int>(arrange.right - arrange.left));
    const float t = static_cast<float>(x - arrange.left) / static_cast<float>(width);
    return state.ui.viewStartBeat + t * state.ui.viewBeatsVisible;
}

int UiLayoutBeatToX(const RECT& area, const AppState& state, float beat) {
    const int width = std::max(1, static_cast<int>(area.right - area.left));
    const float t = (beat - state.ui.viewStartBeat) / state.ui.viewBeatsVisible;
    return area.left + static_cast<int>(t * static_cast<float>(width));
}

int UiLayoutTrackIndexFromY(const RECT& arrange, const AppState& state, int y) {
    if (state.core.project.tracks.empty()) {
        return 0;
    }
    if (y < arrange.top) {
        return 0;
    }
    const int idx = (y - arrange.top + state.ui.tracksScrollY) / Dpi(kTrackRowHeight);
    return std::clamp(idx, 0, static_cast<int>(state.core.project.tracks.size()) - 1);
}

bool UiLayoutClipRectForDraw(const RECT& arrange, const AppState& state, const ClipItem& clip, RECT* outRect) {
    const int rowH = Dpi(kTrackRowHeight);
    const int insetY = Dpi(kClipInsetY);
    const int rowTop = arrange.top + clip.trackIndex * rowH + insetY - state.ui.tracksScrollY;
    const int rowBottom = rowTop + (rowH - 2 * insetY);
    const int left = UiLayoutBeatToX(arrange, state, clip.startBeat);
    const int right = UiLayoutBeatToX(arrange, state, clip.startBeat + clip.lengthBeats);

    RECT r{left, rowTop, right, rowBottom};
    if (r.right < arrange.left || r.left > arrange.right || r.bottom < arrange.top || r.top > arrange.bottom) {
        return false;
    }

    r.left = std::max(r.left, arrange.left);
    r.right = std::min(r.right, arrange.right);
    *outRect = r;
    return true;
}
