#include "audio/DeviceFacade.h"

#include "audio/device_common.h"
#include "audio/device_wasapi.h"

namespace daw::audio {

bool StartPlaybackBackend(AppState& state) {
    if (IsWasapiBackend(state.audio.audioBackend)) {
        if (DeviceStartWasapiAudio(state.ui.hwnd, state.core, state.audio, state.ui.playheadBeat)) {
            return true;
        }
    }
    return DeviceStartPlaybackBackend(state.ui.hwnd, state.core, state.audio, state.ui.playheadBeat);
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
        if (DeviceStartWasapiRecording(state.ui.hwnd, state.core, state.audio, armedTrack, wasPlaying, state.ui.playheadBeat)) {
            return true;
        }
    }
    return DeviceStartRecordingBackend(state.ui.hwnd, state.core, state.audio, armedTrack, wasPlaying, state.ui.playheadBeat);
}

void StopRecordingBackend(AppState& state) {
    if (state.audio.recordUsingWasapi) {
        DeviceStopWasapiRecording(state.audio);
        return;
    }
    DeviceStopRecordingBackend(state.audio);
}

} // namespace daw::audio
