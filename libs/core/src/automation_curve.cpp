#include "core/automation_curve.h"

#include <algorithm>

namespace daw::core {

float EvaluateCurveAtBeat(const AutomationCurve& curve, float beat, float defaultValue) {
    if (curve.points.empty()) {
        return defaultValue;
    }
    if (curve.points.size() == 1) {
        return curve.points[0].value;
    }

    auto cmpBeat = [](const AutomationPoint& p, float b) { return p.beat < b; };
    const auto it = std::lower_bound(curve.points.begin(), curve.points.end(), beat, cmpBeat);

    if (it == curve.points.begin()) {
        return it->value;
    }
    if (it == curve.points.end()) {
        return curve.points.back().value;
    }

    const AutomationPoint& b = *it;
    const AutomationPoint& a = *(it - 1);

    if (curve.interpolation == AutomationInterpolationMode::Step || b.beat <= a.beat) {
        return a.value;
    }

    const float t = std::clamp((beat - a.beat) / (b.beat - a.beat), 0.0f, 1.0f);
    return a.value + (b.value - a.value) * t;
}

} // namespace daw::core
