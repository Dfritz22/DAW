#pragma once

// ── Automation curve (frozen contract) ──────────────────────────────────────
// Pure value types and a pure evaluator for parameter-automation curves
// (track gain, pan, bus assignment, plugin parameters, etc.).
//
// A curve is a sorted list of (beat, value) breakpoints plus an
// interpolation mode. Sorting is the caller's responsibility — the
// evaluator assumes points are non-decreasing in `beat` and uses binary
// search. Out-of-range queries clamp to the first/last point's value.
//
// Layer rule: lives in libs/core. No AppState, no I/O, no Win32. The
// realtime audio thread evaluates curves per-block, so this code must not
// allocate or take locks.

#include <vector>

namespace daw::core {

enum class AutomationInterpolationMode {
    Step,
    Linear,
};

struct AutomationPoint {
    float beat  {0.0f};
    float value {0.0f};
};

struct AutomationCurve {
    std::vector<AutomationPoint> points;
    AutomationInterpolationMode  interpolation {AutomationInterpolationMode::Linear};
};

// Evaluate a curve at the given beat.
//   * Empty curve            → returns `defaultValue`.
//   * Single-point curve     → returns that point's value.
//   * Beat ≤ first point     → returns first point's value.
//   * Beat ≥ last point      → returns last point's value.
//   * Step interpolation     → returns the value of the segment's left point.
//   * Linear interpolation   → linearly blends between the two surrounding
//                              points (clamped to [0, 1] interpolation
//                              parameter to be defensive against
//                              out-of-order points).
//
// Pure, allocation-free, RT-safe.
float EvaluateCurveAtBeat(const AutomationCurve& curve, float beat, float defaultValue);

} // namespace daw::core
