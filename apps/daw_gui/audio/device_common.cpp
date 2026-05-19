#include "device_common.h"
#include "core/CoreState.h"
#include "audio/AudioRuntimeState.h"
#include "audio/mix_snapshot_builder.h"
#include "core/timeline.h"
#include "audio/device_mme.h"

#include <algorithm>
#include <cwctype>
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

     ss << L"Runtime Transport\n";
     ss << L"  playing=" << yn(audio.playing)
         << L", recording=" << yn(audio.recording)
         << L", playingViaWasapi=" << yn(audio.playingViaWasapi)
         << L", recordUsingWasapi=" << yn(audio.recordUsingWasapi) << L"\n";
     ss << L"  audioThreadRunning=" << yn(audio.audioThreadRunning.load())
         << L", recordThreadPresent=" << yn(audio.recordThread != nullptr)
         << L", recordInitState=" << audio.recordInitState.load() << L"\n";
     ss << L"  playbackStartBeat=" << audio.playbackStartBeat
         << L", playbackEndBeat=" << audio.playbackEndBeat
         << L", playbackStartTick=" << audio.playbackStartTick << L"\n";
     ss << L"  playbackFrameCursor=" << audio.playbackFrameCursor.load()
         << L", ui/project clips=" << core.project.clips.size()
         << L", tracks=" << core.project.tracks.size() << L"\n";

     ss << L"Count-In / Record Timeline\n";
     ss << L"  countInEnabled=" << yn(audio.countInEnabled)
         << L", countingIn=" << yn(audio.countingIn)
         << L", countInBars=" << audio.countInBars
         << L", recordPrerollFrames=" << audio.recordPrerollFrames << L"\n";
     ss << L"  recordStartFrame=" << audio.recordStartFrame
         << L", countInEndFrame=" << audio.countInEndFrame
         << L", recordTrackIndex=" << audio.recordTrackIndex << L"\n";
     if (core.project.projectSampleRate > 0) {
          const double startBeat = static_cast<double>(audio.recordStartFrame) / static_cast<double>(core.project.projectSampleRate)
                * static_cast<double>(core.project.bpm) / 60.0;
          const double endBeat = static_cast<double>(audio.countInEndFrame) / static_cast<double>(core.project.projectSampleRate)
                * static_cast<double>(core.project.bpm) / 60.0;
          ss << L"  recordStartBeat(derived)=" << startBeat
              << L", countInEndBeat(derived)=" << endBeat << L"\n";
     }
     ss << L"\n";

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

int DeviceProbeCurrentOutputSampleRate(const AudioRuntimeState& audio) {
    HRESULT hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool shouldUninit = SUCCEEDED(hrInit);

    IMMDeviceEnumerator* enumerator = nullptr;
    if (CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator)) != S_OK || enumerator == nullptr) {
        if (shouldUninit) CoUninitialize();
        return 0;
    }

    auto toLower = [](std::wstring s) {
        std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(::towlower(c)); });
        return s;
    };

    IMMDevice* device = nullptr;
    if (!audio.selectedOutputDeviceName.empty()) {
        IMMDeviceCollection* coll = nullptr;
        if (enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &coll) == S_OK && coll != nullptr) {
            UINT count = 0;
            coll->GetCount(&count);
            const std::wstring target = toLower(audio.selectedOutputDeviceName);
            for (UINT i = 0; i < count; ++i) {
                IMMDevice* dev = nullptr;
                if (coll->Item(i, &dev) != S_OK || dev == nullptr) {
                    continue;
                }
                IPropertyStore* props = nullptr;
                if (dev->OpenPropertyStore(STGM_READ, &props) == S_OK && props != nullptr) {
                    PROPVARIANT pv;
                    PropVariantInit(&pv);
                    if (props->GetValue(PKEY_Device_FriendlyName, &pv) == S_OK && pv.vt == VT_LPWSTR && pv.pwszVal != nullptr) {
                        const std::wstring friendly = toLower(pv.pwszVal);
                        if (friendly.find(target) != std::wstring::npos) {
                            device = dev;
                            PropVariantClear(&pv);
                            props->Release();
                            break;
                        }
                    }
                    PropVariantClear(&pv);
                    props->Release();
                }
                dev->Release();
            }
            coll->Release();
        }
    }

    if (device == nullptr) {
        enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    }
    enumerator->Release();

    int sampleRate = 0;
    if (device != nullptr) {
        IAudioClient* client = nullptr;
        WAVEFORMATEX* mixFmt = nullptr;
        if (device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client)) == S_OK && client != nullptr &&
            client->GetMixFormat(&mixFmt) == S_OK && mixFmt != nullptr) {
            sampleRate = static_cast<int>(mixFmt->nSamplesPerSec);
            CoTaskMemFree(mixFmt);
        }
        if (client != nullptr) {
            client->Release();
        }
        device->Release();
    }

    if (shouldUninit) CoUninitialize();
    return sampleRate;
}

