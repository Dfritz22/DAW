// transport_control.cpp — extracted from main.cpp Phase 16q.
//
// "Audio orchestration" entry points called by user-input dispatchers
// (transport_adapter.cpp + WM_DESTROY teardown + kMsgPlaybackFinished bypass).
// Coordinates both the MME and WASAPI backends through the Device*Backend
// app-state wrappers in app_state_api.cpp.
//
// Public decls live in include/daw_audio.h (have for many phases — these
// definitions previously lived in main.cpp). No new header is needed.

#include "AppState.h"
#include "audio/engine_utils.h"        // ResampleStereoFloatLinear
#include "core/internal_app_services.h" // UpdateWindowTitle
#include "daw_audio.h"
#include "ui/UiRuntimeState.h"          // kPalette

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>
#include <windows.h>

using daw::internal::core::UpdateWindowTitle;

void StopPlayback(AppState& state, bool rewind) {
    DeviceStopPlaybackBackend(state);

    state.audio.playing = false;
    state.audio.audioThreadRunning.store(false);
    if (rewind) {
        state.ui.view.playheadBeat = 0.0f;
        state.audio.playbackFrameCursor.store(0);
    }
    // Force the engine SRC resampler to re-prime on the next start so it
    // begins from the new cursor position with a fresh phase / carry instead
    // of interpolating from a stale boundary frame.
    state.audio.engineSrcPrimed = false;
    state.audio.engineSrcPhase  = 0.0;

    // Clear DSP insert-chain state on every track and bus so reverb tails,
    // delay lines, and compressor envelopes don't bleed into the next play.
    // Without this you'd hear a faint echo of the last position when starting
    // playback in a silent area.
    EnterCriticalSection(&state.audio.audioStateLock);
    for (auto& slotArray : state.audio.trackInsertDspState) {
        slotArray = InsertDspStateArray{};
    }
    for (auto& slotArray : state.audio.busInsertDspState) {
        slotArray = InsertDspStateArray{};
    }
    LeaveCriticalSection(&state.audio.audioStateLock);
}

