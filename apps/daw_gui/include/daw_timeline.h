#pragma once

#include <cstdint>

struct AppState;

float SamplesPerBeat(const AppState& app);
std::uint64_t FramesFromBeats(const AppState& app, float beat);
float BeatsFromFrames(const AppState& app, std::uint64_t frame);
