#include "ui/floating.h"

#include "ui/dpi.h"
#include "ui/UiRuntimeState.h"  // kFloatingClassName
#include "ui/panel.h"           // PanelGet

#include <algorithm>
#include <string>

// The main WindowProc handles all mouse hit-testing on hit rects shared
// with the main window. FloatingWindowProc forwards mouse messages
// through it (with GWLP_USERDATA temporarily swapped to AppState*).
extern LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace {

// Per-floating-window state. Stored as a heap allocation pointed to by
// GWLP_USERDATA so the WindowProc can find both the AppState and which
// PanelKind to draw without scanning the floatingPanels vector each event.
struct FloatingWndData {
    AppState*           state;
    daw::ui::PanelKind  panel;
};

// Re-dock `panel` somewhere visible in the main dock tree. Uses the same
// preference order as the Window menu's panel-toggle path: prefer Tracks,
// then any non-primary leaf, then split off Arrange. Guarantees the panel
// is always reachable after a floating window closes.
void DockReturnPanelToMain(AppState& state, daw::ui::PanelKind panel) {
    if (!state.ui.dock.dockRoot) return;
    if (daw::ui::DockFindLeafContaining(state.ui.dock.dockRoot.get(), panel)) {
        return; // Already present (shouldn't happen, but be safe).
    }
    daw::ui::DockNode* host = daw::ui::DockFindLeafContaining(
        state.ui.dock.dockRoot.get(), daw::ui::PanelKind::Tracks);
    if (host == nullptr) {
        host = daw::ui::DockFindNonPrimaryLeaf(state.ui.dock.dockRoot.get());
    }
    if (host != nullptr) {
        daw::ui::DockInsertTab(host, panel,
            static_cast<int>(host->panels.size()));
    } else {
        daw::ui::DockNode* arrange = daw::ui::DockFindLeafContaining(
            state.ui.dock.dockRoot.get(), daw::ui::PanelKind::Arrange);
        if (arrange != nullptr) {
            daw::ui::DockSplitWith(state.ui.dock.dockRoot, arrange,
                daw::ui::DockDropSide::Left, panel, 0.25f);
        }
    }
}

} // namespace

