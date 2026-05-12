#include "ui/draw.h"
#include "ui/layout.h"
#include "ui/dpi.h"
#include "ui/dock.h"
#include "daw_automation.h"
#include "daw_timeline.h"

#include <algorithm>
#include <cmath>

namespace daw::internal::ui {

static const wchar_t* InsertEffectName(int effectType) {
    static const wchar_t* kNames[kInsertEffectTypeCount] = {
        L"EQ", L"CMP", L"SAT", L"DLY", L"REV", L"GATE", L"DEE", L"LIM"
    };
    if (effectType < 0 || effectType >= kInsertEffectTypeCount) {
        return L"EQ";
    }
    return kNames[effectType];
}

static void Fill(HDC hdc, const RECT& rect, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(hdc, &rect, brush);
    DeleteObject(brush);
}

static void StrokeRect(HDC hdc, const RECT& rect, COLORREF color) {
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Rectangle(hdc, rect.left, rect.top, rect.right, rect.bottom);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(pen);
}

static void DrawButton(HDC hdc, const RECT& rect, const wchar_t* label, bool active) {
    Fill(hdc, rect, active ? kPalette.transportBtnActive : kPalette.transportBtn);
    StrokeRect(hdc, rect, RGB(80, 86, 95));

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, kPalette.textPrimary);
    DrawTextW(hdc, label, -1, const_cast<RECT*>(&rect), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

static void DrawPanKnob(HDC hdc, const RECT& rect, float pan, bool active) {
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

// ── Insert-chain inspector panel ─────────────────────────────────────────────
// Returns the outer panel rect in client coordinates (declared in draw.h).
RECT UiDrawGetInspectorPanelRect(const RECT& client, const AppState& state) {
    const int idx = state.ui.fxInspectorIndex;
    int slotCount = 0;
    if (state.ui.fxInspectorIsTrack) {
        if (idx >= 0 && idx < static_cast<int>(state.core.project.tracks.size()))
            slotCount = std::clamp(state.core.project.tracks[static_cast<size_t>(idx)].insertSlots, 0, kMaxInsertSlots);
    } else {
        if (idx >= 0 && idx < static_cast<int>(state.core.project.buses.size()))
            slotCount = std::clamp(state.core.project.buses[static_cast<size_t>(idx)].insertSlots, 0, kMaxInsertSlots);
    }
    const bool hasSelected = (state.ui.fxInspectorSelectedSlot >= 0 && state.ui.fxInspectorSelectedSlot < slotCount);
    RECT r{};
    r.left   = client.left + kUiDrawInspPadX;
    r.top    = client.top  + Dpi(kTopBarHeight) + Dpi(kRulerHeight) + Dpi(6);
    r.right  = r.left + kUiDrawInspW;
    r.bottom = r.top + kUiDrawInspHeaderH + kUiDrawInspCtrlH + slotCount * kUiDrawInspSlotH + (hasSelected ? kUiDrawInspParamH : 0) + 6;
    return r;
}

void UiDrawInsertInspector(HDC hdc, const RECT& client, const AppState& state) {
    if (!state.ui.fxInspectorOpen || state.ui.fxInspectorIndex < 0) return;

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

    const int idx = state.ui.fxInspectorIndex;
    std::wstring title = state.ui.fxInspectorIsTrack
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
    if (state.ui.fxInspectorIsTrack) {
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
        if (state.ui.fxInspectorIsTrack) {
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
        const bool isSelected = (s == state.ui.fxInspectorSelectedSlot);

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
    if (state.ui.fxInspectorSelectedSlot >= 0 && state.ui.fxInspectorSelectedSlot < slotCount) {
        const int selSlot = state.ui.fxInspectorSelectedSlot;

        const InsertConfigArray* pParams = nullptr;
        const InsertEffectArray* pEff = nullptr;
        if (state.ui.fxInspectorIsTrack) {
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

void UiDrawRuler(HDC hdc, const RECT& rect, const AppState& state) {
    Fill(hdc, rect, kPalette.rulerBg);

    HPEN barPen = CreatePen(PS_SOLID, 1, kPalette.barLine);
    HPEN beatPen = CreatePen(PS_SOLID, 1, kPalette.beatLine);
    HGDIOBJ oldPen = SelectObject(hdc, beatPen);

    const int width = std::max(1, static_cast<int>(rect.right - rect.left));
    const int firstBeat = static_cast<int>(std::floor(state.ui.viewStartBeat));
    const int lastBeat = static_cast<int>(std::ceil(state.ui.viewStartBeat + state.ui.viewBeatsVisible));

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, kPalette.textMuted);

    for (int beat = firstBeat; beat <= lastBeat; ++beat) {
        const float rel = (static_cast<float>(beat) - state.ui.viewStartBeat) / state.ui.viewBeatsVisible;
        const int x = rect.left + static_cast<int>(rel * static_cast<float>(width));
        const bool isBar = (beat % 4 == 0);
        SelectObject(hdc, isBar ? barPen : beatPen);
        MoveToEx(hdc, x, rect.top, nullptr);
        LineTo(hdc, x, rect.bottom);

        if (isBar && x >= rect.left && x <= rect.right) {
            const std::wstring label = std::to_wstring((beat / 4) + 1);
            // Place number to the right of the bar line so the digit is
            // never clipped by the panel edge (e.g. the "1" at the home
            // position used to be cut in half).
            RECT t{x + Dpi(3), rect.top + Dpi(2), x + Dpi(40), rect.bottom - Dpi(2)};
            DrawTextW(hdc, label.c_str(), -1, &t, DT_LEFT | DT_TOP | DT_SINGLELINE);
        }
    }

    SelectObject(hdc, oldPen);
    DeleteObject(barPen);
    DeleteObject(beatPen);

    // Playhead marker - downward-pointing triangle whose base meets the
    // arrange playhead line below. Apex points up.
    const int phX = rect.left + static_cast<int>(
        ((state.ui.playheadBeat - state.ui.viewStartBeat) / std::max(1.0f, state.ui.viewBeatsVisible))
        * static_cast<float>(width));
    if (phX >= rect.left && phX <= rect.right) {
        HBRUSH phBrush = CreateSolidBrush(kPalette.playhead);
        HPEN   phPen   = CreatePen(PS_SOLID, 1, kPalette.playhead);
        HGDIOBJ ob = SelectObject(hdc, phBrush);
        HGDIOBJ op = SelectObject(hdc, phPen);
        const int saved = SaveDC(hdc);
        IntersectClipRect(hdc, rect.left, rect.top, rect.right, rect.bottom);
        // Equilateral triangle: base = 2 * triHalf, height = base * sqrt(3)/2.
        // Wide base at the BOTTOM of the ruler (touching the playhead line
        // below), apex points UP — Reaper-style.
        const int triHalf = Dpi(5);
        const int triHeight = (2 * triHalf * 866) / 1000; // ~base * 0.866
        const int baseY = rect.bottom - 1;
        const POINT tri[3] = {
            {phX - triHalf, baseY},
            {phX + triHalf, baseY},
            {phX,           baseY - triHeight}
        };
        Polygon(hdc, tri, 3);
        RestoreDC(hdc, saved);
        SelectObject(hdc, ob);
        SelectObject(hdc, op);
        DeleteObject(phBrush);
        DeleteObject(phPen);
    }
}

static void EnsurePeakSummary(const LoadedAudio& audio) {
    const std::uint32_t bucket = LoadedAudio::kPeakBucketFrames;
    const size_t expected = (audio.frames + bucket - 1) / bucket;
    if (audio.peakSummary.size() == expected || audio.frames == 0 || audio.stereo.empty()) {
        if (audio.peakSummary.size() == expected) return;
    }
    audio.peakSummary.assign(expected, 0.0f);
    for (std::uint32_t b = 0; b < expected; ++b) {
        const std::uint64_t f0 = static_cast<std::uint64_t>(b) * bucket;
        const std::uint64_t f1 = std::min<std::uint64_t>(f0 + bucket, audio.frames);
        float peak = 0.0f;
        for (std::uint64_t f = f0; f < f1; ++f) {
            const size_t i = static_cast<size_t>(f) * 2;
            if (i + 1 >= audio.stereo.size()) break;
            const float l = std::fabs(audio.stereo[i]);
            const float r = std::fabs(audio.stereo[i + 1]);
            if (l > peak) peak = l;
            if (r > peak) peak = r;
        }
        audio.peakSummary[b] = peak;
    }
}

static void DrawClipWaveform(HDC hdc, const RECT& clipRect, const LoadedAudio& audio, std::uint64_t sourceStartFrame, std::uint64_t sourceEndFrame) {
    const int width = std::max(1, static_cast<int>(clipRect.right - clipRect.left));
    const int height = std::max(1, static_cast<int>(clipRect.bottom - clipRect.top));
    if (width < 2 || height < 4 || audio.frames == 0 || audio.stereo.empty()) {
        return;
    }

    const std::uint64_t totalFrames = static_cast<std::uint64_t>(audio.frames);
    const std::uint64_t startFrame = std::min(sourceStartFrame, totalFrames);
    const std::uint64_t endFrame = std::min(std::max(sourceEndFrame, startFrame + 1), totalFrames);
    if (endFrame <= startFrame) {
        return;
    }

    EnsurePeakSummary(audio);

    const int halfHeight = std::max(1, (height / 2) - 2);
    const int centerY = clipRect.top + (height / 2);

    HPEN wavePen = CreatePen(PS_SOLID, 1, RGB(238, 242, 248));
    HGDIOBJ oldPen = SelectObject(hdc, wavePen);

    const int saved = SaveDC(hdc);
    IntersectClipRect(hdc, clipRect.left, clipRect.top, clipRect.right, clipRect.bottom);

    const std::uint32_t bucket = LoadedAudio::kPeakBucketFrames;
    const size_t bucketCount = audio.peakSummary.size();
    const std::uint64_t sourceSpan = endFrame - startFrame;

    for (int x = clipRect.left; x < clipRect.right; ++x) {
        const int pixelIndex = x - clipRect.left;
        const std::uint64_t frameStart = startFrame + (static_cast<std::uint64_t>(pixelIndex) * sourceSpan) / static_cast<std::uint64_t>(width);
        std::uint64_t frameEnd = startFrame + (static_cast<std::uint64_t>(pixelIndex + 1) * sourceSpan) / static_cast<std::uint64_t>(width);
        frameEnd = std::max(frameEnd, frameStart + 1);
        frameEnd = std::min(frameEnd, endFrame);

        float peak = 0.0f;
        if (!audio.peakSummary.empty()) {
            // Iterate over the buckets covering [frameStart, frameEnd).
            const size_t b0 = static_cast<size_t>(frameStart / bucket);
            const size_t b1 = std::min<size_t>(bucketCount, static_cast<size_t>((frameEnd + bucket - 1) / bucket));
            for (size_t b = b0; b < b1; ++b) {
                const float v = audio.peakSummary[b];
                if (v > peak) peak = v;
            }
        }

        const int amp = static_cast<int>(std::round(peak * static_cast<float>(halfHeight)));
        MoveToEx(hdc, x, centerY - amp, nullptr);
        LineTo(hdc, x, centerY + amp + 1);
    }

    RestoreDC(hdc, saved);
    SelectObject(hdc, oldPen);
    DeleteObject(wavePen);
}

// Draw a live recording waveform from pre-computed float stereo data.
// `framesPerPixel` must be the constant project frames-per-pixel ratio for
// the current view; passing this in (instead of computing it from
// totalFrames/width) keeps each pixel's source-frame range stable as the
// recording grows, which prevents the waveform from jittering left/right.
static void DrawLiveRecordingWaveform(HDC hdc, const RECT& clipRect, const std::vector<float>& stereoWaveform, std::uint64_t totalFrames, double framesPerPixel) {
    const int width = std::max(1, static_cast<int>(clipRect.right - clipRect.left));
    const int height = std::max(1, static_cast<int>(clipRect.bottom - clipRect.top));
    if (width < 2 || height < 4 || totalFrames == 0 || stereoWaveform.empty() || framesPerPixel <= 0.0) {
        return;
    }

    const int halfHeight = std::max(1, (height / 2) - 2);
    const int centerY = clipRect.top + (height / 2);

    HPEN wavePen = CreatePen(PS_SOLID, 1, RGB(238, 242, 248));
    HGDIOBJ oldPen = SelectObject(hdc, wavePen);

    const int saved = SaveDC(hdc);
    IntersectClipRect(hdc, clipRect.left, clipRect.top, clipRect.right, clipRect.bottom);

    for (int x = clipRect.left; x < clipRect.right; ++x) {
        const int pixelIndex = x - clipRect.left;
        const std::uint64_t frameStart = static_cast<std::uint64_t>(static_cast<double>(pixelIndex) * framesPerPixel);
        if (frameStart >= totalFrames) {
            break;
        }
        std::uint64_t frameEnd = static_cast<std::uint64_t>(static_cast<double>(pixelIndex + 1) * framesPerPixel);
        frameEnd = std::max(frameEnd, frameStart + 1);
        frameEnd = std::min(frameEnd, totalFrames);

        float peak = 0.0f;
        for (std::uint64_t f = frameStart; f < frameEnd; ++f) {
            const size_t i = static_cast<size_t>(f) * 2;
            if (i + 1 < stereoWaveform.size()) {
                const float l = std::fabs(stereoWaveform[i]);
                const float r = std::fabs(stereoWaveform[i + 1]);
                peak = std::max(peak, std::max(l, r));
            }
        }

        const int amp = static_cast<int>(std::round(peak * static_cast<float>(halfHeight)));
        MoveToEx(hdc, x, centerY - amp, nullptr);
        LineTo(hdc, x, centerY + amp + 1);
    }

    RestoreDC(hdc, saved);
    SelectObject(hdc, oldPen);
    DeleteObject(wavePen);
}

void UiDrawArrangeLanes(HDC hdc, const RECT& rect, const AppState& state) {
    Fill(hdc, rect, kPalette.arrangeBg);

    const int width = std::max(1, static_cast<int>(rect.right - rect.left));
    const int firstBeat = static_cast<int>(std::floor(state.ui.viewStartBeat));
    const int lastBeat = static_cast<int>(std::ceil(state.ui.viewStartBeat + state.ui.viewBeatsVisible));

    HPEN barPen = CreatePen(PS_SOLID, 1, kPalette.barLine);
    HPEN beatPen = CreatePen(PS_SOLID, 1, kPalette.beatLine);
    HPEN lanePen = CreatePen(PS_SOLID, 1, RGB(40, 44, 50));
    HGDIOBJ oldPen = SelectObject(hdc, lanePen);

    const int laneScrollY = state.ui.tracksScrollY;
    const int laneRowH = Dpi(kTrackRowHeight);
    for (size_t i = 0; i < state.core.project.tracks.size(); ++i) {
        const int y = rect.top + static_cast<int>(i) * laneRowH - laneScrollY;
        if (y >= rect.bottom) break;
        if (y + laneRowH <= rect.top) continue;
        RECT lane{rect.left, y, rect.right, y + laneRowH};
        Fill(hdc, lane, (i % 2 == 0) ? kPalette.laneDark : kPalette.laneLight);
        MoveToEx(hdc, lane.left, lane.bottom - 1, nullptr);
        LineTo(hdc, lane.right, lane.bottom - 1);
    }

    for (int beat = firstBeat; beat <= lastBeat; ++beat) {
        const float rel = (static_cast<float>(beat) - state.ui.viewStartBeat) / state.ui.viewBeatsVisible;
        const int x = rect.left + static_cast<int>(rel * static_cast<float>(width));
        SelectObject(hdc, (beat % 4 == 0) ? barPen : beatPen);
        MoveToEx(hdc, x, rect.top, nullptr);
        LineTo(hdc, x, rect.bottom);
    }

    // Draw live recording clip if currently recording
    if (state.audio.recording && state.audio.recordTrackIndex >= 0 && 
        state.audio.recordTrackIndex < static_cast<int>(state.core.project.tracks.size()) &&
        !state.audio.liveRecordingWaveform.empty()) {
        
        RECT recordingClipRect{};
        if (UiLayoutClipRectForDraw(rect, state, state.audio.liveRecordingClip, &recordingClipRect)) {
            // Draw clip background
            Fill(hdc, recordingClipRect, state.audio.liveRecordingClip.color);
            
            // Draw name badge
            const int clipInnerLeft = recordingClipRect.left + 2;
            const int clipInnerRight = recordingClipRect.right - 2;
            const int clipInnerTop = recordingClipRect.top + 2;
            const int clipInnerBottom = recordingClipRect.bottom - 2;
            const int badgeHeight = 15;
            const int badgeMaxWidth = 140;
            RECT labelRect{clipInnerLeft + 2, clipInnerTop + 1, 
                          std::min(clipInnerRight - 2, clipInnerLeft + badgeMaxWidth), 
                          clipInnerTop + 1 + badgeHeight};
            Fill(hdc, labelRect, RGB(20, 23, 28));
            StrokeRect(hdc, labelRect, RGB(60, 66, 74));
            
            // Draw waveform
            RECT waveRect{clipInnerLeft, labelRect.bottom + 1, clipInnerRight, clipInnerBottom};
            if (waveRect.right > waveRect.left && waveRect.bottom > waveRect.top) {
                const std::uint64_t totalFrames = state.audio.liveRecordingFramesProcessed;
                if (totalFrames > 0) {
                    // Use a constant frames-per-pixel derived from the view
                    // (samples-per-beat and pixels-per-beat are both stable
                    // during recording). This keeps each pixel's source range
                    // fixed as the clip grows, so the waveform doesn't jitter.
                    const double samplesPerBeat = static_cast<double>(SamplesPerBeat(state));
                    const double pixelsPerBeat = static_cast<double>(width) /
                        static_cast<double>(std::max(0.0001f, state.ui.viewBeatsVisible));
                    const double framesPerPixel = (pixelsPerBeat > 0.0)
                        ? (samplesPerBeat / pixelsPerBeat)
                        : 0.0;
                    DrawLiveRecordingWaveform(hdc, waveRect, state.audio.liveRecordingWaveform, totalFrames, framesPerPixel);
                }
            }
            
            // Draw border
            StrokeRect(hdc, recordingClipRect, RGB(24, 24, 24));
            
            // Draw label
            RECT textRect{labelRect.left + 4, labelRect.top, labelRect.right - 4, labelRect.bottom};
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(240, 240, 240));
            DrawTextW(hdc, state.audio.liveRecordingClip.name.c_str(), -1, &textRect, 
                     DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
    }

    for (size_t i = 0; i < state.core.project.clips.size(); ++i) {
        RECT clipRect{};
        if (!UiLayoutClipRectForDraw(rect, state, state.core.project.clips[i], &clipRect)) {
            continue;
        }

        const int clipUnclippedLeft = UiLayoutBeatToX(rect, state, state.core.project.clips[i].startBeat);
        const int clipUnclippedRight = UiLayoutBeatToX(rect, state, state.core.project.clips[i].startBeat + state.core.project.clips[i].lengthBeats);
        const int fullClipPx = std::max(1, clipUnclippedRight - clipUnclippedLeft);
        const int clippedLeft = static_cast<int>(clipRect.left);
        const int clippedRight = static_cast<int>(clipRect.right);
        const int visibleLeftPx = std::max(0, clippedLeft - clipUnclippedLeft);
        const int visibleRightPx = std::min(fullClipPx, visibleLeftPx + std::max(1, clippedRight - clippedLeft));

        Fill(hdc, clipRect, state.core.project.clips[i].color);

        // Name badge at the top, waveform body underneath across full clip width.
        const int clipInnerLeft = clipRect.left + 2;
        const int clipInnerRight = clipRect.right - 2;
        const int clipInnerTop = clipRect.top + 2;
        const int clipInnerBottom = clipRect.bottom - 2;

        const int badgeHeight = 15;
        const int badgeMaxWidth = 140;
        RECT labelRect{clipInnerLeft + 2, clipInnerTop + 1, std::min(clipInnerRight - 2, clipInnerLeft + badgeMaxWidth), clipInnerTop + 1 + badgeHeight};
        Fill(hdc, labelRect, RGB(20, 23, 28));
        StrokeRect(hdc, labelRect, RGB(60, 66, 74));

        RECT waveRect{clipInnerLeft, labelRect.bottom + 1, clipInnerRight, clipInnerBottom};
        if (state.core.project.clips[i].audioIndex >= 0 && state.core.project.clips[i].audioIndex < static_cast<int>(state.core.project.audio.size()) && waveRect.right > waveRect.left && waveRect.bottom > waveRect.top) {
            const LoadedAudio& audio = state.core.project.audio[static_cast<size_t>(state.core.project.clips[i].audioIndex)];
            const std::uint64_t totalFrames = static_cast<std::uint64_t>(audio.frames);
            const float spb = SamplesPerBeat(state);
            const std::uint64_t srcOffset = state.core.project.clips[i].sourceOffsetFrames;
            const std::uint64_t clipLenFrames = std::min(
                static_cast<std::uint64_t>(state.core.project.clips[i].lengthBeats * spb),
                totalFrames > srcOffset ? totalFrames - srcOffset : std::uint64_t{0});
            if (clipLenFrames > 0) {
                const auto fcp = static_cast<std::uint64_t>(std::max(1, fullClipPx));
                std::uint64_t srcStart = srcOffset + (static_cast<std::uint64_t>(visibleLeftPx)  * clipLenFrames) / fcp;
                std::uint64_t srcEnd   = srcOffset + (static_cast<std::uint64_t>(visibleRightPx) * clipLenFrames) / fcp;
                srcEnd = std::min(std::max(srcEnd, srcStart + 1), totalFrames);
                DrawClipWaveform(hdc, waveRect, audio, srcStart, srcEnd);
            }
        }
        StrokeRect(hdc, clipRect, (state.ui.selectedClipIndex == static_cast<int>(i)) ? RGB(232, 232, 232) : RGB(24, 24, 24));

        // Trim edge handles on selected clip
        if (state.ui.selectedClipIndex == static_cast<int>(i)) {
            const int handleW = 5;
            RECT leftHandle  {clipRect.left,            clipRect.top, clipRect.left + handleW,  clipRect.bottom};
            RECT rightHandle {clipRect.right - handleW, clipRect.top, clipRect.right,            clipRect.bottom};
            Fill(hdc, leftHandle,  RGB(255, 255, 255));
            Fill(hdc, rightHandle, RGB(255, 255, 255));
        }

        RECT textRect{labelRect.left + 4, labelRect.top, labelRect.right - 4, labelRect.bottom};
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(240, 240, 240));
        DrawTextW(hdc, state.core.project.clips[i].name.c_str(), -1, &textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    int playheadX = UiLayoutBeatToX(rect, state, state.ui.playheadBeat);
    HPEN playheadPen = CreatePen(PS_SOLID, 2, kPalette.playhead);
    SelectObject(hdc, playheadPen);
    if (playheadX >= rect.left && playheadX <= rect.right) {
        RECT lr{
            std::max<LONG>(playheadX - 1, rect.left),
            rect.top,
            std::min<LONG>(playheadX + 1, rect.right),
            rect.bottom
        };
        if (lr.right > lr.left) {
            Fill(hdc, lr, kPalette.playhead);
        }
    }

    DeleteObject(playheadPen);
    DeleteObject(barPen);
    DeleteObject(beatPen);
    DeleteObject(lanePen);
    SelectObject(hdc, oldPen);

    if (state.core.project.tracks.empty()) {
        RECT hint{rect.left + 24, rect.top + 24, rect.right - 24, rect.top + 80};
        SetTextColor(hdc, kPalette.textMuted);
        SetBkMode(hdc, TRANSPARENT);
        DrawTextW(hdc, L"Import one or more WAV files to create real tracks and clips.", -1, &hint, DT_LEFT | DT_WORDBREAK);
    }
}

void UiDrawDockLeavesAndSplitters(HDC hdc, AppState& state, HFONT smallFont) {
    SelectObject(hdc, smallFont);
    const int tabH = Dpi(daw::ui::kDockTabStripHeightPx);
    for (const auto& leaf : state.ui.dockLayout) {
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
            state.ui.dockTabs.push_back(daw::ui::DockTabHit{tabRect, leaf.node, ti});
            tabX += tabW + tabGap;
        }

        const daw::ui::PanelDef& def = daw::ui::PanelGet(leaf.activePanel);
        def.draw(hdc, contentRect, state);
    }

    // Draw splitters as a thin divider line (centered on the hit zone
    // rect). Highlighted while being dragged.
    for (const auto& sp : state.ui.dockSplitters) {
        const bool active = (state.ui.draggingSplitter && state.ui.dragSplitterNode == sp.node);
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
    if (!state.ui.dragTabActive || state.ui.dropTargetLeaf == nullptr) {
        return;
    }
    const RECT pr = state.ui.dropPreviewRect;
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
    if (state.ui.dropTargetLeaf != state.ui.dockRoot.get()) {
        for (const auto& leaf : state.ui.dockLayout) {
            if (leaf.node == state.ui.dropTargetLeaf) {
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
    const Side side = state.ui.dropTargetSide;

    // Outer (root) drops: only show the single edge square so it reads as
    // "pin to this edge of the whole dock".
    if (state.ui.dropTargetLeaf == state.ui.dockRoot.get()) {
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
