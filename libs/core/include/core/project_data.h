#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "core/audio_clip.h"
#include "dsp/insert_types.h"

namespace daw::core {

// ── Persistent project data model ────────────────────────────────────────────
// The on-disk shape of a project: tracks, buses, audio sources, and their
// timeline placements. Everything in here serializes through io/project_io.
//
// `kProjectMaxInsertSlots` mirrors the dsp constant `kMaxInsertSlots` so
// changing the dsp constant naturally widens the project model. Tracks/buses
// store insert data as fixed-size arrays even though only the first
// `insertSlots` entries are live — keeping the array fixed avoids per-slot
// allocations on the realtime path.
//
// `kProjectBusCount` is the number of mix buses every project has. The default
// constructor for `ProjectData` pre-fills `buses` to this length.

constexpr int kProjectMaxInsertSlots = kMaxInsertSlots;
constexpr int kProjectBusCount = 4;

using ProjectInsertEffectArray = std::array<std::uint8_t, kProjectMaxInsertSlots>;
using ProjectInsertBypassArray = std::array<bool, kProjectMaxInsertSlots>;
using ProjectInsertConfigArray = std::array<InsertConfig, kProjectMaxInsertSlots>;

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
    ProjectInsertConfigArray insertConfig;
};

struct BusData {
    std::wstring name;
    float gainDb {0.0f};
    bool mute {false};
    float pan {0.0f};
    int insertSlots {0};
    ProjectInsertEffectArray insertEffects;
    ProjectInsertBypassArray insertBypass;
    ProjectInsertConfigArray insertConfig;
};

struct ProjectData {
    // Project metadata
    float bpm {120.0f};
    // Project sample rate. This is the storage rate for ALL clips, recordings,
    // and timeline math. It is set once at project creation (or via the
    // Project > Sample Rate menu) and is independent of the audio device's SR.
    // Device code MUST NOT mutate this field. Default 48000 matches the modern
    // industry norm and most audio interfaces.
    int projectSampleRate {48000};

    // Persistent tracks and buses
    std::vector<TrackData> tracks;
    std::vector<BusData> buses;  // size == kProjectBusCount

    // Audio files and clips
    std::vector<LoadedAudio> audio;
    std::vector<ClipItem> clips;

    ProjectData() {
        buses.reserve(kProjectBusCount);
        for (int i = 0; i < kProjectBusCount; ++i) {
            buses.push_back(BusData{});
        }
    }
};

} // namespace daw::core
