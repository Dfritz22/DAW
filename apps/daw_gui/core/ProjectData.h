#pragma once
#include <vector>
#include <string>
#include <cstdint>
#include <array>

// Forward declarations of types from state.h (avoid circular includes)
struct InsertParams;
struct LoadedAudio;
struct ClipItem;

// ── Persistent project data model ────────────────────────────────────────────
// Contains all project-level data that is saved/loaded from .dawproj files.
// This is separate from UI state (viewport, selection, editing modes, etc.)
// and audio device state (backend, sample rate preference, device names, etc.)

// Note: These match the constants in state.h but we can't include it to avoid circular dependency
constexpr int PROJECT_MAX_INSERT_SLOTS = 8;
constexpr int PROJECT_BUS_COUNT = 4;

using ProjectInsertEffectArray = std::array<std::uint8_t, PROJECT_MAX_INSERT_SLOTS>;
using ProjectInsertBypassArray = std::array<bool, PROJECT_MAX_INSERT_SLOTS>;
using ProjectInsertParamsArray = std::array<InsertParams, PROJECT_MAX_INSERT_SLOTS>;

struct TrackData {
    std::wstring name;
    float gainDb {0.0f};
    bool mute {false};
    bool solo {false};
    bool recordArm {false};
    int busIndex {1};  // default to Music bus
    float pan {0.0f};
    int insertSlots {0};
    ProjectInsertEffectArray insertEffects;
    ProjectInsertBypassArray insertBypass;
    ProjectInsertParamsArray insertParams;
};

struct BusData {
    std::wstring name;
    float gainDb {0.0f};
    bool mute {false};
    float pan {0.0f};
    int insertSlots {0};
    ProjectInsertEffectArray insertEffects;
    ProjectInsertBypassArray insertBypass;
    ProjectInsertParamsArray insertParams;
};

struct ProjectData {
    // Project metadata
    float bpm {120.0f};
    int projectSampleRate {0};

    // Persistent tracks and buses
    std::vector<TrackData> tracks;
    std::vector<BusData> buses;  // Fixed size PROJECT_BUS_COUNT, but stored in vector

    // Audio files and clips
    std::vector<LoadedAudio> audio;
    std::vector<ClipItem> clips;

    ProjectData() {
        // Initialize buses with defaults for PROJECT_BUS_COUNT
        buses.reserve(PROJECT_BUS_COUNT);
        for (int i = 0; i < PROJECT_BUS_COUNT; ++i) {
            buses.push_back(BusData{});
        }
    }
};
