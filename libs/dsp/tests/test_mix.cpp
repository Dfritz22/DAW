#include <gtest/gtest.h>

#include "dsp/mix.h"

#include <cmath>
#include <cstdint>
#include <vector>

using daw::dsp::EqualPowerPan;
using daw::dsp::FloatToPcm16Clamped;

TEST(Mix, EqualPowerPanCentreIsHalfPower) {
    float l = 0.0f, r = 0.0f;
    EqualPowerPan(0.0f, &l, &r);
    EXPECT_NEAR(l, 0.70710678f, 1e-5f);
    EXPECT_NEAR(r, 0.70710678f, 1e-5f);
    // Constant-power: L^2 + R^2 == 1.
    EXPECT_NEAR(l * l + r * r, 1.0f, 1e-5f);
}

TEST(Mix, EqualPowerPanFullLeftAndRight) {
    float l = 0.0f, r = 0.0f;
    EqualPowerPan(-1.0f, &l, &r);
    EXPECT_NEAR(l, 1.0f, 1e-5f);
    EXPECT_NEAR(r, 0.0f, 1e-5f);

    EqualPowerPan(1.0f, &l, &r);
    EXPECT_NEAR(l, 0.0f, 1e-5f);
    EXPECT_NEAR(r, 1.0f, 1e-5f);
}

TEST(Mix, EqualPowerPanIsConstantPowerAcrossSweep) {
    for (float p = -1.0f; p <= 1.0f; p += 0.1f) {
        float l = 0.0f, r = 0.0f;
        EqualPowerPan(p, &l, &r);
        EXPECT_NEAR(l * l + r * r, 1.0f, 1e-4f) << "pan=" << p;
        EXPECT_GE(l, 0.0f);
        EXPECT_GE(r, 0.0f);
    }
}

TEST(Mix, EqualPowerPanClampsOutOfRange) {
    float l1 = 0.0f, r1 = 0.0f;
    float l2 = 0.0f, r2 = 0.0f;
    EqualPowerPan(-5.0f, &l1, &r1);
    EqualPowerPan(-1.0f, &l2, &r2);
    EXPECT_FLOAT_EQ(l1, l2);
    EXPECT_FLOAT_EQ(r1, r2);
}

TEST(Mix, FloatToPcm16ClampedExactConversion) {
    std::vector<float> src{0.0f, 1.0f, -1.0f, 0.5f};
    std::vector<std::int16_t> dst(src.size(), 0);
    FloatToPcm16Clamped(src.data(), dst.data(), static_cast<int>(src.size()));
    EXPECT_EQ(dst[0], 0);
    EXPECT_EQ(dst[1], 32767);
    EXPECT_EQ(dst[2], -32767);
    EXPECT_NEAR(static_cast<int>(dst[3]), 16384, 1);
}

TEST(Mix, FloatToPcm16ClampedClipsOverrange) {
    std::vector<float> src{2.5f, -3.0f};
    std::vector<std::int16_t> dst(src.size(), 0);
    FloatToPcm16Clamped(src.data(), dst.data(), static_cast<int>(src.size()));
    EXPECT_EQ(dst[0], 32767);
    EXPECT_EQ(dst[1], -32767);
}
