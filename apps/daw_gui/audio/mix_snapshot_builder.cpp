#include "audio/mix_snapshot_builder.h"
#include "audio/engine_utils.h"

#include <memory>

std::shared_ptr<const daw::engine::MixSnapshot>
BuildMixSnapshotFromCore(const CoreState& core, const AudioRuntimeState& audio) {
    auto snap = std::make_shared<daw::engine::MixSnapshot>();

    // Generation: one more than whatever is currently published. This is
    // racy if multiple UI threads publish concurrently (they shouldn't),
    // but the only correctness invariant is "monotonically increasing
    // under single-writer discipline", which holds.
    const auto prev = audio.mixSnapshotPublisher.Load();
    snap->generation = (prev ? prev->generation : 0u) + 1u;

    const int trackCount = static_cast<int>(core.project.tracks.size());
    snap->trackMixes.resize(static_cast<size_t>(trackCount));
    snap->anySoloTracks = false;
    for (int ti = 0; ti < trackCount; ++ti) {
        snap->trackMixes[static_cast<size_t>(ti)] = ResolveTrackBusMix(core, ti);
        if (core.project.tracks[static_cast<size_t>(ti)].solo) {
            snap->anySoloTracks = true;
        }
    }

    snap->busMixes.resize(static_cast<size_t>(kBusCount));
    for (int b = 0; b < kBusCount; ++b) {
        snap->busMixes[static_cast<size_t>(b)] = ResolveBusRealtimeMix(core, b);
    }

    // Phase 24 / Step K3 \u2014 capture insert-chain configs (effects, bypass,
    // per-effect parameter blocks, slot count) for each track and bus.
    // Persistent DSP state (filter memories, delay buffers) is NOT copied
    // \u2014 those mutate on the audio thread and stay in AudioRuntimeState.
    snap->trackInserts.resize(static_cast<size_t>(trackCount));
    for (int ti = 0; ti < trackCount; ++ti) {
        const auto& t = core.project.tracks[static_cast<size_t>(ti)];
        auto& dst = snap->trackInserts[static_cast<size_t>(ti)];
        dst.effects = t.insertEffects;
        dst.bypass  = t.insertBypass;
        dst.config  = t.insertConfig;
        dst.slots   = t.insertSlots;
    }

    const int busCount = static_cast<int>(core.project.buses.size());
    snap->busInserts.resize(static_cast<size_t>(busCount));
    for (int b = 0; b < busCount; ++b) {
        const auto& bd = core.project.buses[static_cast<size_t>(b)];
        auto& dst = snap->busInserts[static_cast<size_t>(b)];
        dst.effects = bd.insertEffects;
        dst.bypass  = bd.insertBypass;
        dst.config  = bd.insertConfig;
        dst.slots   = bd.insertSlots;
    }

    // Phase 24 / Step K4 \u2014 capture clip placements. ClipItem is a small
    // POD; the per-publish copy is cheap compared to the insert-chain
    // configs above. The underlying LoadedAudio PCM buffers remain owned
    // by core.project.audio (lock-protected); their shared_ptr migration
    // lands in K5.
    snap->clips = core.project.clips;
    snap->audioSourceCount = static_cast<int>(core.project.audio.size());

    // Phase 24 / Step K5a \u2014 share immutable references to audio sources.
    // Element-wise shared_ptr copy: cheap (atomic refcount bump per slot),
    // keeps every LoadedAudio alive for the snapshot's lifetime, and
    // matches the realtime callback's future read path (K5b).
    snap->audioSources.reserve(core.project.audio.size());
    for (const auto& srcPtr : core.project.audio) {
        snap->audioSources.push_back(srcPtr);  // shared_ptr<LoadedAudio> \u2192 shared_ptr<const LoadedAudio>
    }

    return snap;
}

void PublishMixSnapshotFromCore(AudioRuntimeState& audio, const CoreState& core) {
    audio.mixSnapshotPublisher.Publish(BuildMixSnapshotFromCore(core, audio));
}
