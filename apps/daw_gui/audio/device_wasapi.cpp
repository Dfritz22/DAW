#include "device_wasapi.h"
#include "engine.h"
#include "device_common.h"  // GetRenderedPlaybackFrame
#include "core/automation.h"
#include "core/timeline.h"

// ── Forward declarations for orchestration functions in main.cpp ─────────────
void StopPlayback(UiState& state, bool rewind);

// ── Endpoint helpers ──────────────────────────────────────────────────────────

static IMMDevice* FindWasapiCaptureEndpoint(const std::wstring& preferredName) {
    IMMDeviceEnumerator* enumerator = nullptr;
    if (CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator)) != S_OK || enumerator == nullptr) {
        return nullptr;
    }

    auto toLower = [](std::wstring s) {
        std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(::towlower(c)); });
        return s;
    };

    IMMDevice* chosen = nullptr;
    if (!preferredName.empty()) {
        IMMDeviceCollection* coll = nullptr;
        if (enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &coll) == S_OK && coll != nullptr) {
            UINT count = 0;
            coll->GetCount(&count);
            const std::wstring target = toLower(preferredName);
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
                            chosen = dev;
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

    if (chosen == nullptr) {
        enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &chosen);
    }

    enumerator->Release();
    return chosen;
}

static IMMDevice* FindWasapiOutputEndpoint(const std::wstring& preferredName) {
    IMMDeviceEnumerator* enumerator = nullptr;
    if (CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator)) != S_OK || enumerator == nullptr) {
        return nullptr;
    }

    auto toLower = [](std::wstring s) {
        std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(::towlower(c)); });
        return s;
    };

    IMMDevice* chosen = nullptr;
    if (!preferredName.empty()) {
        IMMDeviceCollection* coll = nullptr;
        if (enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &coll) == S_OK && coll != nullptr) {
            UINT count = 0;
            coll->GetCount(&count);
            const std::wstring target = toLower(preferredName);
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
                            chosen = dev;
                            PropVariantClear(&pv);
                            props->Release();
                            break;
                        }
                    }
                    PropVariantClear(&pv);
                    props->Release();
                }
                if (chosen == nullptr) dev->Release();
            }
            coll->Release();
        }
    }

    if (chosen == nullptr) {
        enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &chosen);
    }

    enumerator->Release();
    return chosen;
}

// ── WASAPI render thread ──────────────────────────────────────────────────────

