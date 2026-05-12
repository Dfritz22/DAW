#pragma once

#include "AppState.h"

#include <windows.h>

// Tab drag activation threshold (pixels, in unscaled UI units — caller
// must DPI-scale via Dpi() if comparing pixel deltas).
constexpr int kDragTabThresholdPx = 4;

// Hit-test `pt` against state.ui.dockLayout / dockTabs. Writes the resolved
// drop target into state.ui.drop* fields (dropTargetLeaf, dropTargetSide,
// dropTargetTabAt, dropPreviewRect) and returns true if a target was found.
//
// Per-leaf zones (Unity/VS-style):
//   * Outer 1/4 band on each edge of the dock-bounds → split the root.
//   * Edge band on each leaf → split that leaf.
//   * Center → tab-insert at the cursor's X within the leaf's tab strip
//     (or end-of-list if past the last tab).
bool ResolveDropTarget(AppState& state, POINT pt);
