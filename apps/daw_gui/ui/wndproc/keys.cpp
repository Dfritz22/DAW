#include "ui/wndproc/keys.h"

#include "ai/automix_bridge.h"
#include "audio/transport_adapter.h"
#include "core/internal_app_services.h"
#include "daw_project.h"
#include "vm/timeline_zoom.h"

LRESULT WndProcOnKeyDown(HWND hwnd, WPARAM wParam, AppState& state) {
    using daw::internal::core::UpdateWindowTitle;

    if (wParam == VK_SPACE) {
        // Toggle play. FSM handles "if recording, commit take first".
        using daw::services::TransportEvent;
        const auto ev = state.audio.playing ? TransportEvent::StopPressed
                                            : TransportEvent::PlayPressed;
        daw::app::DispatchTransportEvent(hwnd, state, ev, /*rewindOnStop=*/false);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    if (wParam == VK_HOME) {
        daw::app::DispatchTransportEvent(hwnd, state,
            daw::services::TransportEvent::StopPressed, /*rewindOnStop=*/true);
        state.ui.viewStartBeat = 0.0f;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    if (wParam == 'I') {
        ImportWavFiles(hwnd, state);
        state.core.projectModified = true;
        UpdateWindowTitle(hwnd, state.core);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    if (wParam == 'O' && (GetKeyState(VK_CONTROL) & 0x8000)) {
        DoOpen(hwnd, state);
        if (state.audio.trackInsertDspState.size() != state.core.project.tracks.size())
            state.audio.trackInsertDspState.resize(state.core.project.tracks.size());
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    if (wParam == 'S' && (GetKeyState(VK_CONTROL) & 0x8000)) {
        if (GetKeyState(VK_SHIFT) & 0x8000) {
            DoSaveAs(hwnd, state);
        } else {
            DoSave(hwnd, state);
        }
        return 0;
    }
    if (wParam == 'Z' && (GetKeyState(VK_CONTROL) & 0x8000)) {
        if (state.audio.playing || state.audio.recording) return 0;
        ApplyUndo(hwnd, state);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    if ((wParam == 'Y' && (GetKeyState(VK_CONTROL) & 0x8000)) ||
        (wParam == 'Z' && (GetKeyState(VK_CONTROL) & 0x8000) && (GetKeyState(VK_SHIFT) & 0x8000))) {
        if (state.audio.playing || state.audio.recording) return 0;
        ApplyRedo(hwnd, state);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    if (wParam == 'S' && !(GetKeyState(VK_CONTROL) & 0x8000)) {
        // Split selected clip at playhead
        if (state.audio.playing || state.audio.recording) return 0;
        SplitSelectedClip(state);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    if (wParam == 'D' && (GetKeyState(VK_CONTROL) & 0x8000)) {
        if (state.audio.playing || state.audio.recording) return 0;
        DuplicateSelectedClip(state);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    if (wParam == VK_LEFT && !(GetKeyState(VK_CONTROL) & 0x8000)) {
        if (state.audio.playing || state.audio.recording) return 0;
        NudgeSelectedClip(state, -0.25f);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    if (wParam == VK_RIGHT && !(GetKeyState(VK_CONTROL) & 0x8000)) {
        if (state.audio.playing || state.audio.recording) return 0;
        NudgeSelectedClip(state, 0.25f);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    if (wParam == 'A') {
        StartAutoMixAsync(hwnd, state);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    if (wParam == 'V') {
        AnalyzeSelectedTrackQuality(hwnd, state);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    if (wParam == 'R') {
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
        return 0;
    }
    if (wParam == VK_ESCAPE) {
        if (state.ui.fxInspectorOpen) {
            state.ui.fxInspectorOpen = false;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
    }
    if (wParam == VK_DELETE) {
        if (state.audio.recording || state.audio.playing) {
            MessageBoxW(hwnd, L"Stop playback/recording before deleting.", L"Delete", MB_OK | MB_ICONINFORMATION);
            return 0;
        }

        if (state.ui.selectedClipIndex >= 0 && state.ui.selectedClipIndex < static_cast<int>(state.core.project.clips.size())) {
            DeleteSelectedClip(state);
            state.core.projectModified = true;
            UpdateWindowTitle(hwnd, state.core);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        if (state.ui.selectedTrackIndex >= 0 && state.ui.selectedTrackIndex < static_cast<int>(state.core.project.tracks.size())) {
            DeleteTrackAt(state, state.ui.selectedTrackIndex);
            state.core.projectModified = true;
            UpdateWindowTitle(hwnd, state.core);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }
    if (wParam == VK_OEM_PLUS || wParam == VK_ADD) {
        state.ui.viewBeatsVisible = daw::vm::ZoomVisible(state.ui.viewBeatsVisible, daw::vm::kKeyZoomInFactor);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    if (wParam == VK_OEM_MINUS || wParam == VK_SUBTRACT) {
        state.ui.viewBeatsVisible = daw::vm::ZoomVisible(state.ui.viewBeatsVisible, daw::vm::kKeyZoomOutFactor);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    return 0;
}
