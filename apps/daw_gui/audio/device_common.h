#pragma once

#include <cstdint>
#include <string>

enum class AudioBackend;
struct UiState;
struct HWND__;
using HWND = HWND__*;

// ── Backend-agnostic audio device utilities ───────────────────────────────────
// This module owns all logic that is shared across MME and WASAPI backends.
// It must not include any UI headers (ui/layout.h, draw.h, etc.).

// ── Backend labels ────────────────────────────────────────────────────────────
const wchar_t*  DeviceAudioBackendLabel(AudioBackend backend);
std::string     DeviceAudioBackendToJson(AudioBackend backend);
AudioBackend    DeviceAudioBackendFromJson(const std::string& value);

// ── Device enumeration ────────────────────────────────────────────────────────
void DeviceRefreshInputDevices(UiState& state);
void DeviceRefreshOutputDevices(UiState& state);

// ── Diagnostics ───────────────────────────────────────────────────────────────
std::wstring DeviceBuildAudioDiagnosticsReport(const UiState& state);

// ── Playback cursor ───────────────────────────────────────────────────────────
// Returns the current absolute project-frame position of the playhead,
// taking into account both WASAPI and MME waveOut timing paths.
std::uint64_t DeviceGetRenderedPlaybackFrame(const UiState& state);

// ── Backend-agnostic start/stop entry points for orchestration layers ───────
bool DeviceStartPlaybackBackend(HWND hwnd, UiState& state);
void DeviceStopPlaybackBackend(UiState& state);
bool DeviceStartRecordingBackend(HWND hwnd, UiState& state, int armedTrack, bool wasPlaying);
void DeviceStopRecordingBackend(UiState& state);
