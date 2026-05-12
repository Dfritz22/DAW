#pragma once

// ── Dock tree ────────────────────────────────────────────────────────────────
// Unity-style nested splits with tabbed leaves. The window's UI surface is a
// single `DockNode` tree:
//
//   * A Leaf node holds one or more PanelKind values (= a tab strip with
//     `panels.size()` tabs; the active tab is rendered into the leaf rect).
//   * A SplitH node has two children stacked horizontally (left/right) at
//     a fractional `ratio` of the parent's width.
//   * A SplitV node has two children stacked vertically (top/bottom) at
//     a fractional `ratio` of the parent's height.
//
// `DockLayout()` walks the tree and emits one entry per leaf with its rect
// + which panel is active. The renderer then iterates the layout list and
// dispatches to the panel registry (`PanelGet(kind).draw`).
//
// ── Architectural contract (frozen for the overhaul) ───────────────────────
// This header is the dock subsystem's stable in-process API. It is the
// boundary other UI code is allowed to depend on; everything in dock.cpp
// is implementation detail. The contract:
//
//   * Tree ownership: every DockNode is owned by `unique_ptr` from its
//     parent (or the root holder). No raw new/delete; no shared ownership.
//   * Tree invariants (enforced by mutators + persistence validator):
//       - Every PanelKind referenced by the tree appears in exactly one
//         Leaf.panels[] slot (no duplicates, no orphans).
//       - Every primary panel (PanelDef.primary == true) is present
//         somewhere in the docked tree (primaries can never float).
//       - Splitter ratios are clamped to [0.05, 0.95] so a corrupted
//         file or a degenerate split can't hide a panel.
//   * Layout pass (DockLayout) is pure: it never mutates the tree.
//   * Mutation API (DockRemoveTab, DockInsertTab, DockSplitWith,
//     DockReturnPanelToMain) MAY collapse single-child splitters and
//     prune empty leaves, but never deletes primaries or violates the
//     invariants above.
//   * Floating panels (torn-off windows) are NOT in this tree; they live
//     in `state.ui.floatingPanels` and are persisted by dock_persist.h.
//
// Phases 7–8 of the overhaul will move dock + panel into a dedicated
// library and split the rendering shims out of panel.cpp; the public
// types and function signatures in this header stay the same across that
// move.

#define NOMINMAX
#include <windows.h>
#include <memory>
#include <vector>

#include "ui/panel.h"

namespace daw::ui {

enum class DockKind {
    Leaf,
    SplitH,   // children[0] left,  children[1] right
    SplitV,   // children[0] top,   children[1] bottom
};

struct DockNode {
    DockKind                              kind        {DockKind::Leaf};
    float                                 ratio       {0.5f};      // splits only
    std::vector<PanelKind>                panels;                  // leaves only
    int                                   activeTab   {0};         // leaves only
    std::unique_ptr<DockNode>             children[2] {nullptr, nullptr};

