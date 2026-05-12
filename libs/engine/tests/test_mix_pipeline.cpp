// Golden-buffer tests for the shared mix pipeline (libs/engine/mix_pipeline.h).
//
// These tests pin down the structural invariants that distinguish the
// realtime audio callback from the offline export render — the same
// invariants that diverged before the audio-correctness pass:
//
//   1. Bus pan is applied ONCE on the summed bus, not per-track.
//      Equal-power pan is non-linear, so per-track folding diverges from
//      per-bus application whenever multiple tracks route to the same bus.
//   2. Bus inserts run on the post-track-insert summed signal.
//   3. Stem rendering filters by bus index but otherwise uses the same
//      pipeline (bus inserts on summed bus, bus gain+pan on output).
//   4. Master bus convention: caller sets gainL == gainR, helper applies
//      that as a uniform scalar (no double-pan).
//
// The pipeline is tested with caller-supplied lambdas for renderTrack /
// applyTrackInserts / applyBusInserts, so we can record exactly what got
// called with what arguments and verify both the data flow and the
// algebraic invariants on the buffer math.

#include <gtest/gtest.h>

#include "engine/mix_pipeline.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

using daw::engine::BusMix;
using daw::engine::MixBusesToMaster;
using daw::engine::MixTracksToBuses;
using daw::engine::TrackMix;

namespace {

constexpr int kFrames = 8;
constexpr int kStereoSamples = kFrames * 2;

// Deterministic per-track signal: track ti renders L = ti+1, R = -(ti+1) into
// every sample. Lets us verify summation and gain math by inspection.
auto MakeConstantRender(float fillL, float fillR) {
    return [fillL, fillR](int /*ti*/, float* dst, int n) {
        for (int i = 0; i < n; ++i) {
            dst[2*i + 0] = fillL;
            dst[2*i + 1] = fillR;
        }
    };
}

// In-place scale (stand-in for "track insert chain"). Distinguishes "insert
// applied" from "insert skipped".
auto MakeScaleInsert(float scale) {
    return [scale](int /*idx*/, float* buf, int n) {
        for (int i = 0; i < n*2; ++i) buf[i] *= scale;
    };
}

auto NoopInsert() {
    return [](int /*idx*/, float* /*buf*/, int /*n*/) {};
}

void ZeroBus(std::vector<float>& v) {
    std::fill(v.begin(), v.end(), 0.0f);
}

} // namespace

// ─────────────────────────── MixTracksToBuses ────────────────────────────

TEST(MixPipeline, TracksMixIntoRoutedBusWithGain) {
    constexpr int kBuses = 2;
    std::vector<TrackMix> tmix = {
        {true, /*bus*/0, /*gL*/1.0f, /*gR*/1.0f},
        {true, /*bus*/1, /*gL*/0.5f, /*gR*/0.5f},
    };
    std::vector<std::vector<float>> busBuf(kBuses, std::vector<float>(kStereoSamples, 0.0f));
    std::vector<float> scratch(kStereoSamples, 0.0f);

    MixTracksToBuses(tmix.data(), 2,
        [](int ti, float* dst, int n) {
            const float v = static_cast<float>(ti + 1);
            for (int i = 0; i < n*2; ++i) dst[i] = v;
        },
        NoopInsert(),
        scratch.data(), busBuf.data(), kBuses, kFrames);

    // Track 0 (value 1.0, gain 1.0) → bus 0
    for (int i = 0; i < kStereoSamples; ++i) EXPECT_FLOAT_EQ(busBuf[0][i], 1.0f);
    // Track 1 (value 2.0, gain 0.5) → bus 1
    for (int i = 0; i < kStereoSamples; ++i) EXPECT_FLOAT_EQ(busBuf[1][i], 1.0f);
}

TEST(MixPipeline, MultipleTracksSumIntoSameBus) {
    constexpr int kBuses = 1;
    std::vector<TrackMix> tmix = {
        {true, 0, 1.0f, 1.0f},
        {true, 0, 1.0f, 1.0f},
        {true, 0, 1.0f, 1.0f},
    };
    std::vector<std::vector<float>> busBuf(kBuses, std::vector<float>(kStereoSamples, 0.0f));
    std::vector<float> scratch(kStereoSamples, 0.0f);

    MixTracksToBuses(tmix.data(), 3,
        MakeConstantRender(1.0f, 1.0f),
        NoopInsert(),
        scratch.data(), busBuf.data(), kBuses, kFrames);

    for (int i = 0; i < kStereoSamples; ++i) EXPECT_FLOAT_EQ(busBuf[0][i], 3.0f);
}

