#pragma once
#include "../state.h"

// ── Audio-domain utilities ────────────────────────────────────────────────────
// These helpers are used exclusively by the audio engine (engine.cpp) and by
// call sites in main.cpp that coordinate the engine.  No UI or Win32 drawing
// code should include this header.

// dB / linear conversion
float DbToLinear(float db);

// Bus accessors (definitions move here; declarations also kept in state.h
// so that draw.cpp can call them without depending on this header).
// BusGainDbAt / BusPanAt / BusMuteAt are declared in state.h.

// DSP-state book-keeping
void EnsureInsertDspStateStorage(const UiState& state);

// Track audibility (mute / solo logic – audio domain)
bool IsTrackAudible(const UiState& state, int trackIndex);

// Project-length calculation
std::uint64_t ComputeProjectEndFrameLocked(const UiState& state);

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
