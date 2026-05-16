#include "ui/wndproc/timer.h"

#include "daw_audio.h"
#include "daw_timeline.h"
#include "vm/timeline_zoom.h"

#include <algorithm>
#include <cstdint>
#include "ui/repaint.h"

LRESULT WndProcOnPlaybackTimer(HWND hwnd, AppState& state) {
    (void)hwnd;
    bool needRepaint = false;

    // Update playhead during playback (also runs during recording, since
    // recording starts the playback backend).
    if (state.audio.playing) {
        const std::uint64_t absoluteFrame = DeviceGetRenderedPlaybackFrame(state);
        state.ui.view.playheadBeat = BeatsFromFrames(state, absoluteFrame);

        const float viewRight = state.ui.view.viewStartBeat + state.ui.view.viewBeatsVisible;
        if (state.ui.view.playheadBeat > viewRight - 1.0f) {
            state.ui.view.viewStartBeat = daw::vm::AutoScrollViewStart(state.ui.view.viewBeatsVisible, state.ui.view.playheadBeat);
        }
        needRepaint = true;
    }

    // Update live recording waveform display. Runs in parallel with the
    // playhead update above; recording sets both `playing` and `recording`
    // to true.
    if (state.audio.recording) {
        EnterCriticalSection(&state.audio.audioStateLock);
        const int channels = std::max(1, state.audio.recordInputChannels);
        const std::uint64_t currentFrames = static_cast<std::uint64_t>(
            state.audio.recordedInputPcm.size() / static_cast<size_t>(channels));

        if (currentFrames > state.audio.liveRecordingFramesProcessed) {
            state.audio.liveRecordingWaveform.resize(static_cast<size_t>(currentFrames) * 2, 0.0f);

            for (std::uint64_t f = state.audio.liveRecordingFramesProcessed; f < currentFrames; ++f) {
                float l = 0.0f;
                float r = 0.0f;
                const size_t base = static_cast<size_t>(f) * static_cast<size_t>(channels);
                if (base < state.audio.recordedInputPcm.size()) {
                    l = static_cast<float>(state.audio.recordedInputPcm[base]) / 32768.0f;
                    if (channels > 1 && base + 1 < state.audio.recordedInputPcm.size()) {
                        r = static_cast<float>(state.audio.recordedInputPcm[base + 1]) / 32768.0f;
                    } else {
                        r = l;
                    }
                }
                state.audio.liveRecordingWaveform[static_cast<size_t>(f) * 2]     = l;
                state.audio.liveRecordingWaveform[static_cast<size_t>(f) * 2 + 1] = r;
            }

            state.audio.liveRecordingClip.startBeat = BeatsFromFrames(state, state.audio.recordStartFrame);
            state.audio.liveRecordingClip.lengthBeats = std::max(0.25f, BeatsFromFrames(state, currentFrames));
            state.audio.liveRecordingFramesProcessed = currentFrames;
        }
        LeaveCriticalSection(&state.audio.audioStateLock);
        needRepaint = true;
    }

    if (needRepaint) {
        daw::ui::RequestRepaintAll(state);
    }
    return 0;
}
