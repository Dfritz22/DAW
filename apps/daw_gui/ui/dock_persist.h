#pragma once

// ── Dock layout persistence ─────────────────────────────────────────────────
//
// Serializes the user's full window state — the docked panel tree AND any
// torn-off floating panels — to %APPDATA%\DAW\layout.json so the next
// launch reopens the same arrangement.
//
// On-disk format (schema v2, current):
//   {
//     "version":  2,
//     "root":     <DockNode>,            // the docked tree
//     "floating": [
//       { "panel": "<persistKey>",
//         "x": <int>, "y": <int>,        // top-left in screen pixels
//         "w": <int>, "h": <int> },      // size in pixels
//       ...
//     ]
//   }
//
// Loader is strict on version: anything other than v2 is discarded and
// the caller falls back to `DockBuildDefault()`. There are no shipped
// legacy formats — every bump must add an explicit migration here.
//
// Panels are keyed by `PanelDef.persistKey` (a stable string), NOT by
// enum ordinal — reordering the PanelKind enum can never silently corrupt
// saved layouts.
//
// All failure modes fall back silently: a missing/unreadable/malformed
// file just causes the caller to use `DockBuildDefault()` and an empty
// floating list, so the app always launches with a usable layout.
//
// Validation rejects, in either schema:
//   * unknown persistKeys
//   * duplicate panels (each PanelKind must appear in exactly one place)
//   * trees missing any primary panel (Ruler / Tracks / Arrange)
//   * malformed JSON / missing required fields
//
// ── Architectural contract (frozen for the overhaul) ───────────────────────
// This header is the dock subsystem's stable serialization contract.
// Phases 7–8 of the overhaul will move dock + panel into a dedicated
// library, but this API surface stays the same. Adding fields to the
// schema is allowed (the parser ignores unknown keys); changing or
// removing existing fields requires a version bump and a migrator.

#define NOMINMAX
#include <windows.h>
#include <memory>
#include <string>
#include <vector>

#include "ui/dock.h"
#include "ui/panel.h"

namespace daw::ui {

// One torn-off panel's persisted geometry. Position is in screen pixels at
// the time of save (callers should re-clamp to a visible monitor on load).
struct DockFloatingPanel {
    PanelKind panel;
    int       x;
    int       y;
    int       w;
    int       h;
};

// Result of loading layout.json. `root == nullptr` means the file was
// missing / unreadable / failed validation; callers should fall back to
// `DockBuildDefault()`. `floating` is empty in that case.
struct DockLayoutDocument {
    std::unique_ptr<DockNode>       root;
    std::vector<DockFloatingPanel>  floating;
};

// Absolute path to %APPDATA%\DAW\layout.json. Creates the DAW folder if
// missing. Returns empty string only if SHGetFolderPath itself fails.
std::wstring DockGetLayoutFilePath();

// ── Pure JSON codec (no I/O) ────────────────────────────────────────────────
// These two are the half of the API that never touches the filesystem and
// that future test targets / migrators can exercise directly.

// Serialize the full document into a UTF-8 JSON string at schema v2.
// `floating` may be empty.
std::string DockSerializeToJson(const DockNode* root,
                                const std::vector<DockFloatingPanel>& floating);

// Parse a UTF-8 JSON string back into a document. Schema gate is strict:
// only documents with `"version": 2` (and a valid root + floating list)
// are accepted; anything else (missing version, version != 2, malformed,
// failed validation) returns a document with `root == nullptr` so the
// caller can fall back to `DockBuildDefault()`. There is no in-process
// v1 → v2 migrator — pre-production removed all shipped v1 files; future
// schema bumps must add an explicit migrator here.
DockLayoutDocument DockDeserializeFromJson(const std::string& json);

// ── Filesystem ops ──────────────────────────────────────────────────────────

// Atomic save: writes to a temp sibling then renames over the target.
// Returns true on success. Silently no-ops if `root` is null.
bool DockSaveLayout(const DockNode* root,
                    const std::vector<DockFloatingPanel>& floating);

// Load + validate. Returns a document; check `result.root != nullptr` to
// know whether the file was usable.
DockLayoutDocument DockLoadLayout();

// Delete the persisted layout file (used by Reset Layout so the next
// launch starts fresh). Silently succeeds if the file doesn't exist.
void DockDeleteLayoutFile();

} // namespace daw::ui
