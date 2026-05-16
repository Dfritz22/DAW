#include "ui/repaint.h"

#define NOMINMAX
#include <windows.h>

#include "AppState.h"

namespace daw::ui {

void RequestRepaintAll(AppState& state) {
    if (state.hwnd != nullptr && IsWindow(state.hwnd)) {
        InvalidateRect(state.hwnd, nullptr, FALSE);
    }
    for (const auto& fp : state.ui.dock.floatingPanels) {
        if (fp.hwnd != nullptr && IsWindow(fp.hwnd)) {
            InvalidateRect(fp.hwnd, nullptr, FALSE);
        }
    }
}

}  // namespace daw::ui
