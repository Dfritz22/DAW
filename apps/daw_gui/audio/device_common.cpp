#include "device_common.h"
#include "core/state.h"
#include "core/automation.h"
#include "core/timeline.h"

// ── Backend labels ────────────────────────────────────────────────────────────

const wchar_t* DeviceAudioBackendLabel(AudioBackend backend) {
    switch (backend) {
    case AudioBackend::MME:             return L"MME";
    case AudioBackend::WasapiShared:    return L"WASAPI Shared";
    case AudioBackend::WasapiExclusive: return L"WASAPI Exclusive";
    case AudioBackend::Asio:            return L"ASIO (future)";
    default:                            return L"Unknown";
    }
}

std::string DeviceAudioBackendToJson(AudioBackend backend) {
    switch (backend) {
    case AudioBackend::MME:             return "mme";
    case AudioBackend::WasapiShared:    return "wasapi_shared";
    case AudioBackend::WasapiExclusive: return "wasapi_exclusive";
    case AudioBackend::Asio:            return "asio";
    default:                            return "wasapi_shared";
    }
}

AudioBackend DeviceAudioBackendFromJson(const std::string& value) {
    if (value == "mme")              return AudioBackend::MME;
    if (value == "wasapi_exclusive") return AudioBackend::WasapiExclusive;
    if (value == "asio")             return AudioBackend::Asio;
    return AudioBackend::WasapiShared; // default / "wasapi_shared"
}

// ── Device enumeration ────────────────────────────────────────────────────────

void DeviceRefreshInputDevices(UiState& state) {
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

void DeviceRefreshOutputDevices(UiState& state) {
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

// ── Diagnostics ───────────────────────────────────────────────────────────────

std::wstring DeviceBuildAudioDiagnosticsReport(const UiState& state) {
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
    ss << L"Audio Backend: " << DeviceAudioBackendLabel(state.audioBackend)
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

// ── Playback cursor ───────────────────────────────────────────────────────────

std::uint64_t DeviceGetRenderedPlaybackFrame(const UiState& state) {
    auto clockFrame = [&state]() -> std::uint64_t {
        if (state.project.projectSampleRate <= 0 || state.playbackStartTick == 0) {
            return 0;
        }
        const ULONGLONG elapsedMs = GetTickCount64() - state.playbackStartTick;
        return static_cast<std::uint64_t>((elapsedMs * static_cast<ULONGLONG>(state.project.projectSampleRate)) / 1000ULL);
    };

    const std::uint64_t startFrame = TimelineFramesFromBeats(state, std::max(0.0f, state.playbackStartBeat));

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
