#pragma once

#include "../state.h"

// ── Shared timeline math helpers ─────────────────────────────────────────────
// These helpers are backend/UI agnostic and safe to use from orchestration,
// draw, and audio modules.

float SamplesPerBeat(const UiState& state);
std::uint64_t FramesFromBeats(const UiState& state, float beat);
float BeatsFromFrames(const UiState& state, std::uint64_t frame);

float TrackGainDbAt(const UiState& state, int trackIndex, float beat);
int   TrackBusIndexAt(const UiState& state, int trackIndex, float beat);
float TrackPanAt(const UiState& state, int trackIndex, float beat);

// Backward-compatible convenience overloads for call sites that do not
// provide a beat yet. Behavior remains unchanged (constant track values).
inline float TrackGainDbAt(const UiState& state, int trackIndex) {
	return TrackGainDbAt(state, trackIndex, 0.0f);
}

inline int TrackBusIndexAt(const UiState& state, int trackIndex) {
	return TrackBusIndexAt(state, trackIndex, 0.0f);
}

inline float TrackPanAt(const UiState& state, int trackIndex) {
	return TrackPanAt(state, trackIndex, 0.0f);
}