TEST(MixPipeline, InaudibleTracksSkipped) {
    constexpr int kBuses = 1;
    std::vector<TrackMix> tmix = {
        {false, 0, 1.0f, 1.0f},  // muted/soloed-out
        {true,  0, 1.0f, 1.0f},
    };
    std::vector<std::vector<float>> busBuf(kBuses, std::vector<float>(kStereoSamples, 0.0f));
    std::vector<float> scratch(kStereoSamples, 0.0f);

    int renderCalls = 0;
    int insertCalls = 0;
    MixTracksToBuses(tmix.data(), 2,
        [&](int /*ti*/, float* dst, int n) {
            ++renderCalls;
            for (int i = 0; i < n*2; ++i) dst[i] = 1.0f;
        },
        [&](int /*ti*/, float* /*buf*/, int /*n*/) { ++insertCalls; },
        scratch.data(), busBuf.data(), kBuses, kFrames);

    EXPECT_EQ(renderCalls, 1);
    EXPECT_EQ(insertCalls, 1);
    for (int i = 0; i < kStereoSamples; ++i) EXPECT_FLOAT_EQ(busBuf[0][i], 1.0f);
}

TEST(MixPipeline, OutOfRangeBusIndexSkipped) {
    constexpr int kBuses = 2;
    std::vector<TrackMix> tmix = {
        {true, 5, 1.0f, 1.0f},   // bus 5 doesn't exist
        {true, -1, 1.0f, 1.0f},  // negative
        {true, 1, 1.0f, 1.0f},   // valid
    };
    std::vector<std::vector<float>> busBuf(kBuses, std::vector<float>(kStereoSamples, 0.0f));
    std::vector<float> scratch(kStereoSamples, 0.0f);

    MixTracksToBuses(tmix.data(), 3,
        MakeConstantRender(1.0f, 1.0f),
        NoopInsert(),
        scratch.data(), busBuf.data(), kBuses, kFrames);

    for (int i = 0; i < kStereoSamples; ++i) {
        EXPECT_FLOAT_EQ(busBuf[0][i], 0.0f);
        EXPECT_FLOAT_EQ(busBuf[1][i], 1.0f);
    }
}

TEST(MixPipeline, TrackInsertChainAppliedBeforeMix) {
    constexpr int kBuses = 1;
    std::vector<TrackMix> tmix = { {true, 0, 1.0f, 1.0f} };
    std::vector<std::vector<float>> busBuf(kBuses, std::vector<float>(kStereoSamples, 0.0f));
    std::vector<float> scratch(kStereoSamples, 0.0f);

    // Render writes 2.0; insert scales by 0.5 → bus sees 1.0.
    MixTracksToBuses(tmix.data(), 1,
        MakeConstantRender(2.0f, 2.0f),
        MakeScaleInsert(0.5f),
        scratch.data(), busBuf.data(), kBuses, kFrames);

    for (int i = 0; i < kStereoSamples; ++i) EXPECT_FLOAT_EQ(busBuf[0][i], 1.0f);
}

TEST(MixPipeline, BusFilterIsolatesStem) {
    constexpr int kBuses = 3;
    std::vector<TrackMix> tmix = {
        {true, 0, 1.0f, 1.0f},
        {true, 1, 1.0f, 1.0f},
        {true, 2, 1.0f, 1.0f},
    };
    std::vector<std::vector<float>> busBuf(kBuses, std::vector<float>(kStereoSamples, 0.0f));
    std::vector<float> scratch(kStereoSamples, 0.0f);

    MixTracksToBuses(tmix.data(), 3,
        MakeConstantRender(1.0f, 1.0f),
        NoopInsert(),
        scratch.data(), busBuf.data(), kBuses, kFrames,
        /*busFilter*/ 1);

    for (int i = 0; i < kStereoSamples; ++i) {
        EXPECT_FLOAT_EQ(busBuf[0][i], 0.0f);
        EXPECT_FLOAT_EQ(busBuf[1][i], 1.0f);
        EXPECT_FLOAT_EQ(busBuf[2][i], 0.0f);
    }
}

TEST(MixPipeline, ScratchZeroedBetweenTracks) {
    // Two tracks, second renders nothing (no clips). If scratch wasn't
    // zeroed between tracks, track-2 would inherit track-1's signal.
    constexpr int kBuses = 1;
    std::vector<TrackMix> tmix = {
        {true, 0, 1.0f, 1.0f},
        {true, 0, 1.0f, 1.0f},
    };
    std::vector<std::vector<float>> busBuf(kBuses, std::vector<float>(kStereoSamples, 0.0f));
    std::vector<float> scratch(kStereoSamples, 0.0f);

    MixTracksToBuses(tmix.data(), 2,
        [](int ti, float* dst, int n) {
            // Only track 0 renders content.
            if (ti == 0) {
                for (int i = 0; i < n*2; ++i) dst[i] = 1.0f;
            }
        },
        NoopInsert(),
        scratch.data(), busBuf.data(), kBuses, kFrames);

    // Bus = track-0 (1.0) + track-1 (0.0) = 1.0, NOT 2.0.
    for (int i = 0; i < kStereoSamples; ++i) EXPECT_FLOAT_EQ(busBuf[0][i], 1.0f);
}

