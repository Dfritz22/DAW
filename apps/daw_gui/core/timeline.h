#pragma once

#include "../state.h"

// ── Shared timeline math helpers ─────────────────────────────────────────────
// These helpers are backend/UI agnostic and safe to use from orchestration,
// draw, and audio modules.

float SamplesPerBeat(const UiState& state);
std::uint64_t FramesFromBeats(const UiState& state, float beat);
float BeatsFromFrames(const UiState& state, std::uint64_t frame);
