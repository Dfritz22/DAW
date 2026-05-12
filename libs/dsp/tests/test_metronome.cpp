#include <gtest/gtest.h>

#include "dsp/metronome.h"

#include <algorithm>
#include <cmath>
#include <vector>

using daw::dsp::RenderMetronomeClicks;

namespace {

// Compute per-frame stereo magnitude max over the buffer. Useful for asking
// "did a click happen anywhere in this window?".
float MaxAbs(const std::vector<float>& buf) {
    float m = 0.0f;
    for (float v : buf) m = std::max(m, std::fabs(v));
    return m;
}

} // namespace

TEST(Metronome, ClickAtBeatZeroIsAccented) {
    // 1 second at 48 kHz, 1 beat per second → click at frame 0, 48000, ...
    const int frames = 4800;  // first 100 ms — covers the 40 ms accent click
    std::vector<float> buf(static_cast<size_t>(frames) * 2, 0.0f);
    RenderMetronomeClicks(buf.data(), frames, 48000.0f, 48000.0f, 0, 1.0f);

    // Accent base amplitude is 0.22 (gain=1). Peak should be near amp*env(0)=0.22.
    EXPECT_NEAR(MaxAbs(buf), 0.22f, 0.05f);
}

TEST(Metronome, ClickAtBeatOneIsUnaccented) {
    // baseFrame=48000 lands exactly at beat 1 (not a multiple of 4 → unaccented).
    const int frames = 4800;
    std::vector<float> buf(static_cast<size_t>(frames) * 2, 0.0f);
    RenderMetronomeClicks(buf.data(), frames, 48000.0f, 48000.0f, 48000, 1.0f);

    // Unaccented base amplitude = 0.14.
    EXPECT_NEAR(MaxAbs(buf), 0.14f, 0.05f);
}

TEST(Metronome, NoClickBetweenBeats) {
    // Window from frame 5000 to 5100 — well past the 1920-sample (40 ms) click
    // tail at 48 kHz / 1 beat = 48000 spb. Nothing should be rendered.
    const int frames = 100;
    std::vector<float> buf(static_cast<size_t>(frames) * 2, 0.0f);
    RenderMetronomeClicks(buf.data(), frames, 48000.0f, 48000.0f, 5000, 1.0f);
    EXPECT_FLOAT_EQ(MaxAbs(buf), 0.0f);
}

TEST(Metronome, MixesAdditivelyDoesNotClearBuffer) {
    const int frames = 100;
    std::vector<float> buf(static_cast<size_t>(frames) * 2, 0.5f);
    RenderMetronomeClicks(buf.data(), frames, 48000.0f, 48000.0f, 5000, 1.0f);
    // Untouched region should still hold the pre-existing 0.5.
    for (float v : buf) EXPECT_FLOAT_EQ(v, 0.5f);
}

TEST(Metronome, GainScalesAmplitude) {
    const int frames = 4800;
    std::vector<float> a(static_cast<size_t>(frames) * 2, 0.0f);
    std::vector<float> b(static_cast<size_t>(frames) * 2, 0.0f);
    RenderMetronomeClicks(a.data(), frames, 48000.0f, 48000.0f, 0, 1.0f);
    RenderMetronomeClicks(b.data(), frames, 48000.0f, 48000.0f, 0, 2.0f);
    EXPECT_NEAR(MaxAbs(b) / MaxAbs(a), 2.0f, 1e-4f);
}

TEST(Metronome, LeftAndRightAreIdentical) {
    const int frames = 2000;
    std::vector<float> buf(static_cast<size_t>(frames) * 2, 0.0f);
    RenderMetronomeClicks(buf.data(), frames, 48000.0f, 24000.0f, 0, 1.0f);
    for (int i = 0; i < frames; ++i) {
        EXPECT_FLOAT_EQ(buf[i * 2], buf[i * 2 + 1]);
    }
}