static DWORD WINAPI WasapiRenderThreadProc(LPVOID param) {
    auto* state = reinterpret_cast<UiState*>(param);
    if (state == nullptr) return 0;

    state->lastPlaybackInitError.clear();

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool coInitOk = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
    if (!coInitOk) {
        state->lastPlaybackInitError = L"WASAPI COM init failed";
        state->wasapiOutInitState.store(-1);
        state->audioThreadRunning.store(false);
        return 0;
    }

    IMMDevice*          device       = FindWasapiOutputEndpoint(state->selectedOutputDeviceName);
    IAudioClient*       audioClient  = nullptr;
    IAudioRenderClient* renderClient = nullptr;
    WAVEFORMATEX*       mixFmt       = nullptr;
    WAVEFORMATEX*       exclusiveFmt = nullptr;
    bool                fellBackToShared = false;

    auto fail = [&](const wchar_t* msg) {
        if (renderClient) { renderClient->Release(); renderClient = nullptr; }
        if (audioClient)  { audioClient->Release();  audioClient  = nullptr; }
        if (exclusiveFmt) { CoTaskMemFree(exclusiveFmt); exclusiveFmt = nullptr; }
        if (mixFmt)       { CoTaskMemFree(mixFmt);   mixFmt = nullptr; }
        if (device)       { device->Release();       device = nullptr; }
        if (coInitOk) CoUninitialize();
        state->lastPlaybackInitError = (msg != nullptr) ? msg : L"WASAPI initialization failed";
        state->wasapiOutInitState.store(-1);
        state->audioThreadRunning.store(false);
    };

    do {
        if (device == nullptr) { fail(L"no output endpoint"); return 0; }

        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&audioClient));
        if (FAILED(hr) || audioClient == nullptr) { fail(L"Activate"); return 0; }

        hr = audioClient->GetMixFormat(&mixFmt);
        if (FAILED(hr) || mixFmt == nullptr) { fail(L"GetMixFormat"); return 0; }

        AUDCLNT_SHAREMODE shareMode = AUDCLNT_SHAREMODE_SHARED;
        WAVEFORMATEX* openFmt = mixFmt;
        REFERENCE_TIME hnsBuffer = 0;

        if (state->audioBackend == AudioBackend::WasapiExclusive) {
            const int requestedSR = (state->preferredSampleRate > 0)
                ? state->preferredSampleRate
                : state->project.projectSampleRate;

            if (requestedSR > 0) {
                const size_t fmtBytes = sizeof(WAVEFORMATEX) + static_cast<size_t>(mixFmt->cbSize);
                exclusiveFmt = reinterpret_cast<WAVEFORMATEX*>(CoTaskMemAlloc(fmtBytes));
                if (exclusiveFmt != nullptr) {
                    std::memcpy(exclusiveFmt, mixFmt, fmtBytes);
                    exclusiveFmt->nSamplesPerSec = static_cast<DWORD>(requestedSR);
                    exclusiveFmt->nAvgBytesPerSec = exclusiveFmt->nSamplesPerSec * exclusiveFmt->nBlockAlign;
                    WAVEFORMATEX* closest = nullptr;
                    hr = audioClient->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, exclusiveFmt, &closest);
                    if (closest != nullptr) {
                        CoTaskMemFree(closest);
                        closest = nullptr;
                    }
                    if (SUCCEEDED(hr)) {
                        shareMode = AUDCLNT_SHAREMODE_EXCLUSIVE;
                        openFmt = exclusiveFmt;
                    } else {
                        fellBackToShared = true;
                    }
                } else {
                    fellBackToShared = true;
                }
            } else {
                fellBackToShared = true;
            }

            if (fellBackToShared) {
                state->lastPlaybackInitError = L"WASAPI exclusive failed; falling back to WASAPI shared mode.";
            }
        }

        if (state->preferredBufferFrames > 0 && openFmt->nSamplesPerSec > 0) {
            hnsBuffer = static_cast<REFERENCE_TIME>((10000000LL * static_cast<long long>(state->preferredBufferFrames)) / static_cast<long long>(openFmt->nSamplesPerSec));
            hnsBuffer = std::max<REFERENCE_TIME>(hnsBuffer, 10000);
        }

        hr = audioClient->Initialize(shareMode, 0, hnsBuffer, (shareMode == AUDCLNT_SHAREMODE_EXCLUSIVE) ? hnsBuffer : 0, openFmt, nullptr);
        if (FAILED(hr)) {
            if (shareMode == AUDCLNT_SHAREMODE_EXCLUSIVE) {
                fellBackToShared = true;
                state->lastPlaybackInitError = L"WASAPI exclusive initialization failed; using WASAPI shared mode.";
                hr = audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, hnsBuffer, 0, mixFmt, nullptr);
            }
            if (FAILED(hr)) {
                fail(L"Initialize");
                return 0;
            }
        }

        hr = audioClient->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&renderClient));
        if (FAILED(hr) || renderClient == nullptr) { fail(L"GetService"); return 0; }
    } while (false);

    // Store format metadata for diagnostics (copy to flat WAVEFORMATEX)
    {
        WAVEFORMATEX fmtCopy = *mixFmt;
        EnterCriticalSection(&state->audioStateLock);
        state->wasapiOutFormat              = fmtCopy;
        state->lastOpenedOutputSampleRate   = static_cast<int>(fmtCopy.nSamplesPerSec);
        state->lastOpenedOutputChannels     = static_cast<int>(fmtCopy.nChannels);
        state->activeDeviceSampleRate       = static_cast<int>(fmtCopy.nSamplesPerSec);
        LeaveCriticalSection(&state->audioStateLock);
    }

    const UINT32 nChannels   = mixFmt->nChannels;
    const bool   isFloat     = (mixFmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) ||
                               (mixFmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                                reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mixFmt)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
    const bool   isPcm16     = (mixFmt->wBitsPerSample == 16) &&
                               ((mixFmt->wFormatTag == WAVE_FORMAT_PCM) ||
                                (mixFmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                                 reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mixFmt)->SubFormat == KSDATAFORMAT_SUBTYPE_PCM));

    if (exclusiveFmt != nullptr) {
        CoTaskMemFree(exclusiveFmt);
        exclusiveFmt = nullptr;
    }
    CoTaskMemFree(mixFmt); mixFmt = nullptr;
    device->Release();     device = nullptr;

    UINT32 bufferFrameCount = 0;
    audioClient->GetBufferSize(&bufferFrameCount);
    if (bufferFrameCount == 0) { fail(L"GetBufferSize=0"); return 0; }

    state->activeDeviceBufferFrames = static_cast<int>(bufferFrameCount);

    if (FAILED(audioClient->Start())) { fail(L"Start"); return 0; }

    if (!fellBackToShared) {
        state->lastPlaybackInitError.clear();
    }

    state->wasapiOutInitState.store(1);  // signal ready

    std::vector<std::int16_t> pcmBuf;
    bool draining = false;

    while (!state->audioStopRequested.load()) {
        UINT32 padding = 0;
        if (FAILED(audioClient->GetCurrentPadding(&padding))) break;

        UINT32 available = bufferFrameCount - padding;
        if (available == 0) { Sleep(2); continue; }
        if (available > static_cast<UINT32>(kAudioBufferFrames * 4)) {
            available = static_cast<UINT32>(kAudioBufferFrames * 4);
        }

        if (draining) {
            Sleep(2);
            if (padding == 0) break;
            continue;
        }

        pcmBuf.assign(static_cast<size_t>(available) * 2, 0);
        bool reachedEnd = false;
        EnterCriticalSection(&state->audioStateLock);
        FillRealtimeForDeviceLocked(*state, pcmBuf.data(), static_cast<int>(available), static_cast<int>(state->wasapiOutFormat.nSamplesPerSec), &reachedEnd);
        LeaveCriticalSection(&state->audioStateLock);

        BYTE* pData = nullptr;
        if (FAILED(renderClient->GetBuffer(available, &pData)) || pData == nullptr) break;

        if (isFloat) {
            auto* fOut = reinterpret_cast<float*>(pData);
            for (UINT32 i = 0; i < available; ++i) {
                fOut[i * nChannels]     = static_cast<float>(pcmBuf[i * 2])     / 32767.0f;
                fOut[i * nChannels + 1] = static_cast<float>(pcmBuf[i * 2 + 1]) / 32767.0f;
                for (UINT32 ch = 2; ch < nChannels; ++ch) fOut[i * nChannels + ch] = 0.0f;
            }
        } else if (isPcm16) {
            auto* sOut = reinterpret_cast<std::int16_t*>(pData);
            for (UINT32 i = 0; i < available; ++i) {
                sOut[i * nChannels]     = pcmBuf[i * 2];
                sOut[i * nChannels + 1] = pcmBuf[i * 2 + 1];
                for (UINT32 ch = 2; ch < nChannels; ++ch) sOut[i * nChannels + ch] = 0;
            }
        } else {
            std::memset(pData, 0, static_cast<size_t>(available) * state->wasapiOutFormat.nBlockAlign);
        }

        renderClient->ReleaseBuffer(available, 0);
        if (reachedEnd) draining = true;
    }

    audioClient->Stop();
    renderClient->Release();
    audioClient->Release();

    if (coInitOk) CoUninitialize();
    state->audioThreadRunning.store(false);

    if (!state->audioStopRequested.load()) {
        PostMessage(state->hwnd, kMsgPlaybackFinished, 0, 0);
    }
    return 0;
}

