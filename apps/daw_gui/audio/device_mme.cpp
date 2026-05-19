#include "device_mme.h"
#include "device_common.h"
#include "engine.h"
#include "engine_utils.h"
#include "rt_alloc_trace.h"
#include "core/automation.h"
#include "core/timeline.h"

constexpr UINT kMsgPlaybackFinished = WM_APP + 1;

namespace daw::internal::audio {

static DWORD WINAPI AudioThreadProc(LPVOID param) {
    auto* audio = reinterpret_cast<AudioRuntimeState*>(param);
    if (audio == nullptr || audio->coreContext == nullptr) {
        return 0;
    }

    // Phase 22 / Step D — mark this thread as the realtime audio thread for
    // the duration of the callback loop. Under DAW_RT_ALLOC_TRACE, any heap
    // allocation while this scope is active will trip an assert.
    daw::audio::RtAudioThreadScope rtScope;

    bool draining = false;
    while (!audio->audioStopRequested.load()) {
        bool anyInQueue = false;

        for (int i = 0; i < static_cast<int>(audio->waveHeaders.size()); ++i) {
            WAVEHDR& hdr = audio->waveHeaders[static_cast<size_t>(i)];
            if ((hdr.dwFlags & WHDR_INQUEUE) != 0) {
                anyInQueue = true;
                continue;
            }

            if (draining) {
                continue;
            }

            bool reachedEnd = false;
            // Phase 23 / Step J — non-blocking audio callback. Try the lock;
            // if a UI mutator (project save, AutoMix, future plugin GUI) is
            // holding it, emit silence and bump the miss counter rather than
            // stalling the audio thread. A glitch is preferable to a
            // full-callback block under contention.
            if (TryEnterCriticalSection(&audio->audioStateLock)) {
                EngineFillRealtimeForDeviceLocked(
                    *audio->coreContext,
                    *audio,
                    audio->waveData[static_cast<size_t>(i)].data(),
                    kAudioBufferFrames,
                    static_cast<int>(audio->waveFormat.nSamplesPerSec),
                    &reachedEnd);
                LeaveCriticalSection(&audio->audioStateLock);
            } else {
                audio->audioCallbackLockMisses.fetch_add(1, std::memory_order_relaxed);
                auto& buf = audio->waveData[static_cast<size_t>(i)];
                std::memset(buf.data(), 0, buf.size() * sizeof(std::int16_t));
            }

            hdr.lpData = reinterpret_cast<LPSTR>(audio->waveData[static_cast<size_t>(i)].data());
            hdr.dwBufferLength = static_cast<DWORD>(audio->waveData[static_cast<size_t>(i)].size() * sizeof(std::int16_t));
            hdr.dwFlags &= ~WHDR_DONE;

            if (waveOutWrite(audio->waveOut, &hdr, sizeof(WAVEHDR)) == MMSYSERR_NOERROR) {
                anyInQueue = true;
            }

            if (reachedEnd) {
                draining = true;
            }
        }

        if (draining && !anyInQueue) {
            PostMessage(audio->hwnd, kMsgPlaybackFinished, 0, 0);
            break;
        }

        Sleep(4);
    }

    audio->audioThreadRunning.store(false);
    return 0;
}

static DWORD WINAPI RecordThreadProc(LPVOID param) {
    auto* audio = reinterpret_cast<AudioRuntimeState*>(param);
    if (audio == nullptr) {
        return 0;
    }

    // True once we've drained any in-flight buffers that contained samples
    // captured during count-in. Until that drain happens, the very first
    // buffer we see after countingIn flips false would otherwise prepend
    // up to (kRecordBufferCount * kRecordBufferFrames) of stale audio,
    // which is what made captures appear ~hundreds-of-ms behind the playhead.
    bool prerollDrained = false;

    while (!audio->recordStopRequested.load()) {
        // Detect the count-in -> capture transition and drain stale buffers.
        if (!prerollDrained && !audio->countingIn) {
            for (size_t i = 0; i < audio->waveInHeaders.size(); ++i) {
                WAVEHDR& hdr = audio->waveInHeaders[i];
                if ((hdr.dwFlags & WHDR_DONE) == 0) {
                    continue;
                }
                hdr.dwBytesRecorded = 0;
                hdr.dwFlags &= ~WHDR_DONE;
                if (audio->waveIn != nullptr) {
                    waveInAddBuffer(audio->waveIn, &hdr, sizeof(WAVEHDR));
                }
            }
            prerollDrained = true;
        }

        for (size_t i = 0; i < audio->waveInHeaders.size(); ++i) {
            WAVEHDR& hdr = audio->waveInHeaders[i];
            if ((hdr.dwFlags & WHDR_DONE) == 0) {
                continue;
            }

            if (hdr.dwBytesRecorded > 0) {
                const size_t sampleCount = static_cast<size_t>(hdr.dwBytesRecorded / sizeof(std::int16_t));
                const std::int16_t* samples = reinterpret_cast<const std::int16_t*>(hdr.lpData);
                // Only record audio after count-in ends (do not capture during count-in).
                // prerollDrained guarantees we never append a buffer that was
                // partly filled while countingIn was true.
                if (!audio->countingIn && prerollDrained) {
                    audio->recordedInputPcm.insert(audio->recordedInputPcm.end(), samples, samples + sampleCount);
                }
                hdr.dwBytesRecorded = 0;
                hdr.dwFlags &= ~WHDR_DONE;
                if (!audio->recordStopRequested.load() && audio->waveIn != nullptr) {
                    waveInAddBuffer(audio->waveIn, &hdr, sizeof(WAVEHDR));
                }

                if (audio->inputMonitoring && !audio->countingIn && prerollDrained) {
                    EnterCriticalSection(&audio->audioStateLock);
                    audio->monitorInputPcm.insert(audio->monitorInputPcm.end(), samples, samples + sampleCount);
                    LeaveCriticalSection(&audio->audioStateLock);
                }
            } else if (!audio->recordStopRequested.load() && audio->waveIn != nullptr) {
                hdr.dwBytesRecorded = 0;
                hdr.dwFlags &= ~WHDR_DONE;
                waveInAddBuffer(audio->waveIn, &hdr, sizeof(WAVEHDR));
            }
        }
        Sleep(4);
    }

    return 0;
}

} // namespace daw::internal::audio

