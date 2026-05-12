#pragma once

#include <cstdint>

namespace daw::dsp {

// ── Stereo resamplers (interleaved L,R,L,R...) ───────────────────────────────
// All resamplers are pure functions: caller owns input and output buffers and
// any persistent state. Realtime-safe variants do not allocate; the high
// quality sinc resampler does allocate its kernel table and is for offline
// use only.

// Linear interpolation, integer PCM. Realtime-safe (no allocation).
void ResampleStereoPcm16Linear(
    const std::int16_t* src, int srcFrames,
    std::int16_t* dst,       int dstFrames);

// Linear interpolation, float. Realtime-safe (no allocation).
void ResampleStereoFloatLinear(
    const float* src, int srcFrames,
    float* dst,       int dstFrames);

// High-quality offline resampler: 64-tap windowed-sinc (Kaiser, β=8.6,
// ~80 dB stop-band) with polyphase kernel table and linear sub-phase
// interpolation. Allocates a temporary kernel table — NOT realtime-safe.
void ResampleStereoFloatSincHQ(
    const float* src, int srcFrames, int srcSampleRate,
    float* dst,       int dstFrames, int dstSampleRate);

// Stateful PCM-16 linear resampler for the realtime engine. Carries
// fractional phase + previous boundary frame across calls so successive
// callbacks produce a continuous output stream (no per-buffer click).
//
// On first call after seek/start, set `*primed = false`; the resampler will
// take src[0] as the carry frame and set `*primed = true`. Returns the
// number of source frames consumed (engine should advance its read cursor
// by that amount).
int ResampleStereoPcm16LinearStateful(
    const std::int16_t* src, int srcFrames,
    std::int16_t* dst,       int dstFrames,
    double step,
    double* phase,
    std::int16_t* lastL, std::int16_t* lastR,
    bool* primed);

} // namespace daw::dsp
