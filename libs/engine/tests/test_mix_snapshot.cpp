#include "engine/mix_snapshot.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

using daw::engine::MixSnapshot;
using daw::engine::MixSnapshotPublisher;

TEST(MixSnapshot, DefaultPublisherHoldsGenerationZero) {
    MixSnapshotPublisher pub;
    auto snap = pub.Load();
    ASSERT_NE(snap, nullptr);
    EXPECT_EQ(snap->generation, 0u);
}

TEST(MixSnapshot, PublishReplacesCurrent) {
    MixSnapshotPublisher pub;
    auto next = std::make_shared<MixSnapshot>();
    next->generation = 42;
    pub.Publish(next);

    auto observed = pub.Load();
    EXPECT_EQ(observed->generation, 42u);
}

TEST(MixSnapshot, PreviousSnapshotStaysAliveForExistingReader) {
    MixSnapshotPublisher pub;
    auto first = pub.Load();
    const std::uint64_t firstGen = first->generation;

    auto next = std::make_shared<MixSnapshot>();
    next->generation = firstGen + 1;
    pub.Publish(next);

    // `first` still points at the previous snapshot \u2014 the audio thread
    // can finish its in-flight callback safely.
    EXPECT_EQ(first->generation, firstGen);
    EXPECT_NE(first.get(), pub.Load().get());
}

TEST(MixSnapshot, ConcurrentPublisherAndReaderConverges) {
    // Tightly-spinning reader (simulated audio thread) + steady writer
    // (simulated UI thread). The reader should always see a monotonically
    // non-decreasing generation; the final published value should be
    // observable after writers stop.
    MixSnapshotPublisher pub;
    constexpr int kIterations = 2000;
    std::atomic<bool> stop {false};
    std::atomic<std::uint64_t> lastSeen {0};
    std::atomic<bool> sawRegression {false};

    std::thread reader([&]{
        std::uint64_t prev = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            auto s = pub.Load();
            if (s->generation < prev) {
                sawRegression.store(true);
            }
            prev = s->generation;
            lastSeen.store(prev, std::memory_order_relaxed);
        }
    });

    for (int i = 1; i <= kIterations; ++i) {
        auto next = std::make_shared<MixSnapshot>();
        next->generation = static_cast<std::uint64_t>(i);
        pub.Publish(next);
    }

    // Let the reader catch up to the final write.
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    stop.store(true, std::memory_order_relaxed);
    reader.join();

    EXPECT_FALSE(sawRegression.load());
    EXPECT_EQ(pub.Load()->generation, static_cast<std::uint64_t>(kIterations));
}

// ── Phase 24 / Step K2 ────────────────────────────────────────────────────────

TEST(MixSnapshot, DefaultMixVectorsAreEmpty) {
    MixSnapshot snap;
    EXPECT_TRUE(snap.trackMixes.empty());
    EXPECT_TRUE(snap.busMixes.empty());
    EXPECT_FALSE(snap.anySoloTracks);
}

TEST(MixSnapshot, MixVectorsRoundTripThroughPublisher) {
    MixSnapshotPublisher pub;

    auto next = std::make_shared<MixSnapshot>();
    next->generation = 7;
    next->trackMixes = {
        daw::engine::TrackMix{/*audible*/ true,  /*bus*/ 0, 0.5f, 0.7f},
        daw::engine::TrackMix{/*audible*/ false, /*bus*/ 1, 0.0f, 0.0f},
        daw::engine::TrackMix{/*audible*/ true,  /*bus*/ 2, 1.0f, 0.25f},
    };
    next->busMixes = {
        daw::engine::BusMix{/*active*/ true,  0.8f, 0.6f},
        daw::engine::BusMix{/*active*/ false, 0.0f, 0.0f},
    };
    next->anySoloTracks = true;
    pub.Publish(next);

    auto obs = pub.Load();
    ASSERT_EQ(obs->trackMixes.size(), 3u);
    EXPECT_TRUE (obs->trackMixes[0].audible);
    EXPECT_FALSE(obs->trackMixes[1].audible);
    EXPECT_EQ   (obs->trackMixes[2].busIndex, 2);
    EXPECT_FLOAT_EQ(obs->trackMixes[0].gainR, 0.7f);

    ASSERT_EQ(obs->busMixes.size(), 2u);
    EXPECT_TRUE (obs->busMixes[0].active);
    EXPECT_FALSE(obs->busMixes[1].active);
    EXPECT_FLOAT_EQ(obs->busMixes[0].gainL, 0.8f);

    EXPECT_TRUE(obs->anySoloTracks);
}

// ── Phase 24 / Step K3 ────────────────────────────────────────────────────────

TEST(MixSnapshot, DefaultInsertVectorsAreEmpty) {
    MixSnapshot snap;
    EXPECT_TRUE(snap.trackInserts.empty());
    EXPECT_TRUE(snap.busInserts.empty());
}

