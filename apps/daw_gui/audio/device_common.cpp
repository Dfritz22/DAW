#include "device_common.h"
#include "core/CoreState.h"
#include "audio/AudioRuntimeState.h"
#include "core/timeline.h"
#include "audio/device_mme.h"

#include <sstream>

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

void DeviceRefreshInputDevices(AudioRuntimeState& audio) {
    audio.inputDeviceIds.clear();
    audio.inputDeviceNames.clear();

    const std::wstring previousName = audio.selectedInputDeviceName;

    const UINT count = waveInGetNumDevs();
    for (UINT i = 0; i < count; ++i) {
        WAVEINCAPSW caps{};
        if (waveInGetDevCapsW(i, &caps, sizeof(caps)) != MMSYSERR_NOERROR) {
            continue;
        }
        audio.inputDeviceIds.push_back(i);
        audio.inputDeviceNames.push_back(caps.szPname);
    }

    if (audio.inputDeviceIds.empty()) {
        audio.selectedInputDeviceId = WAVE_MAPPER;
        audio.selectedInputDeviceName = L"No Input Devices";
        return;
    }

    bool selectedStillExists = false;
    for (size_t i = 0; i < audio.inputDeviceIds.size(); ++i) {
        if (audio.inputDeviceIds[i] == audio.selectedInputDeviceId) {
            audio.selectedInputDeviceName = audio.inputDeviceNames[i];
            selectedStillExists = true;
            break;
        }
    }

    if (!selectedStillExists) {
        for (size_t i = 0; i < audio.inputDeviceNames.size(); ++i) {
            if (_wcsicmp(audio.inputDeviceNames[i].c_str(), previousName.c_str()) == 0) {
                audio.selectedInputDeviceId = audio.inputDeviceIds[i];
                audio.selectedInputDeviceName = audio.inputDeviceNames[i];
                selectedStillExists = true;
                break;
            }
        }
    }

    if (!selectedStillExists) {
        audio.selectedInputDeviceId = audio.inputDeviceIds[0];
        audio.selectedInputDeviceName = audio.inputDeviceNames[0];
    }
}

void DeviceRefreshOutputDevices(AudioRuntimeState& audio) {
    audio.outputDeviceIds.clear();
    audio.outputDeviceNames.clear();

    const std::wstring previousName = audio.selectedOutputDeviceName;

    const UINT count = waveOutGetNumDevs();
    for (UINT i = 0; i < count; ++i) {
        WAVEOUTCAPSW caps{};
        if (waveOutGetDevCapsW(i, &caps, sizeof(caps)) != MMSYSERR_NOERROR) {
            continue;
        }
        audio.outputDeviceIds.push_back(i);
        audio.outputDeviceNames.push_back(caps.szPname);
    }

    if (audio.outputDeviceIds.empty()) {
        audio.selectedOutputDeviceId = WAVE_MAPPER;
        audio.selectedOutputDeviceName = L"No Output Devices";
        return;
    }

    bool selectedStillExists = false;
    for (size_t i = 0; i < audio.outputDeviceIds.size(); ++i) {
        if (audio.outputDeviceIds[i] == audio.selectedOutputDeviceId) {
            audio.selectedOutputDeviceName = audio.outputDeviceNames[i];
            selectedStillExists = true;
            break;
        }
    }

    if (!selectedStillExists) {
        for (size_t i = 0; i < audio.outputDeviceNames.size(); ++i) {
            if (_wcsicmp(audio.outputDeviceNames[i].c_str(), previousName.c_str()) == 0) {
                audio.selectedOutputDeviceId = audio.outputDeviceIds[i];
                audio.selectedOutputDeviceName = audio.outputDeviceNames[i];
                selectedStillExists = true;
                break;
            }
        }
    }

    if (!selectedStillExists) {
        audio.selectedOutputDeviceId = audio.outputDeviceIds[0];
        audio.selectedOutputDeviceName = audio.outputDeviceNames[0];
    }
}

// ── Diagnostics ───────────────────────────────────────────────────────────────

