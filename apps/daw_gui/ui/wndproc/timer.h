#pragma once

#ifdef DAW_PUBLIC_API
#error "This header is internal-only."
#endif

#define NOMINMAX
#include <windows.h>

#include "AppState.h"

// WM_TIMER (kPlaybackTimerId only) — advances the playhead during playback,
// auto-scrolls the view to follow it, and updates the live recording
// waveform display when audio.recording is true. Returns 0 always; the
// caller checks `state` for nullptr first and verifies the timer id.
LRESULT WndProcOnPlaybackTimer(HWND hwnd, AppState& state);
