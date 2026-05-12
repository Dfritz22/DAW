#include <gtest/gtest.h>

#include "core/automation_curve.h"

#include <cmath>

using daw::core::AutomationCurve;
using daw::core::AutomationInterpolationMode;
using daw::core::AutomationPoint;
using daw::core::EvaluateCurveAtBeat;

TEST(AutomationCurve, EmptyReturnsDefault) {
    AutomationCurve c;
    EXPECT_FLOAT_EQ(EvaluateCurveAtBeat(c, 0.0f, -3.5f), -3.5f);
    EXPECT_FLOAT_EQ(EvaluateCurveAtBeat(c, 100.0f, 7.0f), 7.0f);
}

TEST(AutomationCurve, SinglePointReturnsItsValueRegardlessOfBeat) {
    AutomationCurve c;
    c.points.push_back({4.0f, 0.5f});
    EXPECT_FLOAT_EQ(EvaluateCurveAtBeat(c, 0.0f,  0.0f), 0.5f);
    EXPECT_FLOAT_EQ(EvaluateCurveAtBeat(c, 4.0f,  0.0f), 0.5f);
    EXPECT_FLOAT_EQ(EvaluateCurveAtBeat(c, 99.0f, 0.0f), 0.5f);
}

TEST(AutomationCurve, BeforeFirstClampsToFirst) {
    AutomationCurve c;
    c.points = {{2.0f, 0.25f}, {6.0f, 0.75f}};
    EXPECT_FLOAT_EQ(EvaluateCurveAtBeat(c, -1.0f, 0.0f), 0.25f);
    EXPECT_FLOAT_EQ(EvaluateCurveAtBeat(c,  0.0f, 0.0f), 0.25f);
    EXPECT_FLOAT_EQ(EvaluateCurveAtBeat(c,  2.0f, 0.0f), 0.25f);
}

TEST(AutomationCurve, AfterLastClampsToLast) {
    AutomationCurve c;
    c.points = {{2.0f, 0.25f}, {6.0f, 0.75f}};
    EXPECT_FLOAT_EQ(EvaluateCurveAtBeat(c,  6.0f, 0.0f), 0.75f);
    EXPECT_FLOAT_EQ(EvaluateCurveAtBeat(c, 10.0f, 0.0f), 0.75f);
}

TEST(AutomationCurve, LinearInterpolatesMidpoint) {
    AutomationCurve c;
    c.interpolation = AutomationInterpolationMode::Linear;
    c.points = {{0.0f, 0.0f}, {4.0f, 1.0f}};
    EXPECT_FLOAT_EQ(EvaluateCurveAtBeat(c, 2.0f, 0.0f), 0.5f);
    EXPECT_FLOAT_EQ(EvaluateCurveAtBeat(c, 1.0f, 0.0f), 0.25f);
    EXPECT_FLOAT_EQ(EvaluateCurveAtBeat(c, 3.0f, 0.0f), 0.75f);
}

TEST(AutomationCurve, StepHoldsLeftValue) {
    AutomationCurve c;
    c.interpolation = AutomationInterpolationMode::Step;
    c.points = {{0.0f, 0.0f}, {4.0f, 1.0f}, {8.0f, -1.0f}};
    EXPECT_FLOAT_EQ(EvaluateCurveAtBeat(c, 2.0f, 0.0f), 0.0f);
    // At exactly the right endpoint, lower_bound lands on that point so the
    // segment's *left* value is still returned. This matches the original
    // implementation's semantics — breakpoints are open on the right.
    EXPECT_FLOAT_EQ(EvaluateCurveAtBeat(c, 4.0f, 0.0f), 0.0f);
    EXPECT_FLOAT_EQ(EvaluateCurveAtBeat(c, 4.0001f, 0.0f), 1.0f);
    EXPECT_FLOAT_EQ(EvaluateCurveAtBeat(c, 6.0f, 0.0f), 1.0f);
    EXPECT_FLOAT_EQ(EvaluateCurveAtBeat(c, 7.999f, 0.0f), 1.0f);
}

TEST(AutomationCurve, ZeroWidthSegmentDoesNotDivideByZero) {
    AutomationCurve c;
    c.interpolation = AutomationInterpolationMode::Linear;
    c.points = {{0.0f, 0.0f}, {2.0f, 0.5f}, {2.0f, 1.0f}, {4.0f, 0.0f}};
    // At exactly the duplicated beat the lower_bound should land on the
    // duplicated pair; the evaluator must not divide by zero.
    const float v = EvaluateCurveAtBeat(c, 2.0f, 0.0f);
    EXPECT_TRUE(std::isfinite(v));
}
