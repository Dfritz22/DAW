#pragma once

#include "core/project_data.h"

#include <vector>

namespace daw::core {

// True if any track in `tracks` has `solo == true`.
bool AnyTrackSoloed(const std::vector<TrackData>& tracks);

// Pure mute/solo audibility rule:
//   - returns false when `trackIndex` is out of range
//   - returns false when the track is muted
//   - if any track is soloed, only soloed tracks are audible
//   - otherwise the track is audible
bool IsTrackAudible(const std::vector<TrackData>& tracks, int trackIndex);

} // namespace daw::core
