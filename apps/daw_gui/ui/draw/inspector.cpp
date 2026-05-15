// ─────────────────────────────────────────────────────────────────────────────
// ui/draw/inspector.cpp
//
// Insert-chain inspector panel — header row, +ADD/-REM controls, slot list,
// and per-slot parameter strip with up to 4 labeled knobs.
// Lifted verbatim from ui/draw.cpp in Phase 17c. Helpers (Fill, StrokeRect,
// InsertEffectName) come from ui/draw/draw_internal.h. BusName() comes from
// AppState.h (defined in main.cpp).
// ─────────────────────────────────────────────────────────────────────────────

#include "ui/draw/draw_internal.h"
#include "ui/draw.h"
#include "ui/dpi.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

using namespace daw::internal::ui;

// ── Insert-chain inspector panel ─────────────────────────────────────────────
// Returns the outer panel rect in client coordinates (declared in draw.h).
RECT UiDrawGetInspectorPanelRect(const RECT& client, const AppState& state) {
    const int idx = state.ui.inspector.fxInspectorIndex;
    int slotCount = 0;
    if (state.ui.inspector.fxInspectorIsTrack) {
        if (idx >= 0 && idx < static_cast<int>(state.core.project.tracks.size()))
            slotCount = std::clamp(state.core.project.tracks[static_cast<size_t>(idx)].insertSlots, 0, kMaxInsertSlots);
    } else {
        if (idx >= 0 && idx < static_cast<int>(state.core.project.buses.size()))
            slotCount = std::clamp(state.core.project.buses[static_cast<size_t>(idx)].insertSlots, 0, kMaxInsertSlots);
    }
    const bool hasSelected = (state.ui.inspector.fxInspectorSelectedSlot >= 0 && state.ui.inspector.fxInspectorSelectedSlot < slotCount);
    RECT r{};
    r.left   = client.left + kUiDrawInspPadX;
    r.top    = client.top  + Dpi(kTopBarHeight) + Dpi(kRulerHeight) + Dpi(6);
    r.right  = r.left + kUiDrawInspW;
    r.bottom = r.top + kUiDrawInspHeaderH + kUiDrawInspCtrlH + slotCount * kUiDrawInspSlotH + (hasSelected ? kUiDrawInspParamH : 0) + 6;
    return r;
}

