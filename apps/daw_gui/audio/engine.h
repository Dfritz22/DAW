#pragma once
#include "engine_utils.h"

// ── Audio engine – realtime and offline render entry points ──────────────────
// These functions contain the core mix/render logic.  They operate on UiState
// and must not depend on any Win32 UI, drawing, or layout code.

// Realtime path
bool FillRealtimeBufferLocked(UiState& state, std::int16_t* outInterleaved, int frames, bool* reachedEnd);
bool FillRealtimeForDeviceLocked(UiState& state, std::int16_t* outInterleaved, int deviceFrames, int deviceSampleRate, bool* reachedEnd);

// Offline render path (also declared in state.h for other consumers)
// bool RenderTrackToStereoLocked(...)    – see state.h
// bool RenderFullMixToStereoLocked(...)  – see state.h
// bool RenderBusStemToStereoLocked(...)  – see state.h
