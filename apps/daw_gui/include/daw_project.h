#pragma once

#include <string>

struct HWND__;
using HWND = HWND__*;

struct UiState;

bool SaveProject(const std::wstring& path, UiState& state);
bool LoadProject(const std::wstring& path, UiState& state);
bool DoSaveAs(HWND hwnd, UiState& state);
bool DoSave(HWND hwnd, UiState& state);
bool DoOpen(HWND hwnd, UiState& state);

// Timeline-edit API exposed for orchestration/UI layers.
void PushUndo(UiState& state);
void ApplyUndo(HWND hwnd, UiState& state);
void ApplyRedo(HWND hwnd, UiState& state);
void SplitSelectedClip(UiState& state);
void DuplicateSelectedClip(UiState& state);
void NudgeSelectedClip(UiState& state, float deltaBeats);
void DeleteSelectedClip(UiState& state);
int AddNewTrack(UiState& state);
void DeleteTrackAt(UiState& state, int trackIndex);
