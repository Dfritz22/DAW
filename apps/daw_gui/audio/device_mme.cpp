#include "device_mme.h"
#include "engine.h"

// ── Forward declarations for orchestration functions in main.cpp ─────────────
// These are defined in main.cpp and called by StartMmeAudio / StartMmeRecording
// on error paths or when playback must be started alongside recording.
bool StartPlayback(HWND hwnd, UiState& state);
void StopPlayback(UiState& state, bool rewind);
void StopRecording(UiState& state, bool commitTake);

// ── File-private layout helper ────────────────────────────────────────────────
// Exact duplicate of layout.cpp::SamplesPerBeat; internal linkage avoids ODR.
namespace {

float SamplesPerBeat(const UiState& state) {
    int sr = state.project.projectSampleRate;
    if (sr <= 0) sr = state.activeDeviceSampleRate;
    if (sr <= 0) sr = state.preferredSampleRate;
    if (sr <= 0) sr = 1;
    return static_cast<float>(sr) * 60.0f / state.project.bpm;
}

static const wchar_t* AudioBackendLabel(AudioBackend backend) {
    switch (backend) {
    case AudioBackend::MME:            return L"MME";
    case AudioBackend::WasapiShared:   return L"WASAPI Shared";
    case AudioBackend::WasapiExclusive:return L"WASAPI Exclusive";
    case AudioBackend::Asio:           return L"ASIO (future)";
    default:                           return L"Unknown";
    }
}

} // namespace

// ── MME playback thread ───────────────────────────────────────────────────────

static DWORD WINAPI AudioThreadProc(LPVOID param) {
    auto* state = reinterpret_cast<UiState*>(param);
    if (state == nullptr) {
        return 0;
    }

    bool draining = false;
    while (!state->audioStopRequested.load()) {
        bool anyInQueue = false;

        for (int i = 0; i < static_cast<int>(state->waveHeaders.size()); ++i) {
            WAVEHDR& hdr = state->waveHeaders[static_cast<size_t>(i)];
            if ((hdr.dwFlags & WHDR_INQUEUE) != 0) {
                anyInQueue = true;
                continue;
            }

            if (draining) {
                continue;
            }

            bool reachedEnd = false;
            EnterCriticalSection(&state->audioStateLock);
            FillRealtimeForDeviceLocked(
                *state,
                state->waveData[static_cast<size_t>(i)].data(),
                kAudioBufferFrames,
                static_cast<int>(state->waveFormat.nSamplesPerSec),
                &reachedEnd);
            LeaveCriticalSection(&state->audioStateLock);

            hdr.lpData = reinterpret_cast<LPSTR>(state->waveData[static_cast<size_t>(i)].data());
            hdr.dwBufferLength = static_cast<DWORD>(state->waveData[static_cast<size_t>(i)].size() * sizeof(std::int16_t));
            hdr.dwFlags &= ~WHDR_DONE;

            if (waveOutWrite(state->waveOut, &hdr, sizeof(WAVEHDR)) == MMSYSERR_NOERROR) {
                anyInQueue = true;
            }

            if (reachedEnd) {
                draining = true;
            }
        }

        if (draining && !anyInQueue) {
            PostMessage(state->hwnd, kMsgPlaybackFinished, 0, 0);
            break;
        }

        Sleep(4);
    }

    state->audioThreadRunning.store(false);
    return 0;
}

// ── MME record thread ─────────────────────────────────────────────────────────

