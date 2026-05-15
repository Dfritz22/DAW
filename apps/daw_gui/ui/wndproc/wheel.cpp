#include "ui/wndproc/wheel.h"

#include "ui/dpi.h"
#include "ui/layout.h"
#include "vm/timeline_zoom.h"

#include <windowsx.h>

#include <algorithm>

LRESULT WndProcOnMouseWheel(HWND hwnd, WPARAM wParam, LPARAM lParam, AppState& state) {
    const short delta = GET_WHEEL_DELTA_WPARAM(wParam);
    const bool ctrl = (GET_KEYSTATE_WPARAM(wParam) & MK_CONTROL) != 0;

    // Determine cursor location in client coords.
    POINT wpt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
    ScreenToClient(hwnd, &wpt);
    const LayoutRects wlayout = UiLayoutComputeHitTestLayout(hwnd, state);

    // Wheel over the left panel scrolls the tracks list vertically.
    if (PtInRect(&wlayout.leftPanel, wpt) &&
        wpt.y >= UiLayoutTracksRegionTop(wlayout.leftPanel) &&
        wpt.y <  UiLayoutTracksRegionBottom(wlayout.leftPanel)) {
        const int step = Dpi(kTrackRowHeight);
        const int notches = delta / WHEEL_DELTA;
        state.ui.view.tracksScrollY -= notches * step;
        const int maxScroll = UiLayoutMaxTracksScrollY(wlayout.leftPanel, state);
        state.ui.view.tracksScrollY = std::clamp(state.ui.view.tracksScrollY, 0, maxScroll);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    if (ctrl) {
        const float factor = (delta > 0) ? daw::vm::kWheelZoomInFactor : daw::vm::kWheelZoomOutFactor;
        const int focusX = std::clamp(wpt.x, wlayout.arrange.left, wlayout.arrange.right);
        const float beatAtCursor = UiLayoutXToBeat(wlayout.arrange, state, focusX);
        const auto z = daw::vm::ZoomVisibleAround(state.ui.view.viewStartBeat,
                                                   state.ui.view.viewBeatsVisible,
                                                   beatAtCursor, factor);
        state.ui.view.viewStartBeat    = z.viewStartBeat;
        state.ui.view.viewBeatsVisible = z.viewBeatsVisible;
    } else if ((GET_KEYSTATE_WPARAM(wParam) & MK_SHIFT) != 0) {
        // Shift+wheel over arrange: vertical track scroll.
        const int step = Dpi(kTrackRowHeight);
        const int notches = delta / WHEEL_DELTA;
        state.ui.view.tracksScrollY -= notches * step;
        const int maxScroll = UiLayoutMaxTracksScrollY(wlayout.leftPanel, state);
        state.ui.view.tracksScrollY = std::clamp(state.ui.view.tracksScrollY, 0, maxScroll);
    } else {
        const float step = state.ui.view.viewBeatsVisible * daw::vm::kWheelPanStepFraction;
        state.ui.view.viewStartBeat = daw::vm::PanViewStartBeat(state.ui.view.viewStartBeat,
                                                            (delta > 0) ? -step : step);
    }
    state.ui.view.viewStartBeat = std::max(0.0f, state.ui.view.viewStartBeat);
    InvalidateRect(hwnd, nullptr, FALSE);
    return 0;
}

LRESULT WndProcOnMouseHWheel(HWND hwnd, WPARAM wParam, AppState& state) {
    const short delta = GET_WHEEL_DELTA_WPARAM(wParam);
    const float step = state.ui.view.viewBeatsVisible * 0.08f;
    state.ui.view.viewStartBeat += (delta > 0) ? -step : step;
    state.ui.view.viewStartBeat = std::max(0.0f, state.ui.view.viewStartBeat);
    InvalidateRect(hwnd, nullptr, FALSE);
    return 0;
}

LRESULT WndProcOnCaptureChanged(AppState& state) {
    state.ui.tools.draggingClip = false;
    state.ui.tools.dragClipIndex = -1;
    state.ui.tools.draggingFader = false;
    state.ui.tools.dragFaderTrack = -1;
    state.ui.tools.draggingPan = false;
    state.ui.tools.dragPanIndex = -1;
    state.ui.tools.draggingPlayhead = false;
    state.ui.tools.trimmingClip = false;
    state.ui.tools.trimClipIndex = -1;
    return 0;
}