// ── Engine lifecycle ──────────────────────────────────────────────────────────

namespace {

// Single source of truth for "what sample rate should we run at if no clip
// or user preference says otherwise?". Tries, in order:
//   1. user-preferred sample rate,
//   2. WASAPI mix-format probe of the current/selected output endpoint,
//   3. last opened output sample rate,
//   4. last opened input sample rate,
//   5. 44100 Hz as a last resort.
int ResolveDefaultSampleRate(const AudioRuntimeState& audio) {
    if (audio.preferredSampleRate > 0) return audio.preferredSampleRate;
    if (const int sr = DeviceProbeCurrentOutputSampleRate(audio); sr > 0) return sr;
    if (audio.lastOpenedOutputSampleRate > 0) return audio.lastOpenedOutputSampleRate;
    if (audio.lastOpenedInputSampleRate  > 0) return audio.lastOpenedInputSampleRate;
    return 44100;
}

} // namespace

bool AudioInitializeRuntime(HWND hwnd, CoreState& core, AudioRuntimeState& audio) {
    // Idempotent: if we're already past Uninitialized, don't re-init the
    // critical section or stomp engineState. Just refresh devices and SR.
    const bool firstInit = (audio.engineState.load() == AudioEngineState::Uninitialized);

    audio.hwnd = hwnd;
    audio.coreContext = &core;
    audio.engineInitError.clear();

    if (firstInit) {
        InitializeCriticalSection(&audio.audioStateLock);
    }

    DeviceRefreshInputDevices(audio);
    DeviceRefreshOutputDevices(audio);

    if (audio.outputDeviceIds.empty() && audio.inputDeviceIds.empty()) {
        audio.engineInitError = L"No audio input or output devices were detected.";
        audio.engineState.store(AudioEngineState::Error);
        return false;
    }

    // Note: project sample rate is owned by ProjectData (default 48000) and
    // is set only by project load or the Project > Sample Rate menu. Device
    // code does not write to it.
    (void)core;

    // Phase 24 / Step K2 — publish an initial MixSnapshot so the audio
    // callback's Load() is never null. Subsequent K phases add publishes
    // at every UI-side mix-parameter mutation site; for now this single
    // bootstrap publish is sufficient because callback only reads the
    // generation for diagnostics.
    PublishMixSnapshotFromCore(audio, core);

    audio.engineState.store(AudioEngineState::Ready);
    return true;
}

bool AudioEnsureReadyForTransport(CoreState& core, AudioRuntimeState& audio) {
    const AudioEngineState s = audio.engineState.load();
    if (s == AudioEngineState::Error) {
        // Caller may want to surface engineInitError.
        return false;
    }
    if (s == AudioEngineState::Uninitialized) {
        // Late init. Devices weren't enumerated at startup for some reason.
        DeviceRefreshInputDevices(audio);
        DeviceRefreshOutputDevices(audio);
        audio.engineState.store(AudioEngineState::Ready);
    }

    // Project sample rate is owned by ProjectData; never mutated here.
    return core.project.projectSampleRate > 0;
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

