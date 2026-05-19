#pragma once
#ifdef DAW_PUBLIC_API
#error "This header is internal-only."
#endif

#include "core/CoreState.h"
#include "audio/AudioRuntimeState.h"
#include "engine/mix_snapshot.h"

#include <memory>

// Phase 24 / Step K2 — app-layer bridge between CoreState and the
// engine-layer MixSnapshotPublisher. Lives in the app layer because it
// touches CoreState (which engine/ cannot depend on at this layer level).
//
// All functions are UI / control-thread only. The audio thread reads
// snapshots via audio.mixSnapshotPublisher.Load() — never via these
// helpers.

// Build a fresh MixSnapshot from `core`. Captures per-track and per-bus
// non-automation mix params (gain, pan, mute, bus index, solo) using the
// same semantics as ResolveTrackBusMix / ResolveBusRealtimeMix. The
// snapshot's generation is one greater than the currently published
// snapshot in `audio.mixSnapshotPublisher`.
std::shared_ptr<const daw::engine::MixSnapshot>
BuildMixSnapshotFromCore(const CoreState& core, const AudioRuntimeState& audio);

// Build + Publish in one call. Convenience wrapper; UI-thread mutation
// sites (track gain change, bus pan change, etc.) call this after the
// mutation to publish a fresh snapshot for the audio thread.
void PublishMixSnapshotFromCore(AudioRuntimeState& audio, const CoreState& core);
