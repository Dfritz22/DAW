#include "ui/wndproc/lbuttondown.h"

#include "ai/automix_bridge.h"      // StartAutoMixAsync, AnalyzeSelectedTrackQuality
#include "audio/transport_adapter.h"// DispatchTransportEvent, WillCountIn
#include "core/internal_app_services.h" // UpdateWindowTitle
#include "core/timeline_edit.h"     // PushUndo (CoreState overload via shim)
#include "daw_automation.h"         // TrackBusIndexAt
#include "daw_project.h"            // ImportWavFiles, PushUndo(AppState)
#include "dsp/insert_types.h"       // InsertConfig*, kFx*, kInsertEffectTypeCount, kMaxInsertSlots
#include "ui/UiRuntimeState.h"      // kBus*, kFader*, kRulerHeight, kBusCount, kBusPanel*
#include "ui/dock.h"                // PanelKind
#include "ui/dpi.h"                 // Dpi
#include "ui/draw.h"                // UiDrawGetInspectorPanelRect, kUiDrawInsp*
#include "ui/layout.h"              // UiLayout*, LayoutRects
#include "ui/panel.h"               // PanelGet

#include <windows.h>
#include <windowsx.h>
#include <algorithm>
#include <cstdint>

namespace {

using daw::internal::core::UpdateWindowTitle;

// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
// Phase 16n: WM_LBUTTONDOWN internal split.
// Each helper returns true if it consumed the click. The dispatcher in
// WindowProc calls them in priority order. Verbatim split â€” no behavior
// change. Designed for follow-up extraction to ui/wndproc/lbuttondown.cpp.
// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

static bool LButtonOnDockSplitterOrTab(HWND hwnd, AppState& state, POINT pt) {
            // the floating WM_PAINT). They have no dock tree of their own,
            // so skip splitter / tab hit-tests in that case to avoid
            // spurious matches against the main window's dock layout.
            const bool isMainHwnd = (hwnd == state.hwnd);

            // ── Dock splitter drag start ────────────────────────────────
            // Check splitters before anything else so the user can grab a
            // divider even if its hit zone overlaps a panel's controls.
            if (isMainHwnd) {
                for (const auto& sp : state.ui.dock.dockSplitters) {
                    if (PtInRect(&sp.rect, pt)) {
                        state.ui.dock.draggingSplitter       = true;
                        state.ui.dock.dragSplitterNode       = sp.node;
                        state.ui.dock.dragSplitterHorizontal = sp.horizontal;
                        SetCapture(hwnd);
                        return true;
                    }
                }
            }

            // ── Dock tab click → activate that tab + arm potential drag ──
            if (isMainHwnd) {
                for (const auto& tab : state.ui.dock.dockTabs) {
                    if (PtInRect(&tab.rect, pt)) {
                        if (tab.node != nullptr) {
                            tab.node->activeTab = tab.tabIndex;
                            // Arm a tab drag — promoted to active in WM_MOUSEMOVE
                            // once cursor moves past kDragTabThresholdPx. Primary
                            // panels can't be dragged out of their leaf (they're
                            // pinned to the layout), but can still be clicked to
                            // activate when sharing a tab strip with others.
                            const daw::ui::PanelKind pk =
                                tab.node->panels[static_cast<size_t>(tab.tabIndex)];
                            if (!daw::ui::PanelGet(pk).primary) {
                                state.ui.dock.dragTabArmed   = true;
                                state.ui.dock.dragTabActive  = false;
                                state.ui.dock.dragTabSource  = tab.node;
                                state.ui.dock.dragTabIndex   = tab.tabIndex;
                                state.ui.dock.dragTabPanel   = pk;
                                state.ui.dock.dragTabStartPt = pt;
                                state.ui.dock.dragTabCurPt   = pt;
                                SetCapture(hwnd);
                            }
                            InvalidateRect(hwnd, nullptr, FALSE);
                        }
                        return true;
                    }
                }
            }

    return false;
}

static bool LButtonOnTopBarButtons(HWND hwnd, AppState& state, POINT pt) {
            if (PtInRect(&state.ui.topBar.playRect, pt)) {
                using daw::services::TransportEvent;
                const auto ev = state.audio.playing ? TransportEvent::StopPressed
                                                     : TransportEvent::PlayPressed;
                daw::app::DispatchTransportEvent(hwnd, state, ev, /*rewindOnStop=*/false);
                InvalidateRect(hwnd, nullptr, FALSE);
                return true;
            }
            if (PtInRect(&state.ui.topBar.stopRect, pt)) {
                // Stop button always rewinds.
                daw::app::DispatchTransportEvent(hwnd, state,
                    daw::services::TransportEvent::StopPressed, /*rewindOnStop=*/true);
                InvalidateRect(hwnd, nullptr, FALSE);
                return true;
            }
            if (PtInRect(&state.ui.topBar.recordRect, pt)) {
                using daw::services::TransportEvent;
                TransportEvent ev;
                if (state.audio.recording || state.audio.countingIn) {
                    ev = TransportEvent::StopPressed;
                } else if (daw::app::WillCountIn(state.audio)) {
                    ev = TransportEvent::RecordPressedWithCountIn;
                } else {
                    ev = TransportEvent::RecordPressed;
                }
                daw::app::DispatchTransportEvent(hwnd, state, ev, /*rewindOnStop=*/true);
                InvalidateRect(hwnd, nullptr, FALSE);
                return true;
            }
            if (PtInRect(&state.ui.topBar.importRect, pt)) {
                ImportWavFiles(hwnd, state);
                InvalidateRect(hwnd, nullptr, FALSE);
                return true;
            }
            if (PtInRect(&state.ui.topBar.automixRect, pt)) {
                StartAutoMixAsync(hwnd, state);
                InvalidateRect(hwnd, nullptr, FALSE);
                return true;
            }
            if (PtInRect(&state.ui.topBar.vocalCheckRect, pt)) {
                AnalyzeSelectedTrackQuality(hwnd, state);
                InvalidateRect(hwnd, nullptr, FALSE);
                return true;
            }
            if (PtInRect(&state.ui.topBar.autoMasterRect, pt)) {
                DoAutoMaster(hwnd, state);
                InvalidateRect(hwnd, nullptr, FALSE);
                return true;
            }
            if (PtInRect(&state.ui.topBar.metPlayRect, pt)) {
                state.audio.metronomePlay = !state.audio.metronomePlay;
                InvalidateRect(hwnd, nullptr, FALSE);
                return true;
            }
            if (PtInRect(&state.ui.topBar.metRecRect, pt)) {
                state.audio.metronomeRecord = !state.audio.metronomeRecord;
                InvalidateRect(hwnd, nullptr, FALSE);
                return true;
            }
            if (PtInRect(&state.ui.topBar.monitorRect, pt)) {
                state.audio.inputMonitoring = !state.audio.inputMonitoring;
                InvalidateRect(hwnd, nullptr, FALSE);
                return true;
            }
            if (PtInRect(&state.ui.topBar.bpmDownRect, pt)) {
                const bool coarse = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                state.core.project.bpm = static_cast<float>(std::max(40, static_cast<int>(state.core.project.bpm) - (coarse ? 5 : 1)));
                InvalidateRect(hwnd, nullptr, FALSE);
                return true;
            }
            if (PtInRect(&state.ui.topBar.bpmUpRect, pt)) {
                const bool coarse = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                state.core.project.bpm = static_cast<float>(std::min(260, static_cast<int>(state.core.project.bpm) + (coarse ? 5 : 1)));
                InvalidateRect(hwnd, nullptr, FALSE);
                return true;
            }
            if (PtInRect(&state.ui.topBar.countInRect, pt)) {
                state.audio.countInEnabled = !state.audio.countInEnabled;
                InvalidateRect(hwnd, nullptr, FALSE);
                return true;
            }
    return false;
}

static bool LButtonOnFxInspector(HWND hwnd, AppState& state, POINT pt, const RECT& client) {
            if (state.ui.inspector.fxInspectorOpen && state.ui.inspector.fxInspectorIndex >= 0) {
                const RECT inspPanel = UiDrawGetInspectorPanelRect(client, state);
                if (!PtInRect(&inspPanel, pt)) {
                    // Click outside → close
                    state.ui.inspector.fxInspectorOpen = false;
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return false;
                } else {
                    // Click inside inspector - handle controls
                    const int idx = state.ui.inspector.fxInspectorIndex;
                    int* pSlots       = nullptr;
                    InsertEffectArray* pEffects = nullptr;
                    InsertBypassArray* pBypass  = nullptr;
                    InsertConfigArray* pParams  = nullptr;
                    if (state.ui.inspector.fxInspectorIsTrack) {
                        if (idx < static_cast<int>(state.core.project.tracks.size()))
                            pSlots = &state.core.project.tracks[static_cast<size_t>(idx)].insertSlots;
                        if (idx < static_cast<int>(state.core.project.tracks.size()))
                            pEffects = &state.core.project.tracks[static_cast<size_t>(idx)].insertEffects;
                        if (idx < static_cast<int>(state.core.project.tracks.size()))
                            pBypass = &state.core.project.tracks[static_cast<size_t>(idx)].insertBypass;
                        if (idx < static_cast<int>(state.core.project.tracks.size()))
                            pParams = &state.core.project.tracks[static_cast<size_t>(idx)].insertConfig;
                    } else {
                        if (idx < static_cast<int>(state.core.project.buses.size()))
                            pSlots = &state.core.project.buses[static_cast<size_t>(idx)].insertSlots;
                        if (idx < static_cast<int>(state.core.project.buses.size()))
                            pEffects = &state.core.project.buses[static_cast<size_t>(idx)].insertEffects;
                        if (idx < static_cast<int>(state.core.project.buses.size()))
                            pBypass = &state.core.project.buses[static_cast<size_t>(idx)].insertBypass;
                        if (idx < static_cast<int>(state.core.project.buses.size()))
                            pParams = &state.core.project.buses[static_cast<size_t>(idx)].insertConfig;
                    }

                    const int slotCount = pSlots ? std::clamp(*pSlots, 0, kMaxInsertSlots) : 0;

                    // Close button
                    RECT closeBtn{inspPanel.right - 24, inspPanel.top + 4, inspPanel.right - 4, inspPanel.top + kUiDrawInspHeaderH - 4};
                    if (PtInRect(&closeBtn, pt)) {
                        state.ui.inspector.fxInspectorOpen = false;
                        InvalidateRect(hwnd, nullptr, FALSE);
                        return true;
                    }

                    // ADD / REM buttons
                    const int ctrlTop = inspPanel.top + kUiDrawInspHeaderH;
                    RECT addBtn{inspPanel.left + 6,  ctrlTop + 4, inspPanel.left + 66,  ctrlTop + kUiDrawInspCtrlH - 4};
                    RECT remBtn{inspPanel.left + 72, ctrlTop + 4, inspPanel.left + 132, ctrlTop + kUiDrawInspCtrlH - 4};
                    if (PtInRect(&addBtn, pt) && pSlots && slotCount < kMaxInsertSlots) {
                        EnterCriticalSection(&state.audio.audioStateLock);
                        (*pSlots)++;
                        state.core.projectModified = true;
                        UpdateWindowTitle(hwnd, state.core);
                        LeaveCriticalSection(&state.audio.audioStateLock);
                        InvalidateRect(hwnd, nullptr, FALSE);
                        return true;
                    }
                    if (PtInRect(&remBtn, pt) && pSlots && slotCount > 0) {
                        EnterCriticalSection(&state.audio.audioStateLock);
                        (*pSlots)--;
                        // Clear removed slot's bypass so it doesn't persist
                        if (pBypass) (*pBypass)[static_cast<size_t>(*pSlots)] = false;
                        state.core.projectModified = true;
                        UpdateWindowTitle(hwnd, state.core);
                        LeaveCriticalSection(&state.audio.audioStateLock);
                        InvalidateRect(hwnd, nullptr, FALSE);
                        return true;
                    }

                    // Per-slot rows
                    const int slotsTop = ctrlTop + kUiDrawInspCtrlH;
                    for (int s = 0; s < slotCount; ++s) {
                        const int rowTop = slotsTop + s * kUiDrawInspSlotH;
                        const int rowBot = rowTop + kUiDrawInspSlotH;
                        RECT typeBtn  {inspPanel.left + 26, rowTop + 2, inspPanel.left + 84, rowBot - 2};
                        RECT bypassBtn{inspPanel.left + 88, rowTop + 2, inspPanel.left + 130, rowBot - 2};
                        RECT arrowBtn {inspPanel.left + 134, rowTop + 2, inspPanel.left + 154, rowBot - 2};
                        if (PtInRect(&typeBtn, pt) && pEffects) {
                            EnterCriticalSection(&state.audio.audioStateLock);
                            std::uint8_t& fx = (*pEffects)[static_cast<size_t>(s)];
                            fx = static_cast<std::uint8_t>((fx + 1) % kInsertEffectTypeCount);
                            state.core.projectModified = true;
                            UpdateWindowTitle(hwnd, state.core);
                            LeaveCriticalSection(&state.audio.audioStateLock);
                            InvalidateRect(hwnd, nullptr, FALSE);
                            return true;
                        }
                        if (PtInRect(&bypassBtn, pt) && pBypass) {
                            EnterCriticalSection(&state.audio.audioStateLock);
                            (*pBypass)[static_cast<size_t>(s)] = !(*pBypass)[static_cast<size_t>(s)];
                            state.core.projectModified = true;
                            UpdateWindowTitle(hwnd, state.core);
                            LeaveCriticalSection(&state.audio.audioStateLock);
                            InvalidateRect(hwnd, nullptr, FALSE);
                            return true;
                        }
                        if (PtInRect(&arrowBtn, pt)) {
                            state.ui.inspector.fxInspectorSelectedSlot = (state.ui.inspector.fxInspectorSelectedSlot == s) ? -1 : s;
                            InvalidateRect(hwnd, nullptr, FALSE);
                            return true;
                        }
                    }

                    // Param knob clicks in the expanded strip
                    if (state.ui.inspector.fxInspectorSelectedSlot >= 0 && state.ui.inspector.fxInspectorSelectedSlot < slotCount && pParams && pEffects) {
                        const int selSlot = state.ui.inspector.fxInspectorSelectedSlot;
                        const int paramTop = slotsTop + slotCount * kUiDrawInspSlotH;
                        const int ky = paramTop + 16;
                        const int kw = (kUiDrawInspW - 12) / 4;
                        const int fxT = std::clamp(static_cast<int>((*pEffects)[static_cast<size_t>(selSlot)]), 0, kInsertEffectTypeCount - 1);

                        // Build same knob list as in Draw
                        struct KnobDef2 { float lo; float hi; int paramId; };
                        KnobDef2 knobs[4] = {};
                        int knobCount = 0;
                        switch (fxT) {
                        case kFxEQ:  knobs[0]={20,20000,0}; knobs[1]={-18,18,1}; knobs[2]={20,20000,2}; knobs[3]={-18,18,3}; knobCount=4; break;
                        case kFxCMP: knobs[0]={-60,0,10}; knobs[1]={1,20,11}; knobs[2]={0.1f,200,12}; knobs[3]={0,24,13}; knobCount=4; break;
                        case kFxSAT: knobs[0]={0,1,20}; knobs[1]={0,1,21}; knobCount=2; break;
                        case kFxDLY: knobs[0]={10,2000,30}; knobs[1]={0,0.95f,31}; knobs[2]={0,1,32}; knobCount=3; break;
                        case kFxREV: knobs[0]={0,1,40}; knobs[1]={0,1,41}; knobs[2]={0,1,42}; knobCount=3; break;
                        case kFxGATE:knobs[0]={-80,0,50}; knobs[1]={0.1f,200,51}; knobs[2]={10,500,52}; knobCount=3; break;
                        case kFxDEE: knobs[0]={-40,0,60}; knobs[1]={2000,16000,61}; knobs[2]={200,8000,62}; knobs[3]={0,24,63}; knobCount=4; break;
                        case kFxLIM: knobs[0]={-12,0,70}; knobs[1]={1,500,71}; knobCount=2; break;
                        }

                        for (int k = 0; k < knobCount; ++k) {
                            const int kx = inspPanel.left + 6 + k * kw;
                            const RECT kRect{kx, ky, kx + kw - 2, ky + kUiDrawInspParamH - 18};
                            if (PtInRect(&kRect, pt)) {
                                // Get current value for this paramId
                                const InsertConfig& P = (*pParams)[static_cast<size_t>(selSlot)];
                                float curVal = 0.0f;
                                switch (knobs[k].paramId) {
                                case 0: curVal=P.eq[0].freq_hz; break; case 1: curVal=P.eq[0].gain_db; break;
                                case 2: curVal=P.eq[1].freq_hz; break; case 3: curVal=P.eq[1].gain_db; break;
                                case 10:curVal=P.cmp_threshold_db; break; case 11:curVal=P.cmp_ratio; break;
                                case 12:curVal=P.cmp_attack_ms; break; case 13:curVal=P.cmp_makeup_db; break;
                                case 20:curVal=P.sat_drive; break; case 21:curVal=P.sat_mix; break;
                                case 30:curVal=P.dly_time_ms; break; case 31:curVal=P.dly_feedback; break; case 32:curVal=P.dly_mix; break;
                                case 40:curVal=P.rev_room_size; break; case 41:curVal=P.rev_damping; break; case 42:curVal=P.rev_mix; break;
                                case 50:curVal=P.gate_threshold_db; break; case 51:curVal=P.gate_attack_ms; break; case 52:curVal=P.gate_release_ms; break;
                                case 60:curVal=P.dee_threshold_db; break; case 61:curVal=P.dee_freq_hz; break; case 62:curVal=P.dee_bandwidth_hz; break; case 63:curVal=P.dee_reduction_db; break;
                                case 70:curVal=P.lim_ceiling_db; break; case 71:curVal=P.lim_release_ms; break;
                                }
                                state.ui.tools.draggingParamKnob = true;
                                state.ui.tools.paramKnobParamId  = knobs[k].paramId * 100 + selSlot;  // encode slot in lower 2 digits
                                state.ui.tools.paramKnobDragStartY   = pt.y;
                                state.ui.tools.paramKnobDragStartVal = curVal;
                                SetCapture(hwnd);
                                return true;
                            }
                        }
                    }
                    // Consumed by inspector (click on non-interactive area inside)
                    return true;
                }
            }
    return false;
}

static bool LButtonOnRuler(HWND hwnd, AppState& state, POINT pt, const LayoutRects& layout) {
            if (PtInRect(&layout.ruler, pt)) {
                const float beat = std::max(0.0f, UiLayoutXToBeat(layout.ruler, state, pt.x));
                state.ui.view.playheadBeat = UiLayoutSnapBeat(beat);
                state.ui.tools.draggingPlayhead = true;
                SetCapture(hwnd);
                InvalidateRect(hwnd, nullptr, FALSE);
                return true;
            }
    return false;
}

static bool LButtonOnTracksAndBusesPanel(HWND hwnd, AppState& state, POINT pt, const LayoutRects& layout) {
            const bool inTracksLeaf = (PtInRect(&layout.leftPanel, pt) && pt.y > layout.leftPanel.top + Dpi(kRulerHeight));
            bool inBusesLeaf = false;
            for (const auto& leaf : state.ui.dock.dockLayout) {
                if (leaf.activePanel == daw::ui::PanelKind::Buses && PtInRect(&leaf.rect, pt)) {
                    inBusesLeaf = true;
                    break;
                }
            }
            if ((inTracksLeaf || inBusesLeaf) && !state.core.project.tracks.empty()) {
                const bool inTracksRegion = inTracksLeaf && (pt.y < UiLayoutTracksRegionBottom(layout.leftPanel));
                const int trackIndex = UiLayoutTrackIndexFromY(layout.arrange, state, pt.y);
                if (inTracksRegion && trackIndex >= 0 && trackIndex < static_cast<int>(state.core.project.tracks.size())) {
                    state.ui.view.selectedTrackIndex = trackIndex;
                    state.ui.view.selectedClipIndex = -1;

                    RECT busRect{};
                    RECT panKnobRect{};
                    RECT panValRect{};
                    RECT fxRect{};
                    UiLayoutGetTrackRoutingRects(layout.leftPanel, trackIndex, &busRect, &panKnobRect, &panValRect, &fxRect, state.ui.view.tracksScrollY);
                    if (PtInRect(&busRect, pt)) {
                        EnterCriticalSection(&state.audio.audioStateLock);
                        if (trackIndex < static_cast<int>(state.core.project.tracks.size())) {
                            const int cur = TrackBusIndexAt(state, trackIndex);
                            state.core.project.tracks[static_cast<size_t>(trackIndex)].busIndex = (cur + 1) % kBusCount;
                            state.core.projectModified = true;
                            UpdateWindowTitle(hwnd, state.core);
                        }
                        LeaveCriticalSection(&state.audio.audioStateLock);
                        InvalidateRect(hwnd, nullptr, FALSE);
                        return true;
                    }
                    if (PtInRect(&fxRect, pt)) {
                        // Open insert-chain inspector for this track
                        state.ui.inspector.fxInspectorOpen    = true;
                        state.ui.inspector.fxInspectorIsTrack = true;
                        state.ui.inspector.fxInspectorIndex   = trackIndex;
                        InvalidateRect(hwnd, nullptr, FALSE);
                        return true;
                    }
                    if (PtInRect(&panKnobRect, pt) || PtInRect(&panValRect, pt)) {
                        if (PtInRect(&panValRect, pt) && (GetKeyState(VK_LBUTTON) & 0x8000)) {
                            // Double-click on value label resets to center
                            EnterCriticalSection(&state.audio.audioStateLock);
                            if (trackIndex < static_cast<int>(state.core.project.tracks.size()))
                                state.core.project.tracks[static_cast<size_t>(trackIndex)].pan = 0.0f;
                            state.core.projectModified = true;
                            UpdateWindowTitle(hwnd, state.core);
                            LeaveCriticalSection(&state.audio.audioStateLock);
                        } else if (trackIndex < static_cast<int>(state.core.project.tracks.size())) {
                            // Start drag
                            state.ui.tools.draggingPan    = true;
                            state.ui.tools.dragPanIsBus   = false;
                            state.ui.tools.dragPanIndex   = trackIndex;
                            state.ui.tools.dragPanStartY  = pt.y;
                            state.ui.tools.dragPanStartVal = state.core.project.tracks[static_cast<size_t>(trackIndex)].pan;
                            SetCapture(hwnd);
                        }
                        InvalidateRect(hwnd, nullptr, FALSE);
                        return true;
                    }

                    RECT muteRect{};
                    RECT soloRect{};
                    RECT recRect{};
                    UiLayoutGetTrackButtonRects(layout.leftPanel, trackIndex, &muteRect, &soloRect, &recRect, state.ui.view.tracksScrollY);
                    if (PtInRect(&muteRect, pt)) {
                        EnterCriticalSection(&state.audio.audioStateLock);
                        if (trackIndex < static_cast<int>(state.core.project.tracks.size())) {
                            state.core.project.tracks[static_cast<size_t>(trackIndex)].mute = !state.core.project.tracks[static_cast<size_t>(trackIndex)].mute;
                        }
                        LeaveCriticalSection(&state.audio.audioStateLock);
                        InvalidateRect(hwnd, nullptr, FALSE);
                        return true;
                    }
                    if (PtInRect(&recRect, pt)) {
                        EnterCriticalSection(&state.audio.audioStateLock);
                        if (trackIndex < static_cast<int>(state.core.project.tracks.size())) {
                            state.core.project.tracks[static_cast<size_t>(trackIndex)].recordArm = !state.core.project.tracks[static_cast<size_t>(trackIndex)].recordArm;
                        }
                        LeaveCriticalSection(&state.audio.audioStateLock);
                        InvalidateRect(hwnd, nullptr, FALSE);
                        return true;
                    }
                    if (PtInRect(&soloRect, pt)) {
                        EnterCriticalSection(&state.audio.audioStateLock);
                        if (trackIndex < static_cast<int>(state.core.project.tracks.size())) {
                            state.core.project.tracks[static_cast<size_t>(trackIndex)].solo = !state.core.project.tracks[static_cast<size_t>(trackIndex)].solo;
                        }
                        LeaveCriticalSection(&state.audio.audioStateLock);
                        InvalidateRect(hwnd, nullptr, FALSE);
                        return true;
                    }

                    RECT rail{};
                    RECT knob{};
                    UiLayoutGetTrackFaderRects(layout.leftPanel, trackIndex, &rail, &knob, state.ui.view.tracksScrollY);
                    RECT hitRect{rail.left - 12, rail.top, rail.right + 12, rail.bottom};
                    if (PtInRect(&hitRect, pt)) {
                        PushUndo(state);
                        state.ui.tools.draggingFader = true;
                        state.ui.tools.dragFaderTrack = trackIndex;
                        state.ui.tools.dragFaderStartY = pt.y;
                        EnterCriticalSection(&state.audio.audioStateLock);
                        state.ui.tools.dragFaderStartDb = UiLayoutGainFromFaderY(rail, pt.y);
                        state.core.project.tracks[static_cast<size_t>(trackIndex)].gainDb = state.ui.tools.dragFaderStartDb;
                        LeaveCriticalSection(&state.audio.audioStateLock);
                        SetCapture(hwnd);
                        InvalidateRect(hwnd, nullptr, FALSE);
                        return true;
                    }
                }

                // Bus hit-test: use the Buses dock leaf's actual rect so it
                // works regardless of where the panel has been moved/sized.
                RECT busPanelRect{};
                bool hasBusPanel = false;
                for (const auto& leaf : state.ui.dock.dockLayout) {
                    if (leaf.activePanel == daw::ui::PanelKind::Buses) {
                        busPanelRect = leaf.rect;
                        hasBusPanel = true;
                        break;
                    }
                }
                const int busTop = hasBusPanel ? (busPanelRect.top + Dpi(kBusPanelTopMargin))
                                               : (layout.leftPanel.bottom - Dpi(kBusPanelHeight) + Dpi(kBusPanelTopMargin));
                if (hasBusPanel && PtInRect(&busPanelRect, pt) && pt.y >= busTop + Dpi(kBusPanelHeaderHeight)) {
                    for (int b = 0; b < kBusCount; ++b) {
                        RECT rowRect{};
                        RECT muteRect{};
                        RECT gainDownRect{};
                        RECT gainUpRect{};
                        RECT panKnobRect{};
                        RECT panValRect{};
                        RECT fxRect{};
                        UiLayoutGetBusControlRectsInPanel(busPanelRect, b, &rowRect, &muteRect, &gainDownRect, &gainUpRect, &panKnobRect, &panValRect, &fxRect);                        if (!PtInRect(&rowRect, pt)) {
                            continue;
                        }

                        EnterCriticalSection(&state.audio.audioStateLock);
                        if (PtInRect(&muteRect, pt) && b < static_cast<int>(state.core.project.buses.size())) {
                            state.core.project.buses[static_cast<size_t>(b)].mute = !state.core.project.buses[static_cast<size_t>(b)].mute;
                        } else if (PtInRect(&gainDownRect, pt) && b < static_cast<int>(state.core.project.buses.size())) {
                            state.core.project.buses[static_cast<size_t>(b)].gainDb = std::max(kFaderMinDb, state.core.project.buses[static_cast<size_t>(b)].gainDb - 1.0f);
                        } else if (PtInRect(&gainUpRect, pt) && b < static_cast<int>(state.core.project.buses.size())) {
                            state.core.project.buses[static_cast<size_t>(b)].gainDb = std::min(kFaderMaxDb, state.core.project.buses[static_cast<size_t>(b)].gainDb + 1.0f);
                        } else if ((PtInRect(&panKnobRect, pt) || PtInRect(&panValRect, pt)) && b < static_cast<int>(state.core.project.buses.size())) {
                            LeaveCriticalSection(&state.audio.audioStateLock);
                            state.ui.tools.draggingPan    = true;
                            state.ui.tools.dragPanIsBus   = true;
                            state.ui.tools.dragPanIndex   = b;
                            state.ui.tools.dragPanStartY  = pt.y;
                            state.ui.tools.dragPanStartVal = state.core.project.buses[static_cast<size_t>(b)].pan;
                            SetCapture(hwnd);
                            InvalidateRect(hwnd, nullptr, FALSE);
                            return true;
                        } else if (PtInRect(&fxRect, pt) && b < static_cast<int>(state.core.project.buses.size())) {
                            // Open insert-chain inspector for this bus
                            LeaveCriticalSection(&state.audio.audioStateLock);
                            state.ui.inspector.fxInspectorOpen    = true;
                            state.ui.inspector.fxInspectorIsTrack = false;
                            state.ui.inspector.fxInspectorIndex   = b;
                            InvalidateRect(hwnd, nullptr, FALSE);
                            return true;
                        }
                        state.core.projectModified = true;
                        UpdateWindowTitle(hwnd, state.core);
                        LeaveCriticalSection(&state.audio.audioStateLock);

                        InvalidateRect(hwnd, nullptr, FALSE);
                        return true;
                    }
                }
            }
    return false;
}

static bool LButtonOnArrangeClip(HWND hwnd, AppState& state, POINT pt, const LayoutRects& layout) {
    if (!PtInRect(&layout.arrange, pt)) return false;
            if (PtInRect(&layout.arrange, pt)) {
                state.ui.view.selectedClipIndex = -1;
                for (int i = static_cast<int>(state.core.project.clips.size()) - 1; i >= 0; --i) {
                    RECT r{};
                    if (!UiLayoutClipRectForDraw(layout.arrange, state, state.core.project.clips[static_cast<size_t>(i)], &r)) {
                        continue;
                    }
                    if (PtInRect(&r, pt)) {
                        state.ui.view.selectedClipIndex = i;
                        state.ui.view.selectedTrackIndex = state.core.project.clips[static_cast<size_t>(i)].trackIndex;

                        constexpr int kEdgeThresh = 7;
                        const int fullLeft  = UiLayoutBeatToX(layout.arrange, state, state.core.project.clips[static_cast<size_t>(i)].startBeat);
                        const int fullRight = UiLayoutBeatToX(layout.arrange, state, state.core.project.clips[static_cast<size_t>(i)].startBeat + state.core.project.clips[static_cast<size_t>(i)].lengthBeats);
                        const bool nearLeft  = (pt.x - fullLeft)  <= kEdgeThresh && (pt.x - fullLeft)  >= 0;
                        const bool nearRight = (fullRight - pt.x)  <= kEdgeThresh && (fullRight - pt.x) >= 0;

                        if (nearLeft || nearRight) {
                            // Trim
                            state.ui.tools.trimmingClip         = true;
                            state.ui.tools.trimClipIndex        = i;
                            state.ui.tools.trimIsLeft           = nearLeft;
                            state.ui.tools.trimOrigStart        = state.core.project.clips[static_cast<size_t>(i)].startBeat;
                            state.ui.tools.trimOrigLen          = state.core.project.clips[static_cast<size_t>(i)].lengthBeats;
                            state.ui.tools.trimOrigSourceOffset = state.core.project.clips[static_cast<size_t>(i)].sourceOffsetFrames;
                            PushUndo(state);
                            SetCapture(hwnd);
                        } else {
                            // Drag
                            PushUndo(state);
                            state.ui.tools.draggingClip = true;
                            state.ui.tools.dragClipIndex = i;
                            state.ui.tools.dragOffsetBeats = UiLayoutXToBeat(layout.arrange, state, pt.x) - state.core.project.clips[static_cast<size_t>(i)].startBeat;
                            SetCapture(hwnd);
                        }
                        break;
                    }
                }
                InvalidateRect(hwnd, nullptr, FALSE);
            }
    return true;
}


}  // namespace

LRESULT WndProcOnLButtonDown(HWND hwnd, LPARAM lParam, AppState& state) {
    const POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
    if (LButtonOnDockSplitterOrTab(hwnd, state, pt)) return 0;
    if (LButtonOnTopBarButtons(hwnd, state, pt))    return 0;

    RECT client{};
    GetClientRect(hwnd, &client);
    const LayoutRects layout = UiLayoutComputeHitTestLayout(hwnd, state);

    if (LButtonOnFxInspector(hwnd, state, pt, client))          return 0;
    if (LButtonOnRuler(hwnd, state, pt, layout))                return 0;
    if (LButtonOnTracksAndBusesPanel(hwnd, state, pt, layout))  return 0;
    LButtonOnArrangeClip(hwnd, state, pt, layout);
    return 0;
}