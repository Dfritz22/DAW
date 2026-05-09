#pragma once
#include "core/state.h"

// ── Backend-agnostic audio device utilities ───────────────────────────────────
// This module owns all logic that is shared across MME and WASAPI backends.
// It must not include any UI headers (ui/layout.h, draw.h, etc.).

// ── Backend labels ────────────────────────────────────────────────────────────
const wchar_t*  AudioBackendLabel(AudioBackend backend);
std::string     AudioBackendToJson(AudioBackend backend);
AudioBackend    AudioBackendFromJson(const std::string& value);

// ── Device enumeration ────────────────────────────────────────────────────────
void RefreshInputDevices(UiState& state);
void RefreshOutputDevices(UiState& state);

// ── Diagnostics ───────────────────────────────────────────────────────────────
std::wstring BuildAudioDiagnosticsReport(const UiState& state);

// ── Playback cursor ───────────────────────────────────────────────────────────
// Returns the current absolute project-frame position of the playhead,
// taking into account both WASAPI and MME waveOut timing paths.
std::uint64_t GetRenderedPlaybackFrame(const UiState& state);
