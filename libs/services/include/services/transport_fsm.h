#pragma once

// ── Transport state machine ─────────────────────────────────────────────────
// Pure FSM that captures the legal transitions of the DAW's transport
// without touching AppState, Win32, or the audio engine.
//
// Real transport in apps/daw_gui/main.cpp couples four concerns into the
// same call sites: state mutation, device backend coordination, UI
// invariants (HWND for MessageBox), and recording-thread management. This
// FSM splits the *rules* (what state should we move to, what action does
// the orchestrator owe the engine) away from the *side-effects* (actually
// starting the device, raising error dialogs, etc.).
//
// Wiring is opt-in: a caller can drive the FSM, observe the suggested
// action, and dispatch the existing Start/StopPlayback / Start/StopRecording
// helpers. Failures in those helpers translate to a TransportEvent::Failed
// fed back to the FSM so it can return to Stopped.
//
// Realtime-safe: pure switch on POD enums, no allocations, no locks.

namespace daw::services {

enum class TransportState {
    Stopped,
    Playing,
    CountingIn,   // count-in click before recording starts
    Recording,
};

enum class TransportEvent {
    PlayPressed,
    StopPressed,
    RecordPressed,       // armed track + count-in disabled => goes straight to Recording
    RecordPressedWithCountIn,
    CountInComplete,
    Failed,              // device start / recording-thread spawn failed
};

enum class TransportAction {
    None,
    StartPlayback,
    StopPlayback,
    StartCountIn,
    StartRecording,      // begin actual capture (no count-in or after CountInComplete)
    StopRecording,
};

struct TransportTransition {
    TransportState  newState;
    TransportAction action;
};

// Returns the next state and the side-effect the orchestrator should perform.
// Illegal transitions (e.g. PlayPressed while Recording) are no-ops:
// {state, TransportAction::None}.
TransportTransition TransportNext(TransportState state, TransportEvent ev);

} // namespace daw::services
