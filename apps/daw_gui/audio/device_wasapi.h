#pragma once
#include "../state.h"

// ── WASAPI audio device backend ───────────────────────────────────────────────
// All IAudioClient / IAudioRenderClient / IAudioCaptureClient operations live
// here. No UI headers are included.

// Configures state (playbackFrameCursor, playing, etc.), starts the internal
// WasapiRenderThreadProc thread, and waits for device init.
// Returns true if WASAPI output is running; false if init failed (in which
// case state is reset so the MME fallback in StartPlayback can proceed).
bool StartWasapiAudio(HWND hwnd, UiState& state);

// Stops WASAPI render thread and clears playback backend flags.
void StopWasapiAudio(UiState& state);

// Starts WASAPI capture for recording; launches WasapiRecordThreadProc.
// Returns true if capture is running.  On failure, shows an error dialog and
// returns false so the MME recording path can be tried.
bool StartWasapiRecording(HWND hwnd, UiState& state, int armedTrack, bool wasPlaying);

// Stops WASAPI capture thread and clears capture backend flags.
void StopWasapiRecording(UiState& state);
