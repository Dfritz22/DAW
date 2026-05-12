#include "ui/dock.h"
#include "ui/dpi.h"
#include "ui/UiRuntimeState.h"   // kTopBarHeight, kLeftPanelWidth, kBusPanelHeight, etc.

#include <algorithm>

namespace daw::ui {

void DockLayout(DockNode* root, const RECT& clientRect,
                std::vector<DockLeafLayout>& out,
                std::vector<DockSplitterLayout>* splittersOut) {
    if (root == nullptr) return;

    if (root->kind == DockKind::Leaf) {
        if (root->panels.empty()) return;
        const int tab = std::clamp(root->activeTab, 0, static_cast<int>(root->panels.size()) - 1);
        out.push_back(DockLeafLayout{root->panels[static_cast<size_t>(tab)], clientRect, root});
        return;
    }

    const int width  = clientRect.right  - clientRect.left;
    const int height = clientRect.bottom - clientRect.top;
    const float ratio = std::clamp(root->ratio, 0.05f, 0.95f);
    const int half = std::max(1, Dpi(kDockSplitterThicknessPx) / 2);

    if (root->kind == DockKind::SplitH) {
        const int splitX = clientRect.left + static_cast<int>(static_cast<float>(width) * ratio);
        const RECT leftRect {clientRect.left, clientRect.top, splitX,           clientRect.bottom};
        const RECT rightRect{splitX,          clientRect.top, clientRect.right, clientRect.bottom};
        if (splittersOut != nullptr) {
            splittersOut->push_back(DockSplitterLayout{
                RECT{splitX - half, clientRect.top, splitX + half, clientRect.bottom},
                /*horizontal*/ false,
                root
            });
        }
        DockLayout(root->children[0].get(), leftRect,  out, splittersOut);
        DockLayout(root->children[1].get(), rightRect, out, splittersOut);
        return;
    }

    if (root->kind == DockKind::SplitV) {
        const int splitY = clientRect.top + static_cast<int>(static_cast<float>(height) * ratio);
        const RECT topRect   {clientRect.left, clientRect.top, clientRect.right, splitY};
        const RECT bottomRect{clientRect.left, splitY,         clientRect.right, clientRect.bottom};
        if (splittersOut != nullptr) {
            splittersOut->push_back(DockSplitterLayout{
                RECT{clientRect.left, splitY - half, clientRect.right, splitY + half},
                /*horizontal*/ true,
                root
            });
        }
        DockLayout(root->children[0].get(), topRect,    out, splittersOut);
        DockLayout(root->children[1].get(), bottomRect, out, splittersOut);
        return;
    }
}

// ── Tree mutation (Phase 2.2b: drag tabs to reorder/dock) ───────────────────

std::unique_ptr<DockNode>* DockFindOwner(std::unique_ptr<DockNode>& rootSlot,
                                         DockNode* node) {
    if (!rootSlot) return nullptr;
    if (rootSlot.get() == node) return &rootSlot;
    if (rootSlot->kind == DockKind::Leaf) return nullptr;
    if (auto* p = DockFindOwner(rootSlot->children[0], node)) return p;
    return DockFindOwner(rootSlot->children[1], node);
}

namespace {
struct ParentRef { DockNode* parent; int childIndex; };
ParentRef FindParent(DockNode* root, DockNode* child) {
    if (root == nullptr || root->kind == DockKind::Leaf) return {nullptr, -1};
    for (int i = 0; i < 2; ++i) {
        if (root->children[i].get() == child) return {root, i};
        ParentRef r = FindParent(root->children[i].get(), child);
        if (r.parent != nullptr) return r;
    }
    return {nullptr, -1};
}
} // namespace

void DockRemoveTab(std::unique_ptr<DockNode>& rootSlot,
                   DockNode* leaf, int tabIndex) {
    if (leaf == nullptr || leaf->kind != DockKind::Leaf) return;
    if (tabIndex < 0 || tabIndex >= static_cast<int>(leaf->panels.size())) return;
    leaf->panels.erase(leaf->panels.begin() + tabIndex);
    if (leaf->activeTab >= static_cast<int>(leaf->panels.size())) {
        leaf->activeTab = static_cast<int>(leaf->panels.size()) - 1;
    }
    if (leaf->activeTab < 0) leaf->activeTab = 0;
    if (!leaf->panels.empty()) return;

    // Leaf is empty — collapse it out. If it's the root, leave it; the
    // renderer's Leaf branch early-returns on an empty panels list.
    if (rootSlot.get() == leaf) return;

    const ParentRef pr = FindParent(rootSlot.get(), leaf);
    if (pr.parent == nullptr) return;
    auto* parentSlot = DockFindOwner(rootSlot, pr.parent);
    if (parentSlot == nullptr) return;
    const int siblingIdx = 1 - pr.childIndex;
    *parentSlot = std::move(pr.parent->children[siblingIdx]);
}

void DockInsertTab(DockNode* targetLeaf, PanelKind panel, int atIndex) {
    if (targetLeaf == nullptr || targetLeaf->kind != DockKind::Leaf) return;
    const int n = static_cast<int>(targetLeaf->panels.size());
    if (atIndex < 0 || atIndex > n) atIndex = n;
    targetLeaf->panels.insert(targetLeaf->panels.begin() + atIndex, panel);
    targetLeaf->activeTab = atIndex;
}

void DockSplitWith(std::unique_ptr<DockNode>& rootSlot,
                   DockNode* targetLeaf,
                   DockDropSide side,
                   PanelKind panel,
                   float ratio) {
    if (side == DockDropSide::Center) return;
    auto* slot = DockFindOwner(rootSlot, targetLeaf);
    if (slot == nullptr) return;

    auto target  = std::move(*slot);  // detach target from tree
    auto newLeaf = DockNode::Leaf(panel);

    const bool horizontal = (side == DockDropSide::Left || side == DockDropSide::Right);
    const bool newFirst   = (side == DockDropSide::Left || side == DockDropSide::Top);

    auto split   = std::make_unique<DockNode>();
    split->kind  = horizontal ? DockKind::SplitH : DockKind::SplitV;
    split->ratio = newFirst ? ratio : (1.0f - ratio);
    split->children[0] = newFirst ? std::move(newLeaf) : std::move(target);
    split->children[1] = newFirst ? std::move(target)  : std::move(newLeaf);
    *slot = std::move(split);
}

DockNode* DockFindLeafContaining(DockNode* root, PanelKind panel) {
    if (root == nullptr) return nullptr;
    if (root->kind == DockKind::Leaf) {
        for (PanelKind p : root->panels) if (p == panel) return root;
        return nullptr;
    }
    if (auto* r = DockFindLeafContaining(root->children[0].get(), panel)) return r;
    return DockFindLeafContaining(root->children[1].get(), panel);
}

DockNode* DockFindNonPrimaryLeaf(DockNode* root) {
    if (root == nullptr) return nullptr;
    if (root->kind == DockKind::Leaf) {
        for (PanelKind p : root->panels) {
            if (!PanelGet(p).primary) return root;
        }
        return nullptr;
    }
    if (auto* r = DockFindNonPrimaryLeaf(root->children[0].get())) return r;
    return DockFindNonPrimaryLeaf(root->children[1].get());
}

bool DockLeafShowsTabStrip(const DockNode* leaf) {
    if (leaf == nullptr || leaf->kind != DockKind::Leaf) return false;
    if (leaf->panels.empty()) return false;
    if (leaf->panels.size() > 1) return true;
    return !PanelGet(leaf->panels[0]).primary;
}

std::unique_ptr<DockNode> DockBuildDefault() {
    // The dock owns the body area BELOW the top bar. The top bar is still a
    // fixed strip painted directly by the renderer (it will become dockable
    // in a future phase once tab strips and a top-bar layout exist).
    //
    //                       ┌──────────────────────────┐
    //                       │     (top bar — fixed)    │
    //                       ├───────────┬──────────────┤
    //                       │  Tracks   │   Ruler      │
    //                       │  -----    ├──────────────┤
    //                       │  Buses    │   Arrange    │
    //                       └───────────┴──────────────┘

    auto leftCol = DockNode::SplitV(
        DockNode::Leaf(PanelKind::Tracks),
        DockNode::Leaf(PanelKind::Buses),
        0.78f
    );

    auto rightCol = DockNode::SplitV(
        DockNode::Leaf(PanelKind::Ruler),
        DockNode::Leaf(PanelKind::Arrange),
        0.05f
    );

    auto root = DockNode::SplitH(
        std::move(leftCol),
        std::move(rightCol),
        0.25f   // ~300 px on a 1200 px wide window (= legacy kLeftPanelWidth)
    );

    return root;
}

} // namespace daw::ui