std::wstring DeviceBuildAudioDiagnosticsReport(const CoreState& core, const AudioRuntimeState& audio) {
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
    ss << L"Project SR: " << core.project.projectSampleRate << L"\n";
    ss << L"Audio Backend: " << DeviceAudioBackendLabel(audio.audioBackend)
       << (audio.playingViaWasapi ? L" (output: WASAPI)" : L" (output: MME)") << L"\n";
    ss << L"Preferred SR: " << audio.preferredSampleRate << L"\n";
    ss << L"Preferred Buffer: " << audio.preferredBufferFrames << L" frames\n";
    ss << L"Active Device SR: " << audio.activeDeviceSampleRate << L"\n";
    ss << L"Active Device Buffer: " << audio.activeDeviceBufferFrames << L" frames\n";
    ss << L"Selected Input: " << audio.selectedInputDeviceName << L" (id=" << audio.selectedInputDeviceId << L")\n";
    ss << L"Selected Output: " << audio.selectedOutputDeviceName << L" (id=" << audio.selectedOutputDeviceId << L")\n\n";

    ss << L"Last Opened Input Format: "
       << audio.lastOpenedInputSampleRate << L" Hz, " << audio.lastOpenedInputChannels << L" ch\n";
    ss << L"Last Opened Output Format: "
       << audio.lastOpenedOutputSampleRate << L" Hz, " << audio.lastOpenedOutputChannels << L" ch\n";
    if (!audio.lastPlaybackInitError.empty()) {
        ss << L"Last Playback Init Error: " << audio.lastPlaybackInitError << L"\n";
    }
    ss << L"Last Committed Take: "
       << audio.lastCommittedTakeSampleRate << L" Hz, "
       << audio.lastCommittedTakeChannels << L" ch, "
       << audio.lastCommittedTakeFrames << L" frames\n\n";
    ss << L"Capture Elapsed: " << audio.lastCaptureElapsedMs << L" ms\n";
    ss << L"Observed Frame Ratio: " << audio.lastCaptureObservedRateRatio << L"\n";
    ss << L"Applied Capture Stride: " << audio.lastCaptureFrameStride << L"\n\n";

    const int probeA = (audio.preferredSampleRate > 0) ? audio.preferredSampleRate : core.project.projectSampleRate;
    const int probeB = (core.project.projectSampleRate > 0 && core.project.projectSampleRate != probeA) ? core.project.projectSampleRate : audio.lastOpenedOutputSampleRate;

    ss << L"Input Query (16-bit PCM)\n";
    if (probeA > 0) {
        ss << L"  " << probeA << L" mono:   " << yn(queryInputFmt(audio.selectedInputDeviceId, probeA, 1)) << L"\n";
        ss << L"  " << probeA << L" stereo: " << yn(queryInputFmt(audio.selectedInputDeviceId, probeA, 2)) << L"\n";
    }
    if (probeB > 0 && probeB != probeA) {
        ss << L"  " << probeB << L" mono:   " << yn(queryInputFmt(audio.selectedInputDeviceId, probeB, 1)) << L"\n";
        ss << L"  " << probeB << L" stereo: " << yn(queryInputFmt(audio.selectedInputDeviceId, probeB, 2)) << L"\n";
    }
    ss << L"\nOutput Query (16-bit PCM)\n";
    if (probeA > 0) {
        ss << L"  " << probeA << L" stereo: " << yn(queryOutputFmt(audio.selectedOutputDeviceId, probeA, 2)) << L"\n";
    }
    if (probeB > 0 && probeB != probeA) {
        ss << L"  " << probeB << L" stereo: " << yn(queryOutputFmt(audio.selectedOutputDeviceId, probeB, 2)) << L"\n";
    }
    ss << L"\nTip: For MME full-duplex stability, input and output should both report OK at the same sample rate.\n";

    return ss.str();
}

// ── Playback cursor ───────────────────────────────────────────────────────────

std::uint64_t DeviceGetRenderedPlaybackFrame(const CoreState& core, const AudioRuntimeState& audio) {
    auto clockFrame = [&core, &audio]() -> std::uint64_t {
        if (core.project.projectSampleRate <= 0 || audio.playbackStartTick == 0) {
            return 0;
        }
        const ULONGLONG elapsedMs = GetTickCount64() - audio.playbackStartTick;
        return static_cast<std::uint64_t>((elapsedMs * static_cast<ULONGLONG>(core.project.projectSampleRate)) / 1000ULL);
    };

    const std::uint64_t startFrame = TimelineFramesFromBeats(core, std::max(0.0f, audio.playbackStartBeat));

    // Return absolute project frames. Audio generation may queue ahead, so clamp to elapsed transport time.
    if (audio.playingViaWasapi || audio.waveOut == nullptr || core.project.projectSampleRate <= 0) {
        const std::uint64_t cursor = audio.playbackFrameCursor.load();
        const std::uint64_t clock  = clockFrame();
        return (clock > 0) ? std::min(cursor, startFrame + clock) : cursor;
    }

    const std::uint64_t clock = clockFrame();

    MMTIME mm{};
    mm.wType = TIME_BYTES;
    if (waveOutGetPosition(audio.waveOut, &mm, sizeof(mm)) == MMSYSERR_NOERROR) {
        std::uint64_t deviceFrame = 0;
        if (mm.wType == TIME_BYTES && audio.waveFormat.nBlockAlign > 0) {
            deviceFrame = static_cast<std::uint64_t>(mm.u.cb / audio.waveFormat.nBlockAlign);
        } else if (mm.wType == TIME_SAMPLES) {
            const std::uint64_t rawSamples = static_cast<std::uint64_t>(mm.u.sample);
            const std::uint64_t channels = std::max<std::uint64_t>(1, audio.waveFormat.nChannels);
            deviceFrame = rawSamples / channels;
        } else if (mm.wType == TIME_MS) {
            deviceFrame = static_cast<std::uint64_t>((static_cast<ULONGLONG>(mm.u.ms) * static_cast<ULONGLONG>(core.project.projectSampleRate)) / 1000ULL);
        }

        if (deviceFrame > 0) {
            const std::uint64_t maxReasonableAhead = static_cast<std::uint64_t>(core.project.projectSampleRate);
            if (clock > 0 && deviceFrame > clock + maxReasonableAhead) {
                return startFrame + clock;
            }
            return startFrame + deviceFrame;
        }
    }

    if (clock > 0) {
        return startFrame + clock;
    }
    return audio.playbackFrameCursor.load();
}

bool DeviceStartPlaybackBackend(HWND hwnd, CoreState& core, AudioRuntimeState& audio, float playheadBeat) {
    // Split-state WASAPI entry points are still being migrated.
    // Keep backend start functional via MME path in the meantime.
    return DeviceStartMmeAudio(hwnd, core, audio, playheadBeat);
}

void DeviceStopPlaybackBackend(AudioRuntimeState& audio) {
    DeviceStopMmeAudio(audio);
}

bool DeviceStartRecordingBackend(HWND hwnd, CoreState& core, AudioRuntimeState& audio, int armedTrack, bool wasPlaying, float playheadBeat) {
    // Split-state WASAPI entry points are still being migrated.
    // Keep backend start functional via MME path in the meantime.
    return DeviceStartMmeRecording(hwnd, core, audio, armedTrack, wasPlaying, playheadBeat);
}

void DeviceStopRecordingBackend(AudioRuntimeState& audio) {
    DeviceStopMmeRecording(audio);
}

