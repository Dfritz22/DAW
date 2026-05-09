#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct HWND__;
using HWND = HWND__*;

struct UiState;
struct LoadedAudio;
enum class AudioBackend;

bool EngineInit(UiState& state);
void EngineShutdown(UiState& state);

bool StartPlayback(HWND hwnd, UiState& state);
void StopPlayback(UiState& state, bool rewind);
bool StartRecording(HWND hwnd, UiState& state);
void StopRecording(UiState& state, bool commitTake);

bool RenderFullMixToStereoLocked(const UiState& state, std::vector<float>* outStereo, int* outSampleRate);

// Non-backend-specific device control and diagnostics.
void DeviceRefreshInputDevices(UiState& state);
void DeviceRefreshOutputDevices(UiState& state);
std::wstring DeviceBuildAudioDiagnosticsReport(const UiState& state);
const wchar_t* DeviceAudioBackendLabel(AudioBackend backend);
std::uint64_t DeviceGetRenderedPlaybackFrame(const UiState& state);

bool DeviceStartPlaybackBackend(HWND hwnd, UiState& state);
void DeviceStopPlaybackBackend(UiState& state);
bool DeviceStartRecordingBackend(HWND hwnd, UiState& state, int armedTrack, bool wasPlaying);
void DeviceStopRecordingBackend(UiState& state);

bool LoadWavStereo(const std::wstring& path, LoadedAudio* out, std::wstring* error);
bool WriteWavPcm16Stereo(const std::wstring& path, const std::vector<float>& stereo, int sampleRate);
