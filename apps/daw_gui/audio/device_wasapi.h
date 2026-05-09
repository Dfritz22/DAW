#pragma once
#ifdef DAW_PUBLIC_API
#error "This header is internal-only."
#endif

#include "core/state.h"

// ── WASAPI audio device backend ───────────────────────────────────────────────
// All IAudioClient / IAudioRenderClient / IAudioCaptureClient operations live
// here. No UI headers are included.

// Configures state (playbackFrameCursor, playing, etc.), starts the internal
// WasapiRenderThreadProc thread, and waits for device init.
// Returns true if WASAPI output is running; false if init failed (in which
// case state is reset so the MME fallback in StartPlayback can proceed).
bool DeviceStartWasapiAudio(HWND hwnd, UiState& state);

// Stops WASAPI render thread and clears playback backend flags.
void DeviceStopWasapiAudio(UiState& state);

// Starts WASAPI capture for recording; launches WasapiRecordThreadProc.
// Returns true if capture is running.  On failure, shows an error dialog and
// returns false so the MME recording path can be tried.
bool DeviceStartWasapiRecording(HWND hwnd, UiState& state, int armedTrack, bool wasPlaying);

// Stops WASAPI capture thread and clears capture backend flags.
void DeviceStopWasapiRecording(UiState& state);
