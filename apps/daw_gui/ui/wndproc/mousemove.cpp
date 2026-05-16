#include "ui/wndproc/mousemove.h"

#include "daw_timeline.h"      // SamplesPerBeat
#include "ui/dock_drop.h"      // ResolveDropTarget, kDragTabThresholdPx
#include "ui/dpi.h"
#include "ui/layout.h"
#include "ui/UiRuntimeState.h" // kFaderMin/MaxDb, kTopBarHeight, kStatusBarHeight

#include <windowsx.h>
#include <algorithm>
#include "ui/repaint.h"

LRESULT WndProcOnMouseMove(HWND hwnd, LPARAM lParam, AppState& state) {
    // ── Dock tab drag update ────────────────────────────────────────
    // Promotes an armed tab click into an active drag once the cursor
    // moves past the threshold, then resolves a drop target every move.
    if (state.ui.dock.dragTabArmed) {
        const POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        state.ui.dock.dragTabCurPt = pt;
        if (!state.ui.dock.dragTabActive) {
            const int dx = pt.x - state.ui.dock.dragTabStartPt.x;
            const int dy = pt.y - state.ui.dock.dragTabStartPt.y;
            if (dx * dx + dy * dy >= Dpi(kDragTabThresholdPx) * Dpi(kDragTabThresholdPx)) {
                state.ui.dock.dragTabActive = true;
            }
        }
        if (state.ui.dock.dragTabActive) {
            ResolveDropTarget(state, pt);
            daw::ui::RequestRepaintAll(state);
        }
        return 0;
    }

    // ── Dock splitter drag update ───────────────────────────────────
    if (state.ui.dock.draggingSplitter && state.ui.dock.dragSplitterNode != nullptr) {
        RECT client{};
        GetClientRect(hwnd, &client);
        const RECT bodyRect{client.left, client.top + Dpi(kTopBarHeight), client.right, client.bottom - Dpi(kStatusBarHeight)};
        for (const auto& sp : state.ui.dock.dockSplitters) {
            if (sp.node != state.ui.dock.dragSplitterNode) continue;
            if (sp.horizontal) {
                int parentTop = bodyRect.top, parentBot = bodyRect.bottom;
                for (const auto& leaf : state.ui.dock.dockLayout) {
                    if (leaf.rect.left == sp.rect.left && leaf.rect.right == sp.rect.right) {
                        parentTop = std::min<LONG>(parentTop, leaf.rect.top);
                        parentBot = std::max<LONG>(parentBot, leaf.rect.bottom);
                    }
                }
                int y = std::clamp(static_cast<int>(GET_Y_LPARAM(lParam)), parentTop + Dpi(20), parentBot - Dpi(20));
                const float ratio = static_cast<float>(y - parentTop) / static_cast<float>(std::max(1, parentBot - parentTop));
                state.ui.dock.dragSplitterNode->ratio = std::clamp(ratio, 0.05f, 0.95f);
            } else {
                int parentLeft = bodyRect.left, parentRight = bodyRect.right;
                for (const auto& leaf : state.ui.dock.dockLayout) {
                    if (leaf.rect.top == sp.rect.top && leaf.rect.bottom == sp.rect.bottom) {
                        parentLeft  = std::min<LONG>(parentLeft,  leaf.rect.left);
                        parentRight = std::max<LONG>(parentRight, leaf.rect.right);
                    }
                }
                int x = std::clamp(static_cast<int>(GET_X_LPARAM(lParam)), parentLeft + Dpi(40), parentRight - Dpi(40));
                const float ratio = static_cast<float>(x - parentLeft) / static_cast<float>(std::max(1, parentRight - parentLeft));
                state.ui.dock.dragSplitterNode->ratio = std::clamp(ratio, 0.05f, 0.95f);
            }
            break;
        }
        daw::ui::RequestRepaintAll(state);
        return 0;
    }

    if (state.ui.tools.draggingPlayhead) {
        RECT client{};
        GetClientRect(hwnd, &client);
        const LayoutRects layout = UiLayoutComputeHitTestLayout(hwnd, state);
        const float beat = std::max(0.0f, UiLayoutXToBeat(layout.ruler, state, GET_X_LPARAM(lParam)));
        state.ui.view.playheadBeat = UiLayoutSnapBeat(beat);
        daw::ui::RequestRepaintAll(state);
        return 0;
    }

    if (state.ui.tools.trimmingClip && state.ui.tools.trimClipIndex >= 0 &&
        state.ui.tools.trimClipIndex < static_cast<int>(state.core.project.clips.size())) {
        RECT client{};
        GetClientRect(hwnd, &client);
        const LayoutRects layout = UiLayoutComputeHitTestLayout(hwnd, state);
        const float mouseBeat = UiLayoutSnapBeat(std::max(0.0f, UiLayoutXToBeat(layout.arrange, state, GET_X_LPARAM(lParam))));
        ClipItem& clip = state.core.project.clips[static_cast<size_t>(state.ui.tools.trimClipIndex)];
        if (state.ui.tools.trimIsLeft) {
            const float newStart = std::min(mouseBeat, state.ui.tools.trimOrigStart + state.ui.tools.trimOrigLen - 0.25f);
            const float delta = newStart - state.ui.tools.trimOrigStart;
            clip.startBeat   = std::max(0.0f, state.ui.tools.trimOrigStart + delta);
            clip.lengthBeats = std::max(0.25f, state.ui.tools.trimOrigLen   - delta);
            const float spb = SamplesPerBeat(state);
            const std::int64_t offsetDelta = static_cast<std::int64_t>(delta * spb);
            const std::int64_t newOff = static_cast<std::int64_t>(state.ui.tools.trimOrigSourceOffset) + offsetDelta;
            clip.sourceOffsetFrames = static_cast<std::uint64_t>(std::max<std::int64_t>(0, newOff));
        } else {
            const float newEnd = std::max(mouseBeat, state.ui.tools.trimOrigStart + 0.25f);
            clip.lengthBeats = newEnd - clip.startBeat;
        }
        daw::ui::RequestRepaintAll(state);
        return 0;
    }

    if (state.ui.tools.draggingFader && state.ui.tools.dragFaderTrack >= 0) {
        RECT client{};
        GetClientRect(hwnd, &client);
        const LayoutRects layout = UiLayoutComputeHitTestLayout(hwnd, state);
        RECT rail{};
        RECT knob{};
        UiLayoutGetTrackFaderRects(layout.leftPanel, state.ui.tools.dragFaderTrack, &rail, &knob, state.ui.view.tracksScrollY);
        const int mouseY = GET_Y_LPARAM(lParam);
        const bool shiftFine = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        float newDb;
        if (shiftFine) {
            // Fine mode: 1px = 0.05 dB (drag relative from start point)
            const int dy = mouseY - state.ui.tools.dragFaderStartY;
            newDb = std::clamp(state.ui.tools.dragFaderStartDb - dy * 0.05f, kFaderMinDb, kFaderMaxDb);
        } else {
            newDb = UiLayoutGainFromFaderY(rail, mouseY);
        }
        EnterCriticalSection(&state.audio.audioStateLock);
        state.core.project.tracks[static_cast<size_t>(state.ui.tools.dragFaderTrack)].gainDb = newDb;
        LeaveCriticalSection(&state.audio.audioStateLock);
        daw::ui::RequestRepaintAll(state);
        return 0;
    }

    if (state.ui.tools.draggingPan && state.ui.tools.dragPanIndex >= 0) {
        const int mouseY = GET_Y_LPARAM(lParam);
        const bool shiftFine = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        // Drag up = more right (+), drag down = more left (-).
        // Normal: full range (-1..+1) over ~200px. Fine (Shift): 10x slower.
        const int dy = mouseY - state.ui.tools.dragPanStartY;
        const float sensitivity = shiftFine ? 0.001f : 0.01f;
        const float newPan = std::clamp(state.ui.tools.dragPanStartVal - dy * sensitivity, -1.0f, 1.0f);
        EnterCriticalSection(&state.audio.audioStateLock);
        if (state.ui.tools.dragPanIsBus) {
            if (state.ui.tools.dragPanIndex < static_cast<int>(state.core.project.buses.size()))
                state.core.project.buses[static_cast<size_t>(state.ui.tools.dragPanIndex)].pan = newPan;
        } else {
            if (state.ui.tools.dragPanIndex < static_cast<int>(state.core.project.tracks.size()))
                state.core.project.tracks[static_cast<size_t>(state.ui.tools.dragPanIndex)].pan = newPan;
        }
        LeaveCriticalSection(&state.audio.audioStateLock);
        daw::ui::RequestRepaintAll(state);
        return 0;
    }

    if (state.ui.tools.draggingParamKnob && state.ui.tools.paramKnobParamId >= 0) {
        const int dy = GET_Y_LPARAM(lParam) - state.ui.tools.paramKnobDragStartY;
        const bool shiftFine = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        // paramKnobParamId = paramId * 100 + slotIndex
        const int paramId = state.ui.tools.paramKnobParamId / 100;
        const int slotIdx = state.ui.tools.paramKnobParamId % 100;
        const int inspIdx = state.ui.inspector.fxInspectorIndex;
        InsertConfigArray* pPA = nullptr;
        if (state.ui.inspector.fxInspectorIsTrack) {
            if (inspIdx >= 0 && inspIdx < static_cast<int>(state.core.project.tracks.size()))
                pPA = &state.core.project.tracks[static_cast<size_t>(inspIdx)].insertConfig;
        } else {
            if (inspIdx >= 0 && inspIdx < static_cast<int>(state.core.project.buses.size()))
                pPA = &state.core.project.buses[static_cast<size_t>(inspIdx)].insertConfig;
        }
        if (pPA && slotIdx >= 0 && slotIdx < kMaxInsertSlots) {
            InsertConfig& P = (*pPA)[static_cast<size_t>(slotIdx)];
            auto applyDrag = [&](float lo, float hi) -> float {
                const float range = hi - lo;
                const float sens = shiftFine ? 0.001f : 0.01f;
                return std::clamp(state.ui.tools.paramKnobDragStartVal - dy * range * sens, lo, hi);
            };
            EnterCriticalSection(&state.audio.audioStateLock);
            switch (paramId) {
            case 0: P.eq[0].freq_hz  = applyDrag(20.0f, 20000.0f); break;
            case 1: P.eq[0].gain_db  = applyDrag(-18.0f, 18.0f);   break;
            case 2: P.eq[1].freq_hz  = applyDrag(20.0f, 20000.0f); break;
            case 3: P.eq[1].gain_db  = applyDrag(-18.0f, 18.0f);   break;
            case 10: P.cmp_threshold_db = applyDrag(-60.0f, 0.0f);  break;
            case 11: P.cmp_ratio        = applyDrag(1.0f, 20.0f);   break;
            case 12: P.cmp_attack_ms    = applyDrag(0.1f, 200.0f);  break;
            case 13: P.cmp_makeup_db    = applyDrag(0.0f, 24.0f);   break;
            case 20: P.sat_drive = applyDrag(0.0f, 1.0f); break;
            case 21: P.sat_mix   = applyDrag(0.0f, 1.0f); break;
            case 30: P.dly_time_ms   = applyDrag(10.0f, 2000.0f);  break;
            case 31: P.dly_feedback  = applyDrag(0.0f, 0.95f);     break;
            case 32: P.dly_mix       = applyDrag(0.0f, 1.0f);      break;
            case 40: P.rev_room_size = applyDrag(0.0f, 1.0f); break;
            case 41: P.rev_damping   = applyDrag(0.0f, 1.0f); break;
            case 42: P.rev_mix       = applyDrag(0.0f, 1.0f); break;
            case 50: P.gate_threshold_db = applyDrag(-80.0f, 0.0f);   break;
            case 51: P.gate_attack_ms    = applyDrag(0.1f, 200.0f);   break;
            case 52: P.gate_release_ms   = applyDrag(10.0f, 500.0f);  break;
            case 60: P.dee_threshold_db  = applyDrag(-40.0f, 0.0f);   break;
            case 61: P.dee_freq_hz       = applyDrag(2000.0f, 16000.0f); break;
            case 62: P.dee_bandwidth_hz  = applyDrag(200.0f, 8000.0f); break;
            case 63: P.dee_reduction_db  = applyDrag(0.0f, 24.0f);    break;
            case 70: P.lim_ceiling_db = applyDrag(-12.0f, 0.0f);  break;
            case 71: P.lim_release_ms = applyDrag(1.0f, 500.0f);  break;
            }
            LeaveCriticalSection(&state.audio.audioStateLock);
            state.core.projectModified = true;
            daw::ui::RequestRepaintAll(state);
        }
        return 0;
    }

    if (!state.ui.tools.draggingClip || state.ui.tools.dragClipIndex < 0) {
        return 0;
    }

    {
        RECT client{};
        GetClientRect(hwnd, &client);
        const LayoutRects layout = UiLayoutComputeHitTestLayout(hwnd, state);

        const int mouseX = GET_X_LPARAM(lParam);
        const int mouseY = GET_Y_LPARAM(lParam);
        float newStart = UiLayoutXToBeat(layout.arrange, state, mouseX) - state.ui.tools.dragOffsetBeats;
        newStart = std::max(0.0f, UiLayoutSnapBeat(newStart));

        EnterCriticalSection(&state.audio.audioStateLock);
        ClipItem& clip = state.core.project.clips[static_cast<size_t>(state.ui.tools.dragClipIndex)];
        clip.startBeat = newStart;
        clip.trackIndex = UiLayoutTrackIndexFromY(layout.arrange, state, mouseY);
        LeaveCriticalSection(&state.audio.audioStateLock);

        daw::ui::RequestRepaintAll(state);
    }
    return 0;
}
