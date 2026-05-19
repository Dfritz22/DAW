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
