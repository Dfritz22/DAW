#include "AppState.h"

#include "audio/device_common.h"
#include "audio/device_mme.h"
#include "audio/device_wasapi.h"
#include "audio/engine.h"
#include "core/automation.h"
#include "core/internal_app_services.h"
#include "core/timeline.h"
#include "core/timeline_edit.h"

#include <filesystem>

using daw::internal::core::UpdateWindowTitle;

namespace daw::internal::core {

void UpdateWindowTitle(HWND hwnd, const CoreState& core) {
    std::wstring name = core.projectFilePath.empty()
        ? L"Untitled"
        : std::filesystem::path(core.projectFilePath).stem().wstring();
    std::wstring title = L"DAW  -  " + name + (core.projectModified ? L" *" : L"");
    SetWindowTextW(hwnd, title.c_str());
}

} // namespace daw::internal::core

bool EngineInit(AppState& state) {
    (void)state;
    return true;
}

void EngineShutdown(AppState& state) {
    (void)state;
}

float SamplesPerBeat(const AppState& app) {
    return SamplesPerBeat(app.core);
}

std::uint64_t FramesFromBeats(const AppState& app, float beat) {
    return FramesFromBeats(app.core, beat);
}

float BeatsFromFrames(const AppState& app, std::uint64_t frame) {
    return BeatsFromFrames(app.core, frame);
}

float TrackGainDbAt(const AppState& app, int trackIndex, float beat) {
    return TrackGainDbAt(app.core, trackIndex, beat);
}

float TrackPanAt(const AppState& app, int trackIndex, float beat) {
    return TrackPanAt(app.core, trackIndex, beat);
}

int TrackBusIndexAt(const AppState& app, int trackIndex, float beat) {
    return TrackBusIndexAt(app.core, trackIndex, beat);
}

float TrackGainDbAt(const AppState& app, int trackIndex) {
    return TrackGainDbAt(app.core, trackIndex);
}

float TrackPanAt(const AppState& app, int trackIndex) {
    return TrackPanAt(app.core, trackIndex);
}

int TrackBusIndexAt(const AppState& app, int trackIndex) {
    return TrackBusIndexAt(app.core, trackIndex);
}

void DeviceRefreshInputDevices(AppState& state) {
    DeviceRefreshInputDevices(state.audio);
}

void DeviceRefreshOutputDevices(AppState& state) {
    DeviceRefreshOutputDevices(state.audio);
}

std::wstring DeviceBuildAudioDiagnosticsReport(const AppState& state) {
    return DeviceBuildAudioDiagnosticsReport(state.core, state.audio);
}

std::uint64_t DeviceGetRenderedPlaybackFrame(const AppState& state) {
    return DeviceGetRenderedPlaybackFrame(state.core, state.audio);
}

bool DeviceStartPlaybackBackend(HWND hwnd, AppState& state) {
    if (IsWasapiBackend(state.audio.audioBackend)) {
        if (DeviceStartWasapiAudio(hwnd, state.core, state.audio, state.ui.playheadBeat)) {
            return true;
        }
    }
    return DeviceStartPlaybackBackend(hwnd, state.core, state.audio, state.ui.playheadBeat);
}

void DeviceStopPlaybackBackend(AppState& state) {
    if (state.audio.playingViaWasapi) {
        DeviceStopWasapiAudio(state.audio);
        return;
    }
    DeviceStopPlaybackBackend(state.audio);
}

bool DeviceStartRecordingBackend(HWND hwnd, AppState& state, int armedTrack, bool wasPlaying) {
    if (IsWasapiBackend(state.audio.audioBackend)) {
        if (DeviceStartWasapiRecording(hwnd, state.core, state.audio, armedTrack, wasPlaying, state.ui.playheadBeat)) {
            return true;
        }
    }
    return DeviceStartRecordingBackend(hwnd, state.core, state.audio, armedTrack, wasPlaying, state.ui.playheadBeat);
}

void DeviceStopRecordingBackend(AppState& state) {
    if (state.audio.recordUsingWasapi) {
        DeviceStopWasapiRecording(state.audio);
        return;
    }
    DeviceStopRecordingBackend(state.audio);
}

void PushUndo(AppState& state) {
    PushUndo(state.core);
}

void ApplyUndo(HWND hwnd, AppState& state) {
    if (!ApplyUndo(state.core)) {
        return;
    }
    state.ui.selectedClipIndex = -1;
    UpdateWindowTitle(hwnd, state.core);
}

void ApplyRedo(HWND hwnd, AppState& state) {
    if (!ApplyRedo(state.core)) {
        return;
    }
    state.ui.selectedClipIndex = -1;
    UpdateWindowTitle(hwnd, state.core);
}

void SplitSelectedClip(AppState& state) {
    if (SplitSelectedClip(state.core, state.ui.selectedClipIndex, state.ui.playheadBeat)) {
        state.ui.selectedClipIndex = -1;
    }
}

void DuplicateSelectedClip(AppState& state) {
    const int newSelected = DuplicateSelectedClip(state.core, state.ui.selectedClipIndex);
    if (newSelected >= 0) {
        state.ui.selectedClipIndex = newSelected;
    }
}

void NudgeSelectedClip(AppState& state, float deltaBeats) {
    NudgeSelectedClip(state.core, state.ui.selectedClipIndex, deltaBeats);
}

void DeleteSelectedClip(AppState& state) {
    if (DeleteSelectedClip(state.core, state.ui.selectedClipIndex)) {
        state.ui.selectedClipIndex = -1;
    }
}

int AddNewTrack(AppState& state) {
    return AddNewTrack(state.core);
}

void DeleteTrackAt(AppState& state, int trackIndex) {
    if (trackIndex < 0 || trackIndex >= static_cast<int>(state.core.project.tracks.size())) {
        return;
    }

    EnterCriticalSection(&state.audio.audioStateLock);
    const bool removed = DeleteTrackAt(state.core, trackIndex);
    LeaveCriticalSection(&state.audio.audioStateLock);
    if (!removed) {
        return;
    }

    if (state.ui.selectedClipIndex >= static_cast<int>(state.core.project.clips.size())) {
        state.ui.selectedClipIndex = -1;
    }
    if (state.ui.selectedTrackIndex >= static_cast<int>(state.core.project.tracks.size())) {
        state.ui.selectedTrackIndex = static_cast<int>(state.core.project.tracks.size()) - 1;
    }
    if (state.ui.dragFaderTrack == trackIndex) {
        state.ui.dragFaderTrack = -1;
        state.ui.draggingFader = false;
    } else if (state.ui.dragFaderTrack > trackIndex) {
        state.ui.dragFaderTrack -= 1;
    }
    if (!state.ui.dragPanIsBus && state.ui.dragPanIndex == trackIndex) {
        state.ui.dragPanIndex = -1;
        state.ui.draggingPan = false;
    } else if (!state.ui.dragPanIsBus && state.ui.dragPanIndex > trackIndex) {
        state.ui.dragPanIndex -= 1;
    }
}
