#pragma once

#ifdef DAW_PUBLIC_API
#error "This header is internal-only."
#endif

// ── apps/daw_gui WAV adaptor ────────────────────────────────────────────────
// Thin wrapper around `daw::io::wav` that translates between the lib's
// pure POD `StereoBuffer` and the app's richer `LoadedAudio` (which
// carries source path + display name and is mutated in place by the rest
// of the app).
//
// The codec itself lives in libs/io. All format logic, error mapping, and
// future format support belongs there; this file only adapts types.

#include <string>
#include <vector>

#include "core/audio_clip.h"
using LoadedAudio = daw::core::LoadedAudio;

bool IoLoadWavStereo(const std::wstring& path, LoadedAudio* out, std::wstring* error);
bool IoWriteWavPcm16Stereo(const std::wstring& path, const std::vector<float>& stereo, int sampleRate);
