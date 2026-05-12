#include <gtest/gtest.h>

#include "io/wav.h"

#include <cmath>
#include <vector>

namespace {

using daw::io::wav::EncodePcm16Stereo;
using daw::io::wav::Error;
using daw::io::wav::ParseStereo;
using daw::io::wav::StereoBuffer;

// A 10-frame interleaved stereo signal at distinct, non-symmetric values
// across the full int16 range. Values are exact int16 quantization steps so
// the round-trip is bit-exact.
std::vector<float> SampleStereo() {
    return {
        // L,        R
         0.0f,       1.0f / 32767.0f,
        -1.0f,       1.0f,
         0.5f,      -0.5f,
         0.25f,     -0.25f,
         0.125f,    -0.125f,
         0.0625f,   -0.0625f,
         0.03125f,  -0.03125f,
         0.015625f, -0.015625f,
         1.0f / 32767.0f, -1.0f / 32767.0f,
         0.0f,       0.0f,
    };
}

} // namespace

TEST(WavCodec, EncodesValidHeader) {
    const auto enc = EncodePcm16Stereo(SampleStereo(), 48000);
    ASSERT_TRUE(enc);
    const auto& bytes = enc.value();
    ASSERT_GE(bytes.size(), 44u);
    EXPECT_EQ(bytes[0], 'R');
    EXPECT_EQ(bytes[1], 'I');
    EXPECT_EQ(bytes[2], 'F');
    EXPECT_EQ(bytes[3], 'F');
    EXPECT_EQ(bytes[8],  'W');
    EXPECT_EQ(bytes[9],  'A');
    EXPECT_EQ(bytes[10], 'V');
    EXPECT_EQ(bytes[11], 'E');
}

TEST(WavCodec, EncodeRejectsInvalidArguments) {
    EXPECT_FALSE(EncodePcm16Stereo({}, 48000));
    EXPECT_FALSE(EncodePcm16Stereo({0.0f}, 48000));   // odd sample count
    EXPECT_FALSE(EncodePcm16Stereo({0.0f, 0.0f}, 0));
    EXPECT_FALSE(EncodePcm16Stereo({0.0f, 0.0f}, -1));

    const auto err = EncodePcm16Stereo({}, 48000);
    EXPECT_EQ(err.error(), Error::InvalidArguments);
}

TEST(WavCodec, RoundTripPreservesSampleRateAndFrameCount) {
    const std::vector<float> input = SampleStereo();
    const auto enc = EncodePcm16Stereo(input, 48000);
    ASSERT_TRUE(enc);
    const auto dec = ParseStereo(enc.value());
    ASSERT_TRUE(dec);
    const StereoBuffer& buf = dec.value();
    EXPECT_EQ(buf.sampleRate, 48000);
    EXPECT_EQ(buf.frames, input.size() / 2);
    EXPECT_EQ(buf.interleaved.size(), input.size());
}

TEST(WavCodec, RoundTripIsApproximatelyBitExactForInt16AlignedValues) {
    const std::vector<float> input = SampleStereo();
    const auto enc = EncodePcm16Stereo(input, 44100);
    ASSERT_TRUE(enc);
    const auto dec = ParseStereo(enc.value());
    ASSERT_TRUE(dec);
    const auto& got = dec.value().interleaved;
    ASSERT_EQ(got.size(), input.size());
    for (size_t i = 0; i < input.size(); ++i) {
        // PCM16 quantization step is 1/32768. Allow one step of error.
        EXPECT_NEAR(got[i], input[i], 1.0f / 32768.0f) << "i=" << i;
    }
}

TEST(WavCodec, ParseRejectsTooSmallBuffer) {
    std::vector<std::uint8_t> tiny(10, 0);
    const auto dec = ParseStereo(tiny);
    ASSERT_FALSE(dec);
    EXPECT_EQ(dec.error(), Error::TooSmall);
}

TEST(WavCodec, ParseRejectsNonRiff) {
    std::vector<std::uint8_t> bytes(64, 0);
    // Set fmt-area bytes to plausible values but leave magic blank.
    const auto dec = ParseStereo(bytes);
    ASSERT_FALSE(dec);
    EXPECT_EQ(dec.error(), Error::NotRiffWave);
}

TEST(WavCodec, DescribeReturnsNonEmptyForEveryError) {
    for (auto e : {Error::OpenFailed,
                   Error::ReadFailed,
                   Error::WriteFailed,
                   Error::TooSmall,
                   Error::NotRiffWave,
                   Error::MissingChunks,
                   Error::UnsupportedChannels,
                   Error::UnsupportedFormat,
                   Error::InvalidArguments}) {
        const wchar_t* msg = daw::io::wav::Describe(e);
        ASSERT_NE(msg, nullptr);
        EXPECT_NE(msg[0], L'\0');
    }
}
