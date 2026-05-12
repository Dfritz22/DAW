#include "daw_sdk.h"
#include <windowsx.h>
#include <algorithm>
#include "core/internal_app_services.h"
#include "ui/draw.h"
#include "ui/layout.h"
#include "ui/dpi.h"
#include "ui/dock.h"
#include "ui/dock_persist.h"
#include "ui/panel.h"
#include "ai/automix_bridge.h"
#include "audio/engine_utils.h"
#include "audio/transport_adapter.h"
#include "vm/timeline_zoom.h"
#include "ui/wndproc/wheel.h"
#include "ui/wndproc/timer.h"
#include "ui/wndproc/keys.h"
#include "ui/wndproc/rbutton.h"
#include "ui/wndproc/lbutton.h"
#include "ui/wndproc/mousemove.h"
#include "ui/wndproc/paint.h"
#include "ui/wndproc/command.h"
#include "ui/wndproc/lbuttondown.h"
#include "ui/menu_build.h"
#include "ui/dialogs.h"
#include "ui/floating.h"

using daw::internal::core::UpdateWindowTitle;

const wchar_t* BusName(int busIndex) {
    static const wchar_t* kNames[kBusCount] = {L"Drums", L"Music", L"Vocals", L"Master"};
    if (busIndex < 0 || busIndex >= kBusCount) {
        return L"Music";
    }
    return kNames[busIndex];
}

void ApplyBalancePan(float pan, float* left, float* right) {
    const float p = std::clamp(pan, -1.0f, 1.0f);
    if (p < 0.0f) {
        *right *= (1.0f + p);
    } else if (p > 0.0f) {
        *left *= (1.0f - p);
    }
}


// ── Dock-aware layout for hit-tests ─────────────────────────────────────────
// FindDockLeafRect / ComputeHitTestLayout were hoisted to ui/layout.{h,cpp}
// (UiLayoutFindDockLeafRect / UiLayoutComputeHitTestLayout) so per-message
// handler files extracted from WindowProc can share the same dock-aware
// layout without depending on main.cpp internals.

