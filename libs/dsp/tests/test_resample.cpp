#include <gtest/gtest.h>

#include "dsp/resample.h"
#include "dsp/util.h"

#include <cmath>
#include <cstdint>
#include <vector>

using daw::dsp::DbToLinear;
using daw::dsp::ResampleStereoFloatLinear;
using daw::dsp::ResampleStereoPcm16Linear;
using daw::dsp::ResampleStereoFloatSincHQ;
using daw::dsp::ResampleStereoPcm16LinearStateful;

TEST(DspUtil, DbToLinearKnownPoints) {
    EXPECT_FLOAT_EQ(DbToLinear(0.0f), 1.0f);
    EXPECT_NEAR(DbToLinear(-6.0f), 0.5012f, 0.001f);
    EXPECT_NEAR(DbToLinear(6.0f), 1.9953f, 0.001f);
    EXPECT_NEAR(DbToLinear(-20.0f), 0.1f, 1e-5f);
}

TEST(Resample, FloatLinearIdentityWhenSameLength) {
    std::vector<float> src{1.0f, -1.0f, 0.5f, -0.5f, 0.25f, -0.25f, 0.0f, 0.0f};
    std::vector<float> dst(src.size(), 0.0f);
    ResampleStereoFloatLinear(src.data(), 4, dst.data(), 4);
    for (size_t i = 0; i < src.size(); ++i) EXPECT_FLOAT_EQ(dst[i], src[i]);
}

TEST(Resample, FloatLinearUpsamplingMidpointInterpolates) {
    std::vector<float> src{0.0f, 0.0f, 1.0f, -1.0f};
    std::vector<float> dst(6, 0.0f);  // 3 frames
    ResampleStereoFloatLinear(src.data(), 2, dst.data(), 3);
    EXPECT_FLOAT_EQ(dst[0], 0.0f);
    EXPECT_FLOAT_EQ(dst[1], 0.0f);
    EXPECT_NEAR(dst[2], 0.5f, 1e-5f);
    EXPECT_NEAR(dst[3], -0.5f, 1e-5f);
    EXPECT_FLOAT_EQ(dst[4], 1.0f);
    EXPECT_FLOAT_EQ(dst[5], -1.0f);
}

TEST(Resample, FloatLinearSingleFrameFillsConstant) {
    std::vector<float> src{0.7f, -0.3f};
    std::vector<float> dst(8, 0.0f);
    ResampleStereoFloatLinear(src.data(), 1, dst.data(), 4);
    for (int i = 0; i < 4; ++i) {
        EXPECT_FLOAT_EQ(dst[i * 2], 0.7f);
        EXPECT_FLOAT_EQ(dst[i * 2 + 1], -0.3f);
    }
}

TEST(Resample, Pcm16LinearClampsToInt16Range) {
    // Two-frame source; output clamps to int16 range during interpolation.
    std::vector<std::int16_t> src{32767, -32768, 32767, -32768};
    std::vector<std::int16_t> dst(8, 0);
    ResampleStereoPcm16Linear(src.data(), 2, dst.data(), 4);
    for (int i = 0; i < 4; ++i) {
        EXPECT_EQ(dst[i * 2], 32767);
        EXPECT_EQ(dst[i * 2 + 1], -32768);
    }
}

TEST(Resample, SincHQIdentityWhenRatesMatchAndLengthMatches) {
    std::vector<float> src{0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f};
    std::vector<float> dst(src.size(), 0.0f);
    ResampleStereoFloatSincHQ(src.data(), 3, 48000, dst.data(), 3, 48000);
    for (size_t i = 0; i < src.size(); ++i) EXPECT_FLOAT_EQ(dst[i], src[i]);
}

TEST(Resample, Pcm16StatefulConsumesSourceAtRate) {
    // step = srcSR/dstSR = 1.0 → identity-rate; should consume ~dstFrames+1 source frames.
    std::vector<std::int16_t> src(200, 0);
    for (int i = 0; i < 100; ++i) {
        src[i * 2]     = static_cast<std::int16_t>(i * 100);
        src[i * 2 + 1] = static_cast<std::int16_t>(-i * 100);
    }
    std::vector<std::int16_t> dst(200, 0);
    double phase = 0.0;
    std::int16_t lL = 0, lR = 0;
    bool primed = false;
    const int consumed = ResampleStereoPcm16LinearStateful(
        src.data(), 100, dst.data(), 100, 1.0, &phase, &lL, &lR, &primed);
    EXPECT_TRUE(primed);
    EXPECT_GE(consumed, 99);
    EXPECT_LE(consumed, 100);
}
