#pragma once
#ifdef DAW_PUBLIC_API
#error "This header is internal-only."
#endif

#include "core/CoreState.h"
#include "audio/AudioRuntimeState.h"
#include "engine/mix_pipeline.h"

#include <vector>

// ── Audio-domain utilities ────────────────────────────────────────────────────
// These helpers are used exclusively by the audio engine (engine.cpp) and by
// call sites in main.cpp that coordinate the engine.  No UI or Win32 drawing
// code should include this header.

// dB / linear conversion
float DbToLinear(float db);

// Bus accessors (definitions move here; declarations also kept in state.h
// so that draw.cpp can call them without depending on this header).
// BusGainDbAt / BusPanAt / BusMuteAt are declared in state.h.
float BusGainDbAt(const CoreState& core, int busIndex);
float BusPanAt(const CoreState& core, int busIndex);
bool BusMuteAt(const CoreState& core, int busIndex);

// DSP-state book-keeping
void EnsureInsertDspStateStorage(const CoreState& core, AudioRuntimeState& audio);

// Track audibility (mute / solo logic – audio domain)
bool IsTrackAudible(const CoreState& core, int trackIndex);

// Resolved per-track routing for the offline mix-down path. Returns the
// track's *track-only* gain/pan (NOT folded with bus dB/pan) so the
// caller can accumulate into a per-bus buffer and then apply bus
// gain+pan at the bus stage — mirroring the realtime path. Equal-power
// pan is non-linear, so folding bus pan per-track and folding it after
// summing produce different output for any non-trivial bus pan; offline
// must do the latter to match what the user hears in realtime.
//   - audible: false when the track is out of range, the track is muted,
//     the resolved bus is muted, or the bus index is out of range.
//   - busIndex: the track's busIndex clamped to [0, kBusCount).
//   - gainL/gainR: linear stereo gains from track gain + track pan only
//     (only valid when audible).
// POD aliased from libs/engine so the shared mix-pipeline helpers
// (MixTracksToBuses / MixBusesToMaster) can consume app-resolved mix
// parameters without conversion.
using TrackBusMix = daw::engine::TrackMix;
TrackBusMix ResolveTrackBusMix(const CoreState& core, int trackIndex);

// Realtime per-track resolve for the audio callback path. Differs from
// ResolveTrackBusMix in that:
//   - audibility uses the solo-aware IsTrackAudible (offline does not),
//   - track gain/pan/bus come from automation accessors (offline reads
//     the static struct fields).
// Both helpers return track-only gain/pan; bus dB/pan are applied at the
// bus stage in both paths.
TrackBusMix ResolveTrackRealtimeMix(const CoreState& core, int trackIndex);

// Realtime per-bus resolve for the bus→master mix stage. Bus 3 is treated
// as the master bus and skips the pan stage (gainL == gainR == busGain).
//   - active: false when the bus is muted or out of range.
//   - gainL/gainR: linear stereo gains (only valid when active).
using BusRealtimeMix = daw::engine::BusMix;
BusRealtimeMix ResolveBusRealtimeMix(const CoreState& core, int busIndex);

// Project-length calculation
std::uint64_t ComputeProjectEndFrameLocked(const CoreState& core);

// Apply a track's insert FX chain to `buf` (interleaved stereo). No-op when
// `trackIndex` is out of range. `audio.trackInsertDspState` must already be
// sized via EnsureInsertDspStateStorage().
void ApplyTrackInsertChain(
    const CoreState& core,
    AudioRuntimeState& audio,
    int trackIndex,
    std::vector<float>& buf,
    float sampleRate);

// Apply a bus's insert FX chain to `buf`. No-op when `busIndex` is out of
// range or the bus has no insert slots configured.
void ApplyBusInsertChain(
    const CoreState& core,
    AudioRuntimeState& audio,
    int busIndex,
    std::vector<float>& buf,
    float sampleRate);

// Single-sample interpolated read from a LoadedAudio clip
bool ReadClipSampleAtProjectFrame(
    const LoadedAudio& audio,
    std::uint64_t clipFrameInProjectRate,
    int projectSampleRate,
    std::uint64_t sourceOffsetFrames,
    float* outL,
    float* outR);

// PCM-16 stereo linear resampler
void ResampleStereoPcm16Linear(
    const std::int16_t* src, int srcFrames,
    std::int16_t* dst,       int dstFrames);

// Float interleaved stereo linear resampler (for offline import / commit paths)
void ResampleStereoFloatLinear(
    const float* src, int srcFrames,
    float* dst,       int dstFrames);

// High-quality offline stereo SRC: windowed-sinc (Kaiser, ~80 dB) with
// anti-alias cutoff appropriate for downsampling. Use for one-shot
// import/convert operations — too expensive for the realtime callback.
void ResampleStereoFloatSincHQ(
    const float* src, int srcFrames, int srcSampleRate,
    float* dst,       int dstFrames, int dstSampleRate);

// Stateful PCM-16 stereo linear resampler for the realtime engine path.
// Carries a fractional phase and the previous boundary frame across calls so
// successive callbacks produce a continuous output stream (no per-buffer
// click). Returns the number of source frames consumed (so the engine can
// advance its cursor by the exact amount).
//
// Inputs:
//   src/srcFrames    : source PCM at srcSampleRate
//   dst/dstFrames    : output PCM at dstSampleRate (must be filled in full)
//   step             : srcSampleRate / dstSampleRate
//   phase [in/out]   : fractional position in current source frame, [0, 1)
//   lastL/R [in/out] : previous source frame (for the carry across boundaries)
//   primed [in/out]  : false on first call after seek/start; the resampler
//                      will take the first source frame as the carry instead
//                      of using lastL/R, then set primed = true.
int ResampleStereoPcm16LinearStateful(
    const std::int16_t* src, int srcFrames,
    std::int16_t* dst,       int dstFrames,
    double step,
    double* phase,
    std::int16_t* lastL, std::int16_t* lastR,
    bool* primed);