    // ── Convenience constructors ─────────────────────────────────────────────
    static std::unique_ptr<DockNode> Leaf(PanelKind p) {
        auto n = std::make_unique<DockNode>();
        n->kind = DockKind::Leaf;
        n->panels.push_back(p);
        return n;
    }
    static std::unique_ptr<DockNode> Tabbed(std::initializer_list<PanelKind> ps) {
        auto n = std::make_unique<DockNode>();
        n->kind = DockKind::Leaf;
        for (PanelKind p : ps) n->panels.push_back(p);
        return n;
    }
    static std::unique_ptr<DockNode> SplitH(std::unique_ptr<DockNode> left,
                                            std::unique_ptr<DockNode> right,
                                            float ratio) {
        auto n = std::make_unique<DockNode>();
        n->kind = DockKind::SplitH;
        n->ratio = ratio;
        n->children[0] = std::move(left);
        n->children[1] = std::move(right);
        return n;
    }
    static std::unique_ptr<DockNode> SplitV(std::unique_ptr<DockNode> top,
                                            std::unique_ptr<DockNode> bottom,
                                            float ratio) {
        auto n = std::make_unique<DockNode>();
        n->kind = DockKind::SplitV;
        n->ratio = ratio;
        n->children[0] = std::move(top);
        n->children[1] = std::move(bottom);
        return n;
    }
};

// One entry per leaf produced by the layout walker.
struct DockLeafLayout {
    PanelKind activePanel;     // panels[activeTab]
    RECT      rect;            // leaf rect in client coords
    DockNode* node;            // back-pointer (for hit-test, tab clicks)
};

// One entry per split node produced by the layout walker.
// `rect` is the thin draggable splitter region (between the two children).
// `horizontal == true` means a horizontal divider line whose Y is dragged
// (i.e. parent is a SplitV); `horizontal == false` means a vertical divider
// line whose X is dragged (parent is a SplitH).
struct DockSplitterLayout {
    RECT      rect;
    bool      horizontal;      // true = drag Y (SplitV), false = drag X (SplitH)
    DockNode* node;            // points at the split node whose ratio you mutate
};

// Width/height of the splitter hit zone, in DPI-scaled pixels (un-scaled
// here; callers should pass through Dpi() if they want a pixel value).
constexpr int kDockSplitterThicknessPx = 6;

// Height of the tab strip drawn at the top of every dock leaf. Reserved
// from the leaf rect before the panel is drawn into it.
constexpr int kDockTabStripHeightPx = 20;

// Per-tab hit-test record produced by the renderer alongside DockLayout.
// Lets the click handler resolve a mouse point to (node, tab index) so it
// can switch the active tab and (Phase 2.2b) start a tab drag.
struct DockTabHit {
    RECT      rect;        // tab rect in client coords (full strip pixel area)
    DockNode* node;        // owning leaf node
    int       tabIndex;    // index into node->panels
};

// Walk `root` over `clientRect` and append one DockLeafLayout per leaf.
// If `splittersOut` is non-null, also append one DockSplitterLayout per
// split node so callers can hit-test the draggable dividers.
void DockLayout(DockNode* root, const RECT& clientRect,
                std::vector<DockLeafLayout>& out,
                std::vector<DockSplitterLayout>* splittersOut = nullptr);

// ── Tree mutation (Phase 2.2b: drag tabs to reorder/dock) ───────────────────
// Where a panel is being dropped relative to a target leaf. Center means the
// drop becomes a tab within the target's strip; the four edge values mean
// the target leaf is split and the dragged panel becomes its new sibling.
enum class DockDropSide { Center, Left, Right, Top, Bottom };

// Find the unique_ptr slot that owns `node` somewhere under `rootSlot`. The
// slot lets callers move-out / replace the node in place. Returns nullptr
// if the node isn't in this tree.
std::unique_ptr<DockNode>* DockFindOwner(std::unique_ptr<DockNode>& rootSlot,
                                         DockNode* node);

// Remove the panel at `tabIndex` from `leaf`. If the leaf becomes empty it's
// collapsed out of the tree (its parent split is replaced by the surviving
// sibling). Pass the root slot so the collapse can mutate it. No-op if the
// resulting tree would be empty (i.e. the only leaf would be removed).
void DockRemoveTab(std::unique_ptr<DockNode>& rootSlot,
                   DockNode* leaf, int tabIndex);

// Insert `panel` as a new tab in `targetLeaf` at position `atIndex` (clamped
// to [0, panels.size()]); the new tab becomes active.
void DockInsertTab(DockNode* targetLeaf, PanelKind panel, int atIndex);

// Replace `targetLeaf` in the tree with a split node holding the original
// leaf and a new sibling leaf containing `panel`. Side decides axis +
// which child is the new one. `ratio` is the new sibling's fraction.
void DockSplitWith(std::unique_ptr<DockNode>& rootSlot,
                   DockNode* targetLeaf,
                   DockDropSide side,
                   PanelKind panel,
                   float ratio = 0.4f);

// Walk the tree under `root` and return the first leaf containing `panel`,
// or nullptr if no leaf does. Useful for "is this panel visible?" checks.
DockNode* DockFindLeafContaining(DockNode* root, PanelKind panel);

// Walk the tree under `root` and return the first leaf whose active panel
// is NOT primary (suitable as a default tab-insert host). Falls back to any
// leaf if all leaves are primary, or nullptr for an empty tree.
DockNode* DockFindNonPrimaryLeaf(DockNode* root);

// A leaf shows a tab strip when it has more than one panel OR when its
// single panel is non-primary. A lone primary panel (e.g. Tracks/Ruler/
// Arrange in default layout) draws full-bleed with no chrome — but as soon
// as another panel docks alongside it, the strip appears so users can
// switch tabs. Primary panels themselves can never be dragged out, but
// non-primary tabs in the same leaf can.
bool DockLeafShowsTabStrip(const DockNode* leaf);

// Build the default dock layout used on first launch (and Reset Layout).
// Mirrors the current fixed layout so swapping in is visually a no-op:
//   ┌─────────────────────────────────────────────────────┐
//   │                  Transport                          │   ← top
//   ├──────────────┬──────────────────────────────────────┤
//   │              │              Ruler                   │
//   │   Tracks     ├──────────────────────────────────────┤
//   │              │             Arrange                  │
//   ├──────────────┤                                      │
//   │   Buses      │                                      │
//   └──────────────┴──────────────────────────────────────┘
std::unique_ptr<DockNode> DockBuildDefault();

} // namespace daw::ui
