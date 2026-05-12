#pragma once

#include <cstdint>

namespace daw::core {

// ── Pure timeline math ───────────────────────────────────────────────────────
// All helpers take primitive (sampleRate, bpm) inputs — no CoreState, no
// ProjectData. The app keeps a thin adapter that forwards from CoreState.
//
// Contract:
//   - sampleRate <= 0 is treated as 1 (defensive; never divide-by-zero).
//   - bpm        <= 0 is treated as 1.0f (same).
//   - Conversions round to nearest using llround for frame counts.
//   - All functions are pure, allocation-free, and RT-safe.

float SamplesPerBeat(int sampleRate, float bpm);
std::uint64_t FramesFromBeats(int sampleRate, float bpm, float beat);
float BeatsFromFrames(int sampleRate, float bpm, std::uint64_t frame);

} // namespace daw::core
