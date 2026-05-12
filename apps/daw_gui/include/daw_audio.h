#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct HWND__;
using HWND = HWND__*;

struct AppState;
struct AudioRuntimeState;
struct CoreState;
#include "core/audio_clip.h"
using LoadedAudio = daw::core::LoadedAudio;
enum class AudioBackend;

bool EngineInit(AppState& state);
void EngineShutdown(AppState& state);

bool StartPlayback(HWND hwnd, AppState& state);
void StopPlayback(AppState& state, bool rewind);
bool StartRecording(HWND hwnd, AppState& state);
void StopRecording(AppState& state, bool commitTake);

bool RenderFullMixToStereoLocked(const AppState& state, std::vector<float>* outStereo, int* outSampleRate);

// Non-backend-specific device control and diagnostics.
void DeviceRefreshInputDevices(AppState& state);
void DeviceRefreshOutputDevices(AppState& state);
std::wstring DeviceBuildAudioDiagnosticsReport(const AppState& state);
const wchar_t* DeviceAudioBackendLabel(AudioBackend backend);
std::uint64_t DeviceGetRenderedPlaybackFrame(const AppState& state);
int DeviceProbeCurrentOutputSampleRate(const AudioRuntimeState& audio);

// Engine lifecycle (see audio/device_common.h for full docs).
bool AudioInitializeRuntime(HWND hwnd, CoreState& core, AudioRuntimeState& audio);
bool AudioEnsureReadyForTransport(CoreState& core, AudioRuntimeState& audio);

bool DeviceStartPlaybackBackend(HWND hwnd, AppState& state);
void DeviceStopPlaybackBackend(AppState& state);
bool DeviceStartRecordingBackend(HWND hwnd, AppState& state, int armedTrack, bool wasPlaying);
void DeviceStopRecordingBackend(AppState& state);

bool IoLoadWavStereo(const std::wstring& path, LoadedAudio* out, std::wstring* error);
bool IoWriteWavPcm16Stereo(const std::wstring& path, const std::vector<float>& stereo, int sampleRate);
