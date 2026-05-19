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

    return snap;
}

void PublishMixSnapshotFromCore(AudioRuntimeState& audio, const CoreState& core) {
    audio.mixSnapshotPublisher.Publish(BuildMixSnapshotFromCore(core, audio));
}