static DWORD WINAPI RecordThreadProc(LPVOID param) {
    auto* state = reinterpret_cast<UiState*>(param);
    if (state == nullptr) {
        return 0;
    }

    while (!state->recordStopRequested.load()) {
        for (size_t i = 0; i < state->waveInHeaders.size(); ++i) {
            WAVEHDR& hdr = state->waveInHeaders[i];
            if ((hdr.dwFlags & WHDR_DONE) == 0) {
                continue;
            }

            if (hdr.dwBytesRecorded > 0) {
                const size_t sampleCount = static_cast<size_t>(hdr.dwBytesRecorded / sizeof(std::int16_t));
                const std::int16_t* samples = reinterpret_cast<const std::int16_t*>(hdr.lpData);
                state->recordedInputPcm.insert(state->recordedInputPcm.end(), samples, samples + sampleCount);
                // Requeue first so capture does not stall while doing monitor bookkeeping.
                hdr.dwBytesRecorded = 0;
                hdr.dwFlags &= ~WHDR_DONE;
                if (!state->recordStopRequested.load() && state->waveIn != nullptr) {
                    waveInAddBuffer(state->waveIn, &hdr, sizeof(WAVEHDR));
                }

                if (state->inputMonitoring) {
                    EnterCriticalSection(&state->audioStateLock);
                    state->monitorInputPcm.insert(state->monitorInputPcm.end(), samples, samples + sampleCount);
                    LeaveCriticalSection(&state->audioStateLock);
                }
            } else if (!state->recordStopRequested.load() && state->waveIn != nullptr) {
                hdr.dwBytesRecorded = 0;
                hdr.dwFlags &= ~WHDR_DONE;
                waveInAddBuffer(state->waveIn, &hdr, sizeof(WAVEHDR));
            }
        }
        Sleep(4);
    }

    return 0;
}

// ── Public API ────────────────────────────────────────────────────────────────

std::wstring BuildAudioDiagnosticsReport(const UiState& state) {
    auto queryInputFmt = [](UINT dev, int sr, int ch) {
        WAVEFORMATEX fmt{};
        fmt.wFormatTag = WAVE_FORMAT_PCM;
        fmt.nChannels = static_cast<WORD>(ch);
        fmt.nSamplesPerSec = static_cast<DWORD>(sr);
        fmt.wBitsPerSample = 16;
        fmt.nBlockAlign = static_cast<WORD>((fmt.nChannels * fmt.wBitsPerSample) / 8);
        fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;
        fmt.cbSize = 0;
        const MMRESULT r = waveInOpen(nullptr, dev, &fmt, 0, 0, WAVE_FORMAT_QUERY);
        return r == MMSYSERR_NOERROR;
    };

    auto queryOutputFmt = [](UINT dev, int sr, int ch) {
        WAVEFORMATEX fmt{};
        fmt.wFormatTag = WAVE_FORMAT_PCM;
        fmt.nChannels = static_cast<WORD>(ch);
        fmt.nSamplesPerSec = static_cast<DWORD>(sr);
        fmt.wBitsPerSample = 16;
        fmt.nBlockAlign = static_cast<WORD>((fmt.nChannels * fmt.wBitsPerSample) / 8);
        fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;
        fmt.cbSize = 0;
        const MMRESULT r = waveOutOpen(nullptr, dev, &fmt, 0, 0, WAVE_FORMAT_QUERY);
        return r == MMSYSERR_NOERROR;
    };

    auto yn = [](bool ok) -> const wchar_t* { return ok ? L"OK" : L"NO"; };

    std::wstringstream ss;
    ss << L"Project SR: " << state.project.projectSampleRate << L"\n";
     ss << L"Audio Backend: " << AudioBackendLabel(state.audioBackend)
       << (state.playingViaWasapi ? L" (output: WASAPI)" : L" (output: MME)") << L"\n";
     ss << L"Preferred SR: " << state.preferredSampleRate << L"\n";
     ss << L"Preferred Buffer: " << state.preferredBufferFrames << L" frames\n";
     ss << L"Active Device SR: " << state.activeDeviceSampleRate << L"\n";
     ss << L"Active Device Buffer: " << state.activeDeviceBufferFrames << L" frames\n";
    ss << L"Selected Input: " << state.selectedInputDeviceName << L" (id=" << state.selectedInputDeviceId << L")\n";
    ss << L"Selected Output: " << state.selectedOutputDeviceName << L" (id=" << state.selectedOutputDeviceId << L")\n\n";

    ss << L"Last Opened Input Format: "
       << state.lastOpenedInputSampleRate << L" Hz, " << state.lastOpenedInputChannels << L" ch\n";
    ss << L"Last Opened Output Format: "
       << state.lastOpenedOutputSampleRate << L" Hz, " << state.lastOpenedOutputChannels << L" ch\n";
    if (!state.lastPlaybackInitError.empty()) {
        ss << L"Last Playback Init Error: " << state.lastPlaybackInitError << L"\n";
    }
    ss << L"Last Committed Take: "
       << state.lastCommittedTakeSampleRate << L" Hz, "
       << state.lastCommittedTakeChannels << L" ch, "
       << state.lastCommittedTakeFrames << L" frames\n\n";
    ss << L"Capture Elapsed: " << state.lastCaptureElapsedMs << L" ms\n";
    ss << L"Observed Frame Ratio: " << state.lastCaptureObservedRateRatio << L"\n";
    ss << L"Applied Capture Stride: " << state.lastCaptureFrameStride << L"\n\n";

    const int probeA = (state.preferredSampleRate > 0) ? state.preferredSampleRate : state.project.projectSampleRate;
    const int probeB = (state.project.projectSampleRate > 0 && state.project.projectSampleRate != probeA) ? state.project.projectSampleRate : state.lastOpenedOutputSampleRate;

    ss << L"Input Query (16-bit PCM)\n";
    if (probeA > 0) {
        ss << L"  " << probeA << L" mono:   " << yn(queryInputFmt(state.selectedInputDeviceId, probeA, 1)) << L"\n";
        ss << L"  " << probeA << L" stereo: " << yn(queryInputFmt(state.selectedInputDeviceId, probeA, 2)) << L"\n";
    }
    if (probeB > 0 && probeB != probeA) {
        ss << L"  " << probeB << L" mono:   " << yn(queryInputFmt(state.selectedInputDeviceId, probeB, 1)) << L"\n";
        ss << L"  " << probeB << L" stereo: " << yn(queryInputFmt(state.selectedInputDeviceId, probeB, 2)) << L"\n";
    }
    ss << L"\nOutput Query (16-bit PCM)\n";
    if (probeA > 0) {
        ss << L"  " << probeA << L" stereo: " << yn(queryOutputFmt(state.selectedOutputDeviceId, probeA, 2)) << L"\n";
    }
    if (probeB > 0 && probeB != probeA) {
        ss << L"  " << probeB << L" stereo: " << yn(queryOutputFmt(state.selectedOutputDeviceId, probeB, 2)) << L"\n";
    }
    ss << L"\nTip: For MME full-duplex stability, input and output should both report OK at the same sample rate.\n";

    return ss.str();
}

