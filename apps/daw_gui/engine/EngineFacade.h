#pragma once

#ifdef DAW_PUBLIC_API
#error "This header is internal-only."
#endif

#include "AppState.h"

#include <cstdint>
#include <vector>

namespace daw::engine {

bool EngineInit(AppState& state);
void EngineShutdown(AppState& state);
bool EngineFillRealtime(AppState& state, std::int16_t* outInterleaved, int deviceFrames, int deviceSampleRate, bool* reachedEnd);
bool RenderFullMix(AppState& state, std::vector<float>* outStereo, int* outSampleRate);

} // namespace daw::engine
