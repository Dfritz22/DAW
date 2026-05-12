#include <gtest/gtest.h>

#include "dsp/mix.h"

#include <vector>

using daw::dsp::ClampStereoBuffer;
using daw::dsp::MixAddStereoWithGain;

TEST(MixAdd, AdditiveSemantics) {
    std::vector<float> src{1.0f, 2.0f, 3.0f, 4.0f};
    std::vector<float> dst{0.5f, 0.5f, 0.5f, 0.5f};
    MixAddStereoWithGain(src.data(), dst.data(), 2, 1.0f, 1.0f);
    EXPECT_FLOAT_EQ(dst[0], 1.5f);
    EXPECT_FLOAT_EQ(dst[1], 2.5f);
    EXPECT_FLOAT_EQ(dst[2], 3.5f);
    EXPECT_FLOAT_EQ(dst[3], 4.5f);
}

TEST(MixAdd, SeparateLeftRightGains) {
    std::vector<float> src{1.0f, 1.0f, 1.0f, 1.0f};
    std::vector<float> dst(4, 0.0f);
    MixAddStereoWithGain(src.data(), dst.data(), 2, 0.5f, 2.0f);
    EXPECT_FLOAT_EQ(dst[0], 0.5f);
    EXPECT_FLOAT_EQ(dst[1], 2.0f);
    EXPECT_FLOAT_EQ(dst[2], 0.5f);
    EXPECT_FLOAT_EQ(dst[3], 2.0f);
}

TEST(MixAdd, ZeroFramesIsNoOp) {
    std::vector<float> src{99.0f, 99.0f};
    std::vector<float> dst{0.25f, 0.25f};
    MixAddStereoWithGain(src.data(), dst.data(), 0, 1.0f, 1.0f);
    EXPECT_FLOAT_EQ(dst[0], 0.25f);
    EXPECT_FLOAT_EQ(dst[1], 0.25f);
}

TEST(ClampStereo, ClipsOutOfRange) {
    std::vector<float> buf{2.5f, -3.0f, 0.5f, -0.5f, 1.0f, -1.0f};
    ClampStereoBuffer(buf.data(), static_cast<int>(buf.size()));
    EXPECT_FLOAT_EQ(buf[0], 1.0f);
    EXPECT_FLOAT_EQ(buf[1], -1.0f);
    EXPECT_FLOAT_EQ(buf[2], 0.5f);
    EXPECT_FLOAT_EQ(buf[3], -0.5f);
    EXPECT_FLOAT_EQ(buf[4], 1.0f);
    EXPECT_FLOAT_EQ(buf[5], -1.0f);
}

TEST(ClampStereo, ZeroLengthIsNoOp) {
    std::vector<float> buf{5.0f};
    ClampStereoBuffer(buf.data(), 0);
    EXPECT_FLOAT_EQ(buf[0], 5.0f);
}
