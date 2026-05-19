#pragma once

#include "core/audio_clip.h"
#include "dsp/insert_types.h"
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

    // Phase 24 / Step K3 \u2014 insert-chain configuration snapshot.
    //
    // Per-track and per-bus copies of the DSP insert chain configuration
    // (effect type ids, bypass flags, per-effect parameter blocks, slot
    // count). The audio callback's persistent DSP *state* (filter
    // memories, delay buffers, envelope followers) is NOT snapshotted \u2014
    // that stays in AudioRuntimeState::trackInsertDspState /
    // busInsertDspState because it is mutated on the audio thread
    // every block.
    //
    // K3 only populates the data; the realtime callback's
    // ApplyTrackInsertChain / ApplyBusInsertChain still read configs
    // from core.project under the audio lock. K5 switches the callback
    // to read from these vectors and wires UI mutators (FX knob drags,
    // AutoMix completion, project load) to call Publish.
    //
    // Empty vectors mean "no snapshot yet" or "pre-K3"; callers must
    // continue using the legacy CoreState-driven path until non-empty.
    struct InsertChainConfig {
        InsertEffectArray effects {};
        InsertBypassArray bypass  {};
        InsertConfigArray config  {};
        int               slots   {0};
    };
    std::vector<InsertChainConfig> trackInserts;
    std::vector<InsertChainConfig> busInserts;

    // Phase 24 / Step K4 \u2014 clip-placement snapshot.
    //
    // Immutable copy of core.project.clips at publish time. ClipItem is a
    // small POD (track index, audio index, start/length beats, color,
    // source-offset) so the per-publish copy is cheap relative to the
    // mix-param + insert-config copies above. Captures only the timeline
    // *placements*; the underlying LoadedAudio PCM buffers are NOT
    // snapshotted here \u2014 they remain owned by core.project.audio and
    // are read by the realtime callback under the audio lock today.
    //
    // The shared_ptr-ification of audio sources (required to drop the
    // lock from the audio thread when the buffer vector resizes during
    // recording / import) lands in K5 alongside the lock removal.
    //
    // `audioSourceCount` mirrors core.project.audio.size() so realtime
    // bounds checks against clip.audioIndex can run from snapshot data.
    std::vector<daw::core::ClipItem> clips;
    int                              audioSourceCount {0};
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
