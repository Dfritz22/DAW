#pragma once

#ifdef DAW_PUBLIC_API
#error "This header is internal-only."
#endif

#include <cstdint>
#include <string>
#include <vector>

#include "core/audio_clip.h"
#include "core/automation_types.h"

constexpr int kBusCount = 4;

// Historical app-side names. The underlying types live in libs/core; these
// aliases keep every existing callsite (main.cpp, ui/draw.cpp, audio/engine.cpp,
// io/project_io.cpp, ...) compiling unchanged.
using LoadedAudio = daw::core::LoadedAudio;
using ClipItem    = daw::core::ClipItem;

constexpr std::uint32_t CoreRgb(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    return daw::core::Rgb(r, g, b);
}

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
