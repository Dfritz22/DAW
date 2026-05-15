#include "audio/DeviceFacade.h"

#include "audio/device_common.h"
#include "audio/device_wasapi.h"

namespace daw::audio {

bool StartPlaybackBackend(AppState& state) {
    if (IsWasapiBackend(state.audio.audioBackend)) {
        if (DeviceStartWasapiAudio(state.hwnd, state.core, state.audio, state.ui.view.playheadBeat)) {
            return true;
        }
    }
    return DeviceStartPlaybackBackend(state.hwnd, state.core, state.audio, state.ui.view.playheadBeat);
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
        if (DeviceStartWasapiRecording(state.hwnd, state.core, state.audio, armedTrack, wasPlaying, state.ui.view.playheadBeat)) {
            return true;
        }
    }
    return DeviceStartRecordingBackend(state.hwnd, state.core, state.audio, armedTrack, wasPlaying, state.ui.view.playheadBeat);
}

void StopRecordingBackend(AppState& state) {
    if (state.audio.recordUsingWasapi) {
        DeviceStopWasapiRecording(state.audio);
        return;
    }
    DeviceStopRecordingBackend(state.audio);
}

} // namespace daw::audio