// ── WASAPI capture thread ─────────────────────────────────────────────────────

static DWORD WINAPI WasapiRecordThreadProc(LPVOID param) {
    auto* state = reinterpret_cast<UiState*>(param);
    if (state == nullptr) {
        return 0;
    }

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool coInitOk = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
    if (!coInitOk) {
        state->lastRecordInitError = L"WASAPI COM init failed";
        state->recordInitState.store(-1);
        return 0;
    }

    IMMDevice* device = FindWasapiCaptureEndpoint(state->selectedInputDeviceName);
    IAudioClient* audioClient = nullptr;
    IAudioCaptureClient* captureClient = nullptr;
    WAVEFORMATEX* mixFmt = nullptr;

    do {
        if (device == nullptr) {
            state->lastRecordInitError = L"No WASAPI capture endpoint found";
            break;
        }
        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&audioClient));
        if (FAILED(hr) || audioClient == nullptr) {
            state->lastRecordInitError = L"WASAPI Activate(IAudioClient) failed";
            break;
        }

        hr = audioClient->GetMixFormat(&mixFmt);
        if (FAILED(hr) || mixFmt == nullptr) {
            state->lastRecordInitError = L"WASAPI GetMixFormat failed";
            break;
        }

        hr = audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 0, 0, mixFmt, nullptr);
        if (FAILED(hr)) {
            state->lastRecordInitError = L"WASAPI Initialize(shared) failed";
            break;
        }

        hr = audioClient->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&captureClient));
        if (FAILED(hr) || captureClient == nullptr) {
            state->lastRecordInitError = L"WASAPI GetService(IAudioCaptureClient) failed";
            break;
        }

        const int mixCh = std::max<int>(1, static_cast<int>(mixFmt->nChannels));
        const int outCh = (mixCh >= 2) ? 2 : 1;
        const int sr = static_cast<int>(mixFmt->nSamplesPerSec);

        state->waveInFormat = *mixFmt;
        state->recordInputChannels = outCh;
        state->lastOpenedInputSampleRate = sr;
        state->lastOpenedInputChannels = outCh;
        if (state->project.projectSampleRate <= 0 && sr > 0) {
            state->project.projectSampleRate = sr;
        }

        if (FAILED(audioClient->Start())) {
            state->lastRecordInitError = L"WASAPI capture start failed";
            break;
        }

        state->recordInitState.store(1);

        while (!state->recordStopRequested.load()) {
            UINT32 packetFrames = 0;
            if (FAILED(captureClient->GetNextPacketSize(&packetFrames))) {
                break;
            }

            if (packetFrames == 0) {
                Sleep(2);
                continue;
            }

            while (packetFrames > 0) {
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                if (FAILED(captureClient->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) {
                    packetFrames = 0;
                    break;
                }

                const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
                if (!silent && data != nullptr && frames > 0) {
                    const bool isFloat = (mixFmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) ||
                        (mixFmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE && reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mixFmt)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
                    const bool isPcm16 = (mixFmt->wBitsPerSample == 16) &&
                        ((mixFmt->wFormatTag == WAVE_FORMAT_PCM) ||
                         (mixFmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE && reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mixFmt)->SubFormat == KSDATAFORMAT_SUBTYPE_PCM));

                    if (isFloat) {
                        const float* f = reinterpret_cast<const float*>(data);
                        for (UINT32 i = 0; i < frames; ++i) {
                            float l = f[static_cast<size_t>(i) * mixCh];
                            float r = (mixCh > 1) ? f[static_cast<size_t>(i) * mixCh + 1] : l;
                            const std::int16_t li = static_cast<std::int16_t>(std::lrint(std::clamp(l, -1.0f, 1.0f) * 32767.0f));
                            const std::int16_t ri = static_cast<std::int16_t>(std::lrint(std::clamp(r, -1.0f, 1.0f) * 32767.0f));
                            if (outCh == 1) {
                                state->recordedInputPcm.push_back(li);
                            } else {
                                state->recordedInputPcm.push_back(li);
                                state->recordedInputPcm.push_back(ri);
                            }
                        }
                    } else if (isPcm16) {
                        const std::int16_t* s = reinterpret_cast<const std::int16_t*>(data);
                        for (UINT32 i = 0; i < frames; ++i) {
                            const std::int16_t li = s[static_cast<size_t>(i) * mixCh];
                            const std::int16_t ri = (mixCh > 1) ? s[static_cast<size_t>(i) * mixCh + 1] : li;
                            if (outCh == 1) {
                                state->recordedInputPcm.push_back(li);
                            } else {
                                state->recordedInputPcm.push_back(li);
                                state->recordedInputPcm.push_back(ri);
                            }
                        }
                    }

                    if (state->inputMonitoring) {
                        EnterCriticalSection(&state->audioStateLock);
                        const size_t n = state->recordedInputPcm.size();
                        const size_t appendCount = static_cast<size_t>(frames) * static_cast<size_t>(outCh);
                        if (appendCount <= n) {
                            state->monitorInputPcm.insert(state->monitorInputPcm.end(),
                                state->recordedInputPcm.end() - static_cast<std::vector<std::int16_t>::difference_type>(appendCount),
                                state->recordedInputPcm.end());
                        }
                        LeaveCriticalSection(&state->audioStateLock);
                    }
                }

                captureClient->ReleaseBuffer(frames);
                if (FAILED(captureClient->GetNextPacketSize(&packetFrames))) {
                    packetFrames = 0;
                }
            }
        }

        audioClient->Stop();
    } while (false);

    if (state->recordInitState.load() == 0) {
        state->recordInitState.store(-1);
    }

    if (mixFmt != nullptr) CoTaskMemFree(mixFmt);
    if (captureClient != nullptr) captureClient->Release();
    if (audioClient != nullptr) audioClient->Release();
    if (device != nullptr) device->Release();
    if (coInitOk) CoUninitialize();
    return 0;
}

