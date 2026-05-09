#pragma once

#ifdef DAW_PUBLIC_API
#error "This header is internal-only."
#endif

#include <cstdint>

struct CoreState;

// ── Shared timeline math helpers ─────────────────────────────────────────────
// These helpers are backend/UI agnostic and safe to use from orchestration,
// draw, and audio modules.

float TimelineSamplesPerBeat(const CoreState& state);
std::uint64_t TimelineFramesFromBeats(const CoreState& state, float beat);
float TimelineBeatsFromFrames(const CoreState& state, std::uint64_t frame);