using namespace daw::internal::audio;

void DeviceStopMmeAudio(AudioRuntimeState& audio) {
    audio.audioStopRequested.store(true);

    if (audio.audioThread != nullptr) {
        WaitForSingleObject(audio.audioThread, INFINITE);
        CloseHandle(audio.audioThread);
        audio.audioThread = nullptr;
    }

    if (audio.waveOut != nullptr) {
        waveOutReset(audio.waveOut);
        for (size_t i = 0; i < audio.waveHeaders.size(); ++i) {
            waveOutUnprepareHeader(audio.waveOut, &audio.waveHeaders[i], sizeof(WAVEHDR));
        }
        waveOutClose(audio.waveOut);
        audio.waveOut = nullptr;
    }
    audio.waveHeaders.clear();
    audio.waveData.clear();
}

bool DeviceStartMmeAudio(HWND hwnd, CoreState& core, AudioRuntimeState& audio, float playheadBeat) {
    audio.hwnd = hwnd;
    audio.coreContext = &core;

    int mmeSampleRate = 0;
    if (audio.preferredSampleRate > 0) {
        mmeSampleRate = audio.preferredSampleRate;
    } else if (core.project.projectSampleRate > 0) {
        mmeSampleRate = core.project.projectSampleRate;
    } else if (audio.lastOpenedOutputSampleRate > 0) {
        mmeSampleRate = audio.lastOpenedOutputSampleRate;
    }
    if (mmeSampleRate <= 0) {
        MessageBoxW(hwnd,
            L"No playback sample rate is configured.\nOpen Audio menu and choose a sample rate first.",
            L"Playback error",
            MB_OK | MB_ICONERROR);
        return false;
    }

    audio.waveFormat.nChannels = 2;
    audio.waveFormat.nSamplesPerSec = static_cast<DWORD>(mmeSampleRate);
    audio.waveFormat.wBitsPerSample = 16;
    audio.waveFormat.nBlockAlign = static_cast<WORD>((audio.waveFormat.nChannels * audio.waveFormat.wBitsPerSample) / 8);
    audio.waveFormat.nAvgBytesPerSec = audio.waveFormat.nSamplesPerSec * audio.waveFormat.nBlockAlign;
    audio.waveFormat.cbSize = 0;

    MMRESULT outOpen = waveOutOpen(&audio.waveOut, audio.selectedOutputDeviceId, &audio.waveFormat, 0, 0, CALLBACK_NULL);
    if (outOpen != MMSYSERR_NOERROR) {
        outOpen = waveOutOpen(&audio.waveOut, WAVE_MAPPER, &audio.waveFormat, 0, 0, CALLBACK_NULL);
    }
    if (outOpen != MMSYSERR_NOERROR) {
        wchar_t mmeErr[256]{};
        if (waveOutGetErrorTextW(outOpen, mmeErr, static_cast<UINT>(std::size(mmeErr))) != MMSYSERR_NOERROR) {
            wcscpy_s(mmeErr, L"Unknown MME error");
        }
        if (audio.lastPlaybackInitError.empty()) {
            audio.lastPlaybackInitError = std::wstring(L"MME open failed: ") + mmeErr;
        } else {
            audio.lastPlaybackInitError += std::wstring(L"; MME open failed: ") + mmeErr;
        }
        audio.waveOut = nullptr;
        const std::wstring msg = L"Unable to open selected output device.\n\n" + audio.lastPlaybackInitError;
        MessageBoxW(hwnd, msg.c_str(), L"Playback error", MB_OK | MB_ICONERROR);
        return false;
    }

    audio.lastOpenedOutputSampleRate = static_cast<int>(audio.waveFormat.nSamplesPerSec);
    audio.lastOpenedOutputChannels = static_cast<int>(audio.waveFormat.nChannels);
    audio.activeDeviceSampleRate = audio.lastOpenedOutputSampleRate;
    audio.activeDeviceBufferFrames = kAudioBufferFrames;

    // Snap the user's requested rate to whatever the device actually opened
    // at, so the Audio Settings UI and persisted project file reflect reality
    // when the device refuses an SR change.
    if (audio.preferredSampleRate > 0 &&
        audio.activeDeviceSampleRate > 0 &&
        audio.preferredSampleRate != audio.activeDeviceSampleRate) {
        audio.preferredSampleRate = audio.activeDeviceSampleRate;
    }

    audio.waveData.assign(kAudioBufferCount, std::vector<std::int16_t>(kAudioBufferFrames * 2, 0));
    audio.waveHeaders.assign(kAudioBufferCount, WAVEHDR{});

    for (int i = 0; i < kAudioBufferCount; ++i) {
        WAVEHDR& hdr = audio.waveHeaders[static_cast<size_t>(i)];
        hdr.lpData = reinterpret_cast<LPSTR>(audio.waveData[static_cast<size_t>(i)].data());
        hdr.dwBufferLength = static_cast<DWORD>(audio.waveData[static_cast<size_t>(i)].size() * sizeof(std::int16_t));
        hdr.dwFlags = 0;
        if (waveOutPrepareHeader(audio.waveOut, &hdr, sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
            DeviceStopMmeAudio(audio);
            MessageBoxW(hwnd, L"Unable to prepare audio buffers.", L"Playback error", MB_OK | MB_ICONERROR);
            return false;
        }
    }

    EnterCriticalSection(&audio.audioStateLock);
    const float startBeat = std::max(0.0f, playheadBeat);
    const std::uint64_t startFrame = TimelineFramesFromBeats(core, startBeat);
    audio.playbackFrameCursor.store(startFrame);
    audio.engineSrcPrimed = false;
    audio.engineSrcPhase  = 0.0;
    const std::uint64_t endFrame = ComputeProjectEndFrameLocked(core);
    LeaveCriticalSection(&audio.audioStateLock);

    audio.playbackStartTick = GetTickCount64();
    audio.playbackStartBeat = startBeat;
    audio.playbackEndBeat = TimelineBeatsFromFrames(core, endFrame);
    audio.playing = true;
    audio.audioStopRequested.store(false);
    audio.audioThreadRunning.store(true);
    audio.audioThread = CreateThread(nullptr, 0, AudioThreadProc, &audio, 0, nullptr);

    if (audio.audioThread == nullptr) {
        DeviceStopMmeAudio(audio);
        audio.playing = false;
        MessageBoxW(hwnd, L"Unable to start audio thread.", L"Playback error", MB_OK | MB_ICONERROR);
        return false;
    }

    return true;
}

void DeviceStopMmeRecording(AudioRuntimeState& audio) {
    audio.recordStopRequested.store(true);

    if (audio.recordThread != nullptr) {
        WaitForSingleObject(audio.recordThread, INFINITE);
        CloseHandle(audio.recordThread);
        audio.recordThread = nullptr;
    }

    if (audio.waveIn != nullptr) {
        waveInStop(audio.waveIn);
        waveInReset(audio.waveIn);
        for (size_t i = 0; i < audio.waveInHeaders.size(); ++i) {
            waveInUnprepareHeader(audio.waveIn, &audio.waveInHeaders[i], sizeof(WAVEHDR));
        }
        waveInClose(audio.waveIn);
        audio.waveIn = nullptr;
    }

    audio.waveInHeaders.clear();
    audio.waveInData.clear();
}

bool DeviceStartMmeRecording(HWND hwnd, CoreState& core, AudioRuntimeState& audio, int armedTrack, bool wasPlaying, float playheadBeat) {
    audio.hwnd = hwnd;
    audio.coreContext = &core;

    std::vector<int> sampleRates;
    if (audio.preferredSampleRate > 0) {
        sampleRates.push_back(audio.preferredSampleRate);
    }
    if (core.project.projectSampleRate > 0) {
        sampleRates.push_back(core.project.projectSampleRate);
    }
    if (audio.lastOpenedInputSampleRate > 0 &&
        std::find(sampleRates.begin(), sampleRates.end(), audio.lastOpenedInputSampleRate) == sampleRates.end()) {
        sampleRates.push_back(audio.lastOpenedInputSampleRate);
    }
    if (sampleRates.empty()) {
        const int deviceSampleRate = DeviceProbeCurrentOutputSampleRate(audio);
        if (deviceSampleRate > 0) {
            sampleRates.push_back(deviceSampleRate);
        }
    }
    if (sampleRates.empty()) {
        MessageBoxW(hwnd,
            L"No input sample rate is configured.\nOpen Audio menu and choose a sample rate first.",
            L"Record",
            MB_OK | MB_ICONERROR);
        return false;
    }

    int chosenSampleRate = 0;

    auto fillFormat = [&](int channels, int sampleRate) {
        audio.waveInFormat.wFormatTag = WAVE_FORMAT_PCM;
        audio.waveInFormat.nChannels = static_cast<WORD>(channels);
        audio.waveInFormat.nSamplesPerSec = static_cast<DWORD>(sampleRate);
        audio.waveInFormat.wBitsPerSample = 16;
        audio.waveInFormat.nBlockAlign = static_cast<WORD>((audio.waveInFormat.nChannels * audio.waveInFormat.wBitsPerSample) / 8);
        audio.waveInFormat.nAvgBytesPerSec = audio.waveInFormat.nSamplesPerSec * audio.waveInFormat.nBlockAlign;
        audio.waveInFormat.cbSize = 0;
    };

    MMRESULT openResult = MMSYSERR_ERROR;
    const UINT preferredDevice = audio.selectedInputDeviceId;
    const UINT fallbackDevice = WAVE_MAPPER;
    const UINT devicesToTry[2] = {preferredDevice, fallbackDevice};
    for (UINT dev : devicesToTry) {
        if (dev == fallbackDevice && preferredDevice == fallbackDevice) {
            continue;
        }
        for (int srTry : sampleRates) {
            for (int chTry = 1; chTry <= 2; ++chTry) {
                fillFormat(chTry, srTry);
                openResult = waveInOpen(&audio.waveIn, dev, &audio.waveInFormat, 0, 0, CALLBACK_NULL);
                if (openResult == MMSYSERR_NOERROR && audio.waveIn != nullptr) {
                    chosenSampleRate = srTry;
                    break;
                }
                audio.waveIn = nullptr;
            }
            if (openResult == MMSYSERR_NOERROR && audio.waveIn != nullptr) {
                break;
            }
        }
        if (openResult == MMSYSERR_NOERROR && audio.waveIn != nullptr) {
            break;
        }
    }
    if (openResult != MMSYSERR_NOERROR || audio.waveIn == nullptr) {
        audio.waveIn = nullptr;
        MessageBoxW(hwnd, L"Could not open selected input device for recording (tried stereo and mono).", L"Record", MB_OK | MB_ICONERROR);
        return false;
    }

    if (chosenSampleRate > 0) {
        audio.lastOpenedInputSampleRate = chosenSampleRate;
    }

    audio.waveInData.assign(kRecordBufferCount, std::vector<std::int16_t>(kRecordBufferFrames * audio.waveInFormat.nChannels, 0));
    audio.waveInHeaders.assign(kRecordBufferCount, WAVEHDR{});
    for (int i = 0; i < kRecordBufferCount; ++i) {
        WAVEHDR& hdr = audio.waveInHeaders[static_cast<size_t>(i)];
        hdr.lpData = reinterpret_cast<LPSTR>(audio.waveInData[static_cast<size_t>(i)].data());
        hdr.dwBufferLength = static_cast<DWORD>(audio.waveInData[static_cast<size_t>(i)].size() * sizeof(std::int16_t));
        hdr.dwFlags = 0;
        hdr.dwBytesRecorded = 0;
        if (waveInPrepareHeader(audio.waveIn, &hdr, sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
            DeviceStopMmeRecording(audio);
            MessageBoxW(hwnd, L"Could not prepare recording buffers.", L"Record", MB_OK | MB_ICONERROR);
            return false;
        }
        waveInAddBuffer(audio.waveIn, &hdr, sizeof(WAVEHDR));
    }

    audio.recordedInputPcm.clear();
    audio.monitorInputPcm.clear();
    audio.monitorInputReadPos = 0;
    audio.recordInputChannels = audio.waveInFormat.nChannels;
    audio.recordTrackIndex = armedTrack;
    audio.recordCaptureStartTickMs = GetTickCount64();

    // Project sample rate is owned by ProjectData (default 48000). Device code
    // does not write to it; SRC will reconcile any device/project mismatch.

    const bool playbackWasRunning = audio.playing;
    const std::uint64_t timelineStartFrame = playbackWasRunning
        ? DeviceGetRenderedPlaybackFrame(core, audio)
        : TimelineFramesFromBeats(core, std::max(0.0f, playheadBeat));

    audio.recordPrerollFrames = 0;
    if (!wasPlaying && audio.countInEnabled) {
        audio.recordPrerollFrames = TimelineFramesFromBeats(core, 4.0f * static_cast<float>(audio.countInBars));
    }
    // Count-in is absolute: record starts at the original playhead, clicks play before it
    audio.recordStartFrame = timelineStartFrame;
    audio.countInEndFrame = timelineStartFrame + audio.recordPrerollFrames;
    audio.countingIn = (audio.recordPrerollFrames > 0);
    audio.countInFrameCursor.store(0);
    // Mark recording active BEFORE starting the playback backend so that the
    // very first engine callback (which can fire as soon as the playback
    // thread starts) sees recording=true and keeps the render loop alive.
    // Setting it later races with the engine and can cause an immediate
    // reachedEnd=true / kMsgPlaybackFinished tear-down.
    audio.recording = true;

    // Always start the playback backend during recording. The render thread
    // owns playbackFrameCursor advance (and therefore the visible playhead);
    // without it, recording with no clips, no metronome, and no count-in
    // would capture audio fine but the playhead would never move.
    if (!audio.playing) {
        if (!DeviceStartPlaybackBackend(hwnd, core, audio, playheadBeat)) {
            waveInClose(audio.waveIn);
            audio.waveIn = nullptr;
            audio.countingIn = false;
            audio.recording = false;
            return false;
        }
    }

    audio.recordStopRequested.store(false);
    audio.recordThread = CreateThread(nullptr, 0, RecordThreadProc, &audio, 0, nullptr);
    if (audio.recordThread == nullptr) {
        audio.countingIn = false;
        audio.recording = false;
        DeviceStopMmeRecording(audio);
        MessageBoxW(hwnd, L"Could not start recording thread.", L"Record", MB_OK | MB_ICONERROR);
        return false;
    }

    waveInStart(audio.waveIn);
    // Only recalculate timing if playback was already running (recording during playback)
    if (audio.playing) {
        const std::uint64_t captureNow = DeviceGetRenderedPlaybackFrame(core, audio);
        // If we're behind the scheduled start, add preroll
        if (captureNow < audio.recordStartFrame && audio.countInEnabled && audio.recordPrerollFrames == 0) {
            audio.recordPrerollFrames = audio.recordStartFrame - captureNow;
            audio.countInEndFrame = audio.recordStartFrame;
            audio.countingIn = true;
        }
    }

    audio.lastOpenedInputSampleRate = static_cast<int>(audio.waveInFormat.nSamplesPerSec);
    audio.lastOpenedInputChannels = static_cast<int>(audio.waveInFormat.nChannels);
    audio.recordUsingWasapi = false;
    audio.recordInitState.store(1);
    audio.recording = true;
    
    // Initialize live recording clip for real-time waveform display.
    // Note: startBeat will be set correctly in the UI timer callback.
    audio.liveRecordingClip.trackIndex = armedTrack;
    audio.liveRecordingClip.audioIndex = -1;
    audio.liveRecordingClip.startBeat = 0.0f;
    audio.liveRecordingClip.lengthBeats = 0.0f;
    audio.liveRecordingClip.color = CoreRgb(88, 131, 199);
    audio.liveRecordingClip.name = L"[Recording]";
    audio.liveRecordingClip.sourceOffsetFrames = 0;
    audio.liveRecordingWaveform.clear();
    audio.liveRecordingFramesProcessed = 0;
    
    return true;
}