void StopRecording(AppState& state, bool commitTake) {
    if (!state.audio.recording && state.audio.recordThread == nullptr) {
        return;
    }

    DeviceStopRecordingBackend(state);

    if (commitTake && state.audio.recordTrackIndex >= 0 && !state.audio.recordedInputPcm.empty()) {
        const int channels = std::max(1, state.audio.recordInputChannels);
        const std::uint32_t totalFrames = static_cast<std::uint32_t>(state.audio.recordedInputPcm.size() / static_cast<size_t>(channels));
        const ULONGLONG nowTick = GetTickCount64();
        const ULONGLONG elapsedMsUll = (state.audio.recordCaptureStartTickMs > 0 && nowTick > state.audio.recordCaptureStartTickMs)
            ? (nowTick - state.audio.recordCaptureStartTickMs)
            : 0ULL;
        const int elapsedMs = static_cast<int>(std::min<ULONGLONG>(elapsedMsUll, static_cast<ULONGLONG>(std::numeric_limits<int>::max())));
        const int captureSampleRate = (state.audio.lastOpenedInputSampleRate > 0)
            ? state.audio.lastOpenedInputSampleRate
            : state.core.project.projectSampleRate;
        const double expectedFrames = (elapsedMs > 0 && captureSampleRate > 0)
            ? (static_cast<double>(elapsedMs) * static_cast<double>(captureSampleRate) / 1000.0)
            : 0.0;
        const double observedRatio = (expectedFrames > 1.0)
            ? (static_cast<double>(totalFrames) / expectedFrames)
            : 1.0;

        int frameStride = 1;
        const double rounded = std::round(observedRatio);
        if (rounded >= 2.0 && rounded <= 4.0 && std::fabs(observedRatio - rounded) <= 0.20) {
            frameStride = static_cast<int>(rounded);
        }

        state.audio.lastCaptureElapsedMs = elapsedMs;
        state.audio.lastCaptureObservedRateRatio = observedRatio;
        state.audio.lastCaptureFrameStride = frameStride;

        // The capture thread already discards every buffer that contained
        // count-in audio (it drains queued buffers the instant countingIn
        // flips false). So recordedInputPcm starts at the first sample AFTER
        // the count-in, which corresponds 1:1 with timeline frame
        // recordStartFrame. We must NOT subtract recordPrerollFrames again
        // here -- doing so would chop the first measure of real audio off
        // the front of the take and shift the remaining audio earlier.
        const std::uint32_t frames = totalFrames / static_cast<std::uint32_t>(frameStride);
        const std::uint32_t skipFramesRaw = 0;

        if (frames > 0) {
            std::vector<float> stereo(static_cast<size_t>(frames) * 2, 0.0f);
            for (std::uint32_t f = 0; f < frames; ++f) {
                float l = 0.0f;
                float r = 0.0f;
                const std::uint32_t srcFrame = skipFramesRaw + f * static_cast<std::uint32_t>(frameStride);
                if (channels == 1) {
                    const float v = static_cast<float>(state.audio.recordedInputPcm[static_cast<size_t>(srcFrame)]) / 32768.0f;
                    l = v;
                    r = v;
                } else {
                    const size_t base = static_cast<size_t>(srcFrame) * static_cast<size_t>(channels);
                    l = static_cast<float>(state.audio.recordedInputPcm[base]) / 32768.0f;
                    r = static_cast<float>(state.audio.recordedInputPcm[base + 1]) / 32768.0f;
                }
                stereo[static_cast<size_t>(f) * 2] = l;
                stereo[static_cast<size_t>(f) * 2 + 1] = r;
            }

            // Capture-side sample rate conversion. The captured stereo buffer
            // is at the device's input sample rate (captureSampleRate). The
            // project owns its sample rate, so we convert to project SR on
            // commit — same approach as the import path. This keeps every
            // LoadedAudio in the project at audio.sampleRate == projectSR.
            std::uint32_t committedFrames    = frames;
            int           committedSampleRate = captureSampleRate;
            const int     projectSR          = state.core.project.projectSampleRate;
            if (projectSR > 0 && captureSampleRate > 0 && captureSampleRate != projectSR && frames > 0) {
                const std::uint64_t dstFrames64 =
                    (static_cast<std::uint64_t>(frames) * static_cast<std::uint64_t>(projectSR)
                     + static_cast<std::uint64_t>(captureSampleRate) / 2)
                    / static_cast<std::uint64_t>(captureSampleRate);
                if (dstFrames64 > 0 && dstFrames64 <= static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
                    const int dstFrames = static_cast<int>(dstFrames64);
                    std::vector<float> resampled(static_cast<size_t>(dstFrames) * 2, 0.0f);
                    ResampleStereoFloatLinear(stereo.data(), static_cast<int>(frames),
                                              resampled.data(), dstFrames);
                    stereo              = std::move(resampled);
                    committedFrames     = static_cast<std::uint32_t>(dstFrames);
                    committedSampleRate = projectSR;
                }
            }

            LoadedAudio take{};
            take.sourcePath = L"[recording]";
            take.displayName = L"Take " + std::to_wstring(static_cast<int>(state.core.project.audio.size()) + 1);
            take.sampleRate = committedSampleRate;
            take.frames = committedFrames;
            take.stereo = std::move(stereo);

            state.audio.lastCommittedTakeSampleRate = take.sampleRate;
            state.audio.lastCommittedTakeFrames = static_cast<int>(take.frames);
            state.audio.lastCommittedTakeChannels = 2;

            const COLORREF clipColors[4] = {kPalette.clip1, kPalette.clip2, kPalette.clip3, kPalette.clip4};
            EnterCriticalSection(&state.audio.audioStateLock);
            const int audioIndex = static_cast<int>(state.core.project.audio.size());
            state.core.project.audio.push_back(std::move(take));

            // The engine holds the playback cursor at recordStartFrame for the
            // entire count-in (count-in runs in its own time domain), and the
            // record thread only begins writing once countingIn flips false.
            // So the captured PCM aligns 1:1 with timeline frames starting at
            // recordStartFrame, and the clip is placed there.
            const float startBeat = BeatsFromFrames(state, state.audio.recordStartFrame);
            const float lengthBeats = BeatsFromFrames(state, committedFrames);

            if (state.audio.recordTrackIndex >= 0 && state.audio.recordTrackIndex < static_cast<int>(state.core.project.tracks.size())) {
                state.core.project.clips.push_back(ClipItem{
                    state.audio.recordTrackIndex,
                    audioIndex,
                    startBeat,
                    std::max(0.25f, lengthBeats),
                    clipColors[state.audio.recordTrackIndex % 4],
                    state.core.project.tracks[static_cast<size_t>(state.audio.recordTrackIndex)].name + L" Rec",
                });
                state.core.projectModified = true;
                if (state.hwnd) UpdateWindowTitle(state.hwnd, state.core);
            }
            LeaveCriticalSection(&state.audio.audioStateLock);
        }
    }

    state.audio.recordedInputPcm.clear();
    state.audio.monitorInputPcm.clear();
    state.audio.monitorInputReadPos = 0;
    state.audio.recordInputChannels = 0;
    state.audio.recordTrackIndex = -1;
    state.audio.recordCaptureStartTickMs = 0;
    state.audio.recordStartFrame = 0;
    state.audio.liveRecordingClip = ClipItem{};
    state.audio.liveRecordingWaveform.clear();
    state.audio.liveRecordingFramesProcessed = 0;
    state.audio.recordPrerollFrames = 0;
    state.audio.countingIn = false;
    state.audio.recordUsingWasapi = false;
    state.audio.recordInitState.store(0);
    state.audio.recording = false;
}

