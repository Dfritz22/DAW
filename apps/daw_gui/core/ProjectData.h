#pragma once

#ifdef DAW_PUBLIC_API
#error "This header is internal-only."
#endif

// Persistent project data lives in libs/core. The app keeps this thin
// alias header so existing callsites (TrackData, BusData, ProjectData,
// PROJECT_BUS_COUNT, etc.) keep compiling unchanged.

#include "core/project_data.h"

using TrackData    = daw::core::TrackData;
using BusData      = daw::core::BusData;
using ProjectData  = daw::core::ProjectData;

using ProjectInsertEffectArray = daw::core::ProjectInsertEffectArray;
using ProjectInsertBypassArray = daw::core::ProjectInsertBypassArray;
using ProjectInsertConfigArray = daw::core::ProjectInsertConfigArray;

constexpr int PROJECT_MAX_INSERT_SLOTS = daw::core::kProjectMaxInsertSlots;
constexpr int PROJECT_BUS_COUNT        = daw::core::kProjectBusCount;
