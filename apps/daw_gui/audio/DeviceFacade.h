#pragma once

#ifdef DAW_PUBLIC_API
#error "This header is internal-only."
#endif

#include "AppState.h"

namespace daw::audio {

bool StartPlaybackBackend(AppState& state);
void StopPlaybackBackend(AppState& state);
bool StartRecordingBackend(AppState& state, int armedTrack, bool wasPlaying);
void StopRecordingBackend(AppState& state);

} // namespace daw::audio
