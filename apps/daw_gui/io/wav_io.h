#pragma once

#ifdef DAW_PUBLIC_API
#error "This header is internal-only."
#endif

#include <string>
#include <vector>

struct LoadedAudio;

// WAV file I/O helpers.
// LoadedAudio is defined in state.h.

bool IoLoadWavStereo(const std::wstring& path, LoadedAudio* out, std::wstring* error);
bool IoWriteWavPcm16Stereo(const std::wstring& path, const std::vector<float>& stereo, int sampleRate);
