#include "ui/repaint.h"

#define NOMINMAX
#include <windows.h>

#include "AppState.h"
#include "audio/mix_snapshot_builder.h"

namespace daw::ui {

void RequestRepaintAll(AppState& state) {
    // Phase 24 / Step K5c.1 \u2014 publish a fresh MixSnapshot for the audio
    // thread on every repaint. Every UI-thread mutation site already
    // calls RequestRepaintAll after editing core (track gain/pan/bus/
    // mute, insert chains, clip add/move/delete, project load/import/
    // orchestration, undo/redo), so hooking publish here gives
    // comprehensive coverage with one edit and no risk of missing a
    // mutator. The extra publishes on no-mutation repaints (e.g. timer
    // tick for playhead animation) cost only a few microseconds of
    // snapshot rebuild on the UI thread and never block the audio
    // thread \u2014 the publisher is atomic-shared_ptr.
    PublishMixSnapshotFromCore(state.audio, state.core);

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
