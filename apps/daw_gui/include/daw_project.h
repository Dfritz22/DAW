#pragma once

#include <string>

struct HWND__;
using HWND = HWND__*;

struct AppState;

bool SaveProject(const std::wstring& path, AppState& state);
bool LoadProject(const std::wstring& path, AppState& state);
bool DoSaveAs(HWND hwnd, AppState& state);
bool DoSave(HWND hwnd, AppState& state);
bool DoOpen(HWND hwnd, AppState& state);

// Timeline-edit API exposed for orchestration/UI layers.
void PushUndo(AppState& state);
void ApplyUndo(HWND hwnd, AppState& state);
void ApplyRedo(HWND hwnd, AppState& state);
void SplitSelectedClip(AppState& state);
void DuplicateSelectedClip(AppState& state);
void NudgeSelectedClip(AppState& state, float deltaBeats);
void DeleteSelectedClip(AppState& state);
int AddNewTrack(AppState& state);
void DeleteTrackAt(AppState& state, int trackIndex);

// WAV import / SR-conversion. Defined in main.cpp (pending its own TU).
void ImportWavFiles(HWND hwnd, AppState& state);
void ConvertImportedAudioToProjectSampleRate(HWND hwnd, AppState& state);
