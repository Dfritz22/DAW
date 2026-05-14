// ─────────────────────────────────────────────────────────────────────────────
// ui/draw/mixer_strips.cpp
//
// Track strip panel + bus strip panel. Both render fader/pan/mute/solo/FX
// controls and use shared DrawPanKnob/Fill/StrokeRect helpers from
// ui/draw/draw_internal.h.
// Lifted verbatim from ui/draw.cpp in Phase 17d.
// ─────────────────────────────────────────────────────────────────────────────

#include "ui/draw/draw_internal.h"
#include "ui/draw.h"
#include "ui/layout.h"
#include "ui/dpi.h"
#include "daw_automation.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace daw::internal::ui;

void UiDrawLeftTrackPanel(HDC hdc, const RECT& rect, const AppState& state) {
    Fill(hdc, rect, kPalette.leftPanel);

    RECT header{rect.left, rect.top, rect.right, rect.top + Dpi(kRulerHeight)};
    Fill(hdc, header, kPalette.leftPanelHeader);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, kPalette.textMuted);
    RECT headerText{header.left + 14, header.top, header.right, header.bottom};
    DrawTextW(hdc, L"TRACKS", -1, &headerText, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    if (state.core.project.tracks.empty()) {
        RECT emptyText{rect.left + 14, rect.top + 46, rect.right - 14, rect.top + 100};
        DrawTextW(hdc, L"No tracks loaded.\nClick Import WAV.", -1, &emptyText, DT_LEFT | DT_WORDBREAK);
        return;
    }

    // Scrollable tracks region: clipped between the header and the pinned bus panel.
    const int tracksTop = UiLayoutTracksRegionTop(rect);
    const int tracksBottom = UiLayoutTracksRegionBottom(rect);
    const int scrollY = state.ui.tracksScrollY;
    const int tracksSaved = SaveDC(hdc);
    IntersectClipRect(hdc, rect.left, tracksTop, rect.right, tracksBottom);

    for (size_t i = 0; i < state.core.project.tracks.size(); ++i) {
        const int rowH = Dpi(kTrackRowHeight);
        const int y = rect.top + Dpi(kRulerHeight) + static_cast<int>(i) * rowH - scrollY;
        if (y >= tracksBottom) break;
        if (y + rowH <= tracksTop) continue;
        RECT row{rect.left, y, rect.right, y + rowH};
        Fill(hdc, row, (i % 2 == 0) ? RGB(39, 43, 49) : RGB(42, 47, 53));
        if (static_cast<int>(i) == state.ui.selectedTrackIndex) {
            Fill(hdc, RECT{row.left, row.top, row.right, row.bottom}, RGB(55, 66, 84));
            StrokeRect(hdc, row, RGB(120, 143, 179));
        }

        RECT nameRect{row.left + 14, row.top + 10, row.right - 82, row.top + 30};
        SetTextColor(hdc, kPalette.textPrimary);
        DrawTextW(hdc, state.core.project.tracks[i].name.c_str(), -1, &nameRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        RECT busRect{};
        RECT panKnobRect{};
        RECT panValRect{};
        RECT fxRect{};
        UiLayoutGetTrackRoutingRects(rect, static_cast<int>(i), &busRect, &panKnobRect, &panValRect, &fxRect, scrollY);

        Fill(hdc, busRect, RGB(49, 54, 61));
        StrokeRect(hdc, busRect, RGB(82, 88, 97));
        SetTextColor(hdc, RGB(220, 224, 230));
        DrawTextW(hdc, BusName(TrackBusIndexAt(state, static_cast<int>(i))), -1, &busRect,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        const float trPan = TrackPanAt(state, static_cast<int>(i));
        DrawPanKnob(hdc, panKnobRect, trPan, static_cast<int>(i) == state.ui.selectedTrackIndex);
        Fill(hdc, panValRect, RGB(40, 44, 49));
        StrokeRect(hdc, panValRect, RGB(82, 88, 97));
        Fill(hdc, fxRect, RGB(40, 44, 49));
        StrokeRect(hdc, fxRect, RGB(82, 88, 97));

        wchar_t panText[16] = {};
        if (std::fabs(trPan) < 0.01f) {
            swprintf_s(panText, L"C");
        } else if (trPan < 0.0f) {
            swprintf_s(panText, L"L%02d", static_cast<int>(std::round(-trPan * 100.0f)));
        } else {
            swprintf_s(panText, L"R%02d", static_cast<int>(std::round(trPan * 100.0f)));
        }
        DrawTextW(hdc, panText, -1, &panValRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        const int trackFxSlots = (i < state.core.project.tracks.size()) ? std::clamp(state.core.project.tracks[i].insertSlots, 0, 8) : 0;
        wchar_t fxText[16] = {};
        if (trackFxSlots <= 0 || i >= state.core.project.tracks.size()) {
            swprintf_s(fxText, L"FX0");
        } else {
            const int effectType = std::clamp(static_cast<int>(state.core.project.tracks[i].insertEffects[0]), 0, kInsertEffectTypeCount - 1);
            const bool bypass = (i < state.core.project.tracks.size()) ? state.core.project.tracks[i].insertBypass[0] : false;
            const wchar_t* nm = InsertEffectName(effectType);
            if (trackFxSlots > 1) {
                swprintf_s(fxText, L"%s%s+%d", bypass ? L"~" : L"", nm, trackFxSlots - 1);
            } else {
                swprintf_s(fxText, L"%s%s", bypass ? L"~" : L"", nm);
            }
        }
        DrawTextW(hdc, fxText, -1, &fxRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        RECT muteRect{};
        RECT soloRect{};
        RECT recRect{};
        UiLayoutGetTrackButtonRects(rect, static_cast<int>(i), &muteRect, &soloRect, &recRect, scrollY);

        const bool muted = i < state.core.project.tracks.size() && state.core.project.tracks[i].mute;
        const bool soloed = i < state.core.project.tracks.size() && state.core.project.tracks[i].solo;
        const bool armed = i < state.core.project.tracks.size() && state.core.project.tracks[i].recordArm;

        Fill(hdc, muteRect, muted ? RGB(176, 76, 76) : RGB(56, 60, 67));
        Fill(hdc, soloRect, soloed ? RGB(186, 154, 63) : RGB(56, 60, 67));
        Fill(hdc, recRect, armed ? RGB(193, 63, 63) : RGB(56, 60, 67));
        StrokeRect(hdc, muteRect, RGB(82, 88, 97));
        StrokeRect(hdc, soloRect, RGB(82, 88, 97));
        StrokeRect(hdc, recRect, RGB(82, 88, 97));

        SetTextColor(hdc, RGB(228, 232, 238));
        DrawTextW(hdc, L"M", -1, &muteRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        DrawTextW(hdc, L"S", -1, &soloRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        DrawTextW(hdc, L"R", -1, &recRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        const float gainDb = TrackGainDbAt(state, static_cast<int>(i));
        wchar_t dbText[32] = {};
        swprintf_s(dbText, L"%+.1f dB", gainDb);
        RECT gainRect{row.right - 106, row.top + 34, row.right - 56, row.bottom - 12};
        SetTextColor(hdc, RGB(196, 202, 210));
        DrawTextW(hdc, dbText, -1, &gainRect, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

        RECT rail{};
        RECT knob{};
        UiLayoutGetTrackFaderRects(rect, static_cast<int>(i), &rail, &knob, scrollY);
        Fill(hdc, rail, RGB(26, 29, 33));
        StrokeRect(hdc, rail, RGB(60, 64, 72));

        const int knobTop = UiLayoutFaderKnobTopFromGain(rail, gainDb);
        knob.top = knobTop;
        knob.bottom = knobTop + Dpi(kFaderKnobHeight);
        Fill(hdc, knob, RGB(214, 218, 224));
        StrokeRect(hdc, knob, RGB(90, 95, 105));

        RECT meterBg{row.right - 26, row.top + 12, row.right - 14, row.bottom - 12};
        Fill(hdc, meterBg, RGB(28, 31, 36));
        RECT meterLevel{meterBg.left + 2, meterBg.top + 2, meterBg.right - 2,
                        meterBg.top + 2 + static_cast<int>((meterBg.bottom - meterBg.top - 4) * (0.3 + 0.1 * (i % 4)))};
        Fill(hdc, meterLevel, RGB(78, 162, 121));
    }

    // Restore clip from tracks region, then draw scrollbar + pinned bus panel.
    RestoreDC(hdc, tracksSaved);

    // Tiny vertical scrollbar on the right edge of the tracks region (only if scrollable).
    {
        const int regionH = std::max(1, tracksBottom - tracksTop);
        const int contentH = static_cast<int>(state.core.project.tracks.size()) * Dpi(kTrackRowHeight);
        if (contentH > regionH) {
            const int sbW = 4;
            RECT sbTrack{rect.right - sbW - 2, tracksTop + 2, rect.right - 2, tracksBottom - 2};
            Fill(hdc, sbTrack, RGB(28, 31, 36));
            const float ratio = static_cast<float>(regionH) / static_cast<float>(contentH);
            const int thumbH = std::max(20, static_cast<int>(ratio * (sbTrack.bottom - sbTrack.top)));
            const int maxScroll = std::max(1, contentH - regionH);
            const float scrollT = static_cast<float>(scrollY) / static_cast<float>(maxScroll);
            const int thumbTop = sbTrack.top + static_cast<int>(scrollT * (sbTrack.bottom - sbTrack.top - thumbH));
            RECT thumb{sbTrack.left, thumbTop, sbTrack.right, thumbTop + thumbH};
            Fill(hdc, thumb, RGB(96, 102, 112));
        }
    }

    // ── Buses ────────────────────────────────────────────────────────────
    // The bus mixer is its own dock leaf (UiDrawBusesPanel) and is rendered
    // separately by the dock walker. Tracks panel no longer reserves space
    // for buses.
    (void)tracksSaved;
}

// ── Buses panel ──────────────────────────────────────────────────────────────
// Standalone bus mixer (Drums / Music / Vocals / Master). Self-contained:
// `rect` is the panel's full area (header + rows). Used both by the legacy
// pinned-bottom layout (via UiDrawLeftTrackPanel) and by the dock walker.
void UiDrawBusesPanel(HDC hdc, const RECT& rect, const AppState& state) {
    Fill(hdc, rect, kPalette.leftPanel);
    RECT divider{rect.left, rect.top, rect.right, rect.top + 1};
    Fill(hdc, divider, RGB(70, 74, 81));

    const int headerTop = rect.top + Dpi(kBusPanelTopMargin);
    RECT bHeader{rect.left, headerTop, rect.right, headerTop + Dpi(kBusPanelHeaderHeight)};
    Fill(hdc, bHeader, RGB(48, 52, 58));
    SetTextColor(hdc, kPalette.textMuted);
    RECT bHeaderText{bHeader.left + 10, bHeader.top, bHeader.right, bHeader.bottom};
    DrawTextW(hdc, L"BUSES", -1, &bHeaderText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    for (int b = 0; b < kBusCount; ++b) {
        RECT rowRect{};
        RECT muteRect{};
        RECT gainDownRect{};
        RECT gainUpRect{};
        RECT panKnobRect{};
        RECT panValRect{};
        RECT fxRect{};
        UiLayoutGetBusControlRectsInPanel(rect, b, &rowRect, &muteRect, &gainDownRect, &gainUpRect, &panKnobRect, &panValRect, &fxRect);
        if (rowRect.bottom > rect.bottom) break;

        Fill(hdc, rowRect, (b % 2 == 0) ? RGB(39, 43, 49) : RGB(42, 47, 53));

        RECT nameRect{rowRect.left + 6, rowRect.top, rowRect.left + 90, rowRect.bottom};
        SetTextColor(hdc, kPalette.textPrimary);
        DrawTextW(hdc, BusName(b), -1, &nameRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        Fill(hdc, muteRect,     BusMuteAt(state, b) ? RGB(176, 76, 76) : RGB(56, 60, 67));
        Fill(hdc, gainDownRect, RGB(56, 60, 67));
        Fill(hdc, gainUpRect,   RGB(56, 60, 67));
        Fill(hdc, panValRect,   RGB(40, 44, 49));
        Fill(hdc, fxRect,       RGB(40, 44, 49));
        StrokeRect(hdc, muteRect,     RGB(82, 88, 97));
        StrokeRect(hdc, gainDownRect, RGB(82, 88, 97));
        StrokeRect(hdc, gainUpRect,   RGB(82, 88, 97));
        StrokeRect(hdc, panValRect,   RGB(82, 88, 97));
        StrokeRect(hdc, fxRect,       RGB(82, 88, 97));

        SetTextColor(hdc, RGB(228, 232, 238));
        DrawTextW(hdc, L"M", -1, &muteRect,     DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        DrawTextW(hdc, L"-", -1, &gainDownRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        DrawTextW(hdc, L"+", -1, &gainUpRect,   DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        const float busGain = BusGainDbAt(state, b);
        wchar_t gainText[20] = {};
        swprintf_s(gainText, L"%+.1f", busGain);
        RECT gainTextRect{rowRect.right - 38, rowRect.top, rowRect.right - 6, rowRect.bottom};
        SetTextColor(hdc, RGB(196, 202, 210));
        DrawTextW(hdc, gainText, -1, &gainTextRect, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

        const float busPan = BusPanAt(state, b);
        DrawPanKnob(hdc, panKnobRect, busPan, false);
        wchar_t panText[16] = {};
        if (std::fabs(busPan) < 0.01f) {
            swprintf_s(panText, L"C");
        } else if (busPan < 0.0f) {
            swprintf_s(panText, L"L%02d", static_cast<int>(std::round(-busPan * 100.0f)));
        } else {
            swprintf_s(panText, L"R%02d", static_cast<int>(std::round(busPan * 100.0f)));
        }
        DrawTextW(hdc, panText, -1, &panValRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        const int busFxSlots = (b < static_cast<int>(state.core.project.buses.size())) ? std::clamp(state.core.project.buses[static_cast<size_t>(b)].insertSlots, 0, 8) : 0;
        wchar_t busFxText[16] = {};
        if (busFxSlots <= 0 || b >= static_cast<int>(state.core.project.buses.size())) {
            swprintf_s(busFxText, L"FX0");
        } else {
            const int effectType = std::clamp(static_cast<int>(state.core.project.buses[static_cast<size_t>(b)].insertEffects[0]), 0, kInsertEffectTypeCount - 1);
            const bool bypass = (b < static_cast<int>(state.core.project.buses.size())) ? state.core.project.buses[static_cast<size_t>(b)].insertBypass[0] : false;
            const wchar_t* nm = InsertEffectName(effectType);
            if (busFxSlots > 1) {
                swprintf_s(busFxText, L"%s%s+%d", bypass ? L"~" : L"", nm, busFxSlots - 1);
            } else {
                swprintf_s(busFxText, L"%s%s", bypass ? L"~" : L"", nm);
            }
        }
        DrawTextW(hdc, busFxText, -1, &fxRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
}