// ── Public API ────────────────────────────────────────────────────────────────

bool StartWasapiAudio(HWND hwnd, UiState& state) {
    EnterCriticalSection(&state.audioStateLock);
    const float startBeat  = std::max(0.0f, state.playheadBeat);
    const std::uint64_t startFrame = FramesFromBeats(state, startBeat);
    state.playbackFrameCursor.store(startFrame);
    const std::uint64_t endFrame = ComputeProjectEndFrameLocked(state);
    LeaveCriticalSection(&state.audioStateLock);

    state.playbackStartTick = 0;
    state.playbackStartBeat = startBeat;
    state.playbackEndBeat   = BeatsFromFrames(state, endFrame);
    state.playing           = true;
    state.playingViaWasapi  = true;
    state.audioStopRequested.store(false);
    state.wasapiOutInitState.store(0);
    state.audioThreadRunning.store(true);
    state.audioThread = CreateThread(nullptr, 0, WasapiRenderThreadProc, &state, 0, nullptr);

    if (state.audioThread != nullptr) {
        // Wait up to 600 ms for the thread to open the device
        for (int i = 0; i < 60 && state.wasapiOutInitState.load() == 0; ++i) {
            Sleep(10);
        }
        if (state.wasapiOutInitState.load() == 1) {
            state.playbackStartTick = GetTickCount64();
            const int devSR = state.activeDeviceSampleRate;
            const bool hasTimelineAudio = !state.project.clips.empty() || !state.project.audio.empty();
            if (hasTimelineAudio && state.project.projectSampleRate > 0 && devSR > 0 && state.project.projectSampleRate != devSR) {
                std::wstringstream mismatch;
                mismatch
                    << L"The selected device does not support " << state.project.projectSampleRate << L" Hz.\n\n"
                    << L"Yes: Switch project to " << devSR << L" Hz\n"
                    << L"No: Use device at " << devSR << L" Hz and resample project audio in real time\n"
                    << L"Cancel: Abort playback (choose a different device from Audio menu)";
                const int choice = MessageBoxW(hwnd, mismatch.str().c_str(), L"Sample Rate Mismatch", MB_YESNOCANCEL | MB_ICONWARNING);
                if (choice == IDYES) {
                    state.project.projectSampleRate = devSR;
                } else if (choice == IDCANCEL) {
                    StopPlayback(state, false);
                    return false;
                }
            }
            if (!state.lastPlaybackInitError.empty()) {
                MessageBoxW(hwnd, state.lastPlaybackInitError.c_str(), L"Playback warning", MB_OK | MB_ICONWARNING);
            }
            return true;  // WASAPI output running
        }
        // Thread failed to open device – signal MME fallback
        state.audioStopRequested.store(true);
        WaitForSingleObject(state.audioThread, INFINITE);
        CloseHandle(state.audioThread);
        state.audioThread = nullptr;
    }
    // Reset for MME fallback
    state.playing          = false;
    state.playingViaWasapi = false;
    state.audioStopRequested.store(false);
    state.audioThreadRunning.store(false);
    return false;
}

