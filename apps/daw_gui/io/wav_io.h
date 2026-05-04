#pragma once
#include <string>
#include <vector>
#include "../state.h"

// WAV file I/O helpers.
// LoadedAudio is defined in state.h.

bool LoadWavStereo(const std::wstring& path, LoadedAudio* out, std::wstring* error);
bool WriteWavPcm16Stereo(const std::wstring& path, const std::vector<float>& stereo, int sampleRate);
