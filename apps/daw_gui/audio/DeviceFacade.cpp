#include "audio/DeviceFacade.h"

#include "audio/device_common.h"
#include "audio/device_wasapi.h"

namespace daw::audio {

bool StartPlaybackBackend(AppState& state) {
    if (IsWasapiBackend(state.audio.audioBackend)) {
        if (DeviceStartWasapiAudio(state.ui.view.hwnd, state.core, state.audio, state.ui.view.playheadBeat)) {
            return true;
        }
    }
    return DeviceStartPlaybackBackend(state.ui.view.hwnd, state.core, state.audio, state.ui.view.playheadBeat);
}

void StopPlaybackBackend(AppState& state) {
    if (state.audio.playingViaWasapi) {
        DeviceStopWasapiAudio(state.audio);
        return;
    }
    DeviceStopPlaybackBackend(state.audio);
}

bool StartRecordingBackend(AppState& state, int armedTrack, bool wasPlaying) {
    if (IsWasapiBackend(state.audio.audioBackend)) {
        if (DeviceStartWasapiRecording(state.ui.view.hwnd, state.core, state.audio, armedTrack, wasPlaying, state.ui.view.playheadBeat)) {
            return true;
        }
    }
    return DeviceStartRecordingBackend(state.ui.view.hwnd, state.core, state.audio, armedTrack, wasPlaying, state.ui.view.playheadBeat);
}

void StopRecordingBackend(AppState& state) {
    if (state.audio.recordUsingWasapi) {
        DeviceStopWasapiRecording(state.audio);
        return;
    }
    DeviceStopRecordingBackend(state.audio);
}

} // namespace daw::audio
