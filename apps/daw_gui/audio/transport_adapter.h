// transport_adapter.h
//
// Phase 12a: thin glue between user-input transport gestures (button clicks,
// keyboard shortcuts) and the pure FSM in libs/services. The adapter derives
// the current TransportState from AudioRuntimeState's flat bools, runs
// TransportNext, and dispatches the resulting TransportAction by calling the
// existing Start/Stop helpers in main.cpp. This keeps the legacy side-effecting
// orchestration in place while giving us a single chokepoint for transport
// rules (illegal transitions, punch-in, etc).
//
// Not yet wired: count-in completion, async failure events, shutdown/playback-
// finished callbacks (those bypass the FSM and force-stop directly).

#pragma once

#include <windows.h>

#include "services/transport_fsm.h"

struct AppState;
struct AudioRuntimeState;

namespace daw::app {

// Pure projection: AudioRuntimeState bools → FSM state.
// Order matters: countingIn > recording > playing (matches Recording-implies-
// playing invariant in the existing code).
daw::services::TransportState TransportStateFromAudio(const AudioRuntimeState& a);

// Predicate: would a record-press right now actually invoke a count-in
// preroll? Mirrors the condition inside DeviceStartRecordingBackend
// (recordPrerollFrames > 0 iff !wasPlaying && countInEnabled && countInBars > 0).
// Used to decide whether to dispatch RecordPressed vs RecordPressedWithCountIn.
bool WillCountIn(const AudioRuntimeState& a);

// Run the FSM on (currentState, ev) and execute the resulting action.
// `rewindOnStop` is forwarded to StopPlayback's rewind parameter when the
// FSM emits StopPlayback or StopRecording (latter chains a StopPlayback to
// preserve existing UX where stopping a take also stops transport).
// Returns true iff the FSM produced a non-None action.
bool DispatchTransportEvent(HWND hwnd,
                            AppState& state,
                            daw::services::TransportEvent ev,
                            bool rewindOnStop);

}  // namespace daw::app
