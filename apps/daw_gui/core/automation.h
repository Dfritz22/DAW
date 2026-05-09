#pragma once

#ifdef DAW_PUBLIC_API
#error "This header is internal-only."
#endif

struct UiState;

// ── Track automation curve evaluation ────────────────────────────────────────
// Single authoritative source for gain/pan/bus value lookup at timeline beat.

float AutomationTrackGainDbAt(const UiState& state, int trackIndex, float beat);
float AutomationTrackPanAt(const UiState& state, int trackIndex, float beat);
int   AutomationTrackBusIndexAt(const UiState& state, int trackIndex, float beat);

// Convenience overloads for call sites that currently evaluate static values.
inline float AutomationTrackGainDbAt(const UiState& state, int trackIndex) {
    return AutomationTrackGainDbAt(state, trackIndex, 0.0f);
}

inline float AutomationTrackPanAt(const UiState& state, int trackIndex) {
    return AutomationTrackPanAt(state, trackIndex, 0.0f);
}

inline int AutomationTrackBusIndexAt(const UiState& state, int trackIndex) {
    return AutomationTrackBusIndexAt(state, trackIndex, 0.0f);
}
