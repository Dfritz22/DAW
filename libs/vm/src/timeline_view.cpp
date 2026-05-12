#include "vm/timeline_view.h"

#include <algorithm>
#include <cmath>

namespace daw::vm {

int BeatToXIn(const Rect& area, const TimelineViewport& vp, float beat) {
    const int width = std::max(1, area.right - area.left);
    const float visible = (vp.viewBeatsVisible > 0.0f) ? vp.viewBeatsVisible : 1.0f;
    const float t = (beat - vp.viewStartBeat) / visible;
    return area.left + static_cast<int>(t * static_cast<float>(width));
}

int BeatToX(const TimelineViewport& vp, float beat) {
    return BeatToXIn(vp.arrange, vp, beat);
}

float XToBeat(const TimelineViewport& vp, int x) {
    const int width = std::max(1, vp.arrange.right - vp.arrange.left);
    const float t = static_cast<float>(x - vp.arrange.left) / static_cast<float>(width);
    return vp.viewStartBeat + t * vp.viewBeatsVisible;
}

int TrackIndexFromY(const TimelineViewport& vp, int y) {
    if (vp.trackCount <= 0) return 0;
    if (y < vp.arrange.top) return 0;
    const int rowH = (vp.rowHeightPx > 0) ? vp.rowHeightPx : 1;
    const int idx = (y - vp.arrange.top + vp.tracksScrollY) / rowH;
    return std::clamp(idx, 0, vp.trackCount - 1);
}

bool ClipRectForDraw(const TimelineViewport& vp,
                     int trackIndex,
                     float startBeat,
                     float lengthBeats,
                     Rect* outRect)
{
    if (outRect == nullptr) return false;

    const int rowH    = vp.rowHeightPx;
    const int insetY  = vp.clipInsetYPx;
    const int rowTop  = vp.arrange.top + trackIndex * rowH + insetY - vp.tracksScrollY;
    const int rowBot  = rowTop + (rowH - 2 * insetY);
    const int left    = BeatToX(vp, startBeat);
    const int right   = BeatToX(vp, startBeat + lengthBeats);

    Rect r{left, rowTop, right, rowBot};

    if (r.right < vp.arrange.left || r.left > vp.arrange.right ||
        r.bottom < vp.arrange.top || r.top   > vp.arrange.bottom) {
        return false;
    }

    r.left  = std::max(r.left,  vp.arrange.left);
    r.right = std::min(r.right, vp.arrange.right);
    *outRect = r;
    return true;
}

float SnapBeat(float beat) {
    constexpr float kGrid = 0.25f;
    return std::round(beat / kGrid) * kGrid;
}

} // namespace daw::vm
