#pragma once

#include <cstdint>

struct UiState;

// ── Shared timeline math helpers ─────────────────────────────────────────────
// These helpers are backend/UI agnostic and safe to use from orchestration,
// draw, and audio modules.

float TimelineSamplesPerBeat(const UiState& state);
std::uint64_t TimelineFramesFromBeats(const UiState& state, float beat);
float TimelineBeatsFromFrames(const UiState& state, std::uint64_t frame);