bool StartRecording(HWND hwnd, AppState& state) {
    if (state.audio.recording) {
        return true;
    }

    // Phase 21 / Step C — engine-readiness contract.
    // Callers must route through DispatchTransportEvent which observes
    // TransportStateFromAudio + WillCountIn; both require a resolved device
    // SR. AudioEnsureReadyForTransport below is the runtime guard; this
    // assert is the debug-build contract-violation catch.
    assert(state.audio.activeDeviceSampleRate > 0
           && "StartRecording called before activeDeviceSampleRate resolved");

    int armedTrack = -1;
    for (size_t i = 0; i < state.core.project.tracks.size(); ++i) {
        if (state.core.project.tracks[i].recordArm) {
            armedTrack = static_cast<int>(i);
            break;
        }
    }
    if (armedTrack < 0) {
        MessageBoxW(hwnd, L"Arm a track first using the R button.", L"Record", MB_OK | MB_ICONINFORMATION);
        return false;
    }

    // Engine readiness invariant: SR known, devices enumerated, no Error state.
    if (!AudioEnsureReadyForTransport(state.core, state.audio)) {
        const std::wstring& reason = state.audio.engineInitError;
        MessageBoxW(hwnd,
            reason.empty() ? L"Audio engine is not ready." : reason.c_str(),
            L"Record", MB_OK | MB_ICONERROR);
        return false;
    }

    const bool wasPlaying = state.audio.playing;
    return DeviceStartRecordingBackend(hwnd, state, armedTrack, wasPlaying);
}

bool StartPlayback(HWND hwnd, AppState& state) {
    state.audio.lastPlaybackInitError.clear();

    // Phase 21 / Step C — engine-readiness contract. See StartRecording.
    assert(state.audio.activeDeviceSampleRate > 0
           && "StartPlayback called before activeDeviceSampleRate resolved");

    // Engine readiness invariant: SR known, devices enumerated, no Error state.
    if (!AudioEnsureReadyForTransport(state.core, state.audio)) {
        const std::wstring& reason = state.audio.engineInitError;
        MessageBoxW(hwnd,
            reason.empty() ? L"Audio engine is not ready." : reason.c_str(),
            L"Playback error", MB_OK | MB_ICONERROR);
        return false;
    }

    DeviceRefreshOutputDevices(state);
    if (state.audio.outputDeviceIds.empty()) {
        MessageBoxW(hwnd, L"No audio output devices detected.", L"Playback error", MB_OK | MB_ICONERROR);
        return false;
    }

    if (state.core.project.clips.empty() && !state.audio.metronomePlay && !state.audio.metronomeRecord && !state.audio.countInEnabled) {
        MessageBoxW(hwnd, L"Import at least one supported WAV file first.", L"No audio to play", MB_OK | MB_ICONINFORMATION);
        return false;
    }

    StopPlayback(state, false);

    return DeviceStartPlaybackBackend(hwnd, state);
}
