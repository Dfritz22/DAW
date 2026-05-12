#pragma once

#ifdef DAW_PUBLIC_API
#error "This header is internal-only."
#endif

// ── Project file I/O ─────────────────────────────────────────────────────────
// Load/save the entire user project (tracks, clips, buses, insert chains,
// audio asset metadata, transport state, audio backend prefs) to a single
// hand-written JSON file at the user-chosen path.
//
// On-disk format: a flat JSON object with a mandatory "version" field.
// Current version = 1. The loader is strict: an absent or non-matching
// version field causes IoLoadProject to return false without touching
// `state`. There are no shipped legacy versions to support, so version
// bumps must add an explicit migration path here — never silent fallbacks.
//
// Failure semantics:
//   * IoSaveProject: returns false on any I/O error. Best-effort atomicity
//     is the caller's responsibility (the writer truncates in place).
//   * IoLoadProject: returns false on missing file, empty file, missing
//     or wrong version. On true, AppState has been fully replaced with
//     the loaded session. On false, AppState may have been partially
//     reset — callers should treat false as "scrap the session".
//
// ── Architectural contract (frozen for the overhaul) ───────────────────────
// This header is the project I/O subsystem's stable API. Phase 2 of the
// overhaul will move it into a dedicated `io` library and introduce
// versioned migrators; the function signatures here stay the same.

#include "AppState.h"

bool IoSaveProject(const std::wstring& path, AppState& state);
bool IoLoadProject(const std::wstring& path, AppState& state);
bool IoDoSaveAs(HWND hwnd, AppState& state);
bool IoDoSave(HWND hwnd, AppState& state);
bool IoDoOpen(HWND hwnd, AppState& state);
