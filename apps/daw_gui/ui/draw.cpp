#include "ui/draw/draw_internal.h"  // Fill, StrokeRect, DrawButton, DrawPanKnob,
                                    // InsertEffectName (in daw::internal::ui)
#include "ui/draw.h"
#include "ui/layout.h"
#include "ui/dpi.h"
#include "ui/dock.h"
#include "daw_automation.h"
#include "daw_timeline.h"

#include <algorithm>
#include <cmath>

using namespace daw::internal::ui;

// UiDrawTopBar / UiDrawTransport / UiDrawStatusBar / UiDrawTools moved to
// ui/draw/chrome.cpp in Phase 17b.

// UiDrawGetInspectorPanelRect / UiDrawInsertInspector moved to
// ui/draw/inspector.cpp in Phase 17c.

// UiDrawLeftTrackPanel / UiDrawBusesPanel moved to ui/draw/mixer_strips.cpp in Phase 17d.

// UiDrawRuler / UiDrawArrangeLanes (and waveform helpers) moved to ui/draw/timeline.cpp in Phase 17e.

// UiDrawDockLeavesAndSplitters / UiDrawDockDropOverlay moved to ui/draw/dock_chrome.cpp in Phase 17f.

