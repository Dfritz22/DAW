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
#include "threading/thread_identity.h"
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
#include "ui/repaint.h"

using daw::internal::core::UpdateWindowTitle;

const wchar_t* BusName(int busIndex) {
    static const wchar_t* kNames[kBusCount] = {L"Drums", L"Music", L"Vocals", L"Master"};
    if (busIndex < 0 || busIndex >= kBusCount) {
        return L"Music";
    }
    return kNames[busIndex];
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<AppState*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));

    switch (msg) {
    case WM_CREATE: {
        // Pick up the monitor's DPI before the first paint so HiDPI layout is correct from frame zero.
        g_uiDpi = static_cast<int>(GetDpiForWindow(hwnd));
        if (g_uiDpi <= 0) g_uiDpi = 96;

        auto* initial = new AppState();
        initial->hwnd = hwnd;
        AudioInitializeRuntime(hwnd, initial->core, initial->audio);
        SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(initial));

        if (HMENU bar = UiBuildMainMenuBar(*initial); bar != nullptr) {
            SetMenu(hwnd, bar);
            DrawMenuBar(hwnd);
        }

        SetTimer(hwnd, kPlaybackTimerId, kPlaybackTimerMs, nullptr);
        return 0;
    }
    case WM_SIZE:
        // Default WM_SIZE only invalidates newly-exposed area; force full redraw so layout reflows.
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_DPICHANGED: {
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
        // HIWORD(lParam) != 0 means the system (window) menu — let Windows render that.
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
            // Persist dock layout + floating-panel geometry so next launch restores arrangement.
            if (state->ui.dock.dockRoot) {
                std::vector<daw::ui::DockFloatingPanel> floats;
                floats.reserve(state->ui.dock.floatingPanels.size());
                for (const auto& fp : state->ui.dock.floatingPanels) {
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
                daw::ui::DockSaveLayout(state->ui.dock.dockRoot.get(), floats);
            }
            // Phase 20 / Step H: explicit floating-window teardown.
            // FloatingWndData holds a raw AppState*; if a float's WM_CLOSE fires
            // after `delete state` below (out-of-order session-end), we'd crash.
            // Snapshot hwnds (each DestroyWindow triggers WM_DESTROY which erases
            // from state->ui.dock.floatingPanels — iterating a copy avoids
            // vector invalidation), DestroyWindow each, then drain the message
            // queue so all WM_DESTROY/WM_NCDESTROY are processed before we free
            // the state they reference.
            {
                std::vector<HWND> floatHwnds;
                floatHwnds.reserve(state->ui.dock.floatingPanels.size());
                for (const auto& fp : state->ui.dock.floatingPanels) {
                    if (fp.hwnd && IsWindow(fp.hwnd)) floatHwnds.push_back(fp.hwnd);
                }
                for (HWND fh : floatHwnds) {
                    DestroyWindow(fh);
                }
                MSG drainMsg;
                while (PeekMessageW(&drainMsg, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&drainMsg);
                    DispatchMessageW(&drainMsg);
                }
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
            // Engine signaled natural end-of-song; route through FSM as the single transport source of truth.
            daw::app::DispatchTransportEvent(hwnd, *state,
                daw::services::TransportEvent::StopPressed,
                /*rewindOnStop=*/false);
            daw::ui::RequestRepaintAll(*state);
        }
        return 0;
    case kMsgCountInComplete:
        if (state != nullptr) {
            // Audio thread already cleared audio.countingIn; FSM observes CountingIn → Recording.
            daw::app::DispatchTransportEvent(hwnd, *state,
                daw::services::TransportEvent::CountInComplete,
                /*rewindOnStop=*/false);
            daw::ui::RequestRepaintAll(*state);
        }
        return 0;
    case kMsgAutoMixFinished:
        if (state != nullptr) {
            if (state->audio.automixThread != nullptr) {
                CloseHandle(state->audio.automixThread);
                state->audio.automixThread = nullptr;
            }
            daw::ui::RequestRepaintAll(*state);
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
        return 1; // Fully repainted in WM_PAINT via backbuffer.
    case WM_PAINT:
        if (state == nullptr) return DefWindowProc(hwnd, msg, wParam, lParam);
        return WndProcOnPaint(hwnd, *state);
    case WM_SETCURSOR: {
        // Resize cursor when hovering or dragging a dock splitter.
        if (state != nullptr && reinterpret_cast<HWND>(wParam) == hwnd && LOWORD(lParam) == HTCLIENT) {
            POINT pt{};
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            for (const auto& sp : state->ui.dock.dockSplitters) {
                if (PtInRect(&sp.rect, pt) || (state->ui.dock.draggingSplitter && state->ui.dock.dragSplitterNode == sp.node)) {
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
    // Phase 25 / Step L1 — record the main thread id before anything else so
    // daw::threading::IsMainThread() answers correctly from the first
    // WndProc dispatch onward.
    daw::threading::RegisterMainThread();

    // Optional .dawproj path as first command-line argument.
    std::wstring startupProjectPath;
    if (lpCmdLine && lpCmdLine[0] != L'\0') {
        std::wstring arg = lpCmdLine;
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

    // Floating tear-off windows: separate WindowProc, paints its own background.
    WNDCLASS fwc{};
    fwc.lpfnWndProc   = FloatingWindowProc;
    fwc.hInstance     = hInstance;
    fwc.lpszClassName = kFloatingClassName;
    fwc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    fwc.hbrBackground = nullptr;
    RegisterClass(&fwc);

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