void RefreshInputDevices(UiState& state) {
    state.inputDeviceIds.clear();
    state.inputDeviceNames.clear();

    const std::wstring previousName = state.selectedInputDeviceName;

    const UINT count = waveInGetNumDevs();
    for (UINT i = 0; i < count; ++i) {
        WAVEINCAPSW caps{};
        if (waveInGetDevCapsW(i, &caps, sizeof(caps)) != MMSYSERR_NOERROR) {
            continue;
        }
        state.inputDeviceIds.push_back(i);
        state.inputDeviceNames.push_back(caps.szPname);
    }

    if (state.inputDeviceIds.empty()) {
        state.selectedInputDeviceId = WAVE_MAPPER;
        state.selectedInputDeviceName = L"No Input Devices";
        return;
    }

    bool selectedStillExists = false;
    for (size_t i = 0; i < state.inputDeviceIds.size(); ++i) {
        if (state.inputDeviceIds[i] == state.selectedInputDeviceId) {
            state.selectedInputDeviceName = state.inputDeviceNames[i];
            selectedStillExists = true;
            break;
        }
    }

    if (!selectedStillExists) {
        for (size_t i = 0; i < state.inputDeviceNames.size(); ++i) {
            if (_wcsicmp(state.inputDeviceNames[i].c_str(), previousName.c_str()) == 0) {
                state.selectedInputDeviceId = state.inputDeviceIds[i];
                state.selectedInputDeviceName = state.inputDeviceNames[i];
                selectedStillExists = true;
                break;
            }
        }
    }

    if (!selectedStillExists) {
        state.selectedInputDeviceId = state.inputDeviceIds[0];
        state.selectedInputDeviceName = state.inputDeviceNames[0];
    }
}

