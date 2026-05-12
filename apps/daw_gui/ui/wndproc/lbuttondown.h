#pragma once

#include "AppState.h"

#include <windows.h>

// WM_LBUTTONDOWN handler (Phase 16o extraction). Returns the LRESULT for
// the case body. WindowProc supplies hwnd/lParam/state. Caller has already
// done the state-null check.
LRESULT WndProcOnLButtonDown(HWND hwnd, LPARAM lParam, AppState& state);