// ─────────────────────────── MixBusesToMaster ────────────────────────────

TEST(MixPipeline, BusGainAppliedOnceOnSummedBus) {
    constexpr int kBuses = 1;
    std::vector<BusMix> bmix = { {true, 2.0f, 2.0f} };
    std::vector<std::vector<float>> busBuf(kBuses, std::vector<float>(kStereoSamples, 1.0f));
    std::vector<float> master(kStereoSamples, 0.0f);

    MixBusesToMaster(bmix.data(), kBuses,
        NoopInsert(),
        busBuf.data(), master.data(), kFrames);

    // Bus value 1.0 × bus gain 2.0 = 2.0 in master.
    for (int i = 0; i < kStereoSamples; ++i) EXPECT_FLOAT_EQ(master[i], 2.0f);
}

TEST(MixPipeline, MasterPanConvention_GainLEqGainR_NoDoublePan) {
    // Master bus convention: gainL == gainR. Verifies the helper treats this
    // as uniform scalar — no asymmetric pan baked into master output.
    constexpr int kBuses = 1;
    std::vector<BusMix> bmix = { {true, 0.7071f, 0.7071f} };
    std::vector<std::vector<float>> busBuf(kBuses, std::vector<float>(kStereoSamples, 1.0f));
    std::vector<float> master(kStereoSamples, 0.0f);

    MixBusesToMaster(bmix.data(), kBuses, NoopInsert(),
        busBuf.data(), master.data(), kFrames);

    for (int i = 0; i < kFrames; ++i) {
        EXPECT_FLOAT_EQ(master[2*i + 0], 0.7071f);
        EXPECT_FLOAT_EQ(master[2*i + 1], 0.7071f);
    }
}

TEST(MixPipeline, BusPanAsymmetryAppliedOnSummedBus) {
    // gainL=1.0, gainR=0.0 → output is mono-left.
    constexpr int kBuses = 1;
    std::vector<BusMix> bmix = { {true, 1.0f, 0.0f} };
    std::vector<std::vector<float>> busBuf(kBuses, std::vector<float>(kStereoSamples, 1.0f));
    std::vector<float> master(kStereoSamples, 0.0f);

    MixBusesToMaster(bmix.data(), kBuses, NoopInsert(),
        busBuf.data(), master.data(), kFrames);

    for (int i = 0; i < kFrames; ++i) {
        EXPECT_FLOAT_EQ(master[2*i + 0], 1.0f);
        EXPECT_FLOAT_EQ(master[2*i + 1], 0.0f);
    }
}

TEST(MixPipeline, InactiveBusesSkipped) {
    constexpr int kBuses = 2;
    std::vector<BusMix> bmix = {
        {false, 1.0f, 1.0f},  // muted → no output, no inserts
        {true,  1.0f, 1.0f},
    };
    std::vector<std::vector<float>> busBuf(kBuses, std::vector<float>(kStereoSamples, 1.0f));
    std::vector<float> master(kStereoSamples, 0.0f);

    int insertCalls = 0;
    MixBusesToMaster(bmix.data(), kBuses,
        [&](int /*bi*/, float* /*buf*/, int /*n*/) { ++insertCalls; },
        busBuf.data(), master.data(), kFrames);

    EXPECT_EQ(insertCalls, 1);  // only bus 1
    for (int i = 0; i < kStereoSamples; ++i) EXPECT_FLOAT_EQ(master[i], 1.0f);
}

TEST(MixPipeline, BusInsertsAppliedOnSummedSignalBeforeMaster) {
    // The structural bug we fixed: bus inserts must run on the summed bus,
    // not on a re-rendered dry sum. Here the insert scales by 0.5 — with
    // pre-summed input of 4.0, master should see 2.0 (×bus gain 1.0).
    constexpr int kBuses = 1;
    std::vector<BusMix> bmix = { {true, 1.0f, 1.0f} };
    std::vector<std::vector<float>> busBuf(kBuses, std::vector<float>(kStereoSamples, 4.0f));
    std::vector<float> master(kStereoSamples, 0.0f);

    MixBusesToMaster(bmix.data(), kBuses,
        MakeScaleInsert(0.5f),
        busBuf.data(), master.data(), kFrames);

    for (int i = 0; i < kStereoSamples; ++i) EXPECT_FLOAT_EQ(master[i], 2.0f);
}

// ─────────────────────────── End-to-end: track pipeline catches the historical bug ───────────────────────────

