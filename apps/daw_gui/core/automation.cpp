#include "core/CoreState.h"
#include "core/timeline.h"
#include <algorithm>
#include <cmath>

namespace {

static float EvaluateCurveAtBeat(const TrackAutomationCurve& curve, float beat, float defaultValue) {
    if (curve.points.empty()) {
        return defaultValue;
    }

    if (curve.points.size() == 1) {
        return curve.points[0].value;
    }

    auto cmpBeat = [](const TrackAutomationPoint& p, float b) { return p.beat < b; };
    const auto it = std::lower_bound(curve.points.begin(), curve.points.end(), beat, cmpBeat);

    if (it == curve.points.begin()) {
        return it->value;
    }
    if (it == curve.points.end()) {
        return curve.points.back().value;
    }

    const TrackAutomationPoint& b = *it;
    const TrackAutomationPoint& a = *(it - 1);

    if (curve.interpolation == AutomationInterpolationMode::Step || b.beat <= a.beat) {
        return a.value;
    }

    const float t = std::clamp((beat - a.beat) / (b.beat - a.beat), 0.0f, 1.0f);
    return a.value + (b.value - a.value) * t;
}

static int EvaluateBusCurveAtBeat(const TrackAutomationCurve& curve, float beat, int defaultValue) {
    const float raw = EvaluateCurveAtBeat(curve, beat, static_cast<float>(defaultValue));
    return static_cast<int>(std::lround(raw));
}

} // namespace

float AutomationTrackGainDbAt(const CoreState& state, int trackIndex, float beat) {
    if (trackIndex < 0 || trackIndex >= static_cast<int>(state.project.tracks.size())) {
        return 0.0f;
    }

    const float defaultValue = state.project.tracks[static_cast<size_t>(trackIndex)].gainDb;
    if (trackIndex >= static_cast<int>(state.trackGainCurves.size())) {
        return defaultValue;
    }

    return EvaluateCurveAtBeat(state.trackGainCurves[static_cast<size_t>(trackIndex)], beat, defaultValue);
}

float AutomationTrackPanAt(const CoreState& state, int trackIndex, float beat) {
    if (trackIndex < 0 || trackIndex >= static_cast<int>(state.project.tracks.size())) {
        return 0.0f;
    }

    const float defaultValue = std::clamp(state.project.tracks[static_cast<size_t>(trackIndex)].pan, -1.0f, 1.0f);
    if (trackIndex >= static_cast<int>(state.trackPanCurves.size())) {
        return defaultValue;
    }

    return std::clamp(
        EvaluateCurveAtBeat(state.trackPanCurves[static_cast<size_t>(trackIndex)], beat, defaultValue),
        -1.0f,
        1.0f);
}

int AutomationTrackBusIndexAt(const CoreState& state, int trackIndex, float beat) {
    if (trackIndex < 0 || trackIndex >= static_cast<int>(state.project.tracks.size())) {
        return 1;  // default to Music bus
    }

    const int defaultValue = std::clamp(state.project.tracks[static_cast<size_t>(trackIndex)].busIndex, 0, kBusCount - 1);
    if (trackIndex >= static_cast<int>(state.trackBusCurves.size())) {
        return defaultValue;
    }

    return std::clamp(
        EvaluateBusCurveAtBeat(state.trackBusCurves[static_cast<size_t>(trackIndex)], beat, defaultValue),
        0,
        kBusCount - 1);
}

float TrackGainDbAt(const CoreState& state, int trackIndex, float beat) {
    return AutomationTrackGainDbAt(state, trackIndex, beat);
}

float TrackPanAt(const CoreState& state, int trackIndex, float beat) {
    return AutomationTrackPanAt(state, trackIndex, beat);
}

int TrackBusIndexAt(const CoreState& state, int trackIndex, float beat) {
    return AutomationTrackBusIndexAt(state, trackIndex, beat);
}

float TrackGainDbAt(const CoreState& state, int trackIndex) {
    return AutomationTrackGainDbAt(state, trackIndex, 0.0f);
}

float TrackPanAt(const CoreState& state, int trackIndex) {
    return AutomationTrackPanAt(state, trackIndex, 0.0f);
}

int TrackBusIndexAt(const CoreState& state, int trackIndex) {
    return AutomationTrackBusIndexAt(state, trackIndex, 0.0f);
}
