#pragma once

// Phase 14: pure zoom / pan math for the timeline viewport.
//
// Six sites in apps/daw_gui/main.cpp + the View menu used to inline the same
// zoom/clamp formulas. They now route through these helpers so a future
// tweak (e.g. raising the max-zoom-out beats) lands everywhere at once.
//
// Conventions:
//   "visible" is the number of beats currently fitting in the arrange rect
//   ("viewBeatsVisible" in AppState).
//   "viewStartBeat" is the beat at the left edge of the arrange rect.
//   factor < 1.0  → zoom in  (fewer beats visible)
//   factor > 1.0  → zoom out (more beats visible)

namespace daw::vm {

// Hard limits on viewBeatsVisible. Picked to match the legacy inline clamps
// at every callsite; do not loosen without auditing the whole app.
constexpr float kMinViewBeats     = 4.0f;
constexpr float kMaxViewBeats     = 128.0f;
constexpr float kDefaultViewBeats = 32.0f;

// Auto-fit lower bound: even an empty project shows at least 16 beats so
// the user has somewhere to drop the first clip.
constexpr float kFitMinViewBeats  = 16.0f;
// Padding (beats) added past the project's last clip when auto-fitting.
constexpr float kFitTailPaddingBeats = 4.0f;

// Keyboard / menu zoom step (legacy 0.85 in / 1.15 out).
constexpr float kKeyZoomInFactor  = 0.85f;
constexpr float kKeyZoomOutFactor = 1.15f;

// Mouse-wheel zoom step (legacy 0.9 in / 1.1 out — finer than keyboard so
// the wheel feels analog).
constexpr float kWheelZoomInFactor  = 0.9f;
constexpr float kWheelZoomOutFactor = 1.1f;

// Multiplies `visible` by `factor` and clamps into [kMinViewBeats,
// kMaxViewBeats]. Pure; safe for any factor including 1.0 / 0.0 / NaN.
float ZoomVisible(float visible, float factor);

// Result of a zoom-around-focus operation (mouse wheel over a specific
// beat). The beat under `focusBeat` stays at the same screen-x; the
// caller-side viewStartBeat shifts to make that pin true.
struct ZoomAroundResult {
    float viewStartBeat;
    float viewBeatsVisible;
};

// Zooms by `factor` while pinning `focusBeat` to its current pixel
// position. Used by the Ctrl+Wheel handler; equivalent to the legacy
// "ratio = (focus - start) / oldVisible; newStart = focus - ratio *
// newVisible" math, with the new viewStartBeat clamped to ≥ 0.
ZoomAroundResult ZoomVisibleAround(float viewStartBeat,
                                    float visible,
                                    float focusBeat,
                                    float factor);

// Computes the auto-fit viewBeatsVisible for a project whose last content
// ends at `projectEndBeat` (in beats). Result is
// clamp(max(kFitMinViewBeats, projectEndBeat + kFitTailPaddingBeats),
//       kFitMinViewBeats, kMaxViewBeats).
float FitVisibleToContent(float projectEndBeat);

// Returns true if the playhead has scrolled past the right edge of the
// visible area minus a 1-beat margin (the legacy threshold). When true,
// the caller should snap viewStartBeat to AutoScrollViewStart(...).
bool  PlayheadIsPastViewRight(float viewStartBeat, float visible, float playheadBeat);

// New viewStartBeat that places the playhead at 25% from the right edge
// (i.e. shifts so the playhead now sits at viewStart + 0.75*visible).
// Caller is responsible for checking PlayheadIsPastViewRight first if it
// wants the legacy "only when off-screen" semantic.
float AutoScrollViewStart(float visible, float playheadBeat);

// Default reset: viewStartBeat = 0, viewBeatsVisible = kDefaultViewBeats.
struct ResetViewResult {
    float viewStartBeat;
    float viewBeatsVisible;
};
ResetViewResult ResetView();

// Pan helper: shifts viewStartBeat by `deltaBeats` and clamps to ≥ 0.
// The legacy wheel-pan inlines computed `step = visible * 0.08f` and added
// or subtracted; callers can keep that shape and pass the signed delta in.
float PanViewStartBeat(float viewStartBeat, float deltaBeats);

// Pan step matching the legacy wheel-pan rate (8% of visible).
constexpr float kWheelPanStepFraction = 0.08f;

} // namespace daw::vm
