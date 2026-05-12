#include <gtest/gtest.h>

#include "dsp/mix.h"

#include <vector>

using daw::dsp::MixDifferentialAddWithGain;

TEST(MixDifferentialAddWithGain, AddsScaledDifference) {
    std::vector<float> pre  { 1.0f, 2.0f, 3.0f, 4.0f };
    std::vector<float> post { 1.5f, 2.5f, 4.0f, 5.0f };
    std::vector<float> dst  { 10.0f, 10.0f, 10.0f, 10.0f };
    MixDifferentialAddWithGain(pre.data(), post.data(), dst.data(), 4, 2.0f);
    EXPECT_FLOAT_EQ(dst[0], 11.0f);
    EXPECT_FLOAT_EQ(dst[1], 11.0f);
    EXPECT_FLOAT_EQ(dst[2], 12.0f);
    EXPECT_FLOAT_EQ(dst[3], 12.0f);
}

TEST(MixDifferentialAddWithGain, ZeroGainIsNoop) {
    std::vector<float> pre  { 1.0f, 2.0f };
    std::vector<float> post { 9.0f, 9.0f };
    std::vector<float> dst  { 0.5f, 0.5f };
    MixDifferentialAddWithGain(pre.data(), post.data(), dst.data(), 2, 0.0f);
    EXPECT_FLOAT_EQ(dst[0], 0.5f);
    EXPECT_FLOAT_EQ(dst[1], 0.5f);
}

TEST(MixDifferentialAddWithGain, EqualPrePostNoChange) {
    std::vector<float> pre  { 0.25f, 0.5f, 0.75f, 1.0f };
    std::vector<float> post { 0.25f, 0.5f, 0.75f, 1.0f };
    std::vector<float> dst  { 0.0f, 0.0f, 0.0f, 0.0f };
    MixDifferentialAddWithGain(pre.data(), post.data(), dst.data(), 4, 1.0f);
    for (float v : dst) EXPECT_FLOAT_EQ(v, 0.0f);
}

TEST(MixDifferentialAddWithGain, NullOrEmptyIsNoop) {
    std::vector<float> dst{ 1.0f, 2.0f };
    MixDifferentialAddWithGain(nullptr, nullptr, dst.data(), 2, 1.0f);
    EXPECT_FLOAT_EQ(dst[0], 1.0f);
    EXPECT_FLOAT_EQ(dst[1], 2.0f);

    std::vector<float> pre{ 0.0f }, post{ 1.0f };
    MixDifferentialAddWithGain(pre.data(), post.data(), dst.data(), 0, 1.0f);
    EXPECT_FLOAT_EQ(dst[0], 1.0f);
}

TEST(MixDifferentialAddWithGain, NegativeGainSubtractsDelta) {
    std::vector<float> pre  { 0.0f, 0.0f };
    std::vector<float> post { 1.0f, 2.0f };
    std::vector<float> dst  { 5.0f, 5.0f };
    MixDifferentialAddWithGain(pre.data(), post.data(), dst.data(), 2, -1.0f);
    EXPECT_FLOAT_EQ(dst[0], 4.0f);
    EXPECT_FLOAT_EQ(dst[1], 3.0f);
}
