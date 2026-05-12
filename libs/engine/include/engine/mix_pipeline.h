#pragma once

// Shared per-bus mix pipeline used by both the realtime audio callback and
// the offline export paths in apps/daw_gui. Captures the structural
// invariant that bus inserts run on the *post-track-insert summed bus*
// signal and bus gain+pan are applied *once* on the summed bus (not folded
// per-track). Equal-power pan is non-linear, so per-track folding diverges
// from per-bus application whenever a bus has multiple panned tracks; the
// realtime path is the source of truth and offline must mirror it.
//
// Header-only templates so callers (realtime + offline) get zero-overhead
// dispatch — no std::function, no virtuals, no allocations.
//
// Usage pattern (caller owns scratch + DSP state lookups):
//
//   const TrackMix tmix[trackCount];   // pre-resolved per-track gains + bus
//   const BusMix   bmix[busCount];     // pre-resolved per-bus gains + active
//   std::vector<float> busBuf[busCount];  // pre-zeroed, sized frames*2
//   std::vector<float> trackScratch;      // sized frames*2
//
//   MixTracksToBuses(tmix, trackCount,
//       /*renderTrack*/        [&](int ti, float* dst, int n){ ... },
//       /*applyTrackInserts*/  [&](int ti, float* buf, int n){ ... },
//       trackScratch.data(), busBuf, frames, /*busFilter*/ -1);
//
//   MixBusesToMaster(bmix, busCount,
//       /*applyBusInserts*/    [&](int bi, float* buf, int n){ ... },
//       busBuf, masterOut, frames);
//
// busFilter == -1 mixes all audible tracks into their routed buses (full
// mix). busFilter >= 0 mixes only tracks whose busIndex matches (stem).

#include "dsp/mix.h"

#include <algorithm>
#include <cstddef>
#include <vector>

namespace daw::engine {

// Per-track resolved mix parameters. Mirrors the app-side TrackBusMix /
// TrackRealtimeMix shape so app code can `using TrackBusMix =
// daw::engine::TrackMix;` and avoid a churning rename.
struct TrackMix {
    bool  audible;
    int   busIndex;
    float gainL;
    float gainR;
};

// Per-bus resolved mix parameters. Master bus convention: callers set
// gainL == gainR (pan skipped) — captured here as data, not branched.
struct BusMix {
    bool  active;
    float gainL;
    float gainR;
};

// Track stage: for each audible track (optionally filtered to a single bus),
// zero a shared scratch buffer, ask the caller to render the track's clips
// into it, ask the caller to apply that track's insert chain in place, then
// mix the result into the routed bus buffer with track-only gain+pan.
//
//   trackMixes      Length trackCount. Skipped when !audible or (busFilter
//                   >= 0 && busIndex != busFilter) or busIndex out of
//                   busBuf range.
//   renderTrack     void(int trackIndex, float* dst, int frames). dst is
//                   already zeroed; renderer adds clip content.
//   applyTrackInsts void(int trackIndex, float* buf, int frames). In place.
//   trackScratch    Caller-owned, must be at least frames*2 floats.
//   busBuf          Length busCount. Each vector pre-zeroed and sized
//                   frames*2; helper additively mixes into them.
//   frames          Stereo frame count.
//   busFilter       -1 = mix all (full project mix); >=0 = stem (only that
//                   bus receives contributions).
template <typename RenderTrackFn, typename ApplyTrackInsertsFn>
void MixTracksToBuses(
    const TrackMix* trackMixes,
    int             trackCount,
    RenderTrackFn&& renderTrack,
    ApplyTrackInsertsFn&& applyTrackInserts,
    float*          trackScratch,
    std::vector<float>* busBuf,
    int             busCount,
    int             frames,
    int             busFilter = -1)
{
    if (trackCount <= 0 || frames <= 0 || trackScratch == nullptr || busBuf == nullptr) {
        return;
    }
    const std::size_t samples = static_cast<std::size_t>(frames) * 2u;
    for (int ti = 0; ti < trackCount; ++ti) {
        const TrackMix& tbm = trackMixes[ti];
        if (!tbm.audible) continue;
        if (busFilter >= 0 && tbm.busIndex != busFilter) continue;
        if (tbm.busIndex < 0 || tbm.busIndex >= busCount) continue;

        std::fill_n(trackScratch, samples, 0.0f);
        renderTrack(ti, trackScratch, frames);
        applyTrackInserts(ti, trackScratch, frames);
        daw::dsp::MixAddStereoWithGain(
            trackScratch,
            busBuf[static_cast<std::size_t>(tbm.busIndex)].data(),
            frames, tbm.gainL, tbm.gainR);
    }
}

// Bus stage: for each active bus, apply that bus's insert chain to the
// summed bus buffer in place, then mix into masterOut with bus gain+pan.
// Master bus is treated like any other (callers encode "no pan" via
// gainL == gainR == busGain).
//
//   busMixes        Length busCount.
//   applyBusInsts   void(int busIndex, float* buf, int frames). In place.
//                   Caller no-ops for buses with no insert slots.
//   busBuf          Length busCount, already filled by MixTracksToBuses.
//   masterOut       Pre-zeroed, frames*2 floats. Helper additively mixes.
template <typename ApplyBusInsertsFn>
void MixBusesToMaster(
    const BusMix*   busMixes,
    int             busCount,
    ApplyBusInsertsFn&& applyBusInserts,
    std::vector<float>* busBuf,
    float*          masterOut,
    int             frames)
{
    if (busCount <= 0 || frames <= 0 || busBuf == nullptr || masterOut == nullptr) {
        return;
    }
    for (int b = 0; b < busCount; ++b) {
        const BusMix& bm = busMixes[b];
        if (!bm.active) continue;
        applyBusInserts(b, busBuf[static_cast<std::size_t>(b)].data(), frames);
        daw::dsp::MixAddStereoWithGain(
            busBuf[static_cast<std::size_t>(b)].data(),
            masterOut, frames, bm.gainL, bm.gainR);
    }
}

} // namespace daw::engine
