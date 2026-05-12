#include "ui/wndproc/paint.h"

#include "ui/draw.h"
#include "ui/dock.h"
#include "ui/dock_persist.h"
#include "ui/floating.h"
#include "ui/UiRuntimeState.h"  // kPalette, kTopBarHeight, kStatusBarHeight
#include "ui/dpi.h"

#include <algorithm>

LRESULT WndProcOnPaint(HWND hwnd, AppState& state) {
    PAINTSTRUCT ps{};
    HDC hdc = BeginPaint(hwnd, &ps);

    RECT client{};
    GetClientRect(hwnd, &client);

    HDC memDc = CreateCompatibleDC(hdc);
    const int backWidth = std::max(1, static_cast<int>(client.right - client.left));
    const int backHeight = std::max(1, static_cast<int>(client.bottom - client.top));
    HBITMAP backBmp = CreateCompatibleBitmap(hdc, backWidth, backHeight);
    HGDIOBJ oldBmp = SelectObject(memDc, backBmp);

    {
        HBRUSH bgBrush = CreateSolidBrush(kPalette.windowBg);
        FillRect(memDc, &client, bgBrush);
        DeleteObject(bgBrush);
    }

    HFONT uiFont = CreateFontW(
        18, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI"
    );
    HFONT smallFont = CreateFontW(
        15, 0, 0, 0, FW_MEDIUM, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI"
    );

    HGDIOBJ oldFont = SelectObject(memDc, uiFont);

    // ── Top bar (still a fixed strip) ────────────────────────────
    UiDrawTopBar(memDc, client, state);

    // ── Dock-driven paint of the body below the top bar ──────────
    // Lazily build the default dock tree on first paint. The dock
    // walker emits one entry per leaf + one per splitter so we can
    // both dispatch to the panel registry and render draggable
    // dividers.
    if (!state.ui.dockRoot) {
        // Try to restore the user's last layout from
        // %APPDATA%\DAW\layout.json. Falls back to the built-in
        // default if missing/invalid. Floating panels persisted
        // alongside the dock tree are spawned now (one per entry)
        // so a torn-off Mixer survives a restart in the same spot.
        daw::ui::DockLayoutDocument doc = daw::ui::DockLoadLayout();
        if (doc.root) {
            state.ui.dockRoot = std::move(doc.root);
            for (const auto& f : doc.floating) {
                SpawnFloatingPanelAt(state, f.panel,
                                     f.x, f.y, f.w, f.h,
                                     /*restoreOnFail=*/false);
            }
        } else {
            state.ui.dockRoot = daw::ui::DockBuildDefault();
        }
    }
    RECT bodyRect{client.left, client.top + Dpi(kTopBarHeight), client.right, client.bottom - Dpi(kStatusBarHeight)};
    state.ui.dockLayout.clear();
    state.ui.dockSplitters.clear();
    state.ui.dockTabs.clear();
    daw::ui::DockLayout(state.ui.dockRoot.get(), bodyRect,
                        state.ui.dockLayout, &state.ui.dockSplitters);

    // Render dock leaves (panel content + per-leaf tab strip) and
    // splitter dividers. Side effect: populates state.ui.dockTabs
    // for the tab hit-test in WM_LBUTTONDOWN.
    UiDrawDockLeavesAndSplitters(memDc, state, smallFont);

    // ── Tab-drag drop preview (Phase 2.2c) ──────────────────────
    // Translucent accent fill over the resolved drop region plus a
    // Unity-style 5-position compass centered on the target leaf.
    // Self-contained GDI; lives in ui/draw.cpp.
    UiDrawDockDropOverlay(memDc, state);

    // Inspector panel floats on top of everything
    if (state.ui.fxInspectorOpen)
        UiDrawInsertInspector(memDc, client, state);

    // ── Status bar (fixed bottom strip, not dockable) ────────────
    {
        RECT statusRect{client.left,
                        client.bottom - Dpi(kStatusBarHeight),
                        client.right,
                        client.bottom};
        UiDrawStatusBar(memDc, statusRect, state);
    }

    SelectObject(memDc, oldFont);
    DeleteObject(uiFont);
    DeleteObject(smallFont);

    BitBlt(hdc, 0, 0, client.right - client.left, client.bottom - client.top, memDc, 0, 0, SRCCOPY);
    SelectObject(memDc, oldBmp);
    DeleteObject(backBmp);
    DeleteDC(memDc);

    EndPaint(hwnd, &ps);
    return 0;
}
