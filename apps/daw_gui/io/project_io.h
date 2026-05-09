#pragma once

#ifdef DAW_PUBLIC_API
#error "This header is internal-only."
#endif

#include "core/state.h"

bool IoSaveProject(const std::wstring& path, UiState& state);
bool IoLoadProject(const std::wstring& path, UiState& state);
bool IoDoSaveAs(HWND hwnd, UiState& state);
bool IoDoSave(HWND hwnd, UiState& state);
bool IoDoOpen(HWND hwnd, UiState& state);
