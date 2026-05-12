#include "core/track_audibility.h"

#include <cstddef>

namespace daw::core {

bool AnyTrackSoloed(const std::vector<TrackData>& tracks) {
    for (const auto& t : tracks) {
        if (t.solo) return true;
    }
    return false;
}

bool IsTrackAudible(const std::vector<TrackData>& tracks, int trackIndex) {
    if (trackIndex < 0 || trackIndex >= static_cast<int>(tracks.size())) {
        return false;
    }
    const auto& t = tracks[static_cast<std::size_t>(trackIndex)];
    if (t.mute) return false;
    if (AnyTrackSoloed(tracks) && !t.solo) return false;
    return true;
}

} // namespace daw::core
