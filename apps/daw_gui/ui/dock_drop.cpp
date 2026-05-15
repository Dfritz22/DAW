#include "ui/dock_drop.h"

#include "ui/dock.h" // DockDropSide
#include "ui/dpi.h"

#include <algorithm>
#include <windows.h>

bool ResolveDropTarget(AppState& state, POINT pt) {
    state.ui.dock.dropTargetLeaf  = nullptr;
    state.ui.dock.dropTargetSide  = daw::ui::DockDropSide::Center;
    state.ui.dock.dropTargetTabAt = -1;
    state.ui.dock.dropPreviewRect = RECT{0, 0, 0, 0};

    // ── Outer drop zone (split the root) ────────────────────────────────
    // Compute the dock area as the union of all current leaves. A thin band
    // along each edge means "split the WHOLE dock against this side", so a
    // panel can be pinned to the full bottom (under both Tracks AND Arrange)
    // by dropping it on the bottom outer band — without first needing a
    // single leaf that already spans the full width.
    if (!state.ui.dock.dockLayout.empty() && state.ui.dock.dockRoot) {
        RECT dockBounds = state.ui.dock.dockLayout.front().rect;
        for (const auto& leaf : state.ui.dock.dockLayout) {
            dockBounds.left   = std::min<LONG>(dockBounds.left,   leaf.rect.left);
            dockBounds.top    = std::min<LONG>(dockBounds.top,    leaf.rect.top);
            dockBounds.right  = std::max<LONG>(dockBounds.right,  leaf.rect.right);
            dockBounds.bottom = std::max<LONG>(dockBounds.bottom, leaf.rect.bottom);
        }
        const int outerBand = Dpi(16);
        if (PtInRect(&dockBounds, pt)) {
            daw::ui::DockDropSide outer = daw::ui::DockDropSide::Center;
            if      (pt.x < dockBounds.left   + outerBand) outer = daw::ui::DockDropSide::Left;
            else if (pt.x > dockBounds.right  - outerBand) outer = daw::ui::DockDropSide::Right;
            else if (pt.y < dockBounds.top    + outerBand) outer = daw::ui::DockDropSide::Top;
            else if (pt.y > dockBounds.bottom - outerBand) outer = daw::ui::DockDropSide::Bottom;
            if (outer != daw::ui::DockDropSide::Center) {
                state.ui.dock.dropTargetLeaf = state.ui.dock.dockRoot.get();
                state.ui.dock.dropTargetSide = outer;
                const int w = dockBounds.right  - dockBounds.left;
                const int h = dockBounds.bottom - dockBounds.top;
                RECT preview = dockBounds;
                if      (outer == daw::ui::DockDropSide::Left)   preview.right  = dockBounds.left   + w / 4;
                else if (outer == daw::ui::DockDropSide::Right)  preview.left   = dockBounds.right  - w / 4;
                else if (outer == daw::ui::DockDropSide::Top)    preview.bottom = dockBounds.top    + h / 4;
                else /* Bottom */                                 preview.top    = dockBounds.bottom - h / 4;
                state.ui.dock.dropPreviewRect = preview;
                return true;
            }
        }
    }

    for (const auto& leaf : state.ui.dock.dockLayout) {
        if (!PtInRect(&leaf.rect, pt)) continue;
        const RECT r = leaf.rect;
        const int  w = r.right  - r.left;
        const int  h = r.bottom - r.top;
        const int  edge = std::min({w / 4, h / 4, Dpi(60)});

        // Don't allow a single-tab leaf to split against itself (no-op).
        const bool sameAsSource = (leaf.node == state.ui.dock.dragTabSource);

        // Edge bands (split)
        daw::ui::DockDropSide side = daw::ui::DockDropSide::Center;
        if      (pt.x < r.left   + edge) side = daw::ui::DockDropSide::Left;
        else if (pt.x > r.right  - edge) side = daw::ui::DockDropSide::Right;
        else if (pt.y < r.top    + edge) side = daw::ui::DockDropSide::Top;
        else if (pt.y > r.bottom - edge) side = daw::ui::DockDropSide::Bottom;

        if (side != daw::ui::DockDropSide::Center) {
            if (sameAsSource && state.ui.dock.dragTabSource != nullptr &&
                state.ui.dock.dragTabSource->panels.size() <= 1) {
                // Splitting a leaf containing only the dragged tab against
                // itself would be a no-op; skip and let center handle it.
                side = daw::ui::DockDropSide::Center;
            } else {
                state.ui.dock.dropTargetLeaf = leaf.node;
                state.ui.dock.dropTargetSide = side;
                RECT preview = r;
                if      (side == daw::ui::DockDropSide::Left)   preview.right  = r.left   + w / 2;
                else if (side == daw::ui::DockDropSide::Right)  preview.left   = r.right  - w / 2;
                else if (side == daw::ui::DockDropSide::Top)    preview.bottom = r.top    + h / 2;
                else /* Bottom */                                preview.top    = r.bottom - h / 2;
                state.ui.dock.dropPreviewRect = preview;
                return true;
            }
        }

        // Center (tab insert) — allowed on any leaf, including primary,
        // so users can dock new tabs alongside Ruler/Tracks/Arrange.
        state.ui.dock.dropTargetLeaf = leaf.node;
        state.ui.dock.dropTargetSide = daw::ui::DockDropSide::Center;

        // Find insertion index by scanning the tabs that belong to this leaf.
        int insertAt = static_cast<int>(leaf.node->panels.size());
        for (const auto& tab : state.ui.dock.dockTabs) {
            if (tab.node != leaf.node) continue;
            const int mid = (tab.rect.left + tab.rect.right) / 2;
            if (pt.x < mid) { insertAt = tab.tabIndex; break; }
        }
        // If dragging within the same leaf, account for the tab being removed
        // before re-insertion so the visual index matches.
        if (sameAsSource && insertAt > state.ui.dock.dragTabIndex) insertAt -= 1;
        state.ui.dock.dropTargetTabAt = insertAt;

        // Center preview = full leaf rect (signals "join this leaf as a tab").
        // Edge previews fill half the leaf, so the two are visually distinct.
        state.ui.dock.dropPreviewRect = r;
        return true;
    }
    return false;
}
