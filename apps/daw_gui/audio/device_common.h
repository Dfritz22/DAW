#pragma once

#ifdef DAW_PUBLIC_API
#error "This header is internal-only."
#endif

#include <cstdint>
#include <string>

enum class AudioBackend;
struct CoreState;
struct AudioRuntimeState;
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
void DeviceRefreshInputDevices(AudioRuntimeState& audio);
void DeviceRefreshOutputDevices(AudioRuntimeState& audio);

// ── Diagnostics ───────────────────────────────────────────────────────────────
std::wstring DeviceBuildAudioDiagnosticsReport(const CoreState& core, const AudioRuntimeState& audio);
int DeviceProbeCurrentOutputSampleRate(const AudioRuntimeState& audio);

// ── Engine lifecycle ──────────────────────────────────────────────────────────
// Single entry point that performs all "before the UI accepts transport" setup:
//   - enumerates input/output devices,
//   - probes a usable sample rate,
//   - seeds CoreState.projectSampleRate if unset,
//   - initializes the audio state critical section,
//   - transitions audio.engineState to Ready (or Error on failure).
// Safe to call once at startup. Returns true on success.
bool AudioInitializeRuntime(HWND hwnd, CoreState& core, AudioRuntimeState& audio);

// Invariant guard called at the top of StartPlayback / StartRecording.
// Repairs missing sample rate, verifies engineState != Error, and ensures
// projectSampleRate > 0. Returns true if the engine is safe to start a
// transport command. On false, audio.engineInitError contains a reason.
bool AudioEnsureReadyForTransport(CoreState& core, AudioRuntimeState& audio);

// ── Playback cursor ───────────────────────────────────────────────────────────
// Returns the current absolute project-frame position of the playhead,
// taking into account both WASAPI and MME waveOut timing paths.
std::uint64_t DeviceGetRenderedPlaybackFrame(const CoreState& core, const AudioRuntimeState& audio);

// ── Backend-agnostic start/stop entry points for orchestration layers ───────
bool DeviceStartPlaybackBackend(HWND hwnd, CoreState& core, AudioRuntimeState& audio, float playheadBeat);
void DeviceStopPlaybackBackend(AudioRuntimeState& audio);
bool DeviceStartRecordingBackend(HWND hwnd, CoreState& core, AudioRuntimeState& audio, int armedTrack, bool wasPlaying, float playheadBeat);
void DeviceStopRecordingBackend(AudioRuntimeState& audio);
