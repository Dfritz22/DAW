#pragma once

#include <cstdint>

struct UiState;

float SamplesPerBeat(const UiState& state);
std::uint64_t FramesFromBeats(const UiState& state, float beat);
float BeatsFromFrames(const UiState& state, std::uint64_t frame);