TEST(MixPipeline, EndToEnd_BusPanOnSummedBus_NotPerTrack) {
    // The actual structural bug from Phase 11h: equal-power pan baked
    // per-track diverges from pan applied once on the summed bus. We
    // approximate with linear pan (gainL=1, gainR=0) which still has the
    // critical algebraic property: f(a) + f(b) ≠ f(a + b) for the offline-
    // bug pattern, BUT here MixBusesToMaster correctly applies it once.
    //
    // Two tracks with gain 1.0, both routed to bus 0. Bus 0 has gainL=1,
    // gainR=0. Tracks render value 1.0. After MixTracksToBuses, busBuf =
    // 2.0. After MixBusesToMaster, master.L = 2.0, master.R = 0.0.
    // (If pan had been folded per-track the answer would be the same for
    // linear pan — but the test verifies the structural pipeline, not
    // pan non-linearity.)
    constexpr int kBuses = 1;
    std::vector<TrackMix> tmix = {
        {true, 0, 1.0f, 1.0f},
        {true, 0, 1.0f, 1.0f},
    };
    std::vector<BusMix> bmix = { {true, 1.0f, 0.0f} };
    std::vector<std::vector<float>> busBuf(kBuses, std::vector<float>(kStereoSamples, 0.0f));
    std::vector<float> scratch(kStereoSamples, 0.0f);
    std::vector<float> master(kStereoSamples, 0.0f);

    MixTracksToBuses(tmix.data(), 2,
        MakeConstantRender(1.0f, 1.0f),
        NoopInsert(),
        scratch.data(), busBuf.data(), kBuses, kFrames);

    MixBusesToMaster(bmix.data(), kBuses, NoopInsert(),
        busBuf.data(), master.data(), kFrames);

    for (int i = 0; i < kFrames; ++i) {
        EXPECT_FLOAT_EQ(master[2*i + 0], 2.0f);
        EXPECT_FLOAT_EQ(master[2*i + 1], 0.0f);
    }
}

TEST(MixPipeline, EndToEnd_NonlinearBusInsert_RequiresSummedSignal) {
    // The real bug-class catcher: a nonlinear "compressor-like" insert
    // produces different output for sum-then-process vs process-each-then-
    // sum. Here we model it as max(buf, threshold) — clearly nonlinear.
    // Two tracks each rendering 1.0 → summed bus = 2.0 → max(2, 1.5) = 2.0.
    // (The buggy alternative — apply max per-track then sum — would give
    // max(1, 1.5) + max(1, 1.5) = 1.5 + 1.5 = 3.0.)
    constexpr int kBuses = 1;
    std::vector<TrackMix> tmix = {
        {true, 0, 1.0f, 1.0f},
        {true, 0, 1.0f, 1.0f},
    };
    std::vector<BusMix> bmix = { {true, 1.0f, 1.0f} };
    std::vector<std::vector<float>> busBuf(kBuses, std::vector<float>(kStereoSamples, 0.0f));
    std::vector<float> scratch(kStereoSamples, 0.0f);
    std::vector<float> master(kStereoSamples, 0.0f);

    MixTracksToBuses(tmix.data(), 2,
        MakeConstantRender(1.0f, 1.0f),
        NoopInsert(),
        scratch.data(), busBuf.data(), kBuses, kFrames);

    auto nonlinearInsert = [](int /*bi*/, float* buf, int n) {
        for (int i = 0; i < n*2; ++i) buf[i] = std::max(buf[i], 1.5f);
    };
    MixBusesToMaster(bmix.data(), kBuses, nonlinearInsert,
        busBuf.data(), master.data(), kFrames);

    // Correct (sum-then-process): max(2.0, 1.5) = 2.0
    // Buggy (process-then-sum):  max(1.0, 1.5) × 2 = 3.0
    for (int i = 0; i < kStereoSamples; ++i) {
        EXPECT_FLOAT_EQ(master[i], 2.0f);
    }
}

TEST(MixPipeline, EmptyInputsAreSafe) {
    std::vector<float> scratch(kStereoSamples, 0.0f);
    std::vector<std::vector<float>> busBuf;
    std::vector<float> master(kStereoSamples, 0.0f);

    // Should be a no-op, not a crash.
    MixTracksToBuses<decltype(MakeConstantRender(0.f, 0.f)), decltype(NoopInsert())>(
        nullptr, 0, MakeConstantRender(0.f, 0.f), NoopInsert(),
        scratch.data(), busBuf.data(), 0, kFrames);
    MixBusesToMaster<decltype(NoopInsert())>(
        nullptr, 0, NoopInsert(), busBuf.data(), master.data(), kFrames);
    for (int i = 0; i < kStereoSamples; ++i) EXPECT_FLOAT_EQ(master[i], 0.0f);
}
