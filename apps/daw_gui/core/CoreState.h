#pragma once

#ifdef DAW_PUBLIC_API
#error "This header is internal-only."
#endif

#include <cstdint>
#include <string>
#include <vector>

#include "core/automation_types.h"

constexpr int kBusCount = 4;

constexpr std::uint32_t CoreRgb(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    return static_cast<std::uint32_t>(r)
         | (static_cast<std::uint32_t>(g) << 8)
         | (static_cast<std::uint32_t>(b) << 16);
}

struct LoadedAudio {
    std::wstring sourcePath;
    std::wstring displayName;
    int sampleRate {0};
    std::uint32_t frames {0};
    std::vector<float> stereo;
};

struct ClipItem {
    int trackIndex {0};
    int audioIndex {-1};
    float startBeat {0.0f};
    float lengthBeats {4.0f};
    std::uint32_t color {CoreRgb(88, 131, 199)};
    std::wstring name;
    std::uint64_t sourceOffsetFrames {0};
};

#include "core/ProjectData.h"

struct CoreState {
    struct UndoEntry {
        std::vector<ClipItem> clips;
        std::vector<float> trackGainDb;
    };

    // Persistent project data model and metadata
    ProjectData project;
    std::wstring projectFilePath;
    bool projectModified {false};

    // Undo / redo for timeline edits
    std::vector<UndoEntry> undoStack;
    std::vector<UndoEntry> redoStack;

    // Track automation curves (single source of truth for automation data)
    std::vector<TrackAutomationCurve> trackGainCurves;
    std::vector<TrackAutomationCurve> trackPanCurves;
    std::vector<TrackAutomationCurve> trackBusCurves;
};
