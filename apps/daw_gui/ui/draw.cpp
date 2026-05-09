#include "ui/draw.h"
#include "core/automation.h"
#include "core/timeline.h"
#include "ui/layout.h"

namespace {

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

static void DrawMenuTab(HDC hdc, const RECT& rect, const wchar_t* label) {
    Fill(hdc, rect, RGB(39, 44, 50));
    StrokeRect(hdc, rect, RGB(70, 76, 86));
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(206, 212, 220));
    DrawTextW(hdc, label, -1, const_cast<RECT*>(&rect), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

} // namespace

void UiDrawTopBar(HDC hdc, const RECT& client, UiState& state) {
    RECT top{client.left, client.top, client.right, client.top + kTopBarHeight};
    Fill(hdc, top, kPalette.topBar);
    RECT edge{client.left, top.bottom - 1, client.right, top.bottom};
    Fill(hdc, edge, kPalette.topBarEdge);

    state.fileMenuRect = RECT{12, 8, 72, 30};
    state.viewMenuRect = RECT{76, 8, 136, 30};
    state.audioMenuRect = RECT{140, 8, 210, 30};
    state.trackMenuRect = RECT{214, 8, 282, 30};

    state.playRect = RECT{22, 34, 96, 58};
    state.stopRect = RECT{104, 34, 178, 58};
    state.recordRect = RECT{186, 34, 260, 58};
    state.importRect = RECT{268, 34, 378, 58};
    state.automixRect = RECT{386, 34, 522, 58};
    state.vocalCheckRect = RECT{530, 34, 664, 58};
    state.autoMasterRect = RECT{672, 34, 806, 58};
    state.metPlayRect = RECT{814, 34, 900, 58};
    state.metRecRect = RECT{906, 34, 992, 58};
    state.monitorRect = RECT{998, 34, 1088, 58};
    state.bpmDownRect = RECT{1094, 34, 1126, 58};
    state.bpmUpRect = RECT{1132, 34, 1164, 58};
    state.countInRect = RECT{1170, 34, 1292, 58};

    DrawMenuTab(hdc, state.fileMenuRect, L"File");
    DrawMenuTab(hdc, state.viewMenuRect, L"View");
    DrawMenuTab(hdc, state.audioMenuRect, L"Audio");
    DrawMenuTab(hdc, state.trackMenuRect, L"Track");

    DrawButton(hdc, state.playRect, state.playing ? L"Playing" : L"Play", state.playing);
    DrawButton(hdc, state.stopRect, L"Stop", false);
    DrawButton(hdc, state.recordRect, state.recording ? L"Recording" : L"Record", state.recording);
    DrawButton(hdc, state.importRect, L"Import WAV", false);
    DrawButton(hdc, state.automixRect, state.automixRunning.load() ? L"AutoMixing..." : L"Apply AutoMix", state.automixRunning.load());
    DrawButton(hdc, state.vocalCheckRect, L"Vocal Check", false);
    DrawButton(hdc, state.autoMasterRect, L"Auto Master", false);
    DrawButton(hdc, state.metPlayRect, state.metronomePlay ? L"Met Play On" : L"Met Play Off", state.metronomePlay);
    DrawButton(hdc, state.metRecRect, state.metronomeRecord ? L"Met Rec On" : L"Met Rec Off", state.metronomeRecord);
    DrawButton(hdc, state.monitorRect, state.inputMonitoring ? L"Monitor On" : L"Monitor Off", state.inputMonitoring);
    DrawButton(hdc, state.bpmDownRect, L"BPM-", false);
    DrawButton(hdc, state.bpmUpRect, L"BPM+", false);
    DrawButton(hdc, state.countInRect, state.countInEnabled ? L"Count-In On" : L"Count-In Off", state.countInEnabled);

    const int visibleBars = std::max(1, static_cast<int>(std::round(state.viewBeatsVisible / 4.0f)));
    std::wstring status =
        L"BPM " + std::to_wstring(static_cast<int>(state.project.bpm)) +
        L"   |   SR " + std::to_wstring(state.project.projectSampleRate > 0 ? state.project.projectSampleRate : 0) +
        L"   |   Tracks " + std::to_wstring(static_cast<int>(state.project.tracks.size())) +
        L"   |   View " + std::to_wstring(visibleBars) + L" bars" +
        L"   |   In " + state.selectedInputDeviceName +
        L"   |   Out " + state.selectedOutputDeviceName;

    RECT statusRect{1300, 32, client.right - 20, 58};
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, kPalette.textPrimary);
    DrawTextW(hdc, status.c_str(), -1, &statusRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

// ── Insert-chain inspector panel ─────────────────────────────────────────────
// Returns the outer panel rect in client coordinates (declared in draw.h).
RECT UiDrawGetInspectorPanelRect(const RECT& client, const UiState& state) {
    const int idx = state.fxInspectorIndex;
    int slotCount = 0;
    if (state.fxInspectorIsTrack) {
        if (idx >= 0 && idx < static_cast<int>(state.project.tracks.size()))
            slotCount = std::clamp(state.project.tracks[static_cast<size_t>(idx)].insertSlots, 0, kMaxInsertSlots);
    } else {
        if (idx >= 0 && idx < static_cast<int>(state.project.buses.size()))
            slotCount = std::clamp(state.project.buses[static_cast<size_t>(idx)].insertSlots, 0, kMaxInsertSlots);
    }
    const bool hasSelected = (state.fxInspectorSelectedSlot >= 0 && state.fxInspectorSelectedSlot < slotCount);
    RECT r{};
    r.left   = client.left + kUiDrawInspPadX;
    r.top    = client.top  + kTopBarHeight + kRulerHeight + 6;
    r.right  = r.left + kUiDrawInspW;
    r.bottom = r.top + kUiDrawInspHeaderH + kUiDrawInspCtrlH + slotCount * kUiDrawInspSlotH + (hasSelected ? kUiDrawInspParamH : 0) + 6;
    return r;
}

void UiDrawInsertInspector(HDC hdc, const RECT& client, const UiState& state) {
    if (!state.fxInspectorOpen || state.fxInspectorIndex < 0) return;

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

    const int idx = state.fxInspectorIndex;
    std::wstring title = state.fxInspectorIsTrack
        ? (L"INSERTS - " + (idx < static_cast<int>(state.project.tracks.size()) ? state.project.tracks[static_cast<size_t>(idx)].name : L"Track"))
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
    if (state.fxInspectorIsTrack) {
        if (idx >= 0 && idx < static_cast<int>(state.project.tracks.size()))
            slotCount = std::clamp(state.project.tracks[static_cast<size_t>(idx)].insertSlots, 0, kMaxInsertSlots);
    } else {
        if (idx >= 0 && idx < static_cast<int>(state.project.buses.size()))
            slotCount = std::clamp(state.project.buses[static_cast<size_t>(idx)].insertSlots, 0, kMaxInsertSlots);
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
        if (state.fxInspectorIsTrack) {
            if (idx < static_cast<int>(state.project.tracks.size()))
                effects = &state.project.tracks[static_cast<size_t>(idx)].insertEffects;
            if (idx < static_cast<int>(state.project.tracks.size()))
                bypass  = &state.project.tracks[static_cast<size_t>(idx)].insertBypass;
        } else {
            if (idx < static_cast<int>(state.project.buses.size()))
                effects = &state.project.buses[static_cast<size_t>(idx)].insertEffects;
            if (idx < static_cast<int>(state.project.buses.size()))
                bypass  = &state.project.buses[static_cast<size_t>(idx)].insertBypass;
        }

        const int  effectType = (effects && s < kMaxInsertSlots)
            ? std::clamp(static_cast<int>((*effects)[static_cast<size_t>(s)]), 0, kInsertEffectTypeCount - 1) : 0;
        const bool slotBypassed = (bypass && s < kMaxInsertSlots) ? (*bypass)[static_cast<size_t>(s)] : false;
        const bool isSelected = (s == state.fxInspectorSelectedSlot);

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
    if (state.fxInspectorSelectedSlot >= 0 && state.fxInspectorSelectedSlot < slotCount) {
        const int selSlot = state.fxInspectorSelectedSlot;

        const InsertConfigArray* pParams = nullptr;
        const InsertEffectArray* pEff = nullptr;
        if (state.fxInspectorIsTrack) {
            if (idx < static_cast<int>(state.project.tracks.size()))
                pParams = &state.project.tracks[static_cast<size_t>(idx)].insertConfig;
            if (idx < static_cast<int>(state.project.tracks.size()))
                pEff = &state.project.tracks[static_cast<size_t>(idx)].insertEffects;
        } else {
            if (idx < static_cast<int>(state.project.buses.size()))
                pParams = &state.project.buses[static_cast<size_t>(idx)].insertConfig;
            if (idx < static_cast<int>(state.project.buses.size()))
                pEff = &state.project.buses[static_cast<size_t>(idx)].insertEffects;
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

void UiDrawLeftTrackPanel(HDC hdc, const RECT& rect, const UiState& state) {
    Fill(hdc, rect, kPalette.leftPanel);

    RECT header{rect.left, rect.top, rect.right, rect.top + kRulerHeight};
    Fill(hdc, header, kPalette.leftPanelHeader);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, kPalette.textMuted);
    RECT headerText{header.left + 14, header.top, header.right, header.bottom};
    DrawTextW(hdc, L"TRACKS", -1, &headerText, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    if (state.project.tracks.empty()) {
        RECT emptyText{rect.left + 14, rect.top + 46, rect.right - 14, rect.top + 100};
        DrawTextW(hdc, L"No tracks loaded.\nClick Import WAV.", -1, &emptyText, DT_LEFT | DT_WORDBREAK);
        return;
    }

    for (size_t i = 0; i < state.project.tracks.size(); ++i) {
        const int y = rect.top + kRulerHeight + static_cast<int>(i) * kTrackRowHeight;
        RECT row{rect.left, y, rect.right, y + kTrackRowHeight};
        Fill(hdc, row, (i % 2 == 0) ? RGB(39, 43, 49) : RGB(42, 47, 53));
        if (static_cast<int>(i) == state.selectedTrackIndex) {
            Fill(hdc, RECT{row.left, row.top, row.right, row.bottom}, RGB(55, 66, 84));
            StrokeRect(hdc, row, RGB(120, 143, 179));
        }

        RECT nameRect{row.left + 14, row.top + 10, row.right - 82, row.top + 30};
        SetTextColor(hdc, kPalette.textPrimary);
        DrawTextW(hdc, state.project.tracks[i].name.c_str(), -1, &nameRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        RECT busRect{};
        RECT panKnobRect{};
        RECT panValRect{};
        RECT fxRect{};
        UiLayoutGetTrackRoutingRects(rect, static_cast<int>(i), &busRect, &panKnobRect, &panValRect, &fxRect);

        Fill(hdc, busRect, RGB(49, 54, 61));
        StrokeRect(hdc, busRect, RGB(82, 88, 97));
        SetTextColor(hdc, RGB(220, 224, 230));
        DrawTextW(hdc, BusName(AutomationTrackBusIndexAt(state, static_cast<int>(i))), -1, &busRect,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        const float trPan = AutomationTrackPanAt(state, static_cast<int>(i));
        DrawPanKnob(hdc, panKnobRect, trPan, static_cast<int>(i) == state.selectedTrackIndex);
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

        const int trackFxSlots = (i < state.project.tracks.size()) ? std::clamp(state.project.tracks[i].insertSlots, 0, 8) : 0;
        wchar_t fxText[16] = {};
        if (trackFxSlots <= 0 || i >= state.project.tracks.size()) {
            swprintf_s(fxText, L"FX0");
        } else {
            const int effectType = std::clamp(static_cast<int>(state.project.tracks[i].insertEffects[0]), 0, kInsertEffectTypeCount - 1);
            const bool bypass = (i < state.project.tracks.size()) ? state.project.tracks[i].insertBypass[0] : false;
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
        UiLayoutGetTrackButtonRects(rect, static_cast<int>(i), &muteRect, &soloRect, &recRect);

        const bool muted = i < state.project.tracks.size() && state.project.tracks[i].mute;
        const bool soloed = i < state.project.tracks.size() && state.project.tracks[i].solo;
        const bool armed = i < state.project.tracks.size() && state.project.tracks[i].recordArm;

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

        const float gainDb = AutomationTrackGainDbAt(state, static_cast<int>(i));
        wchar_t dbText[32] = {};
        swprintf_s(dbText, L"%+.1f dB", gainDb);
        RECT gainRect{row.right - 106, row.top + 34, row.right - 56, row.bottom - 12};
        SetTextColor(hdc, RGB(196, 202, 210));
        DrawTextW(hdc, dbText, -1, &gainRect, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

        RECT rail{};
        RECT knob{};
        UiLayoutGetTrackFaderRects(rect, static_cast<int>(i), &rail, &knob);
        Fill(hdc, rail, RGB(26, 29, 33));
        StrokeRect(hdc, rail, RGB(60, 64, 72));

        const int knobTop = UiLayoutFaderKnobTopFromGain(rail, gainDb);
        knob.top = knobTop;
        knob.bottom = knobTop + kFaderKnobHeight;
        Fill(hdc, knob, RGB(214, 218, 224));
        StrokeRect(hdc, knob, RGB(90, 95, 105));

        RECT meterBg{row.right - 26, row.top + 12, row.right - 14, row.bottom - 12};
        Fill(hdc, meterBg, RGB(28, 31, 36));
        RECT meterLevel{meterBg.left + 2, meterBg.top + 2, meterBg.right - 2,
                        meterBg.top + 2 + static_cast<int>((meterBg.bottom - meterBg.top - 4) * (0.3 + 0.1 * (i % 4)))};
        Fill(hdc, meterLevel, RGB(78, 162, 121));
    }

    const int busTop = UiLayoutBusPanelTop(rect, state);
    if (busTop + 20 < rect.bottom) {
        RECT bHeader{rect.left, busTop, rect.right, busTop + 16};
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
            UiLayoutGetBusControlRects(rect, state, b, &rowRect, &muteRect, &gainDownRect, &gainUpRect, &panKnobRect, &panValRect, &fxRect);
            if (rowRect.bottom > rect.bottom) break;

            Fill(hdc, rowRect, (b % 2 == 0) ? RGB(39, 43, 49) : RGB(42, 47, 53));

            RECT nameRect{rowRect.left + 6, rowRect.top, rowRect.left + 90, rowRect.bottom};
            SetTextColor(hdc, kPalette.textPrimary);
            DrawTextW(hdc, BusName(b), -1, &nameRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

            Fill(hdc, muteRect, BusMuteAt(state, b) ? RGB(176, 76, 76) : RGB(56, 60, 67));
            Fill(hdc, gainDownRect, RGB(56, 60, 67));
            Fill(hdc, gainUpRect, RGB(56, 60, 67));
            Fill(hdc, panValRect, RGB(40, 44, 49));
            Fill(hdc, fxRect, RGB(40, 44, 49));
            StrokeRect(hdc, muteRect, RGB(82, 88, 97));
            StrokeRect(hdc, gainDownRect, RGB(82, 88, 97));
            StrokeRect(hdc, gainUpRect, RGB(82, 88, 97));
            StrokeRect(hdc, panValRect, RGB(82, 88, 97));
            StrokeRect(hdc, fxRect, RGB(82, 88, 97));

            SetTextColor(hdc, RGB(228, 232, 238));
            DrawTextW(hdc, L"M", -1, &muteRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            DrawTextW(hdc, L"-", -1, &gainDownRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            DrawTextW(hdc, L"+", -1, &gainUpRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

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

            const int busFxSlots = (b < static_cast<int>(state.project.buses.size())) ? std::clamp(state.project.buses[static_cast<size_t>(b)].insertSlots, 0, 8) : 0;
            wchar_t busFxText[16] = {};
            if (busFxSlots <= 0 || b >= static_cast<int>(state.project.buses.size())) {
                swprintf_s(busFxText, L"FX0");
            } else {
                const int effectType = std::clamp(static_cast<int>(state.project.buses[static_cast<size_t>(b)].insertEffects[0]), 0, kInsertEffectTypeCount - 1);
                const bool bypass = (b < static_cast<int>(state.project.buses.size())) ? state.project.buses[static_cast<size_t>(b)].insertBypass[0] : false;
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
}

void UiDrawRuler(HDC hdc, const RECT& rect, const UiState& state) {
    Fill(hdc, rect, kPalette.rulerBg);

    HPEN barPen = CreatePen(PS_SOLID, 1, kPalette.barLine);
    HPEN beatPen = CreatePen(PS_SOLID, 1, kPalette.beatLine);
    HGDIOBJ oldPen = SelectObject(hdc, beatPen);

    const int width = std::max(1, static_cast<int>(rect.right - rect.left));
    const int firstBeat = static_cast<int>(std::floor(state.viewStartBeat));
    const int lastBeat = static_cast<int>(std::ceil(state.viewStartBeat + state.viewBeatsVisible));

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, kPalette.textMuted);

    for (int beat = firstBeat; beat <= lastBeat; ++beat) {
        const float rel = (static_cast<float>(beat) - state.viewStartBeat) / state.viewBeatsVisible;
        const int x = rect.left + static_cast<int>(rel * static_cast<float>(width));
        const bool isBar = (beat % 4 == 0);
        SelectObject(hdc, isBar ? barPen : beatPen);
        MoveToEx(hdc, x, rect.top, nullptr);
        LineTo(hdc, x, rect.bottom);

        if (isBar && x >= rect.left && x <= rect.right) {
            const std::wstring label = std::to_wstring((beat / 4) + 1);
            RECT t{x - 16, rect.top + 2, x + 16, rect.bottom - 2};
            DrawTextW(hdc, label.c_str(), -1, &t, DT_CENTER | DT_TOP | DT_SINGLELINE);
        }
    }

    SelectObject(hdc, oldPen);
    DeleteObject(barPen);
    DeleteObject(beatPen);

    // Playhead marker - downward-pointing triangle on ruler
    const int phX = rect.left + static_cast<int>(
        ((state.playheadBeat - state.viewStartBeat) / std::max(1.0f, state.viewBeatsVisible))
        * static_cast<float>(width));
    if (phX >= rect.left - 8 && phX <= rect.right + 8) {
        HBRUSH phBrush = CreateSolidBrush(kPalette.playhead);
        HPEN   phPen   = CreatePen(PS_SOLID, 1, kPalette.playhead);
        HGDIOBJ ob = SelectObject(hdc, phBrush);
        HGDIOBJ op = SelectObject(hdc, phPen);
        const POINT tri[3] = {{phX, rect.top + 2}, {phX - 6, rect.bottom - 4}, {phX + 6, rect.bottom - 4}};
        Polygon(hdc, tri, 3);
        SelectObject(hdc, ob);
        SelectObject(hdc, op);
        DeleteObject(phBrush);
        DeleteObject(phPen);
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

    const int halfHeight = std::max(1, (height / 2) - 2);
    const int centerY = clipRect.top + (height / 2);

    HPEN wavePen = CreatePen(PS_SOLID, 1, RGB(238, 242, 248));
    HGDIOBJ oldPen = SelectObject(hdc, wavePen);

    const int saved = SaveDC(hdc);
    IntersectClipRect(hdc, clipRect.left, clipRect.top, clipRect.right, clipRect.bottom);

    for (int x = clipRect.left; x < clipRect.right; ++x) {
        const int pixelIndex = x - clipRect.left;
        const std::uint64_t sourceSpan = endFrame - startFrame;
        const std::uint64_t frameStart = startFrame + (static_cast<std::uint64_t>(pixelIndex) * sourceSpan) / static_cast<std::uint64_t>(width);
        std::uint64_t frameEnd = startFrame + (static_cast<std::uint64_t>(pixelIndex + 1) * sourceSpan) / static_cast<std::uint64_t>(width);
        frameEnd = std::max(frameEnd, frameStart + 1);
        frameEnd = std::min(frameEnd, endFrame);

        float peak = 0.0f;
        for (std::uint64_t f = frameStart; f < frameEnd; ++f) {
            const size_t i = static_cast<size_t>(f) * 2;
            const float l = std::fabs(audio.stereo[i]);
            const float r = std::fabs(audio.stereo[i + 1]);
            peak = std::max(peak, std::max(l, r));
        }

        const int amp = static_cast<int>(std::round(peak * static_cast<float>(halfHeight)));
        MoveToEx(hdc, x, centerY - amp, nullptr);
        LineTo(hdc, x, centerY + amp + 1);
    }

    RestoreDC(hdc, saved);
    SelectObject(hdc, oldPen);
    DeleteObject(wavePen);
}

void UiDrawArrangeLanes(HDC hdc, const RECT& rect, const UiState& state) {
    Fill(hdc, rect, kPalette.arrangeBg);

    const int width = std::max(1, static_cast<int>(rect.right - rect.left));
    const int firstBeat = static_cast<int>(std::floor(state.viewStartBeat));
    const int lastBeat = static_cast<int>(std::ceil(state.viewStartBeat + state.viewBeatsVisible));

    HPEN barPen = CreatePen(PS_SOLID, 1, kPalette.barLine);
    HPEN beatPen = CreatePen(PS_SOLID, 1, kPalette.beatLine);
    HPEN lanePen = CreatePen(PS_SOLID, 1, RGB(40, 44, 50));
    HGDIOBJ oldPen = SelectObject(hdc, lanePen);

    for (size_t i = 0; i < state.project.tracks.size(); ++i) {
        const int y = rect.top + static_cast<int>(i) * kTrackRowHeight;
        RECT lane{rect.left, y, rect.right, y + kTrackRowHeight};
        Fill(hdc, lane, (i % 2 == 0) ? kPalette.laneDark : kPalette.laneLight);
        MoveToEx(hdc, lane.left, lane.bottom - 1, nullptr);
        LineTo(hdc, lane.right, lane.bottom - 1);
    }

    for (int beat = firstBeat; beat <= lastBeat; ++beat) {
        const float rel = (static_cast<float>(beat) - state.viewStartBeat) / state.viewBeatsVisible;
        const int x = rect.left + static_cast<int>(rel * static_cast<float>(width));
        SelectObject(hdc, (beat % 4 == 0) ? barPen : beatPen);
        MoveToEx(hdc, x, rect.top, nullptr);
        LineTo(hdc, x, rect.bottom);
    }

    for (size_t i = 0; i < state.project.clips.size(); ++i) {
        RECT clipRect{};
        if (!UiLayoutClipRectForDraw(rect, state, state.project.clips[i], &clipRect)) {
            continue;
        }

        const int clipUnclippedLeft = UiLayoutBeatToX(rect, state, state.project.clips[i].startBeat);
        const int clipUnclippedRight = UiLayoutBeatToX(rect, state, state.project.clips[i].startBeat + state.project.clips[i].lengthBeats);
        const int fullClipPx = std::max(1, clipUnclippedRight - clipUnclippedLeft);
        const int clippedLeft = static_cast<int>(clipRect.left);
        const int clippedRight = static_cast<int>(clipRect.right);
        const int visibleLeftPx = std::max(0, clippedLeft - clipUnclippedLeft);
        const int visibleRightPx = std::min(fullClipPx, visibleLeftPx + std::max(1, clippedRight - clippedLeft));

        Fill(hdc, clipRect, state.project.clips[i].color);

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
        if (state.project.clips[i].audioIndex >= 0 && state.project.clips[i].audioIndex < static_cast<int>(state.project.audio.size()) && waveRect.right > waveRect.left && waveRect.bottom > waveRect.top) {
            const LoadedAudio& audio = state.project.audio[static_cast<size_t>(state.project.clips[i].audioIndex)];
            const std::uint64_t totalFrames = static_cast<std::uint64_t>(audio.frames);
            const float spb = TimelineSamplesPerBeat(state);
            const std::uint64_t srcOffset = state.project.clips[i].sourceOffsetFrames;
            const std::uint64_t clipLenFrames = std::min(
                static_cast<std::uint64_t>(state.project.clips[i].lengthBeats * spb),
                totalFrames > srcOffset ? totalFrames - srcOffset : std::uint64_t{0});
            if (clipLenFrames > 0) {
                const auto fcp = static_cast<std::uint64_t>(std::max(1, fullClipPx));
                std::uint64_t srcStart = srcOffset + (static_cast<std::uint64_t>(visibleLeftPx)  * clipLenFrames) / fcp;
                std::uint64_t srcEnd   = srcOffset + (static_cast<std::uint64_t>(visibleRightPx) * clipLenFrames) / fcp;
                srcEnd = std::min(std::max(srcEnd, srcStart + 1), totalFrames);
                DrawClipWaveform(hdc, waveRect, audio, srcStart, srcEnd);
            }
        }
        StrokeRect(hdc, clipRect, (state.selectedClipIndex == static_cast<int>(i)) ? RGB(232, 232, 232) : RGB(24, 24, 24));

        // Trim edge handles on selected clip
        if (state.selectedClipIndex == static_cast<int>(i)) {
            const int handleW = 5;
            RECT leftHandle  {clipRect.left,            clipRect.top, clipRect.left + handleW,  clipRect.bottom};
            RECT rightHandle {clipRect.right - handleW, clipRect.top, clipRect.right,            clipRect.bottom};
            Fill(hdc, leftHandle,  RGB(255, 255, 255));
            Fill(hdc, rightHandle, RGB(255, 255, 255));
        }

        RECT textRect{labelRect.left + 4, labelRect.top, labelRect.right - 4, labelRect.bottom};
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(240, 240, 240));
        DrawTextW(hdc, state.project.clips[i].name.c_str(), -1, &textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    const int playheadX = UiLayoutBeatToX(rect, state, state.playheadBeat);
    HPEN playheadPen = CreatePen(PS_SOLID, 2, kPalette.playhead);
    SelectObject(hdc, playheadPen);
    MoveToEx(hdc, playheadX, rect.top, nullptr);
    LineTo(hdc, playheadX, rect.bottom);

    DeleteObject(playheadPen);
    DeleteObject(barPen);
    DeleteObject(beatPen);
    DeleteObject(lanePen);
    SelectObject(hdc, oldPen);

    if (state.project.tracks.empty()) {
        RECT hint{rect.left + 24, rect.top + 24, rect.right - 24, rect.top + 80};
        SetTextColor(hdc, kPalette.textMuted);
        SetBkMode(hdc, TRANSPARENT);
        DrawTextW(hdc, L"Import one or more WAV files to create real tracks and clips.", -1, &hint, DT_LEFT | DT_WORDBREAK);
    }
}
