#pragma once

#include "engine/mix_pipeline.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

// ── Phase 24 / Step K \u2014 Lock-free MixSnapshot publication ────────────────────
//
// SCOPE: K1 (skeleton). This phase introduces the publish/load plumbing only.
// `MixSnapshot` carries only a monotonically increasing generation counter
// today; subsequent K phases (K2 per-block mix params, K3 insert chains,
// K4 clip audio refs, K5 lock removal) move data into it incrementally
// until `audioStateLock` can be dropped from the realtime callback.
//
// Threading contract:
//   - UI / control thread is the SOLE writer: builds a new MixSnapshot,
//     calls MixSnapshotPublisher::Publish(std::shared_ptr<const MixSnapshot>).
//   - Audio thread is the SOLE reader: calls Load() at the top of each
//     callback; the returned shared_ptr keeps the snapshot alive for the
//     duration of the callback. Audio thread never mutates.
//   - Old snapshots are freed automatically when the last shared_ptr
//     reference drops \u2014 either when the audio thread overwrites its local
//     reference at the start of the next callback, or when the UI thread
//     drops its retained reference. Free happens on whichever thread sees
//     the last ref drop; no explicit RCU / hazard-pointer machinery.
//
// Future writers of the snapshot are confined to UI-thread code paths.
// Bombing the publisher from multiple writer threads is undefined.

namespace daw::engine {

// Per-block parameter snapshot. Empty for K1; fields are added in K2+ as
// each call site in EngineFillRealtimeBufferLocked is migrated off
// core.project.
struct MixSnapshot {
    // Monotonically increasing generation set by the publisher. Useful for
    // diagnostics ("did the audio thread see the latest publish?") and for
    // future tests. Wraps cleanly at 2^64.
    std::uint64_t generation {0};

    // Phase 24 / Step K2 \u2014 per-block mix parameters.
    //
    // Resolved per-track and per-bus mix values captured from the
    // CoreState at the moment the snapshot was built. These mirror the
    // outputs of the app-side ResolveTrackBusMix / ResolveBusRealtimeMix
    // helpers (i.e. the *non-automation* baseline view: track.gainDb,
    // track.pan, track.mute, bus.gainDb, bus.pan, bus.mute). Automation
    // is cursor-dependent and is NOT precomputed here; the realtime
    // callback still resolves automation per-block.
    //
    // Sized to track / bus counts at publish time; an empty vector means
    // "no snapshot yet" or "snapshot pre-K2" and callers must fall back
    // to the legacy CoreState-driven resolve.
    std::vector<TrackMix> trackMixes;
    std::vector<BusMix>   busMixes;

    // True when at least one track in the source CoreState had `solo`
    // set at snapshot time. Audio callbacks that consult solo state
    // (IsTrackAudible-style) need this to remain consistent with the
    // captured per-track audibility.
    bool anySoloTracks {false};
};

// Atomic publisher. Wraps std::atomic<std::shared_ptr<const T>> so the
// audio thread reads a snapshot via a single atomic load, and the UI
// thread publishes via a single atomic store. The shared_ptr itself
// handles the reference-counted lifetime; the snapshot is freed when
// both the publisher and the audio thread have released their reference.
class MixSnapshotPublisher {
public:
    MixSnapshotPublisher()
        : current_(std::make_shared<const MixSnapshot>()) {}

    // UI thread only. Replaces the published snapshot with `next`. The
    // previous snapshot lives until every concurrent reader (i.e. the
    // audio thread holding it for the duration of the in-flight callback)
    // releases its shared_ptr.
    void Publish(std::shared_ptr<const MixSnapshot> next) noexcept {
        current_.store(std::move(next), std::memory_order_release);
    }

    // Any thread. Returns the most recently published snapshot.
    // Audio-thread typical use: hold for one callback, then drop.
    std::shared_ptr<const MixSnapshot> Load() const noexcept {
        return current_.load(std::memory_order_acquire);
    }

    MixSnapshotPublisher(const MixSnapshotPublisher&) = delete;
    MixSnapshotPublisher& operator=(const MixSnapshotPublisher&) = delete;
    MixSnapshotPublisher(MixSnapshotPublisher&&) = delete;
    MixSnapshotPublisher& operator=(MixSnapshotPublisher&&) = delete;

private:
    std::atomic<std::shared_ptr<const MixSnapshot>> current_;
};

} // namespace daw::engine
