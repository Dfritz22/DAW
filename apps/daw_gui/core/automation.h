#pragma once

#include "../state.h"

// ── Track automation curve evaluation ────────────────────────────────────────
// Single authoritative source for gain/pan/bus value lookup at timeline beat.

float TrackGainDbAt(const UiState& state, int trackIndex, float beat);
float TrackPanAt(const UiState& state, int trackIndex, float beat);
int   TrackBusIndexAt(const UiState& state, int trackIndex, float beat);

// Convenience overloads for call sites that currently evaluate static values.
inline float TrackGainDbAt(const UiState& state, int trackIndex) {
    return TrackGainDbAt(state, trackIndex, 0.0f);
}

inline float TrackPanAt(const UiState& state, int trackIndex) {
    return TrackPanAt(state, trackIndex, 0.0f);
}

inline int TrackBusIndexAt(const UiState& state, int trackIndex) {
    return TrackBusIndexAt(state, trackIndex, 0.0f);
}