void UiDrawInsertInspector(HDC hdc, const RECT& client, const AppState& state) {
    if (!state.ui.inspector.fxInspectorOpen || state.ui.inspector.fxInspectorIndex < 0) return;

    const RECT panel = UiDrawGetInspectorPanelRect(client, state);

    // Shadow / outer border
    RECT shadow{panel.left + 2, panel.top + 2, panel.right + 2, panel.bottom + 2};
    Fill(hdc, shadow, RGB(10, 12, 14));
    Fill(hdc, panel, RGB(28, 31, 36));
    StrokeRect(hdc, panel, RGB(90, 96, 108));

    SetBkMode(hdc, TRANSPARENT);

    // ── Header row ──────────────────────────────────────────────────────────
    RECT headerRow{panel.left, panel.top, panel.right, panel.top + kUiDrawInspHeaderH};
    Fill(hdc, headerRow, RGB(44, 48, 56));

    const int idx = state.ui.inspector.fxInspectorIndex;
    std::wstring title = state.ui.inspector.fxInspectorIsTrack
        ? (L"INSERTS - " + (idx < static_cast<int>(state.core.project.tracks.size()) ? state.core.project.tracks[static_cast<size_t>(idx)].name : L"Track"))
        : (L"INSERTS - " + std::wstring(BusName(idx)));
    RECT titleRect{headerRow.left + 8, headerRow.top, headerRow.right - 28, headerRow.bottom};
    SetTextColor(hdc, RGB(220, 225, 235));
    DrawTextW(hdc, title.c_str(), -1, &titleRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    // [X] close button
    RECT closeBtn{panel.right - 24, headerRow.top + 4, panel.right - 4, headerRow.bottom - 4};
    Fill(hdc, closeBtn, RGB(80, 36, 36));
    StrokeRect(hdc, closeBtn, RGB(140, 60, 60));
    SetTextColor(hdc, RGB(240, 200, 200));
    DrawTextW(hdc, L"X", -1, &closeBtn, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    // ── Controls row: [+ ADD] [- REM] ───────────────────────────────────────
    const int ctrlTop = panel.top + kUiDrawInspHeaderH;
    RECT addBtn  {panel.left + 6,  ctrlTop + 4, panel.left + 66,  ctrlTop + kUiDrawInspCtrlH - 4};
    RECT remBtn  {panel.left + 72, ctrlTop + 4, panel.left + 132, ctrlTop + kUiDrawInspCtrlH - 4};

    int slotCount = 0;
    if (state.ui.inspector.fxInspectorIsTrack) {
        if (idx >= 0 && idx < static_cast<int>(state.core.project.tracks.size()))
            slotCount = std::clamp(state.core.project.tracks[static_cast<size_t>(idx)].insertSlots, 0, kMaxInsertSlots);
    } else {
        if (idx >= 0 && idx < static_cast<int>(state.core.project.buses.size()))
            slotCount = std::clamp(state.core.project.buses[static_cast<size_t>(idx)].insertSlots, 0, kMaxInsertSlots);
    }

    const bool canAdd = slotCount < kMaxInsertSlots;
    const bool canRem = slotCount > 0;
    Fill(hdc, addBtn, canAdd ? RGB(36, 68, 36) : RGB(38, 42, 46));
    Fill(hdc, remBtn, canRem ? RGB(68, 36, 36) : RGB(38, 42, 46));
    StrokeRect(hdc, addBtn, RGB(72, 110, 72));
    StrokeRect(hdc, remBtn, RGB(110, 72, 72));
    SetTextColor(hdc, canAdd ? RGB(160, 220, 160) : RGB(90, 95, 100));
    DrawTextW(hdc, L"+ ADD", -1, &addBtn, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SetTextColor(hdc, canRem ? RGB(220, 160, 160) : RGB(90, 95, 100));
    DrawTextW(hdc, L"- REM", -1, &remBtn, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    // Slot count label
    wchar_t countLabel[32] = {};
    swprintf_s(countLabel, L"%d / %d slots", slotCount, kMaxInsertSlots);
    RECT countRect{panel.left + 138, ctrlTop, panel.right - 6, ctrlTop + kUiDrawInspCtrlH};
    SetTextColor(hdc, RGB(120, 126, 136));
    DrawTextW(hdc, countLabel, -1, &countRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // ── Slot rows ────────────────────────────────────────────────────────────
    const int slotsTop = ctrlTop + kUiDrawInspCtrlH;
    for (int s = 0; s < slotCount; ++s) {
        const int rowTop = slotsTop + s * kUiDrawInspSlotH;
        const int rowBot = rowTop + kUiDrawInspSlotH;

        // Alternate row bg
        Fill(hdc, RECT{panel.left + 1, rowTop, panel.right - 1, rowBot},
             (s % 2 == 0) ? RGB(34, 37, 43) : RGB(38, 42, 48));

        // Slot index number
        wchar_t slotNum[8] = {};
        swprintf_s(slotNum, L"%d", s + 1);
        RECT numRect{panel.left + 6, rowTop, panel.left + 22, rowBot};
        SetTextColor(hdc, RGB(100, 106, 116));
        DrawTextW(hdc, slotNum, -1, &numRect, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

        // Effect type button
        const InsertEffectArray* effects = nullptr;
        const InsertBypassArray* bypass  = nullptr;
        if (state.ui.inspector.fxInspectorIsTrack) {
            if (idx < static_cast<int>(state.core.project.tracks.size()))
                effects = &state.core.project.tracks[static_cast<size_t>(idx)].insertEffects;
            if (idx < static_cast<int>(state.core.project.tracks.size()))
                bypass  = &state.core.project.tracks[static_cast<size_t>(idx)].insertBypass;
        } else {
            if (idx < static_cast<int>(state.core.project.buses.size()))
                effects = &state.core.project.buses[static_cast<size_t>(idx)].insertEffects;
            if (idx < static_cast<int>(state.core.project.buses.size()))
                bypass  = &state.core.project.buses[static_cast<size_t>(idx)].insertBypass;
        }

        const int  effectType = (effects && s < kMaxInsertSlots)
            ? std::clamp(static_cast<int>((*effects)[static_cast<size_t>(s)]), 0, kInsertEffectTypeCount - 1) : 0;
        const bool slotBypassed = (bypass && s < kMaxInsertSlots) ? (*bypass)[static_cast<size_t>(s)] : false;
        const bool isSelected = (s == state.ui.inspector.fxInspectorSelectedSlot);

        // Effect type button: [EQ ] [CMP] etc.
        RECT typeBtn{panel.left + 26, rowTop + 2, panel.left + 84, rowBot - 2};
        Fill(hdc, typeBtn, slotBypassed ? RGB(50, 50, 42) : RGB(40, 56, 72));
        StrokeRect(hdc, typeBtn, slotBypassed ? RGB(90, 88, 60) : (isSelected ? RGB(180, 200, 255) : RGB(60, 96, 130)));
        SetTextColor(hdc, slotBypassed ? RGB(160, 156, 120) : RGB(160, 210, 255));
        DrawTextW(hdc, InsertEffectName(effectType), -1, &typeBtn, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        // Bypass toggle button
        RECT bypassBtn{panel.left + 88, rowTop + 2, panel.left + 130, rowBot - 2};
        Fill(hdc, bypassBtn, slotBypassed ? RGB(80, 72, 28) : RGB(38, 42, 48));
        StrokeRect(hdc, bypassBtn, slotBypassed ? RGB(160, 148, 56) : RGB(68, 74, 82));
        SetTextColor(hdc, slotBypassed ? RGB(240, 220, 100) : RGB(130, 136, 146));
        DrawTextW(hdc, slotBypassed ? L"BYP ON" : L"BYP", -1, &bypassBtn, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        // [>] expand/collapse arrow
        RECT arrowBtn{panel.left + 134, rowTop + 2, panel.left + 154, rowBot - 2};
        Fill(hdc, arrowBtn, isSelected ? RGB(50, 70, 90) : RGB(38, 42, 48));
        StrokeRect(hdc, arrowBtn, isSelected ? RGB(120, 160, 220) : RGB(68, 74, 82));
        SetTextColor(hdc, isSelected ? RGB(180, 220, 255) : RGB(120, 126, 136));
        DrawTextW(hdc, isSelected ? L"v" : L">", -1, &arrowBtn, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    // ── Param expansion area for selected slot ────────────────────────────
    if (state.ui.inspector.fxInspectorSelectedSlot >= 0 && state.ui.inspector.fxInspectorSelectedSlot < slotCount) {
        const int selSlot = state.ui.inspector.fxInspectorSelectedSlot;

        const InsertConfigArray* pParams = nullptr;
        const InsertEffectArray* pEff = nullptr;
        if (state.ui.inspector.fxInspectorIsTrack) {
            if (idx < static_cast<int>(state.core.project.tracks.size()))
                pParams = &state.core.project.tracks[static_cast<size_t>(idx)].insertConfig;
            if (idx < static_cast<int>(state.core.project.tracks.size()))
                pEff = &state.core.project.tracks[static_cast<size_t>(idx)].insertEffects;
        } else {
            if (idx < static_cast<int>(state.core.project.buses.size()))
                pParams = &state.core.project.buses[static_cast<size_t>(idx)].insertConfig;
            if (idx < static_cast<int>(state.core.project.buses.size()))
                pEff = &state.core.project.buses[static_cast<size_t>(idx)].insertEffects;
        }

        // Position param strip after all slots
        const int paramTop = slotsTop + slotCount * kUiDrawInspSlotH;
        const int paramBot = paramTop + kUiDrawInspParamH;
        Fill(hdc, RECT{panel.left + 1, paramTop, panel.right - 1, paramBot}, RGB(24, 28, 36));
        StrokeRect(hdc, RECT{panel.left + 1, paramTop, panel.right - 1, paramBot}, RGB(80, 120, 180));

        // Header: "Slot N params"
        wchar_t pHdr[32] = {};
        swprintf_s(pHdr, L"Slot %d Params  [drag knobs]", selSlot + 1);
        RECT pHdrRect{panel.left + 6, paramTop + 2, panel.right - 6, paramTop + 14};
        SetTextColor(hdc, RGB(120, 160, 210));
        DrawTextW(hdc, pHdr, -1, &pHdrRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        if (pParams && pEff) {
            const InsertConfig& P = (*pParams)[static_cast<size_t>(selSlot)];
            const int fxT = std::clamp(static_cast<int>((*pEff)[static_cast<size_t>(selSlot)]), 0, kInsertEffectTypeCount - 1);

            // Draw up to 4 labeled knobs for the current effect type
            struct KnobDef { const wchar_t* label; float val; float lo; float hi; int paramId; };
            KnobDef knobs[4] = {};
            int knobCount = 0;

            switch (fxT) {
            case kFxEQ:
                // Show band 1 freq and gain, band 2 freq and gain
                knobs[0] = {L"B1 Hz", P.eq[0].freq_hz, 20.0f, 20000.0f, 0};
                knobs[1] = {L"B1 dB", P.eq[0].gain_db, -18.0f, 18.0f,   1};
                knobs[2] = {L"B2 Hz", P.eq[1].freq_hz, 20.0f, 20000.0f, 2};
                knobs[3] = {L"B2 dB", P.eq[1].gain_db, -18.0f, 18.0f,   3};
                knobCount = 4;
                break;
            case kFxCMP:
                knobs[0] = {L"Thr",  P.cmp_threshold_db, -60.0f,  0.0f,  10};
                knobs[1] = {L"Rat",  P.cmp_ratio,          1.0f, 20.0f,  11};
                knobs[2] = {L"Atk",  P.cmp_attack_ms,      0.1f, 200.0f, 12};
                knobs[3] = {L"Mkup", P.cmp_makeup_db,      0.0f, 24.0f,  13};
                knobCount = 4;
                break;
            case kFxSAT:
                knobs[0] = {L"Drive", P.sat_drive, 0.0f, 1.0f, 20};
                knobs[1] = {L"Mix",   P.sat_mix,   0.0f, 1.0f, 21};
                knobCount = 2;
                break;
            case kFxDLY:
                knobs[0] = {L"Time",  P.dly_time_ms,  10.0f, 2000.0f, 30};
                knobs[1] = {L"Fdbk",  P.dly_feedback,  0.0f,    0.95f, 31};
                knobs[2] = {L"Mix",   P.dly_mix,        0.0f,    1.0f, 32};
                knobCount = 3;
                break;
            case kFxREV:
                knobs[0] = {L"Room", P.rev_room_size, 0.0f, 1.0f, 40};
                knobs[1] = {L"Damp", P.rev_damping,   0.0f, 1.0f, 41};
                knobs[2] = {L"Mix",  P.rev_mix,        0.0f, 1.0f, 42};
                knobCount = 3;
                break;
            case kFxGATE:
                knobs[0] = {L"Thr", P.gate_threshold_db, -80.0f, 0.0f, 50};
                knobs[1] = {L"Atk", P.gate_attack_ms,     0.1f, 200.0f, 51};
                knobs[2] = {L"Rel", P.gate_release_ms,    10.0f, 500.0f, 52};
                knobCount = 3;
                break;
            case kFxDEE:
                knobs[0] = {L"Thr",  P.dee_threshold_db,  -40.0f,  0.0f, 60};
                knobs[1] = {L"Freq", P.dee_freq_hz,       2000.0f, 16000.0f, 61};
                knobs[2] = {L"BW",   P.dee_bandwidth_hz,   200.0f, 8000.0f, 62};
                knobs[3] = {L"Red",  P.dee_reduction_db,    0.0f,  24.0f,  63};
                knobCount = 4;
                break;
            case kFxLIM:
                knobs[0] = {L"Ceil", P.lim_ceiling_db, -12.0f, 0.0f, 70};
                knobs[1] = {L"Rel",  P.lim_release_ms,   1.0f, 500.0f, 71};
                knobCount = 2;
                break;
            }

            // Draw knobs evenly spaced
            const int kw = (kUiDrawInspW - 12) / 4;
            for (int k = 0; k < knobCount; ++k) {
                const int kx = panel.left + 6 + k * kw;
                const int ky = paramTop + 16;
                const RECT kRect{kx, ky, kx + kw - 2, ky + kUiDrawInspParamH - 18};

                // Knob background
                Fill(hdc, kRect, RGB(36, 42, 52));
                StrokeRect(hdc, kRect, RGB(70, 100, 140));

                // Label
                RECT lbl{kRect.left, kRect.top, kRect.right, kRect.top + 12};
                SetTextColor(hdc, RGB(150, 170, 200));
                DrawTextW(hdc, knobs[k].label, -1, &lbl, DT_CENTER | DT_TOP | DT_SINGLELINE);

                // Value text
                wchar_t vstr[32] = {};
                if (std::fabs(knobs[k].hi - knobs[k].lo) > 10.0f)
                    swprintf_s(vstr, L"%.0f", knobs[k].val);
                else
                    swprintf_s(vstr, L"%.2f", knobs[k].val);
                RECT vlbl{kRect.left, kRect.top + 14, kRect.right, kRect.bottom};
                SetTextColor(hdc, RGB(220, 230, 255));
                DrawTextW(hdc, vstr, -1, &vlbl, DT_CENTER | DT_TOP | DT_SINGLELINE);
            }
        }
    }

    // Bottom border line
    RECT bottomLine{panel.left + 1, panel.bottom - 2, panel.right - 1, panel.bottom - 1};
    Fill(hdc, bottomLine, RGB(90, 96, 108));
}
