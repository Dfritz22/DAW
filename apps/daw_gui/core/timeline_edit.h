#pragma once

#ifdef DAW_PUBLIC_API
#error "This header is internal-only."
#endif

#include <windows.h>

// Forward declarations
struct UiState;

// ── Timeline editing and undo/redo ───────────────────────────────────────────

/// Push current clips and gain state to undo stack, clear redo stack.
void PushUndo(UiState& state);

/// Restore clips and gain state from undo stack, push current to redo stack.
void ApplyUndo(HWND hwnd, UiState& state);

/// Restore clips and gain state from redo stack, push current to undo stack.
void ApplyRedo(HWND hwnd, UiState& state);

// ── Clip editing ─────────────────────────────────────────────────────────────

/// Split selected clip at playhead position.
void SplitSelectedClip(UiState& state);

/// Duplicate selected clip, placing copy after original.
void DuplicateSelectedClip(UiState& state);

/// Nudge selected clip forward/backward by deltaBeats.
void NudgeSelectedClip(UiState& state, float deltaBeats);

/// Delete selected clip.
void DeleteSelectedClip(UiState& state);

// ── Track management ─────────────────────────────────────────────────────────

/// Create new track with default settings, return track index.
int AddNewTrack(UiState& state);

/// Delete track at index, remove all clips on that track, update all clip track indices.
void DeleteTrackAt(UiState& state, int trackIndex);
