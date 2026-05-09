#pragma once

#include "core/state.h"

bool SaveProject(const std::wstring& path, UiState& state);
bool LoadProject(const std::wstring& path, UiState& state);
bool DoSaveAs(HWND hwnd, UiState& state);
bool DoSave(HWND hwnd, UiState& state);
bool DoOpen(HWND hwnd, UiState& state);