// ── Floating tear-off windows (Phase 4a) ────────────────────────────────────
// A floating window owns a single panel (no nested dock tree yet). Closing
// the window re-docks the panel back into the main DockNode tree so the
// panel is never lost. Mouse interaction inside floating windows is not
// wired in 4a — only paint + close. Phase 4b will route mouse messages
// through a per-panel hit dispatcher so the panel is fully usable while
// floating.

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<AppState*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));

    switch (msg) {
    case WM_CREATE: {
        // Pick up the monitor's DPI before the first paint so all layout math
        // produces correctly-sized rects on HiDPI displays from frame zero.
        g_uiDpi = static_cast<int>(GetDpiForWindow(hwnd));
        if (g_uiDpi <= 0) g_uiDpi = 96;

        auto* initial = new AppState();
        initial->ui.hwnd = hwnd;
        // Single-call audio engine bring-up: device enumeration, SR probe,
        // critical-section init, and engineState transition to Ready.
        AudioInitializeRuntime(hwnd, initial->core, initial->audio);
        SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(initial));

        // Attach the native Win32 menu bar. SetMenu() takes ownership of the
        // HMENU; Windows destroys it when the window is destroyed.
        if (HMENU bar = UiBuildMainMenuBar(*initial); bar != nullptr) {
            SetMenu(hwnd, bar);
            DrawMenuBar(hwnd);
        }

        SetTimer(hwnd, kPlaybackTimerId, kPlaybackTimerMs, nullptr);
        return 0;
    }
    case WM_SIZE: {
        // Default WM_SIZE only invalidates newly-exposed area, so when you
        // maximize / restore / drag-resize the window the existing content
        // (Arrange grid, ruler, etc.) shows stretched stale pixels until
        // the next click triggers a repaint. Force a full redraw on every
        // size change so the layout walker re-runs against the new client.
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_DPICHANGED: {
        // Per-monitor DPI v2: Windows tells us the new DPI in HIWORD(wParam)
        // and a recommended new bounding rect in lParam (already adjusted for
        // the new scale on the destination monitor).
        g_uiDpi = HIWORD(wParam);
        if (g_uiDpi <= 0) g_uiDpi = 96;
        const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
        if (suggested != nullptr) {
            SetWindowPos(hwnd, nullptr,
                         suggested->left, suggested->top,
                         suggested->right  - suggested->left,
                         suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_INITMENUPOPUP: {
        // Refresh dynamic content (device list, current sample rate / backend
        // / buffer-size checkmarks) just before each top-level popup opens.
        // HIWORD(lParam) is non-zero for the system (window) menu, which we
        // skip so Windows can render the standard window menu.
        if (HIWORD(lParam) != 0 || state == nullptr) {
            return DefWindowProc(hwnd, msg, wParam, lParam);
        }
        HMENU popup = reinterpret_cast<HMENU>(wParam);
        if (UiRefreshTopLevelPopup(popup, *state)) {
            return 0;
        }
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    case WM_COMMAND: {
        // Native menu items dispatch here. notification code (HIWORD) is 0 for
        // menu items, 1 for accelerators; we treat both the same way.
        if (state == nullptr) {
            return DefWindowProc(hwnd, msg, wParam, lParam);
        }
        const UINT cmd = LOWORD(wParam);
        WndProcOnMenuCommand(hwnd, *state, cmd);
        return 0;
    }
    case WM_DESTROY:
        KillTimer(hwnd, kPlaybackTimerId);
        if (state != nullptr) {
            // Persist the dock layout AND the geometry of every floating
            // panel before tearing down so the next launch reopens with
            // the same arrangement of panels, tabs, splitter ratios, and
            // torn-off windows.
            if (state->ui.dockRoot) {
                std::vector<daw::ui::DockFloatingPanel> floats;
                floats.reserve(state->ui.floatingPanels.size());
                for (const auto& fp : state->ui.floatingPanels) {
                    if (!fp.hwnd || !IsWindow(fp.hwnd)) continue;
                    RECT wr{};
                    if (!GetWindowRect(fp.hwnd, &wr)) continue;
                    daw::ui::DockFloatingPanel out{};
                    out.panel = fp.panel;
                    out.x = wr.left;
                    out.y = wr.top;
                    out.w = wr.right  - wr.left;
                    out.h = wr.bottom - wr.top;
                    floats.push_back(out);
                }
                daw::ui::DockSaveLayout(state->ui.dockRoot.get(), floats);
            }
            StopRecording(*state, false);
            if (state->audio.automixThread != nullptr) {
                WaitForSingleObject(state->audio.automixThread, INFINITE);
                CloseHandle(state->audio.automixThread);
                state->audio.automixThread = nullptr;
            }
            StopPlayback(*state, false);
            DeleteCriticalSection(&state->audio.audioStateLock);
            delete state;
        }
        PostQuitMessage(0);
        return 0;
    case kMsgPlaybackFinished:
        if (state != nullptr) {
            // Engine signaled natural end-of-song. Route through the FSM so
            // it stays the single source of truth for transport transitions.
            // From Recording → StopRecording (commits take + stops). From
            // Playing/CountingIn → StopPlayback / StopRecording respectively.
            // From Stopped (race) → no-op.
            daw::app::DispatchTransportEvent(hwnd, *state,
                daw::services::TransportEvent::StopPressed,
                /*rewindOnStop=*/false);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case kMsgCountInComplete:
        if (state != nullptr) {
            // Engine signaled the count-in preroll has elapsed. The audio
            // thread already cleared audio.countingIn for tight timing; this
            // message lets the FSM observe the CountingIn → Recording
            // transition. The resulting StartRecording action is idempotent
            // (recording was armed at the start of count-in). Repaint so the
            // transport buttons reflect the new state.
            daw::app::DispatchTransportEvent(hwnd, *state,
                daw::services::TransportEvent::CountInComplete,
                /*rewindOnStop=*/false);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case kMsgAutoMixFinished:
        if (state != nullptr) {
            if (state->audio.automixThread != nullptr) {
                CloseHandle(state->audio.automixThread);
                state->audio.automixThread = nullptr;
            }
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_TIMER:
        if (state != nullptr && wParam == kPlaybackTimerId) {
            return WndProcOnPlaybackTimer(hwnd, *state);
        }
        return 0;
    case WM_KEYDOWN:
        if (state == nullptr) return 0;
        return WndProcOnKeyDown(hwnd, wParam, *state);
    case WM_LBUTTONDOWN:
        if (state == nullptr) return 0;
        return WndProcOnLButtonDown(hwnd, lParam, *state);
    case WM_RBUTTONUP:
        if (state == nullptr) return 0;
        return WndProcOnRButtonUp(hwnd, lParam, *state);
    case WM_MOUSEMOVE:
        if (state == nullptr) return 0;
        return WndProcOnMouseMove(hwnd, lParam, *state);
    case WM_LBUTTONUP:
        if (state == nullptr) return 0;
        return WndProcOnLButtonUp(hwnd, *state);
    case WM_MOUSEWHEEL:
        if (state == nullptr) return 0;
        return WndProcOnMouseWheel(hwnd, wParam, lParam, *state);
    case WM_MOUSEHWHEEL:
        if (state == nullptr) return 0;
        return WndProcOnMouseHWheel(hwnd, wParam, *state);
    case WM_CAPTURECHANGED:
        if (state != nullptr) WndProcOnCaptureChanged(*state);
        return 0;
    case WM_ERASEBKGND:
        // Fully repainted in WM_PAINT using backbuffer.
        return 1;
    case WM_PAINT:
        if (state == nullptr) return DefWindowProc(hwnd, msg, wParam, lParam);
        return WndProcOnPaint(hwnd, *state);
    case WM_SETCURSOR: {
        // Show resize cursor when hovering a dock splitter or while dragging
        // one. For all other regions fall through to the default arrow.
        if (state != nullptr && reinterpret_cast<HWND>(wParam) == hwnd && LOWORD(lParam) == HTCLIENT) {
            POINT pt{};
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            for (const auto& sp : state->ui.dockSplitters) {
                if (PtInRect(&sp.rect, pt) || (state->ui.draggingSplitter && state->ui.dragSplitterNode == sp.node)) {
                    SetCursor(LoadCursor(nullptr, sp.horizontal ? IDC_SIZENS : IDC_SIZEWE));
                    return TRUE;
                }
            }
        }
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    default:
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR lpCmdLine, int nCmdShow) {
    // Optional: path to .dawproj passed as first command-line argument
    std::wstring startupProjectPath;
    if (lpCmdLine && lpCmdLine[0] != L'\0') {
        std::wstring arg = lpCmdLine;
        // Strip surrounding quotes if present
        if (!arg.empty() && arg.front() == L'"') arg = arg.substr(1);
        if (!arg.empty() && arg.back()  == L'"') arg.pop_back();
        if (std::filesystem::exists(arg)) startupProjectPath = arg;
    }

    WNDCLASS wc{};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = kWindowClassName;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);

    RegisterClass(&wc);

    // Phase 4 floating tear-off windows: separate WindowProc, same hInstance.
    WNDCLASS fwc{};
    fwc.lpfnWndProc   = FloatingWindowProc;
    fwc.hInstance     = hInstance;
    fwc.lpszClassName = kFloatingClassName;
    fwc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    fwc.hbrBackground = nullptr; // We paint the background ourselves.
    RegisterClass(&fwc);

    // Per-monitor DPI v2: each monitor reports its own DPI, WM_DPICHANGED fires
    // when the window crosses monitors with different scales (essential for the
    // multi-monitor / tear-off-window workflows planned for Phase 4).
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    HWND hwnd = CreateWindowEx(
        0,
        kWindowClassName,
        L"DAW GUI (C++ Bare Bones)",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        1200,
        700,
        nullptr,
        nullptr,
        hInstance,
        nullptr
    );

    if (hwnd == nullptr) {
        return 0;
    }

    // Load project file if one was specified at launch
    if (!startupProjectPath.empty()) {
        auto* initialState = reinterpret_cast<AppState*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
        if (initialState != nullptr) {
            EnterCriticalSection(&initialState->audio.audioStateLock);
            LoadProject(startupProjectPath, *initialState);
            LeaveCriticalSection(&initialState->audio.audioStateLock);
            if (initialState->audio.trackInsertDspState.size() != initialState->core.project.tracks.size()) initialState->audio.trackInsertDspState.resize(initialState->core.project.tracks.size());
            UpdateWindowTitle(hwnd, initialState->core);
        }
    }

    SetFocus(hwnd);
    ShowWindow(hwnd, nCmdShow);

    MSG msg{};
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return 0;
}
