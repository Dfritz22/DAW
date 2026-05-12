// transport_adapter.cpp — see transport_adapter.h for design notes.

#include "audio/transport_adapter.h"

#include "AppState.h"
#include "daw_audio.h"         // Start/Stop Playback/Recording

namespace daw::app {

daw::services::TransportState TransportStateFromAudio(const AudioRuntimeState& a) {
    using daw::services::TransportState;
    if (a.countingIn) return TransportState::CountingIn;
    if (a.recording)  return TransportState::Recording;
    if (a.playing)    return TransportState::Playing;
    return TransportState::Stopped;
}

bool DispatchTransportEvent(HWND hwnd,
                            AppState& state,
                            daw::services::TransportEvent ev,
                            bool rewindOnStop) {
    using daw::services::TransportAction;

    const auto from = TransportStateFromAudio(state.audio);
    const auto t    = daw::services::TransportNext(from, ev);

    switch (t.action) {
        case TransportAction::None:
            return false;

        case TransportAction::StartPlayback:
            StartPlayback(hwnd, state);
            return true;

        case TransportAction::StopPlayback:
            StopPlayback(state, rewindOnStop);
            return true;

        case TransportAction::StartRecording:
            StartRecording(hwnd, state);
            return true;

        case TransportAction::StopRecording:
            // Match the historical user-input chain: commit the take, then
            // stop the transport. Rewind decision is the caller's (Stop button
            // / Record button rewind; Space does not).
            StopRecording(state, true);
            StopPlayback(state, rewindOnStop);
            return true;

        case TransportAction::StartCountIn:
            // Not wired yet — no UI gesture currently emits
            // RecordPressedWithCountIn. Reserved for a follow-up phase.
            return true;
    }
    return false;
}

}  // namespace daw::app
