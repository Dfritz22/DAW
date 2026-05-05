#pragma once

#include "../state.h"

// ── Shared timeline math helpers ─────────────────────────────────────────────
// These helpers are backend/UI agnostic and safe to use from orchestration,
// draw, and audio modules.

float TrackGainDbAt(const UiState& state, int trackIndex);
int   TrackBusIndexAt(const UiState& state, int trackIndex);
float TrackPanAt(const UiState& state, int trackIndex);
float SamplesPerBeat(const UiState& state);
