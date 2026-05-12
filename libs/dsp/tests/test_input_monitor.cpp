#include <gtest/gtest.h>

#include "dsp/mix.h"

#include <cstdint>
#include <vector>

using daw::dsp::MixPcm16InputToFloatStereo;

TEST(InputMonitor, StereoSourceMixesAdditively) {
    // 2 input frames, stereo → 4 int16 samples.
    std::vector<std::int16_t> in{16384, -16384, 16384, -16384};
    std::vector<float> dst(4, 0.5f);  // pre-fill to verify additive.
    MixPcm16InputToFloatStereo(in.data(), 2, 2, dst.data(), 2, 1.0f);
    // 16384/32768 = 0.5 added to existing 0.5 → 1.0
    EXPECT_NEAR(dst[0], 1.0f, 1e-4f);
    EXPECT_NEAR(dst[1], 0.0f, 1e-4f);  // -0.5 + 0.5
    EXPECT_NEAR(dst[2], 1.0f, 1e-4f);
    EXPECT_NEAR(dst[3], 0.0f, 1e-4f);
}

TEST(InputMonitor, MonoSourceDuplicatedToBothChannels) {
    std::vector<std::int16_t> in{16384, -16384};  // 2 mono frames
    std::vector<float> dst(4, 0.0f);
    MixPcm16InputToFloatStereo(in.data(), 2, 1, dst.data(), 2, 1.0f);
    EXPECT_NEAR(dst[0], 0.5f, 1e-4f);
    EXPECT_NEAR(dst[1], 0.5f, 1e-4f);
    EXPECT_NEAR(dst[2], -0.5f, 1e-4f);
    EXPECT_NEAR(dst[3], -0.5f, 1e-4f);
}

TEST(InputMonitor, GainScalesOutput) {
    std::vector<std::int16_t> in{16384};
    std::vector<float> dst(2, 0.0f);
    MixPcm16InputToFloatStereo(in.data(), 1, 1, dst.data(), 1, 2.0f);
    EXPECT_NEAR(dst[0], 1.0f, 1e-4f);
    EXPECT_NEAR(dst[1], 1.0f, 1e-4f);
}

TEST(InputMonitor, ZeroFramesIsNoOp) {
    std::vector<std::int16_t> in{16384};
    std::vector<float> dst(2, 0.25f);
    MixPcm16InputToFloatStereo(in.data(), 0, 1, dst.data(), 0, 1.0f);
    EXPECT_FLOAT_EQ(dst[0], 0.25f);
    EXPECT_FLOAT_EQ(dst[1], 0.25f);
}

TEST(InputMonitor, ConsumedFramesIsMin) {
    std::vector<std::int16_t> in{1, 2, 3, 4};  // 4 stereo frames available? no, 2 stereo frames.
    std::vector<float> dst(20, 0.0f);  // 10 dst frames requested
    const int consumed = MixPcm16InputToFloatStereo(in.data(), 2, 2, dst.data(), 10, 1.0f);
    EXPECT_EQ(consumed, 2);
}
