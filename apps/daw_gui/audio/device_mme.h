#pragma once
#include "core/state.h"

// ── MME audio device backend ─────────────────────────────────────────────────
// All waveIn / waveOut operations live here. No UI headers are included.
// Device enumeration, diagnostics and playback cursor are in device_common.h.

// ── MME output ────────────────────────────────────────────────────────────────
// Opens waveOut, prepares headers, starts the internal AudioThreadProc thread.
// On success returns true and state.audioThread / state.waveOut are set.
// On failure returns false; state.waveOut stays nullptr.
bool StartMmeAudio(HWND hwnd, UiState& state);

// Stops and closes waveOut; clears state.waveHeaders / waveData / waveOut.
void StopMmeAudio(UiState& state);

// ── MME input ─────────────────────────────────────────────────────────────────
// Opens waveIn, prepares headers, starts the internal RecordThreadProc thread.
// armedTrack and wasPlaying are provided by the orchestration layer in main.cpp.
bool StartMmeRecording(HWND hwnd, UiState& state, int armedTrack, bool wasPlaying);

// Stops and closes waveIn. Does NOT commit captured audio (caller handles that).
void StopMmeRecording(UiState& state);