void StopWasapiAudio(UiState& state) {
    state.audioStopRequested.store(true);

    if (state.audioThread != nullptr) {
        WaitForSingleObject(state.audioThread, INFINITE);
        CloseHandle(state.audioThread);
        state.audioThread = nullptr;
    }

    state.playingViaWasapi = false;
    state.audioThreadRunning.store(false);
}

bool StartWasapiRecording(HWND hwnd, UiState& state, int armedTrack, bool wasPlaying) {
    state.recordedInputPcm.clear();
    state.monitorInputPcm.clear();
    state.monitorInputReadPos = 0;
    state.recordTrackIndex = armedTrack;
    state.recordCaptureStartTickMs = GetTickCount64();
    const std::uint64_t timelineStartFrame = state.playing
        ? GetRenderedPlaybackFrame(state)
        : FramesFromBeats(state, std::max(0.0f, state.playheadBeat));

    // Compute preroll duration upfront so count-in click can play immediately.
    state.recordPrerollFrames = 0;
    if (!wasPlaying && state.countInEnabled) {
        state.recordPrerollFrames = FramesFromBeats(state, 4.0f * static_cast<float>(state.countInBars));
    }
    // Tentative placement: preroll end is deterministic regardless of init latency.
    state.recordStartFrame = timelineStartFrame + state.recordPrerollFrames;
    state.countingIn = (state.recordPrerollFrames > 0);

    state.recordUsingWasapi = true;
    state.recordStopRequested.store(false);
    state.recordInitState.store(0);
    state.lastRecordInitError.clear();
    state.recordThread = CreateThread(nullptr, 0, WasapiRecordThreadProc, &state, 0, nullptr);
    if (state.recordThread == nullptr) {
        state.countingIn = false;
        state.recordUsingWasapi = false;
        return false;
    }

    for (int i = 0; i < 60 && state.recordInitState.load() == 0; ++i) {
        Sleep(10);
    }

    if (state.recordInitState.load() != 1) {
        state.recordStopRequested.store(true);
        WaitForSingleObject(state.recordThread, INFINITE);
        CloseHandle(state.recordThread);
        state.recordThread = nullptr;
        state.countingIn = false;
        state.recordUsingWasapi = false;
        if (!state.lastRecordInitError.empty()) {
            MessageBoxW(hwnd, (L"WASAPI capture failed: " + state.lastRecordInitError + L"\nFalling back to MME.").c_str(), L"Record", MB_OK | MB_ICONWARNING);
        }
        return false;
    }

    // Capture is now running. Refine skip based on actual capture-start position so
    // clip placement is deterministic (same beat) across all takes.
    {
        const std::uint64_t captureNow = state.playing ? GetRenderedPlaybackFrame(state) : 0;
        const std::uint64_t scheduledStart = state.recordStartFrame;
        const std::uint64_t actualSkip = (captureNow < scheduledStart) ? (scheduledStart - captureNow) : 0;
        state.recordPrerollFrames = actualSkip;          // strip only remaining preroll
        state.recordStartFrame    = captureNow + actualSkip; // = max(captureNow, scheduledStart)
    }

    state.recording = true;
    return true;
}

void StopWasapiRecording(UiState& state) {
    state.recordStopRequested.store(true);

    if (state.recordThread != nullptr) {
        WaitForSingleObject(state.recordThread, INFINITE);
        CloseHandle(state.recordThread);
        state.recordThread = nullptr;
    }

    state.recordUsingWasapi = false;
    state.recordInitState.store(0);
}
