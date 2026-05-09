#include "layout.h"
#include "../core/automation.h"
#include "../core/automation_types.h"
#include "../core/timeline.h"

// ── Left panel / track row rects ─────────────────────────────────────────────

void GetTrackFaderRects(const RECT& leftPanel, int trackIndex, RECT* rail, RECT* knob) {
    const int rowTop = leftPanel.top + kRulerHeight + trackIndex * kTrackRowHeight;
    const int rowBottom = rowTop + kTrackRowHeight;

    const int railLeft = leftPanel.right - 52;
    const int railTop = rowTop + 10;
    const int railBottom = rowBottom - 10;
    *rail = RECT{railLeft, railTop, railLeft + kFaderRailWidth, railBottom};

    *knob = RECT{railLeft - (kFaderKnobWidth - kFaderRailWidth) / 2, railTop,
                 railLeft - (kFaderKnobWidth - kFaderRailWidth) / 2 + kFaderKnobWidth,
                 railTop + kFaderKnobHeight};
}

void GetTrackButtonRects(const RECT& leftPanel, int trackIndex, RECT* muteRect, RECT* soloRect, RECT* recRect) {
    const int rowTop = leftPanel.top + kRulerHeight + trackIndex * kTrackRowHeight;
    const int buttonTop = rowTop + 34;
    *muteRect = RECT{leftPanel.left + 10, buttonTop, leftPanel.left + 28, buttonTop + 18};
    *soloRect = RECT{leftPanel.left + 30, buttonTop, leftPanel.left + 48, buttonTop + 18};
    *recRect = RECT{leftPanel.left + 50, buttonTop, leftPanel.left + 68, buttonTop + 18};
}

void GetTrackRoutingRects(const RECT& leftPanel, int trackIndex, RECT* busRect, RECT* panKnobRect, RECT* panValRect, RECT* fxRect) {
    const int rowTop = leftPanel.top + kRulerHeight + trackIndex * kTrackRowHeight;
    *busRect = RECT{leftPanel.left + 74, rowTop + 9, leftPanel.left + 156, rowTop + 27};
    *panKnobRect = RECT{leftPanel.left + 85, rowTop + 29, leftPanel.left + 113, rowTop + 57};
    *panValRect = RECT{leftPanel.left + 118, rowTop + 35, leftPanel.left + 152, rowTop + 53};
    *fxRect = RECT{leftPanel.left + 154, rowTop + 35, leftPanel.left + 190, rowTop + 53};
}

int BusPanelTop(const RECT& leftPanel, const UiState& state) {
    return leftPanel.top + kRulerHeight + static_cast<int>(state.project.tracks.size()) * kTrackRowHeight + 6;
}

void GetBusControlRects(const RECT& leftPanel, const UiState& state, int busIndex,
                        RECT* rowRect, RECT* muteRect, RECT* gainDownRect, RECT* gainUpRect,
                        RECT* panKnobRect, RECT* panValRect, RECT* fxRect) {
    constexpr int kBusRowHeight = 28;
    const int top = BusPanelTop(leftPanel, state) + 18 + busIndex * kBusRowHeight;
    *rowRect = RECT{leftPanel.left + 8, top, leftPanel.right - 8, top + kBusRowHeight - 2};
    *muteRect = RECT{leftPanel.left + 98, top + 4, leftPanel.left + 116, top + 22};
    *gainDownRect = RECT{leftPanel.left + 120, top + 4, leftPanel.left + 136, top + 22};
    *gainUpRect = RECT{leftPanel.left + 138, top + 4, leftPanel.left + 154, top + 22};
    *panKnobRect = RECT{leftPanel.left + 162, top + 3, leftPanel.left + 184, top + 25};
    *panValRect = RECT{leftPanel.left + 188, top + 4, leftPanel.left + 228, top + 22};
    *fxRect = RECT{leftPanel.left + 232, top + 4, leftPanel.left + 258, top + 22};
}

// ── Track audibility / fader math ────────────────────────────────────────────

int FaderKnobTopFromGain(const RECT& rail, float gainDb) {
    const float t = (std::clamp(gainDb, kFaderMinDb, kFaderMaxDb) - kFaderMinDb) / (kFaderMaxDb - kFaderMinDb);
    const int railHeight = static_cast<int>(rail.bottom - rail.top);
    const int travel = std::max(1, railHeight - kFaderKnobHeight);
    return rail.bottom - kFaderKnobHeight - static_cast<int>(t * static_cast<float>(travel));
}

float GainFromFaderY(const RECT& rail, int mouseY) {
    const int railTop = static_cast<int>(rail.top);
    const int railHeight = static_cast<int>(rail.bottom - rail.top);
    const int travel = std::max(1, railHeight - kFaderKnobHeight);
    const int clamped = std::clamp(mouseY - railTop, 0, travel);
    const float t = 1.0f - (static_cast<float>(clamped) / static_cast<float>(travel));
    return kFaderMinDb + t * (kFaderMaxDb - kFaderMinDb);
}

// ── Window layout ─────────────────────────────────────────────────────────────

LayoutRects ComputeLayout(const RECT& client) {
    LayoutRects l{};
    l.topBar = RECT{client.left, client.top, client.right, client.top + kTopBarHeight};
    l.leftPanel = RECT{client.left, l.topBar.bottom, client.left + kLeftPanelWidth, client.bottom};
    l.ruler = RECT{l.leftPanel.right, l.topBar.bottom, client.right, l.topBar.bottom + kRulerHeight};
    l.arrange = RECT{l.leftPanel.right, l.ruler.bottom, client.right, client.bottom};
    return l;
}

// ── Beat / coordinate math ────────────────────────────────────────────────────

float SnapBeat(float beat) {
    const float grid = 0.25f;
    return std::round(beat / grid) * grid;
}

float XToBeat(const RECT& arrange, const UiState& state, int x) {
    const int width = std::max(1, static_cast<int>(arrange.right - arrange.left));
    const float t = static_cast<float>(x - arrange.left) / static_cast<float>(width);
    return state.viewStartBeat + t * state.viewBeatsVisible;
}

int BeatToX(const RECT& area, const UiState& state, float beat) {
    const int width = std::max(1, static_cast<int>(area.right - area.left));
    const float t = (beat - state.viewStartBeat) / state.viewBeatsVisible;
    return area.left + static_cast<int>(t * static_cast<float>(width));
}

int TrackIndexFromY(const RECT& arrange, const UiState& state, int y) {
    if (state.project.tracks.empty()) {
        return 0;
    }
    if (y < arrange.top) {
        return 0;
    }
    const int idx = (y - arrange.top) / kTrackRowHeight;
    return std::clamp(idx, 0, static_cast<int>(state.project.tracks.size()) - 1);
}

bool ClipRectForDraw(const RECT& arrange, const UiState& state, const ClipItem& clip, RECT* outRect) {
    const int rowTop = arrange.top + clip.trackIndex * kTrackRowHeight + kClipInsetY;
    const int rowBottom = rowTop + (kTrackRowHeight - 2 * kClipInsetY);
    const int left = BeatToX(arrange, state, clip.startBeat);
    const int right = BeatToX(arrange, state, clip.startBeat + clip.lengthBeats);

    RECT r{left, rowTop, right, rowBottom};
    if (r.right < arrange.left || r.left > arrange.right || r.bottom < arrange.top || r.top > arrange.bottom) {
        return false;
    }

    r.left = std::max(r.left, arrange.left);
    r.right = std::min(r.right, arrange.right);
    *outRect = r;
    return true;
}
