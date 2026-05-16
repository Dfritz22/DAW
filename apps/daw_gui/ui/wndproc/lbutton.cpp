#include "ui/wndproc/lbutton.h"

#include "core/internal_app_services.h"
#include "ui/dock.h"
#include "ui/repaint.h"

// Forward decl — defined in main.cpp until floating-window code moves
// to ui/floating.{h,cpp}.
void SpawnFloatingPanel(AppState& state, daw::ui::PanelKind panel, POINT screenPt);

LRESULT WndProcOnLButtonUp(HWND hwnd, AppState& state) {
    using daw::internal::core::UpdateWindowTitle;

    bool changed = false;

    // ── Dock tab drag commit ────────────────────────────────────
    // Three outcomes:
    //   * Not active (just a click) → clear arm, no tree mutation.
    //   * Active, no valid target  → cancel, no tree mutation.
    //   * Active, valid target      → mutate tree (reorder / tab-into
    //     / split) and clear state. ReleaseCapture in all three.
    if (state.ui.dock.dragTabArmed) {
        if (state.ui.dock.dragTabActive &&
            state.ui.dock.dropTargetLeaf != nullptr &&
            state.ui.dock.dragTabSource  != nullptr)
        {
            daw::ui::DockNode* src    = state.ui.dock.dragTabSource;
            daw::ui::DockNode* dst    = state.ui.dock.dropTargetLeaf;
            const daw::ui::PanelKind  panel = state.ui.dock.dragTabPanel;
            const daw::ui::DockDropSide side = state.ui.dock.dropTargetSide;
            const int srcIdx = state.ui.dock.dragTabIndex;
            const int dstAt  = state.ui.dock.dropTargetTabAt;
            // If dst aliases the current root, removing the source
            // may collapse the root's split and free the old root
            // node; re-resolve dst from the (possibly new) root
            // after the remove. Same trick covers Reset Layout
            // pointer churn in case the root was an outer split.
            const bool dstIsRoot = (dst == state.ui.dock.dockRoot.get());

            if (side == daw::ui::DockDropSide::Center) {
                // Reorder within same leaf, or move tab between leaves.
                daw::ui::DockRemoveTab(state.ui.dock.dockRoot, src, srcIdx);
                if (dstIsRoot) dst = state.ui.dock.dockRoot.get();
                daw::ui::DockInsertTab(dst, panel, dstAt);
            } else {
                // Split: detach source first, then split destination.
                daw::ui::DockRemoveTab(state.ui.dock.dockRoot, src, srcIdx);
                if (dstIsRoot) dst = state.ui.dock.dockRoot.get();
                daw::ui::DockSplitWith(state.ui.dock.dockRoot, dst, side, panel, 0.4f);
            }
            changed = true;
        } else if (state.ui.dock.dragTabActive &&
                   state.ui.dock.dropTargetLeaf == nullptr &&
                   state.ui.dock.dragTabSource  != nullptr)
        {
            // ── Tear-off (Phase 4a) ─────────────────────────────
            // Drag ended over no valid drop target. If the cursor
            // is outside the main window's bounds entirely, spawn
            // a floating window hosting this panel.
            POINT screenPt; GetCursorPos(&screenPt);
            RECT mainWin; GetWindowRect(hwnd, &mainWin);
            if (!PtInRect(&mainWin, screenPt)) {
                const daw::ui::PanelKind panel = state.ui.dock.dragTabPanel;
                daw::ui::DockNode* src = state.ui.dock.dragTabSource;
                const int srcIdx = state.ui.dock.dragTabIndex;
                // Detach from main BEFORE spawning so the floating
                // window doesn't briefly show a duplicate.
                daw::ui::DockRemoveTab(state.ui.dock.dockRoot, src, srcIdx);
                // Must release capture before creating the new
                // window or it will steal mouse messages back.
                ReleaseCapture();
                SpawnFloatingPanel(state, panel, screenPt);
                changed = true;
            }
        }
        state.ui.dock.dragTabArmed   = false;
        state.ui.dock.dragTabActive  = false;
        state.ui.dock.dragTabSource  = nullptr;
        state.ui.dock.dragTabIndex   = -1;
        state.ui.dock.dropTargetLeaf = nullptr;
        state.ui.dock.dropPreviewRect = RECT{0, 0, 0, 0};
        ReleaseCapture();
    }

    if (state.ui.dock.draggingSplitter) {
        state.ui.dock.draggingSplitter       = false;
        state.ui.dock.dragSplitterNode       = nullptr;
        ReleaseCapture();
        changed = true;
    }
    if (state.ui.tools.draggingPlayhead) {
        state.ui.tools.draggingPlayhead = false;
        changed = true;
    }
    if (state.ui.tools.trimmingClip) {
        state.ui.tools.trimmingClip = false;
        state.ui.tools.trimClipIndex = -1;
        state.core.projectModified = true;
        changed = true;
    }
    if (state.ui.tools.draggingClip) {
        state.ui.tools.draggingClip = false;
        state.ui.tools.dragClipIndex = -1;
        state.core.projectModified = true;
        changed = true;
    }
    if (state.ui.tools.draggingFader) {
        state.ui.tools.draggingFader = false;
        state.ui.tools.dragFaderTrack = -1;
        state.core.projectModified = true;
        changed = true;
    }
    if (state.ui.tools.draggingPan) {
        state.ui.tools.draggingPan = false;
        state.ui.tools.dragPanIndex = -1;
        state.core.projectModified = true;
        changed = true;
    }
    if (state.ui.tools.draggingParamKnob) {
        state.ui.tools.draggingParamKnob = false;
        state.ui.tools.paramKnobParamId  = -1;
        state.core.projectModified = true;
        changed = true;
    }
    if (changed) {
        UpdateWindowTitle(hwnd, state.core);
        ReleaseCapture();
        daw::ui::RequestRepaintAll(state);
    }
    return 0;
}
