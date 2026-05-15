// ─────────────────────────────────────────────────────────────────────────────
// ui/draw/dock_chrome.cpp
//
// Dock leaf rendering (tab strips + panel bodies + splitters) and the
// drag-tab drop overlay. Lifted verbatim from ui/draw.cpp in Phase 17f.
// ─────────────────────────────────────────────────────────────────────────────

#include "ui/draw.h"
#include "ui/dpi.h"
#include "ui/dock.h"

#include <algorithm>

void UiDrawDockLeavesAndSplitters(HDC hdc, AppState& state, HFONT smallFont) {
    SelectObject(hdc, smallFont);
    const int tabH = Dpi(daw::ui::kDockTabStripHeightPx);
    for (const auto& leaf : state.ui.dock.dockLayout) {
        const RECT leafRect = leaf.rect;

        // Lone primary panels render full-bleed (no tab strip). As soon as
        // another panel docks alongside, the strip appears — but the
        // primary panel's own tab can't be dragged out.
        if (!daw::ui::DockLeafShowsTabStrip(leaf.node)) {
            const daw::ui::PanelDef& def = daw::ui::PanelGet(leaf.activePanel);
            def.draw(hdc, leafRect, state);
            continue;
        }

        // Reserve tab strip across the top of the leaf, then draw panel
        // into the remaining content rect.
        const int  stripH   = std::min<int>(tabH, leafRect.bottom - leafRect.top);
        const RECT stripRect{leafRect.left, leafRect.top,
                             leafRect.right, leafRect.top + stripH};
        const RECT contentRect{leafRect.left, leafRect.top + stripH,
                               leafRect.right, leafRect.bottom};

        // Strip background
        HBRUSH stripBg = CreateSolidBrush(RGB(38, 41, 46));
        FillRect(hdc, &stripRect, stripBg);
        DeleteObject(stripBg);
        // 1px separator under the strip
        HBRUSH sepBr = CreateSolidBrush(RGB(70, 74, 81));
        RECT sep{stripRect.left, stripRect.bottom - 1, stripRect.right, stripRect.bottom};
        FillRect(hdc, &sep, sepBr);
        DeleteObject(sepBr);

        // Tab buttons
        const int tabPad = Dpi(10);
        const int tabGap = Dpi(2);
        int tabX = stripRect.left + Dpi(4);
        SetBkMode(hdc, TRANSPARENT);
        for (int ti = 0; ti < static_cast<int>(leaf.node->panels.size()); ++ti) {
            const daw::ui::PanelKind pk = leaf.node->panels[static_cast<size_t>(ti)];
            const daw::ui::PanelDef& pd = daw::ui::PanelGet(pk);
            SIZE sz{};
            GetTextExtentPoint32W(hdc, pd.title, lstrlenW(pd.title), &sz);
            const int tabW = sz.cx + 2 * tabPad;
            RECT tabRect{tabX, stripRect.top + 2, tabX + tabW, stripRect.bottom - 1};
            const bool isActive = (ti == leaf.node->activeTab);
            HBRUSH tabBg = CreateSolidBrush(isActive ? RGB(58, 62, 70) : RGB(46, 49, 55));
            FillRect(hdc, &tabRect, tabBg);
            DeleteObject(tabBg);
            if (isActive) {
                HBRUSH topAccent = CreateSolidBrush(RGB(110, 150, 220));
                RECT acc{tabRect.left, tabRect.top, tabRect.right, tabRect.top + Dpi(2)};
                FillRect(hdc, &acc, topAccent);
                DeleteObject(topAccent);
            }
            SetTextColor(hdc, isActive ? RGB(235, 238, 244) : RGB(170, 175, 184));
            DrawTextW(hdc, pd.title, -1, &tabRect,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            state.ui.dock.dockTabs.push_back(daw::ui::DockTabHit{tabRect, leaf.node, ti});
            tabX += tabW + tabGap;
        }

        const daw::ui::PanelDef& def = daw::ui::PanelGet(leaf.activePanel);
        def.draw(hdc, contentRect, state);
    }

    // Draw splitters as a thin divider line (centered on the hit zone
    // rect). Highlighted while being dragged.
    for (const auto& sp : state.ui.dock.dockSplitters) {
        const bool active = (state.ui.dock.draggingSplitter && state.ui.dock.dragSplitterNode == sp.node);
        const COLORREF col = active ? RGB(110, 150, 220) : RGB(70, 74, 81);
        HBRUSH br = CreateSolidBrush(col);
        if (sp.horizontal) {
            const int midY = (sp.rect.top + sp.rect.bottom) / 2;
            RECT line{sp.rect.left, midY, sp.rect.right, midY + 1};
            FillRect(hdc, &line, br);
        } else {
            // Vertical splitter: draw the 1px divider one column to the
            // LEFT of the geometric center so it sits at the right edge of
            // the left leaf rather than at the first column of the right
            // leaf (which would overdraw content like the playhead at its
            // home position).
            const int midX = (sp.rect.left + sp.rect.right) / 2;
            RECT line{midX - 1, sp.rect.top, midX, sp.rect.bottom};
            FillRect(hdc, &line, br);
        }
        DeleteObject(br);
    }
}

void UiDrawDockDropOverlay(HDC hdc, const AppState& state) {
    if (!state.ui.dock.dragTabActive || state.ui.dock.dropTargetLeaf == nullptr) {
        return;
    }
    const RECT pr = state.ui.dock.dropPreviewRect;
    if (pr.right <= pr.left || pr.bottom <= pr.top) {
        return;
    }

    const COLORREF accent = RGB(110, 150, 220);
    // Mask before cast so MSVC doesn't warn about the GetRValue macro's
    // `(BYTE)(rgb)` truncating the constant COLORREF expression.
    const BYTE accentR = static_cast<BYTE>(accent        & 0xFF);
    const BYTE accentG = static_cast<BYTE>((accent >> 8) & 0xFF);
    const BYTE accentB = static_cast<BYTE>((accent >> 16) & 0xFF);

    // ── Translucent fill via AlphaBlend ─────────────────────────────────
    // 32bpp DIB with premultiplied alpha — the standard GDI requirement
    // for AlphaBlend with per-pixel alpha.
    auto AlphaFill = [&](const RECT& r, BYTE alpha) {
        const int rw = r.right - r.left;
        const int rh = r.bottom - r.top;
        if (rw <= 0 || rh <= 0) return;
        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth       = 1;
        bmi.bmiHeader.biHeight      = 1;
        bmi.bmiHeader.biPlanes      = 1;
        bmi.bmiHeader.biBitCount    = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        void*  bits = nullptr;
        HBITMAP dib = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!dib || !bits) { if (dib) DeleteObject(dib); return; }
        BYTE* px = static_cast<BYTE*>(bits);
        // Premultiply: c' = c * a / 255. Layout is BGRA.
        px[0] = static_cast<BYTE>((accentB * alpha) / 255);
        px[1] = static_cast<BYTE>((accentG * alpha) / 255);
        px[2] = static_cast<BYTE>((accentR * alpha) / 255);
        px[3] = alpha;
        HDC tmpDc = CreateCompatibleDC(hdc);
        HGDIOBJ oldBmp = SelectObject(tmpDc, dib);
        BLENDFUNCTION bf{};
        bf.BlendOp             = AC_SRC_OVER;
        bf.SourceConstantAlpha = 255;
        bf.AlphaFormat         = AC_SRC_ALPHA;
        AlphaBlend(hdc, r.left, r.top, rw, rh, tmpDc, 0, 0, 1, 1, bf);
        SelectObject(tmpDc, oldBmp);
        DeleteDC(tmpDc);
        DeleteObject(dib);
    };
    AlphaFill(pr, 96);

    // Solid 2px accent border around the preview rect.
    HBRUSH border = CreateSolidBrush(accent);
    RECT t{pr.left,      pr.top,         pr.right,    pr.top + 2};
    RECT b{pr.left,      pr.bottom - 2,  pr.right,    pr.bottom};
    RECT l{pr.left,      pr.top,         pr.left + 2, pr.bottom};
    RECT rb{pr.right - 2, pr.top,        pr.right,    pr.bottom};
    FillRect(hdc, &t,  border);
    FillRect(hdc, &b,  border);
    FillRect(hdc, &l,  border);
    FillRect(hdc, &rb, border);
    DeleteObject(border);

    // ── Compass indicators ──────────────────────────────────────────────
    // Five squares (T/L/C/R/B) arranged in a plus, centered on the target
    // leaf rect. The square matching the resolved drop side is filled with
    // accent; the others are dim outlines so users can see all options.
    RECT compassHost = pr;
    // For inner-leaf drops, find the actual leaf rect (the preview is
    // half/full of it). For outer (root) drops the preview is already the
    // right area.
    if (state.ui.dock.dropTargetLeaf != state.ui.dock.dockRoot.get()) {
        for (const auto& leaf : state.ui.dock.dockLayout) {
            if (leaf.node == state.ui.dock.dropTargetLeaf) {
                compassHost = leaf.rect;
                break;
            }
        }
    }
    const int cx  = (compassHost.left + compassHost.right)  / 2;
    const int cy  = (compassHost.top  + compassHost.bottom) / 2;
    const int sq  = Dpi(28);
    const int gap = Dpi(4);
    auto Square = [&](int ox, int oy) -> RECT {
        return RECT{cx + ox - sq / 2, cy + oy - sq / 2,
                    cx + ox + sq / 2, cy + oy + sq / 2};
    };
    const RECT cC = Square(0, 0);
    const RECT cT = Square(0, -(sq + gap));
    const RECT cB = Square(0,  (sq + gap));
    const RECT cL = Square(-(sq + gap), 0);
    const RECT cR = Square( (sq + gap), 0);

    const HBRUSH dim    = CreateSolidBrush(RGB(48, 54, 62));
    const HBRUSH bright = CreateSolidBrush(accent);
    const HBRUSH edge   = CreateSolidBrush(RGB(180, 200, 230));

    auto DrawSquare = [&](const RECT& sr, bool active) {
        FillRect(hdc, &sr, active ? bright : dim);
        FrameRect(hdc, &sr, edge);
    };

    using Side = daw::ui::DockDropSide;
    const Side side = state.ui.dock.dropTargetSide;

    // Outer (root) drops: only show the single edge square so it reads as
    // "pin to this edge of the whole dock".
    if (state.ui.dock.dropTargetLeaf == state.ui.dock.dockRoot.get()) {
        if      (side == Side::Left)   DrawSquare(cL, true);
        else if (side == Side::Right)  DrawSquare(cR, true);
        else if (side == Side::Top)    DrawSquare(cT, true);
        else if (side == Side::Bottom) DrawSquare(cB, true);
    } else {
        DrawSquare(cC, side == Side::Center);
        DrawSquare(cT, side == Side::Top);
        DrawSquare(cB, side == Side::Bottom);
        DrawSquare(cL, side == Side::Left);
        DrawSquare(cR, side == Side::Right);
    }

    DeleteObject(dim);
    DeleteObject(bright);
    DeleteObject(edge);
}
