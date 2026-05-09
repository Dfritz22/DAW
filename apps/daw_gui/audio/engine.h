#pragma once

#ifdef DAW_PUBLIC_API
#error "This header is internal-only."
#endif

#include <cstdint>

struct CoreState;
struct AudioRuntimeState;

// ── Audio engine – realtime and offline render entry points ──────────────────
// These functions contain the core mix/render logic and operate on core/audio
// runtime state only. They must not depend on UI or Win32 UI APIs.

// Realtime path
bool EngineFillRealtimeBufferLocked(CoreState& core, AudioRuntimeState& audio, std::int16_t* outInterleaved, int frames, bool* reachedEnd);
bool EngineFillRealtimeForDeviceLocked(CoreState& core, AudioRuntimeState& audio, std::int16_t* outInterleaved, int deviceFrames, int deviceSampleRate, bool* reachedEnd);

// Offline render path (also declared in state.h for other consumers)
// bool RenderTrackToStereoLocked(...)    – see state.h
// bool RenderFullMixToStereoLocked(...)  – see state.h
// bool RenderBusStemToStereoLocked(...)  – see state.h
