#include <gtest/gtest.h>

#include "dsp/chain.h"
#include "dsp/insert_types.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace {

constexpr float kSampleRate = 48000.0f;

// 2N samples = N stereo frames at the given amplitude (DC).
std::vector<float> MakeDcStereo(size_t frames, float amp) {
    return std::vector<float>(frames * 2, amp);
}

// Build an empty (all-bypassed) effect chain.
struct Chain {
    InsertEffectArray   effects{};
    InsertBypassArray   bypass{};
    InsertConfigArray   configs{};
    InsertDspStateArray states{};
};

Chain MakeEmptyChain() {
    Chain c;
    c.bypass.fill(true);
    return c;
}

} // namespace

TEST(DspChain, NoSlotsRunIsIdentity) {
    auto buf = MakeDcStereo(64, 0.25f);
    const auto orig = buf;
    Chain c = MakeEmptyChain();
    DspApplyInsertChain(buf, kSampleRate, c.effects, c.bypass, c.configs, c.states, 0);
    EXPECT_EQ(buf, orig);
}

TEST(DspChain, AllBypassedIsIdentity) {
    auto buf = MakeDcStereo(64, 0.25f);
    const auto orig = buf;
    Chain c = MakeEmptyChain();
    // Populate effects with non-trivial selectors so we'd notice if bypass were ignored.
    c.effects.fill(static_cast<std::uint8_t>(kFxLIM));
    DspApplyInsertChain(buf, kSampleRate, c.effects, c.bypass, c.configs, c.states, kMaxInsertSlots);
    EXPECT_EQ(buf, orig);
}

TEST(DspChain, ClampsSlotCountToMax) {
    auto buf = MakeDcStereo(64, 0.25f);
    const auto orig = buf;
    Chain c = MakeEmptyChain();
    // slotCount way out of range — must not crash, must not mutate.
    DspApplyInsertChain(buf, kSampleRate, c.effects, c.bypass, c.configs, c.states, 10000);
    EXPECT_EQ(buf, orig);
    DspApplyInsertChain(buf, kSampleRate, c.effects, c.bypass, c.configs, c.states, -5);
    EXPECT_EQ(buf, orig);
}

TEST(DspChain, LimiterEnforcesCeilingOnHotSignal) {
    // DC at 0.95 hits a ceiling of -6 dB (≈ 0.501) hard. After release-time
    // settling the limiter should be holding the output at or below ceiling.
    auto buf = MakeDcStereo(static_cast<size_t>(kSampleRate * 0.5f), 0.95f);
    Chain c = MakeEmptyChain();
    c.bypass.fill(false);
    c.effects[0] = static_cast<std::uint8_t>(kFxLIM);
    c.configs[0].lim_ceiling_db = -6.0f;
    c.configs[0].lim_release_ms = 5.0f;
    DspApplyInsertChain(buf, kSampleRate, c.effects, c.bypass, c.configs, c.states, 1);

    // Skip the first few ms while the envelope is still locking on, then
    // verify everything past that is at-or-below the ceiling (with a tiny
    // float-math slack).
    const float ceilingLin = std::pow(10.0f, -6.0f / 20.0f);
    const size_t skipFrames = static_cast<size_t>(kSampleRate * 0.05f);
    for (size_t f = skipFrames; f < buf.size() / 2; ++f) {
        EXPECT_LE(std::fabs(buf[f * 2]),     ceilingLin + 1e-3f);
        EXPECT_LE(std::fabs(buf[f * 2 + 1]), ceilingLin + 1e-3f);
    }
}

TEST(DspChain, SaturationProducesFiniteOutputAndPreservesSign) {
    // 0.01 amplitude through the soft-clip saturator should remain finite,
    // bounded, and same-sign as the input (no fold-over). The exact gain
    // depends on the curve so we don't assert a specific output level.
    auto buf = MakeDcStereo(64, 0.01f);
    Chain c = MakeEmptyChain();
    c.bypass.fill(false);
    c.effects[0] = static_cast<std::uint8_t>(kFxSAT);
    c.configs[0].sat_drive = 0.5f;
    c.configs[0].sat_mix   = 1.0f;
    DspApplyInsertChain(buf, kSampleRate, c.effects, c.bypass, c.configs, c.states, 1);

    for (size_t i = 0; i < buf.size(); ++i) {
        EXPECT_TRUE(std::isfinite(buf[i]));
        EXPECT_GT(buf[i], 0.0f);
        EXPECT_LT(buf[i], 1.0f);
    }
}

TEST(DspChain, InvalidEffectIndexIsClampedAndDoesNotCrash) {
    auto buf = MakeDcStereo(64, 0.1f);
    Chain c = MakeEmptyChain();
    c.bypass.fill(false);
    c.effects[0] = 250;  // out-of-range
    DspApplyInsertChain(buf, kSampleRate, c.effects, c.bypass, c.configs, c.states, 1);
    // No assertion on the values — clamping just must not UB.
    SUCCEED();
}
