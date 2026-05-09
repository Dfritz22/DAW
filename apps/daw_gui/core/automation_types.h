#pragma once

#include <vector>

enum class AutomationInterpolationMode {
    Step,
    Linear,
};

struct TrackAutomationPoint {
    float beat {0.0f};
    float value {0.0f};
};

struct TrackAutomationCurve {
    std::vector<TrackAutomationPoint> points;
    AutomationInterpolationMode interpolation {AutomationInterpolationMode::Linear};
};

using GainAutomation = TrackAutomationCurve;
using PanAutomation  = TrackAutomationCurve;
using BusAutomation  = TrackAutomationCurve;