LRESULT CALLBACK FloatingWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* fwd = reinterpret_cast<FloatingWndData*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));

    // ── Forward mouse messages into the main WindowProc (Phase 4b) ─────
    // Panels stash their hit rects (playRect, faderRect, etc.) in client
    // coords each paint. After a floating WM_PAINT those rects refer to
    // the floating window's client space, so the main WindowProc's button
    // hit-tests work as-is — provided we trick it into resolving `state`
    // from this hwnd. We do that by swapping the AppState pointer into
    // GWLP_USERDATA for the duration of the forwarded call, then restore
    // FloatingWndData. SetCapture/InvalidateRect on `hwnd` therefore
    // operate on the floating window, which is what we want. After the
    // forwarded handler runs we also invalidate the main window so any
    // shared state changes (fader values, clip moves, etc.) repaint
    // there too.
    if (fwd != nullptr) {
        bool isMouse = false;
        switch (msg) {
            case WM_LBUTTONDOWN: case WM_LBUTTONUP:
            case WM_RBUTTONDOWN: case WM_RBUTTONUP:
            case WM_MOUSEMOVE:   case WM_MOUSEWHEEL:
            case WM_LBUTTONDBLCLK:
                isMouse = true;
                break;
            default: break;
        }
        if (isMouse) {
            // Make sure the panel's hit rects are current before we route
            // a click — otherwise a click that arrives before the first
            // floating paint would hit-test against stale (zero) rects.
            UpdateWindow(hwnd);

            AppState* state = fwd->state;
            HWND      mainHwnd = state->hwnd;

            SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
            LRESULT res = WindowProc(hwnd, msg, wParam, lParam);
            SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(fwd));

            if (mainHwnd != nullptr) InvalidateRect(mainHwnd, nullptr, FALSE);
            // Tab-drag started inside this floating window means "drag the
            // panel back into main". The drag itself is already armed in
            // main's hit-test code; we don't need to do anything special.
            return res;
        }
    }

    switch (msg) {
    case WM_PAINT: {
        if (fwd == nullptr) return DefWindowProc(hwnd, msg, wParam, lParam);
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT client; GetClientRect(hwnd, &client);

        // Double-buffer to a memory DC to match the main window's flicker-
        // free paint pipeline.
        HDC memDc = CreateCompatibleDC(hdc);
        HBITMAP memBmp = CreateCompatibleBitmap(hdc, client.right, client.bottom);
        HGDIOBJ oldBmp = SelectObject(memDc, memBmp);

        HBRUSH bg = CreateSolidBrush(RGB(20, 22, 26));
        FillRect(memDc, &client, bg);
        DeleteObject(bg);

        // Match the main window's UI font selection so panels render with
        // consistent typography wherever they're docked.
        HFONT uiFont = CreateFontW(Dpi(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        HGDIOBJ oldFont = SelectObject(memDc, uiFont);

        daw::ui::PanelGet(fwd->panel).draw(memDc, client, *fwd->state);

        SelectObject(memDc, oldFont);
        DeleteObject(uiFont);

        BitBlt(hdc, 0, 0, client.right, client.bottom, memDc, 0, 0, SRCCOPY);

        SelectObject(memDc, oldBmp);
        DeleteObject(memBmp);
        DeleteDC(memDc);

        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_SIZE:
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_ERASEBKGND:
        return 1; // We paint the entire background ourselves.
    case WM_EXITSIZEMOVE: {
        // ── Drag-back-to-main (Phase 4b) ────────────────────────────────
        // When the user finishes moving / resizing the floating window,
        // check whether its center now sits over the main window's client
        // area. If so, re-dock the panel and destroy the floating frame.
        if (fwd == nullptr || fwd->state == nullptr || fwd->state->hwnd == nullptr) break;
        RECT fwin; GetWindowRect(hwnd, &fwin);
        const POINT center{(fwin.left + fwin.right) / 2,
                           (fwin.top  + fwin.bottom) / 2};
        RECT mwin; GetClientRect(fwd->state->hwnd, &mwin);
        POINT mTopLeft{0, 0};
        ClientToScreen(fwd->state->hwnd, &mTopLeft);
        OffsetRect(&mwin, mTopLeft.x, mTopLeft.y);
        if (PtInRect(&mwin, center)) {
            // PostMessage so we tear down outside this WM_EXITSIZEMOVE
            // (Windows doesn't love DestroyWindow during a modal loop).
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
        }
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY: {
        if (fwd != nullptr) {
            // Re-dock the panel back into the main window so it can't be
            // permanently lost by closing its floating frame.
            DockReturnPanelToMain(*fwd->state, fwd->panel);
            // Drop the entry from the floatingPanels vector.
            auto& vec = fwd->state->ui.floatingPanels;
            vec.erase(std::remove_if(vec.begin(), vec.end(),
                [hwnd](const auto& fp){ return fp.hwnd == hwnd; }), vec.end());
            HWND mainHwnd = fwd->state->hwnd;
            delete fwd;
            SetWindowLongPtr(hwnd, GWLP_USERDATA, 0);
            if (mainHwnd != nullptr) InvalidateRect(mainHwnd, nullptr, FALSE);
        }
        return 0;
    }
    default:
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

HWND SpawnFloatingPanelAt(AppState& state, daw::ui::PanelKind panel,
                          int x, int y, int w, int h,
                          bool restoreOnFail) {
    HINSTANCE hInst = reinterpret_cast<HINSTANCE>(GetWindowLongPtr(state.hwnd, GWLP_HINSTANCE));
    const std::wstring title = std::wstring(daw::ui::PanelGet(panel).title) + L" — DAW";
    HWND fhwnd = CreateWindowExW(
        0,
        kFloatingClassName,
        title.c_str(),
        WS_OVERLAPPEDWINDOW,
        x, y, w, h,
        state.hwnd,    // owner so it stays above main but isn't a child
        nullptr,
        hInst,
        nullptr);
    if (fhwnd == nullptr) {
        if (restoreOnFail) {
            DockReturnPanelToMain(state, panel);
            InvalidateRect(state.hwnd, nullptr, FALSE);
        }
        return nullptr;
    }
    auto* fwd = new FloatingWndData{&state, panel};
    SetWindowLongPtr(fhwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(fwd));
    state.ui.dock.floatingPanels.push_back({fhwnd, panel});
    ShowWindow(fhwnd, SW_SHOWNORMAL);
    return fhwnd;
}

void SpawnFloatingPanel(AppState& state, daw::ui::PanelKind panel, POINT screenPt) {
    const int w = Dpi(640);
    const int h = Dpi(360);
    // Anchor the new window so the cursor lands near its title bar — gives
    // the user immediate "this is what I just tore off" feedback.
    const int x = screenPt.x - Dpi(80);
    const int y = screenPt.y - Dpi(12);
    SpawnFloatingPanelAt(state, panel, x, y, w, h, /*restoreOnFail=*/true);
}
