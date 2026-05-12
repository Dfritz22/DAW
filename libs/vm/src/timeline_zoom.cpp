#include "vm/timeline_zoom.h"

#include <algorithm>
#include <cmath>

namespace daw::vm {

namespace {
inline float ClampVisible(float v) {
    if (!std::isfinite(v)) return kDefaultViewBeats;
    return std::clamp(v, kMinViewBeats, kMaxViewBeats);
}
} // namespace

float ZoomVisible(float visible, float factor) {
    return ClampVisible(visible * factor);
}

ZoomAroundResult ZoomVisibleAround(float viewStartBeat,
                                    float visible,
                                    float focusBeat,
                                    float factor)
{
    const float oldVisible = (visible > 0.0f && std::isfinite(visible)) ? visible : 1.0f;
    const float newVisible = ClampVisible(visible * factor);

    // Pin `focusBeat` to its current screen-x: ratio is the fractional
    // position of focusBeat across the old viewport [0..1].
    const float ratio = (focusBeat - viewStartBeat) / oldVisible;
    float newStart = focusBeat - ratio * newVisible;
    if (!std::isfinite(newStart)) newStart = 0.0f;
    if (newStart < 0.0f) newStart = 0.0f;

    return ZoomAroundResult{newStart, newVisible};
}

float FitVisibleToContent(float projectEndBeat) {
    const float padded = (std::isfinite(projectEndBeat) ? projectEndBeat : 0.0f) + kFitTailPaddingBeats;
    return std::clamp(std::max(kFitMinViewBeats, padded), kFitMinViewBeats, kMaxViewBeats);
}

bool PlayheadIsPastViewRight(float viewStartBeat, float visible, float playheadBeat) {
    const float viewRight = viewStartBeat + visible;
    return playheadBeat > viewRight - 1.0f;
}

float AutoScrollViewStart(float visible, float playheadBeat) {
    float v = playheadBeat - visible * 0.75f;
    if (!std::isfinite(v) || v < 0.0f) v = 0.0f;
    return v;
}

ResetViewResult ResetView() {
    return ResetViewResult{0.0f, kDefaultViewBeats};
}

float PanViewStartBeat(float viewStartBeat, float deltaBeats) {
    const float v = viewStartBeat + deltaBeats;
    if (!std::isfinite(v) || v < 0.0f) return 0.0f;
    return v;
}

} // namespace daw::vm
