// ─────────────────────────────────────────────────────────────────────────────
// ui/draw/chrome.cpp
//
// Outer "chrome" surfaces — top bar, transport panel, tools panel, status bar.
// Lifted from ui/draw.cpp in Phase 17b. Behavior identical; helpers come from
// ui/draw/draw_internal.h (Phase 17a).
// ─────────────────────────────────────────────────────────────────────────────

#include "ui/draw/draw_internal.h"  // Fill, DrawButton, ...
#include "ui/draw.h"
#include "ui/dpi.h"

#include <algorithm>
#include <cmath>
#include <string>

using namespace daw::internal::ui;

void UiDrawTopBar(HDC hdc, const RECT& client, AppState& state) {
    RECT top{client.left, client.top, client.right, client.top + Dpi(kTopBarHeight)};
    Fill(hdc, top, kPalette.topBar);
    RECT edge{client.left, top.bottom - 1, client.right, top.bottom};
    Fill(hdc, edge, kPalette.topBarEdge);

    state.ui.fileMenuRect  = RECT{0, 0, 0, 0};
    state.ui.viewMenuRect  = RECT{0, 0, 0, 0};
    state.ui.audioMenuRect = RECT{0, 0, 0, 0};
    state.ui.trackMenuRect = RECT{0, 0, 0, 0};

    // Legacy compatibility: top bar = transport (left half) + tools (mixed)
    // sharing the same horizontal strip. Once the dock tree owns the layout,
    // each panel's rect will arrive from the dock walker.
    UiDrawTransport(hdc, top, state);
    UiDrawTools(hdc, top, state);
}

// ── Transport panel ──────────────────────────────────────────────────────────
// Play / Stop / Record + BPM-/BPM+ / Count-In + status text + SRC indicator.
// Stores hit-test rects in AppState so main.cpp WM_LBUTTONDOWN keeps working.
void UiDrawTransport(HDC hdc, const RECT& rect, AppState& state) {
    state.ui.playRect    = RECT{rect.left + Dpi(22),   rect.top + Dpi(34), rect.left + Dpi(96),   rect.top + Dpi(58)};
    state.ui.stopRect    = RECT{rect.left + Dpi(104),  rect.top + Dpi(34), rect.left + Dpi(178),  rect.top + Dpi(58)};
    state.ui.recordRect  = RECT{rect.left + Dpi(186),  rect.top + Dpi(34), rect.left + Dpi(260),  rect.top + Dpi(58)};
    state.ui.bpmDownRect = RECT{rect.left + Dpi(1094), rect.top + Dpi(34), rect.left + Dpi(1126), rect.top + Dpi(58)};
    state.ui.bpmUpRect   = RECT{rect.left + Dpi(1132), rect.top + Dpi(34), rect.left + Dpi(1164), rect.top + Dpi(58)};
    state.ui.countInRect = RECT{rect.left + Dpi(1170), rect.top + Dpi(34), rect.left + Dpi(1292), rect.top + Dpi(58)};

    DrawButton(hdc, state.ui.playRect,    state.audio.playing   ? L"Playing"   : L"Play",   state.audio.playing);
    DrawButton(hdc, state.ui.stopRect,    L"Stop", false);
    DrawButton(hdc, state.ui.recordRect,  state.audio.recording ? L"Recording" : L"Record", state.audio.recording);
    DrawButton(hdc, state.ui.bpmDownRect, L"BPM-", false);
    DrawButton(hdc, state.ui.bpmUpRect,   L"BPM+", false);
    DrawButton(hdc, state.ui.countInRect, state.audio.countInEnabled ? L"Count-In On" : L"Count-In Off", state.audio.countInEnabled);
}

// ── Status bar (bottom strip — fixed, not movable) ───────────────────────────
// Project info (BPM/SR/Tracks/View) + selected I/O devices, centered.
// Painted directly by the renderer over a reserved strip at the bottom of
// the client area, so the dock tree never sees this region.
void UiDrawStatusBar(HDC hdc, const RECT& rect, const AppState& state) {
    Fill(hdc, rect, kPalette.topBar);
    RECT edge{rect.left, rect.top, rect.right, rect.top + 1};
    Fill(hdc, edge, kPalette.topBarEdge);

    SetBkMode(hdc, TRANSPARENT);

    const int visibleBars = std::max(1, static_cast<int>(std::round(state.ui.viewBeatsVisible / 4.0f)));
    std::wstring status =
        L"BPM " + std::to_wstring(static_cast<int>(state.core.project.bpm)) +
        L"   |   SR " + std::to_wstring(state.core.project.projectSampleRate > 0 ? state.core.project.projectSampleRate : 0) +
        L"   |   Tracks " + std::to_wstring(static_cast<int>(state.core.project.tracks.size())) +
        L"   |   View " + std::to_wstring(visibleBars) + L" bars" +
        L"   |   In " + state.audio.selectedInputDeviceName +
        L"   |   Out " + state.audio.selectedOutputDeviceName;

    RECT statusRect{rect.left + Dpi(8), rect.top, rect.right - Dpi(8), rect.bottom};
    SetTextColor(hdc, kPalette.textPrimary);
    DrawTextW(hdc, status.c_str(), -1, &statusRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

// ── Tools panel ──────────────────────────────────────────────────────────────
// Import / AutoMix / Vocal Check / Auto Master / Met Play / Met Rec / Monitor.
void UiDrawTools(HDC hdc, const RECT& rect, AppState& state) {
    state.ui.importRect     = RECT{rect.left + Dpi(268),  rect.top + Dpi(34), rect.left + Dpi(378),  rect.top + Dpi(58)};
    state.ui.automixRect    = RECT{rect.left + Dpi(386),  rect.top + Dpi(34), rect.left + Dpi(522),  rect.top + Dpi(58)};
    state.ui.vocalCheckRect = RECT{rect.left + Dpi(530),  rect.top + Dpi(34), rect.left + Dpi(664),  rect.top + Dpi(58)};
    state.ui.autoMasterRect = RECT{rect.left + Dpi(672),  rect.top + Dpi(34), rect.left + Dpi(806),  rect.top + Dpi(58)};
    state.ui.metPlayRect    = RECT{rect.left + Dpi(814),  rect.top + Dpi(34), rect.left + Dpi(900),  rect.top + Dpi(58)};
    state.ui.metRecRect     = RECT{rect.left + Dpi(906),  rect.top + Dpi(34), rect.left + Dpi(992),  rect.top + Dpi(58)};
    state.ui.monitorRect    = RECT{rect.left + Dpi(998),  rect.top + Dpi(34), rect.left + Dpi(1088), rect.top + Dpi(58)};

    DrawButton(hdc, state.ui.importRect,     L"Import WAV", false);
    DrawButton(hdc, state.ui.automixRect,    state.audio.automixRunning.load() ? L"AutoMixing..." : L"Apply AutoMix", state.audio.automixRunning.load());
    DrawButton(hdc, state.ui.vocalCheckRect, L"Vocal Check", false);
    DrawButton(hdc, state.ui.autoMasterRect, L"Auto Master", false);
    DrawButton(hdc, state.ui.metPlayRect,    state.audio.metronomePlay   ? L"Met Play On" : L"Met Play Off", state.audio.metronomePlay);
    DrawButton(hdc, state.ui.metRecRect,     state.audio.metronomeRecord ? L"Met Rec On"  : L"Met Rec Off",  state.audio.metronomeRecord);
    DrawButton(hdc, state.ui.monitorRect,    state.audio.inputMonitoring ? L"Monitor On"  : L"Monitor Off",  state.audio.inputMonitoring);
}