TEST(MixSnapshot, InsertChainConfigRoundTripsThroughPublisher) {
    MixSnapshotPublisher pub;

    auto next = std::make_shared<MixSnapshot>();
    next->generation = 11;

    MixSnapshot::InsertChainConfig tcfg{};
    tcfg.slots = 3;
    tcfg.effects[0] = static_cast<std::uint8_t>(kFxEQ);
    tcfg.effects[1] = static_cast<std::uint8_t>(kFxCMP);
    tcfg.effects[2] = static_cast<std::uint8_t>(kFxLIM);
    tcfg.bypass[1]  = true;
    tcfg.config[0].eq[0].freq_hz = 250.0f;
    tcfg.config[1].cmp_threshold_db = -24.0f;
    next->trackInserts.push_back(tcfg);

    MixSnapshot::InsertChainConfig bcfg{};
    bcfg.slots = 1;
    bcfg.effects[0] = static_cast<std::uint8_t>(kFxLIM);
    bcfg.config[0].lim_ceiling_db = -0.5f;
    next->busInserts.push_back(bcfg);

    pub.Publish(next);

    auto obs = pub.Load();
    ASSERT_EQ(obs->trackInserts.size(), 1u);
    EXPECT_EQ(obs->trackInserts[0].slots, 3);
    EXPECT_EQ(obs->trackInserts[0].effects[1], static_cast<std::uint8_t>(kFxCMP));
    EXPECT_TRUE (obs->trackInserts[0].bypass[1]);
    EXPECT_FALSE(obs->trackInserts[0].bypass[0]);
    EXPECT_FLOAT_EQ(obs->trackInserts[0].config[0].eq[0].freq_hz, 250.0f);
    EXPECT_FLOAT_EQ(obs->trackInserts[0].config[1].cmp_threshold_db, -24.0f);

    ASSERT_EQ(obs->busInserts.size(), 1u);
    EXPECT_EQ(obs->busInserts[0].slots, 1);
    EXPECT_FLOAT_EQ(obs->busInserts[0].config[0].lim_ceiling_db, -0.5f);
}

// ── Phase 24 / Step K4 ────────────────────────────────────────────────────────

TEST(MixSnapshot, DefaultClipFieldsAreEmpty) {
    MixSnapshot snap;
    EXPECT_TRUE(snap.clips.empty());
    EXPECT_EQ(snap.audioSourceCount, 0);
}

TEST(MixSnapshot, ClipPlacementsRoundTripThroughPublisher) {
    MixSnapshotPublisher pub;

    auto next = std::make_shared<MixSnapshot>();
    next->generation = 99;
    next->audioSourceCount = 3;
    next->clips.push_back(daw::core::ClipItem{
        /*trackIndex*/         2,
        /*audioIndex*/         0,
        /*startBeat*/          4.0f,
        /*lengthBeats*/        8.0f,
        /*color*/              0x123456u,
        /*name*/               L"kick",
        /*sourceOffsetFrames*/ 1024u,
    });
    next->clips.push_back(daw::core::ClipItem{
        /*trackIndex*/         0,
        /*audioIndex*/         2,
        /*startBeat*/          0.0f,
        /*lengthBeats*/        2.0f,
        /*color*/              0xABCDEFu,
        /*name*/               L"snare",
        /*sourceOffsetFrames*/ 0u,
    });
    pub.Publish(next);

    auto obs = pub.Load();
    ASSERT_EQ(obs->clips.size(), 2u);
    EXPECT_EQ(obs->clips[0].trackIndex, 2);
    EXPECT_EQ(obs->clips[0].audioIndex, 0);
    EXPECT_FLOAT_EQ(obs->clips[0].startBeat, 4.0f);
    EXPECT_FLOAT_EQ(obs->clips[0].lengthBeats, 8.0f);
    EXPECT_EQ(obs->clips[0].sourceOffsetFrames, 1024u);
    EXPECT_EQ(obs->clips[1].name, std::wstring(L"snare"));
    EXPECT_EQ(obs->audioSourceCount, 3);
}

// ── Phase 24 / Step K5a ───────────────────────────────────────────────────────

TEST(MixSnapshot, DefaultAudioSourcesEmpty) {
    MixSnapshot snap;
    EXPECT_TRUE(snap.audioSources.empty());
}

TEST(MixSnapshot, AudioSourcesShareOwnershipAcrossSnapshots) {
    MixSnapshotPublisher pub;

    auto source = std::make_shared<daw::core::LoadedAudio>();
    source->sampleRate = 48000;
    source->frames     = 4;
    source->stereo     = {0.1f, -0.1f, 0.2f, -0.2f, 0.3f, -0.3f, 0.4f, -0.4f};

    auto next = std::make_shared<MixSnapshot>();
    next->generation = 7;
    next->audioSources.push_back(source);
    next->audioSourceCount = 1;
    pub.Publish(next);

    // Drop our local copy; publisher + observer must keep it alive.
    const daw::core::LoadedAudio* rawAddr = source.get();
    source.reset();

    auto obs = pub.Load();
    ASSERT_EQ(obs->audioSources.size(), 1u);
    ASSERT_NE(obs->audioSources[0], nullptr);
    EXPECT_EQ(obs->audioSources[0].get(), rawAddr);
    EXPECT_EQ(obs->audioSources[0]->sampleRate, 48000);
    EXPECT_EQ(obs->audioSources[0]->frames, 4u);
    EXPECT_FLOAT_EQ(obs->audioSources[0]->stereo[2], 0.2f);
}