void RefreshOutputDevices(UiState& state) {
    state.outputDeviceIds.clear();
    state.outputDeviceNames.clear();

    const std::wstring previousName = state.selectedOutputDeviceName;

    const UINT count = waveOutGetNumDevs();
    for (UINT i = 0; i < count; ++i) {
        WAVEOUTCAPSW caps{};
        if (waveOutGetDevCapsW(i, &caps, sizeof(caps)) != MMSYSERR_NOERROR) {
            continue;
        }
        state.outputDeviceIds.push_back(i);
        state.outputDeviceNames.push_back(caps.szPname);
    }

    if (state.outputDeviceIds.empty()) {
        state.selectedOutputDeviceId = WAVE_MAPPER;
        state.selectedOutputDeviceName = L"No Output Devices";
        return;
    }

    bool selectedStillExists = false;
    for (size_t i = 0; i < state.outputDeviceIds.size(); ++i) {
        if (state.outputDeviceIds[i] == state.selectedOutputDeviceId) {
            state.selectedOutputDeviceName = state.outputDeviceNames[i];
            selectedStillExists = true;
            break;
        }
    }

    if (!selectedStillExists) {
        for (size_t i = 0; i < state.outputDeviceNames.size(); ++i) {
            if (_wcsicmp(state.outputDeviceNames[i].c_str(), previousName.c_str()) == 0) {
                state.selectedOutputDeviceId = state.outputDeviceIds[i];
                state.selectedOutputDeviceName = state.outputDeviceNames[i];
                selectedStillExists = true;
                break;
            }
        }
    }

    if (!selectedStillExists) {
        state.selectedOutputDeviceId = state.outputDeviceIds[0];
        state.selectedOutputDeviceName = state.outputDeviceNames[0];
    }
}

std::uint64_t GetRenderedPlaybackFrame(const UiState& state) {
    auto clockFrame = [&state]() -> std::uint64_t {
        if (state.project.projectSampleRate <= 0 || state.playbackStartTick == 0) {
            return 0;
        }
        const ULONGLONG elapsedMs = GetTickCount64() - state.playbackStartTick;
        return static_cast<std::uint64_t>((elapsedMs * static_cast<ULONGLONG>(state.project.projectSampleRate)) / 1000ULL);
    };

    const float samplesPerBeat = std::max(1.0f, SamplesPerBeat(state));
    const std::uint64_t startFrame = static_cast<std::uint64_t>(
        std::llround(static_cast<double>(std::max(0.0f, state.playbackStartBeat)) * static_cast<double>(samplesPerBeat)));

    // Return absolute project frames. Audio generation may queue ahead, so clamp to elapsed transport time.
    if (state.playingViaWasapi || state.waveOut == nullptr || state.project.projectSampleRate <= 0) {
        const std::uint64_t cursor = state.playbackFrameCursor.load();
        const std::uint64_t clock  = clockFrame();
        return (clock > 0) ? std::min(cursor, startFrame + clock) : cursor;
    }

    const std::uint64_t clock = clockFrame();

    MMTIME mm{};
    mm.wType = TIME_BYTES;
    if (waveOutGetPosition(state.waveOut, &mm, sizeof(mm)) == MMSYSERR_NOERROR) {
        std::uint64_t deviceFrame = 0;
        if (mm.wType == TIME_BYTES && state.waveFormat.nBlockAlign > 0) {
            deviceFrame = static_cast<std::uint64_t>(mm.u.cb / state.waveFormat.nBlockAlign);
        } else if (mm.wType == TIME_SAMPLES) {
            const std::uint64_t rawSamples = static_cast<std::uint64_t>(mm.u.sample);
            const std::uint64_t channels = std::max<std::uint64_t>(1, state.waveFormat.nChannels);
            deviceFrame = rawSamples / channels;
        } else if (mm.wType == TIME_MS) {
            deviceFrame = static_cast<std::uint64_t>((static_cast<ULONGLONG>(mm.u.ms) * static_cast<ULONGLONG>(state.project.projectSampleRate)) / 1000ULL);
        }

        if (deviceFrame > 0) {
            // Guard against bad driver/unit reporting that can run the playhead far ahead.
            const std::uint64_t maxReasonableAhead = static_cast<std::uint64_t>(state.project.projectSampleRate);
            if (clock > 0 && deviceFrame > clock + maxReasonableAhead) {
                return startFrame + clock;
            }
            return startFrame + deviceFrame;
        }
    }

    if (clock > 0) {
        return startFrame + clock;
    }
    return state.playbackFrameCursor.load();
}

