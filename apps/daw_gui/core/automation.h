#pragma once

#ifdef DAW_PUBLIC_API
#error "This header is internal-only."
#endif

struct CoreState;

// ── Track automation curve evaluation ────────────────────────────────────────
// Single authoritative source for gain/pan/bus value lookup at timeline beat.

float AutomationTrackGainDbAt(const CoreState& state, int trackIndex, float beat);
float AutomationTrackPanAt(const CoreState& state, int trackIndex, float beat);
int   AutomationTrackBusIndexAt(const CoreState& state, int trackIndex, float beat);

// Convenience overloads for call sites that currently evaluate static values.
inline float AutomationTrackGainDbAt(const CoreState& state, int trackIndex) {
    return AutomationTrackGainDbAt(state, trackIndex, 0.0f);
}

inline float AutomationTrackPanAt(const CoreState& state, int trackIndex) {
    return AutomationTrackPanAt(state, trackIndex, 0.0f);
}

inline int AutomationTrackBusIndexAt(const CoreState& state, int trackIndex) {
    return AutomationTrackBusIndexAt(state, trackIndex, 0.0f);
}
