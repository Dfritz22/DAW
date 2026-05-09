#pragma once

#ifdef DAW_PUBLIC_API
#error "This header is internal-only."
#endif

#include "core/CoreState.h"

// ── Timeline editing and undo/redo ───────────────────────────────────────────

/// Push current clips and gain state to undo stack, clear redo stack.
void PushUndo(CoreState& state);

/// Restore clips and gain state from undo stack, push current to redo stack.
bool ApplyUndo(CoreState& state);

/// Restore clips and gain state from redo stack, push current to undo stack.
bool ApplyRedo(CoreState& state);

// ── Clip editing ─────────────────────────────────────────────────────────────

/// Split selected clip at playhead position.
bool SplitSelectedClip(CoreState& state, int selectedClipIndex, float playheadBeat);

/// Duplicate selected clip, placing copy after original.
int DuplicateSelectedClip(CoreState& state, int selectedClipIndex);

/// Nudge selected clip forward/backward by deltaBeats.
bool NudgeSelectedClip(CoreState& state, int selectedClipIndex, float deltaBeats);

/// Delete selected clip.
bool DeleteSelectedClip(CoreState& state, int selectedClipIndex);

// ── Track management ─────────────────────────────────────────────────────────

/// Create new track with default settings, return track index.
int AddNewTrack(CoreState& state);

/// Delete track at index, remove all clips on that track, update all clip track indices.
bool DeleteTrackAt(CoreState& state, int trackIndex);
