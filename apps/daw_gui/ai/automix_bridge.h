#pragma once
#include "core/state.h"

// ── AutoMix per-track settings returned by the AI script ──────────────────
struct AutoMixTrackSettings {
    float gainDb {0.0f};
    float pan {0.0f};
    int busIndex {1};

    float highpassHz {0.0f};
    float lowShelfDb {0.0f};
    float lowMidDb {0.0f};
    float presenceDb {0.0f};
    float airDb {0.0f};
    float deesserDb {0.0f};

    float compThresholdDb {-24.0f};
    float compRatio {1.0f};
    float compMakeupDb {0.0f};

    float reverbDecayS {0.0f};
    float reverbPreDelayMs {0.0f};
    float reverbMix {0.0f};
    float delayTimeMs {0.0f};
    float delayFeedback {0.0f};
    float delayMix {0.0f};
    float saturationDriveDb {0.0f};
    float saturationBlend {0.0f};
};

// ── AutoMix master-bus settings returned by the AI script ─────────────────
struct AutoMixMasterSettings {
    bool hasCompressor {false};
    float compThresholdDb {-12.0f};
    float compRatio {2.5f};
    float compMakeupDb {1.5f};
};

// ── Public API ────────────────────────────────────────────────────────────

// Parse a legacy gain-only JSON output file from the AI script.
bool ParseAutoMixGains(const std::filesystem::path& jsonPath,
                       std::map<std::wstring, float>* outGains);

// Parse a full per-track settings JSON output file from the AI script.
bool ParseAutoMixSettings(const std::filesystem::path& jsonPath,
                          std::map<std::wstring, AutoMixTrackSettings>* outTracks,
                          AutoMixMasterSettings* outMaster);

// Export stems, run the Python AutoMix script, and apply results to state.
bool ApplyAutoMixToFaders(HWND hwnd, UiState& state);

// Launch ApplyAutoMixToFaders on a background thread (non-blocking).
void StartAutoMixAsync(HWND hwnd, UiState& state);

// Thread procedure used by StartAutoMixAsync.
DWORD WINAPI AutoMixThreadProc(LPVOID param);

// Render the selected track to a temp WAV, run vocal_check, show the report.
bool AnalyzeSelectedTrackQuality(HWND hwnd, UiState& state);
