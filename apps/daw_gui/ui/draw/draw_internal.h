#pragma once

#ifdef DAW_PUBLIC_API
#error "This header is internal-only."
#endif

// ─────────────────────────────────────────────────────────────────────────────
// ui/draw/draw_internal.h
//
// Private helpers shared between draw.cpp and any future TUs that get split
// out of it (Phase 17 series). Kept inline to avoid linker duplication and
// to preserve the previous file-static behavior — these are implementation
// details of the draw subsystem and must NOT be exposed via draw.h.
//
// Add new helpers here only when they are used by 2+ TUs in ui/draw/. If a
// helper is single-TU, keep it in an anonymous namespace inside that TU.
// ─────────────────────────────────────────────────────────────────────────────

#include "ui/draw.h"   // pulls AppState.h → UiRuntimeState.h (kPalette,
                       //                                       kInsertEffectTypeCount)

#include <algorithm>
#include <cmath>

namespace daw::internal::ui {

inline const wchar_t* InsertEffectName(int effectType) {
    static const wchar_t* kNames[kInsertEffectTypeCount] = {
        L"EQ", L"CMP", L"SAT", L"DLY", L"REV", L"GATE", L"DEE", L"LIM"
    };
    if (effectType < 0 || effectType >= kInsertEffectTypeCount) {
        return L"EQ";
    }
    return kNames[effectType];
}

inline void Fill(HDC hdc, const RECT& rect, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(hdc, &rect, brush);
    DeleteObject(brush);
}

inline void StrokeRect(HDC hdc, const RECT& rect, COLORREF color) {
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Rectangle(hdc, rect.left, rect.top, rect.right, rect.bottom);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(pen);
}

inline void DrawButton(HDC hdc, const RECT& rect, const wchar_t* label, bool active) {
    Fill(hdc, rect, active ? kPalette.transportBtnActive : kPalette.transportBtn);
    StrokeRect(hdc, rect, RGB(80, 86, 95));

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, kPalette.textPrimary);
    DrawTextW(hdc, label, -1, const_cast<RECT*>(&rect), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

inline void DrawPanKnob(HDC hdc, const RECT& rect, float pan, bool active) {
    const int w = rect.right - rect.left;
    const int h = rect.bottom - rect.top;
    const int radius = std::max(2, std::min(w, h) / 2 - 2);
    const int cx = rect.left + w / 2;
    const int cy = rect.top + h / 2;

    HPEN ringPen = CreatePen(PS_SOLID, 1, active ? RGB(196, 209, 232) : RGB(102, 109, 120));
    HBRUSH outer = CreateSolidBrush(active ? RGB(76, 84, 98) : RGB(61, 67, 76));
    HBRUSH inner = CreateSolidBrush(RGB(203, 206, 211));

    HGDIOBJ oldPen = SelectObject(hdc, ringPen);
    HGDIOBJ oldBrush = SelectObject(hdc, outer);
    Ellipse(hdc, cx - radius, cy - radius, cx + radius + 1, cy + radius + 1);

    const int innerRadius = std::max(1, radius - 4);
    SelectObject(hdc, inner);
    Ellipse(hdc, cx - innerRadius, cy - innerRadius, cx + innerRadius + 1, cy + innerRadius + 1);

    const float clamped = std::clamp(pan, -1.0f, 1.0f);
    // Center = North (top, -90°). Full left = -135° from North = 225° CW from East.
    // Full right = +135° from North = -45° from East.
    // In Win32 GDI angles (radians from East, CW): angle = -π/2 + clamped * (3π/4)
    const float angle = (-3.14159265f / 2.0f) + clamped * (3.14159265f * 0.75f);
    const int lineR = std::max(2, innerRadius - 1);
    const int x2 = cx + static_cast<int>(std::cos(angle) * lineR);
    const int y2 = cy + static_cast<int>(std::sin(angle) * lineR);

    HPEN pointerPen = CreatePen(PS_SOLID, 2, RGB(38, 42, 48));
    SelectObject(hdc, pointerPen);
    MoveToEx(hdc, cx, cy, nullptr);
    LineTo(hdc, x2, y2);

    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(pointerPen);
    DeleteObject(inner);
    DeleteObject(outer);
    DeleteObject(ringPen);
}

} // namespace daw::internal::ui