void StopMmeAudio(UiState& state) {
    if (state.waveOut != nullptr) {
        waveOutReset(state.waveOut);
        for (size_t i = 0; i < state.waveHeaders.size(); ++i) {
            waveOutUnprepareHeader(state.waveOut, &state.waveHeaders[i], sizeof(WAVEHDR));
        }
        waveOutClose(state.waveOut);
        state.waveOut = nullptr;
    }
    state.waveHeaders.clear();
    state.waveData.clear();
}

bool StartMmeAudio(HWND hwnd, UiState& state) {
    int mmeSampleRate = 0;
    if (state.preferredSampleRate > 0) {
        mmeSampleRate = state.preferredSampleRate;
    } else if (state.project.projectSampleRate > 0) {
        mmeSampleRate = state.project.projectSampleRate;
    } else if (state.lastOpenedOutputSampleRate > 0) {
        mmeSampleRate = state.lastOpenedOutputSampleRate;
    }
    if (mmeSampleRate <= 0) {
        MessageBoxW(hwnd,
            L"No playback sample rate is configured.\nOpen Audio menu and choose a sample rate first.",
            L"Playback error",
            MB_OK | MB_ICONERROR);
        return false;
    }
    state.waveFormat.nChannels = 2;
    state.waveFormat.nSamplesPerSec = static_cast<DWORD>(mmeSampleRate);
    state.waveFormat.wBitsPerSample = 16;
    state.waveFormat.nBlockAlign = static_cast<WORD>((state.waveFormat.nChannels * state.waveFormat.wBitsPerSample) / 8);
    state.waveFormat.nAvgBytesPerSec = state.waveFormat.nSamplesPerSec * state.waveFormat.nBlockAlign;
    state.waveFormat.cbSize = 0;

    MMRESULT outOpen = waveOutOpen(&state.waveOut, state.selectedOutputDeviceId, &state.waveFormat, 0, 0, CALLBACK_NULL);
    if (outOpen != MMSYSERR_NOERROR) {
        outOpen = waveOutOpen(&state.waveOut, WAVE_MAPPER, &state.waveFormat, 0, 0, CALLBACK_NULL);
    }
    if (outOpen != MMSYSERR_NOERROR) {
        wchar_t mmeErr[256]{};
        if (waveOutGetErrorTextW(outOpen, mmeErr, static_cast<UINT>(std::size(mmeErr))) != MMSYSERR_NOERROR) {
            wcscpy_s(mmeErr, L"Unknown MME error");
        }
        if (state.lastPlaybackInitError.empty()) {
            state.lastPlaybackInitError = std::wstring(L"MME open failed: ") + mmeErr;
        } else {
            state.lastPlaybackInitError += std::wstring(L"; MME open failed: ") + mmeErr;
        }
        state.waveOut = nullptr;
        const std::wstring msg = L"Unable to open selected output device.\n\n" + state.lastPlaybackInitError;
        MessageBoxW(hwnd, msg.c_str(), L"Playback error", MB_OK | MB_ICONERROR);
        return false;
    }
    state.lastOpenedOutputSampleRate = static_cast<int>(state.waveFormat.nSamplesPerSec);
    state.lastOpenedOutputChannels = static_cast<int>(state.waveFormat.nChannels);
    state.activeDeviceSampleRate = state.lastOpenedOutputSampleRate;
    state.activeDeviceBufferFrames = kAudioBufferFrames;

    state.waveData.assign(kAudioBufferCount, std::vector<std::int16_t>(kAudioBufferFrames * 2, 0));
    state.waveHeaders.assign(kAudioBufferCount, WAVEHDR{});

    for (int i = 0; i < kAudioBufferCount; ++i) {
        WAVEHDR& hdr = state.waveHeaders[static_cast<size_t>(i)];
        hdr.lpData = reinterpret_cast<LPSTR>(state.waveData[static_cast<size_t>(i)].data());
        hdr.dwBufferLength = static_cast<DWORD>(state.waveData[static_cast<size_t>(i)].size() * sizeof(std::int16_t));
        hdr.dwFlags = 0;
        if (waveOutPrepareHeader(state.waveOut, &hdr, sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
            StopPlayback(state, false);
            MessageBoxW(hwnd, L"Unable to prepare audio buffers.", L"Playback error", MB_OK | MB_ICONERROR);
            return false;
        }
    }

    EnterCriticalSection(&state.audioStateLock);
    const float startBeat = std::max(0.0f, state.playheadBeat);
    const std::uint64_t startFrame = static_cast<std::uint64_t>(
        std::llround(static_cast<double>(startBeat) * static_cast<double>(SamplesPerBeat(state))));
    state.playbackFrameCursor.store(startFrame);
    const std::uint64_t endFrame = ComputeProjectEndFrameLocked(state);
    LeaveCriticalSection(&state.audioStateLock);

    state.playbackStartTick = GetTickCount64();
    state.playbackStartBeat = startBeat;
    state.playbackEndBeat = static_cast<float>(endFrame) / SamplesPerBeat(state);
    state.playing = true;
    state.audioStopRequested.store(false);
    state.audioThreadRunning.store(true);
    state.audioThread = CreateThread(nullptr, 0, AudioThreadProc, &state, 0, nullptr);

    if (state.audioThread == nullptr) {
        StopPlayback(state, false);
        MessageBoxW(hwnd, L"Unable to start audio thread.", L"Playback error", MB_OK | MB_ICONERROR);
        return false;
    }

    return true;
}

void StopMmeRecording(UiState& state) {
    if (state.waveIn != nullptr) {
        waveInStop(state.waveIn);
        waveInReset(state.waveIn);
        for (size_t i = 0; i < state.waveInHeaders.size(); ++i) {
            waveInUnprepareHeader(state.waveIn, &state.waveInHeaders[i], sizeof(WAVEHDR));
        }
        waveInClose(state.waveIn);
        state.waveIn = nullptr;
    }
}

bool StartMmeRecording(HWND hwnd, UiState& state, int armedTrack, bool wasPlaying) {
    std::vector<int> sampleRates;
    if (state.preferredSampleRate > 0) {
        sampleRates.push_back(state.preferredSampleRate);
    }
    if (state.project.projectSampleRate > 0) {
        sampleRates.push_back(state.project.projectSampleRate);
    }
    if (state.lastOpenedInputSampleRate > 0 &&
        std::find(sampleRates.begin(), sampleRates.end(), state.lastOpenedInputSampleRate) == sampleRates.end()) {
        sampleRates.push_back(state.lastOpenedInputSampleRate);
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
        state.waveInFormat.wFormatTag = WAVE_FORMAT_PCM;
        state.waveInFormat.nChannels = static_cast<WORD>(channels);
        state.waveInFormat.nSamplesPerSec = static_cast<DWORD>(sampleRate);
        state.waveInFormat.wBitsPerSample = 16;
        state.waveInFormat.nBlockAlign = static_cast<WORD>((state.waveInFormat.nChannels * state.waveInFormat.wBitsPerSample) / 8);
        state.waveInFormat.nAvgBytesPerSec = state.waveInFormat.nSamplesPerSec * state.waveInFormat.nBlockAlign;
        state.waveInFormat.cbSize = 0;
    };

    MMRESULT openResult = MMSYSERR_ERROR;
    const UINT preferredDevice = state.selectedInputDeviceId;
    const UINT fallbackDevice = WAVE_MAPPER;
    const UINT devicesToTry[2] = {preferredDevice, fallbackDevice};
    for (UINT dev : devicesToTry) {
        if (dev == fallbackDevice && preferredDevice == fallbackDevice) {
            continue;
        }
        for (int srTry : sampleRates) {
            // Prefer mono for guitar tracking stability on MME, then stereo fallback.
            for (int chTry = 1; chTry <= 2; ++chTry) {
                fillFormat(chTry, srTry);
                openResult = waveInOpen(&state.waveIn, dev, &state.waveInFormat, 0, 0, CALLBACK_NULL);
                if (openResult == MMSYSERR_NOERROR && state.waveIn != nullptr) {
                    chosenSampleRate = srTry;
                    break;
                }
                state.waveIn = nullptr;
            }
            if (openResult == MMSYSERR_NOERROR && state.waveIn != nullptr) {
                break;
            }
        }
        if (openResult == MMSYSERR_NOERROR && state.waveIn != nullptr) {
            break;
        }
    }
    if (openResult != MMSYSERR_NOERROR || state.waveIn == nullptr) {
        state.waveIn = nullptr;
        MessageBoxW(hwnd, L"Could not open selected input device for recording (tried stereo and mono).", L"Record", MB_OK | MB_ICONERROR);
        return false;
    }

    if (chosenSampleRate > 0) {
        state.lastOpenedInputSampleRate = chosenSampleRate;
        if (state.project.projectSampleRate <= 0) {
            state.project.projectSampleRate = chosenSampleRate;
        }
    }

    if (!state.playing && (!state.project.clips.empty() || state.metronomeRecord)) {
        if (!StartPlayback(hwnd, state)) {
            waveInClose(state.waveIn);
            state.waveIn = nullptr;
            return false;
        }
    }

    state.waveInData.assign(kRecordBufferCount, std::vector<std::int16_t>(kRecordBufferFrames * state.waveInFormat.nChannels, 0));
    state.waveInHeaders.assign(kRecordBufferCount, WAVEHDR{});
    for (int i = 0; i < kRecordBufferCount; ++i) {
        WAVEHDR& hdr = state.waveInHeaders[static_cast<size_t>(i)];
        hdr.lpData = reinterpret_cast<LPSTR>(state.waveInData[static_cast<size_t>(i)].data());
        hdr.dwBufferLength = static_cast<DWORD>(state.waveInData[static_cast<size_t>(i)].size() * sizeof(std::int16_t));
        hdr.dwFlags = 0;
        hdr.dwBytesRecorded = 0;
        if (waveInPrepareHeader(state.waveIn, &hdr, sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
            StopRecording(state, false);
            MessageBoxW(hwnd, L"Could not prepare recording buffers.", L"Record", MB_OK | MB_ICONERROR);
            return false;
        }
        waveInAddBuffer(state.waveIn, &hdr, sizeof(WAVEHDR));
    }

    state.recordedInputPcm.clear();
    state.monitorInputPcm.clear();
    state.monitorInputReadPos = 0;
    state.recordInputChannels = state.waveInFormat.nChannels;
    state.recordTrackIndex = armedTrack;
    state.recordCaptureStartTickMs = GetTickCount64();
    const float samplesPerBeat = std::max(1.0f, SamplesPerBeat(state));
    const std::uint64_t timelineStartFrame = state.playing
        ? GetRenderedPlaybackFrame(state)
        : static_cast<std::uint64_t>(std::llround(static_cast<double>(std::max(0.0f, state.playheadBeat)) * static_cast<double>(samplesPerBeat)));

    // Set preroll + tentative placement before starting capture.
    state.recordPrerollFrames = 0;
    if (!wasPlaying && state.countInEnabled) {
        state.recordPrerollFrames = static_cast<std::uint64_t>(std::llround(4.0 * static_cast<double>(state.countInBars) * static_cast<double>(samplesPerBeat)));
    }
    state.recordStartFrame = timelineStartFrame + state.recordPrerollFrames;
    state.countingIn = (state.recordPrerollFrames > 0);

    state.recordStopRequested.store(false);
    state.recordThread = CreateThread(nullptr, 0, RecordThreadProc, &state, 0, nullptr);
    if (state.recordThread == nullptr) {
        state.countingIn = false;
        StopRecording(state, false);
        MessageBoxW(hwnd, L"Could not start recording thread.", L"Record", MB_OK | MB_ICONERROR);
        return false;
    }

    // MME capture starts synchronously, so latency is very small.
    // Sample playback position right as capture goes live for accurate placement.
    waveInStart(state.waveIn);
    {
        const std::uint64_t captureNow = state.playing ? GetRenderedPlaybackFrame(state) : 0;
        const std::uint64_t scheduledStart = state.recordStartFrame;
        const std::uint64_t actualSkip = (captureNow < scheduledStart) ? (scheduledStart - captureNow) : 0;
        state.recordPrerollFrames = actualSkip;
        state.recordStartFrame    = captureNow + actualSkip;
    }

    state.lastOpenedInputSampleRate = static_cast<int>(state.waveInFormat.nSamplesPerSec);
    state.lastOpenedInputChannels = static_cast<int>(state.waveInFormat.nChannels);
    state.recordUsingWasapi = false;
    state.recordInitState.store(1);
    state.recording = true;
    return true;
}
