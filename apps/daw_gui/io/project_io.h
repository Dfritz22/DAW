#pragma once

#ifdef DAW_PUBLIC_API
#error "This header is internal-only."
#endif

#include "AppState.h"

bool IoSaveProject(const std::wstring& path, AppState& state);
bool IoLoadProject(const std::wstring& path, AppState& state);
bool IoDoSaveAs(HWND hwnd, AppState& state);
bool IoDoSave(HWND hwnd, AppState& state);
bool IoDoOpen(HWND hwnd, AppState& state);
