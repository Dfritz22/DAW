#include "ui/wndproc/lbutton.h"

#include "core/internal_app_services.h"
#include "ui/dock.h"

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
    if (state.ui.dragTabArmed) {
        if (state.ui.dragTabActive &&
            state.ui.dropTargetLeaf != nullptr &&
            state.ui.dragTabSource  != nullptr)
        {
            daw::ui::DockNode* src    = state.ui.dragTabSource;
            daw::ui::DockNode* dst    = state.ui.dropTargetLeaf;
            const daw::ui::PanelKind  panel = state.ui.dragTabPanel;
            const daw::ui::DockDropSide side = state.ui.dropTargetSide;
            const int srcIdx = state.ui.dragTabIndex;
            const int dstAt  = state.ui.dropTargetTabAt;
            // If dst aliases the current root, removing the source
            // may collapse the root's split and free the old root
            // node; re-resolve dst from the (possibly new) root
            // after the remove. Same trick covers Reset Layout
            // pointer churn in case the root was an outer split.
            const bool dstIsRoot = (dst == state.ui.dockRoot.get());

            if (side == daw::ui::DockDropSide::Center) {
                // Reorder within same leaf, or move tab between leaves.
                daw::ui::DockRemoveTab(state.ui.dockRoot, src, srcIdx);
                if (dstIsRoot) dst = state.ui.dockRoot.get();
                daw::ui::DockInsertTab(dst, panel, dstAt);
            } else {
                // Split: detach source first, then split destination.
                daw::ui::DockRemoveTab(state.ui.dockRoot, src, srcIdx);
                if (dstIsRoot) dst = state.ui.dockRoot.get();
                daw::ui::DockSplitWith(state.ui.dockRoot, dst, side, panel, 0.4f);
            }
            changed = true;
        } else if (state.ui.dragTabActive &&
                   state.ui.dropTargetLeaf == nullptr &&
                   state.ui.dragTabSource  != nullptr)
        {
            // ── Tear-off (Phase 4a) ─────────────────────────────
            // Drag ended over no valid drop target. If the cursor
            // is outside the main window's bounds entirely, spawn
            // a floating window hosting this panel.
            POINT screenPt; GetCursorPos(&screenPt);
            RECT mainWin; GetWindowRect(hwnd, &mainWin);
            if (!PtInRect(&mainWin, screenPt)) {
                const daw::ui::PanelKind panel = state.ui.dragTabPanel;
                daw::ui::DockNode* src = state.ui.dragTabSource;
                const int srcIdx = state.ui.dragTabIndex;
                // Detach from main BEFORE spawning so the floating
                // window doesn't briefly show a duplicate.
                daw::ui::DockRemoveTab(state.ui.dockRoot, src, srcIdx);
                // Must release capture before creating the new
                // window or it will steal mouse messages back.
                ReleaseCapture();
                SpawnFloatingPanel(state, panel, screenPt);
                changed = true;
            }
        }
        state.ui.dragTabArmed   = false;
        state.ui.dragTabActive  = false;
        state.ui.dragTabSource  = nullptr;
        state.ui.dragTabIndex   = -1;
        state.ui.dropTargetLeaf = nullptr;
        state.ui.dropPreviewRect = RECT{0, 0, 0, 0};
        ReleaseCapture();
    }

    if (state.ui.draggingSplitter) {
        state.ui.draggingSplitter       = false;
        state.ui.dragSplitterNode       = nullptr;
        ReleaseCapture();
        changed = true;
    }
    if (state.ui.draggingPlayhead) {
        state.ui.draggingPlayhead = false;
        changed = true;
    }
    if (state.ui.trimmingClip) {
        state.ui.trimmingClip = false;
        state.ui.trimClipIndex = -1;
        state.core.projectModified = true;
        changed = true;
    }
    if (state.ui.draggingClip) {
        state.ui.draggingClip = false;
        state.ui.dragClipIndex = -1;
        state.core.projectModified = true;
        changed = true;
    }
    if (state.ui.draggingFader) {
        state.ui.draggingFader = false;
        state.ui.dragFaderTrack = -1;
        state.core.projectModified = true;
        changed = true;
    }
    if (state.ui.draggingPan) {
        state.ui.draggingPan = false;
        state.ui.dragPanIndex = -1;
        state.core.projectModified = true;
        changed = true;
    }
    if (state.ui.draggingParamKnob) {
        state.ui.draggingParamKnob = false;
        state.ui.paramKnobParamId  = -1;
        state.core.projectModified = true;
        changed = true;
    }
    if (changed) {
        UpdateWindowTitle(hwnd, state.core);
        ReleaseCapture();
        InvalidateRect(hwnd, nullptr, FALSE);
    }
    return 0;
}
