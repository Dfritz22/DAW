#include "ui/wndproc/rbutton.h"

#include "core/internal_app_services.h"
#include "daw_project.h"
#include "ui/dpi.h"
#include "ui/layout.h"
#include "ui/UiRuntimeState.h"

#include <windowsx.h>
#include "ui/repaint.h"

LRESULT WndProcOnRButtonUp(HWND hwnd, LPARAM lParam, AppState& state) {
    using daw::internal::core::UpdateWindowTitle;

    RECT client{};
    GetClientRect(hwnd, &client);
    const LayoutRects layout = UiLayoutComputeHitTestLayout(hwnd, state);
    const POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};

    if (!PtInRect(&layout.leftPanel, pt) && !PtInRect(&layout.arrange, pt)) {
        return 0;
    }

    HMENU menu = CreatePopupMenu();
    if (menu == nullptr) {
        return 0;
    }

    AppendMenuW(menu, MF_STRING, kCmdTrackNew, L"New Track");

    int trackIndex = -1;
    if (pt.y > layout.leftPanel.top + Dpi(kRulerHeight) && !state.core.project.tracks.empty()) {
        trackIndex = UiLayoutTrackIndexFromY(layout.arrange, state, pt.y);
    }
    if (trackIndex >= 0 && trackIndex < static_cast<int>(state.core.project.tracks.size())) {
        const bool armed = state.core.project.tracks[static_cast<size_t>(trackIndex)].recordArm;
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kCmdTrackNew + 1000, armed ? L"Disarm Track" : L"Arm Track");
    }

    POINT screenPt{pt.x, pt.y};
    ClientToScreen(hwnd, &screenPt);
    const UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, screenPt.x, screenPt.y, 0, hwnd, nullptr);
    DestroyMenu(menu);

    if (cmd == kCmdTrackNew) {
        AddNewTrack(state);
        state.core.projectModified = true;
        UpdateWindowTitle(hwnd, state.core);
        daw::ui::RequestRepaintAll(state);
    } else if (cmd == kCmdTrackNew + 1000 && trackIndex >= 0 && trackIndex < static_cast<int>(state.core.project.tracks.size())) {
        EnterCriticalSection(&state.audio.audioStateLock);
        state.core.project.tracks[static_cast<size_t>(trackIndex)].recordArm = !state.core.project.tracks[static_cast<size_t>(trackIndex)].recordArm;
        LeaveCriticalSection(&state.audio.audioStateLock);
        daw::ui::RequestRepaintAll(state);
    }
    return 0;
}
