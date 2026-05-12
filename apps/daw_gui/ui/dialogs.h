#pragma once

#ifdef DAW_PUBLIC_API
#error "This header is internal-only."
#endif

#define NOMINMAX
#include <windows.h>

#include "AppState.h"

// Modal "Audio Settings" dialog (backend, output/input device, device sample
// rate, buffer size, with Apply/Cancel/OK). Settings live on
// state.audio.* and require the audio device to be reopened to take effect.
void UiShowAudioSettingsDialog(HWND hwndParent, AppState& state);

// Modal "Project Sample Rate" dialog. Project sample rate is independent of
// the audio device; mismatches trigger real-time SRC at playback time.
// Range 8000 - 384000 Hz. On confirm: writes state.core.project.projectSampleRate
// under audioStateLock and sets projectModified.
void UiShowProjectSampleRateDialog(HWND hwndParent, AppState& state);
