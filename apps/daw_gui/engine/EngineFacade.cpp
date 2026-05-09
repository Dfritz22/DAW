#include "engine/EngineFacade.h"

#include "audio/engine.h"
#include "daw_audio.h"

namespace daw::engine {

bool EngineInit(AppState& state) {
    return ::EngineInit(state);
}

void EngineShutdown(AppState& state) {
    ::EngineShutdown(state);
}

bool EngineFillRealtime(AppState& state, std::int16_t* outInterleaved, int deviceFrames, int deviceSampleRate, bool* reachedEnd) {
    return EngineFillRealtimeForDeviceLocked(state.core, state.audio, outInterleaved, deviceFrames, deviceSampleRate, reachedEnd);
}

bool RenderFullMix(AppState& state, std::vector<float>* outStereo, int* outSampleRate) {
    return RenderFullMixToStereoLocked(state.core, state.audio, outStereo, outSampleRate);
}

} // namespace daw::engine
