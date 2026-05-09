#include "device_wasapi.h"

#include "device_common.h"  // DeviceGetRenderedPlaybackFrame
#include "engine.h"
#include "engine_utils.h"
#include "core/automation.h"
#include "core/timeline.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>

namespace {
constexpr UINT kMsgPlaybackFinished = WM_APP + 1;
}

namespace daw::internal::audio {

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
    auto* audio = reinterpret_cast<AudioRuntimeState*>(param);
    if (audio == nullptr || audio->coreContext == nullptr) return 0;
    CoreState& core = *audio->coreContext;

    audio->lastPlaybackInitError.clear();

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool coInitOk = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
    if (!coInitOk) {
        audio->lastPlaybackInitError = L"WASAPI COM init failed";
        audio->wasapiOutInitState.store(-1);
        audio->audioThreadRunning.store(false);
        return 0;
    }

    IMMDevice*          device       = FindWasapiOutputEndpoint(audio->selectedOutputDeviceName);
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
        audio->lastPlaybackInitError = (msg != nullptr) ? msg : L"WASAPI initialization failed";
        audio->wasapiOutInitState.store(-1);
        audio->audioThreadRunning.store(false);
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

        if (audio->audioBackend == AudioBackend::WasapiExclusive) {
            const int requestedSR = (audio->preferredSampleRate > 0)
                ? audio->preferredSampleRate
                : core.project.projectSampleRate;

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
                audio->lastPlaybackInitError = L"WASAPI exclusive failed; falling back to WASAPI shared mode.";
            }
        }

        if (audio->preferredBufferFrames > 0 && openFmt->nSamplesPerSec > 0) {
            hnsBuffer = static_cast<REFERENCE_TIME>((10000000LL * static_cast<long long>(audio->preferredBufferFrames)) / static_cast<long long>(openFmt->nSamplesPerSec));
            hnsBuffer = std::max<REFERENCE_TIME>(hnsBuffer, 10000);
        }

        hr = audioClient->Initialize(shareMode, 0, hnsBuffer, (shareMode == AUDCLNT_SHAREMODE_EXCLUSIVE) ? hnsBuffer : 0, openFmt, nullptr);
        if (FAILED(hr)) {
            if (shareMode == AUDCLNT_SHAREMODE_EXCLUSIVE) {
                fellBackToShared = true;
                audio->lastPlaybackInitError = L"WASAPI exclusive initialization failed; using WASAPI shared mode.";
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

    {
        WAVEFORMATEX fmtCopy = *mixFmt;
        EnterCriticalSection(&audio->audioStateLock);
        audio->wasapiOutFormat              = fmtCopy;
        audio->lastOpenedOutputSampleRate   = static_cast<int>(fmtCopy.nSamplesPerSec);
        audio->lastOpenedOutputChannels     = static_cast<int>(fmtCopy.nChannels);
        audio->activeDeviceSampleRate       = static_cast<int>(fmtCopy.nSamplesPerSec);
        LeaveCriticalSection(&audio->audioStateLock);
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
    CoTaskMemFree(mixFmt);
    mixFmt = nullptr;
    device->Release();
    device = nullptr;

    UINT32 bufferFrameCount = 0;
    audioClient->GetBufferSize(&bufferFrameCount);
    if (bufferFrameCount == 0) { fail(L"GetBufferSize=0"); return 0; }

    audio->activeDeviceBufferFrames = static_cast<int>(bufferFrameCount);

    if (FAILED(audioClient->Start())) { fail(L"Start"); return 0; }

    if (!fellBackToShared) {
        audio->lastPlaybackInitError.clear();
    }

    audio->wasapiOutInitState.store(1);

    std::vector<std::int16_t> pcmBuf;
    bool draining = false;

    while (!audio->audioStopRequested.load()) {
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
        EnterCriticalSection(&audio->audioStateLock);
        EngineFillRealtimeForDeviceLocked(core, *audio, pcmBuf.data(), static_cast<int>(available), static_cast<int>(audio->wasapiOutFormat.nSamplesPerSec), &reachedEnd);
        LeaveCriticalSection(&audio->audioStateLock);

        BYTE* pData = nullptr;
        if (FAILED(renderClient->GetBuffer(available, &pData)) || pData == nullptr) break;

        if (isFloat) {
            auto* fOut = reinterpret_cast<float*>(pData);
            for (UINT32 i = 0; i < available; ++i) {
                fOut[i * nChannels]     = static_cast<float>(pcmBuf[i * 2]) / 32767.0f;
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
            std::memset(pData, 0, static_cast<size_t>(available) * audio->wasapiOutFormat.nBlockAlign);
        }

        renderClient->ReleaseBuffer(available, 0);
        if (reachedEnd) draining = true;
    }

    audioClient->Stop();
    renderClient->Release();
    audioClient->Release();

    if (coInitOk) CoUninitialize();
    audio->audioThreadRunning.store(false);

    if (!audio->audioStopRequested.load()) {
        PostMessage(audio->hwnd, kMsgPlaybackFinished, 0, 0);
    }
    return 0;
}

// ── WASAPI capture thread ─────────────────────────────────────────────────────

static DWORD WINAPI WasapiRecordThreadProc(LPVOID param) {
    auto* audio = reinterpret_cast<AudioRuntimeState*>(param);
    if (audio == nullptr || audio->coreContext == nullptr) {
        return 0;
    }
    CoreState& core = *audio->coreContext;

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool coInitOk = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
    if (!coInitOk) {
        audio->lastRecordInitError = L"WASAPI COM init failed";
        audio->recordInitState.store(-1);
        return 0;
    }

    IMMDevice* device = FindWasapiCaptureEndpoint(audio->selectedInputDeviceName);
    IAudioClient* audioClient = nullptr;
    IAudioCaptureClient* captureClient = nullptr;
    WAVEFORMATEX* mixFmt = nullptr;

    do {
        if (device == nullptr) {
            audio->lastRecordInitError = L"No WASAPI capture endpoint found";
            break;
        }
        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&audioClient));
        if (FAILED(hr) || audioClient == nullptr) {
            audio->lastRecordInitError = L"WASAPI Activate(IAudioClient) failed";
            break;
        }

        hr = audioClient->GetMixFormat(&mixFmt);
        if (FAILED(hr) || mixFmt == nullptr) {
            audio->lastRecordInitError = L"WASAPI GetMixFormat failed";
            break;
        }

        hr = audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 0, 0, mixFmt, nullptr);
        if (FAILED(hr)) {
            audio->lastRecordInitError = L"WASAPI Initialize(shared) failed";
            break;
        }

        hr = audioClient->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&captureClient));
        if (FAILED(hr) || captureClient == nullptr) {
            audio->lastRecordInitError = L"WASAPI GetService(IAudioCaptureClient) failed";
            break;
        }

        const int mixCh = std::max<int>(1, static_cast<int>(mixFmt->nChannels));
        const int outCh = (mixCh >= 2) ? 2 : 1;
        const int sr = static_cast<int>(mixFmt->nSamplesPerSec);

        audio->waveInFormat = *mixFmt;
        audio->recordInputChannels = outCh;
        audio->lastOpenedInputSampleRate = sr;
        audio->lastOpenedInputChannels = outCh;
        if (core.project.projectSampleRate <= 0 && sr > 0) {
            core.project.projectSampleRate = sr;
        }

        if (FAILED(audioClient->Start())) {
            audio->lastRecordInitError = L"WASAPI capture start failed";
            break;
        }

        audio->recordInitState.store(1);

        while (!audio->recordStopRequested.load()) {
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
                                audio->recordedInputPcm.push_back(li);
                            } else {
                                audio->recordedInputPcm.push_back(li);
                                audio->recordedInputPcm.push_back(ri);
                            }
                        }
                    } else if (isPcm16) {
                        const std::int16_t* s = reinterpret_cast<const std::int16_t*>(data);
                        for (UINT32 i = 0; i < frames; ++i) {
                            const std::int16_t li = s[static_cast<size_t>(i) * mixCh];
                            const std::int16_t ri = (mixCh > 1) ? s[static_cast<size_t>(i) * mixCh + 1] : li;
                            if (outCh == 1) {
                                audio->recordedInputPcm.push_back(li);
                            } else {
                                audio->recordedInputPcm.push_back(li);
                                audio->recordedInputPcm.push_back(ri);
                            }
                        }
                    }

                    if (audio->inputMonitoring) {
                        EnterCriticalSection(&audio->audioStateLock);
                        const size_t n = audio->recordedInputPcm.size();
                        const size_t appendCount = static_cast<size_t>(frames) * static_cast<size_t>(outCh);
                        if (appendCount <= n) {
                            audio->monitorInputPcm.insert(audio->monitorInputPcm.end(),
                                audio->recordedInputPcm.end() - static_cast<std::vector<std::int16_t>::difference_type>(appendCount),
                                audio->recordedInputPcm.end());
                        }
                        LeaveCriticalSection(&audio->audioStateLock);
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

    if (audio->recordInitState.load() == 0) {
        audio->recordInitState.store(-1);
    }

    if (mixFmt != nullptr) CoTaskMemFree(mixFmt);
    if (captureClient != nullptr) captureClient->Release();
    if (audioClient != nullptr) audioClient->Release();
    if (device != nullptr) device->Release();
    if (coInitOk) CoUninitialize();
    return 0;
}

} // namespace daw::internal::audio

using namespace daw::internal::audio;

// ── Public API ────────────────────────────────────────────────────────────────

bool DeviceStartWasapiAudio(HWND hwnd, CoreState& core, AudioRuntimeState& audio, float playheadBeat) {
    audio.hwnd = hwnd;
    audio.coreContext = &core;

    EnterCriticalSection(&audio.audioStateLock);
    const float startBeat = std::max(0.0f, playheadBeat);
    const std::uint64_t startFrame = TimelineFramesFromBeats(core, startBeat);
    audio.playbackFrameCursor.store(startFrame);
    const std::uint64_t endFrame = ComputeProjectEndFrameLocked(core);
    LeaveCriticalSection(&audio.audioStateLock);

    audio.playbackStartTick = 0;
    audio.playbackStartBeat = startBeat;
    audio.playbackEndBeat = TimelineBeatsFromFrames(core, endFrame);
    audio.playing = true;
    audio.playingViaWasapi = true;
    audio.audioStopRequested.store(false);
    audio.wasapiOutInitState.store(0);
    audio.audioThreadRunning.store(true);
    audio.audioThread = CreateThread(nullptr, 0, WasapiRenderThreadProc, &audio, 0, nullptr);

    if (audio.audioThread != nullptr) {
        for (int i = 0; i < 60 && audio.wasapiOutInitState.load() == 0; ++i) {
            Sleep(10);
        }
        if (audio.wasapiOutInitState.load() == 1) {
            audio.playbackStartTick = GetTickCount64();
            const int devSR = audio.activeDeviceSampleRate;
            const bool hasTimelineAudio = !core.project.clips.empty() || !core.project.audio.empty();
            if (hasTimelineAudio && core.project.projectSampleRate > 0 && devSR > 0 && core.project.projectSampleRate != devSR) {
                std::wstringstream mismatch;
                mismatch
                    << L"The selected device does not support " << core.project.projectSampleRate << L" Hz.\n\n"
                    << L"Yes: Switch project to " << devSR << L" Hz\n"
                    << L"No: Use device at " << devSR << L" Hz and resample project audio in real time\n"
                    << L"Cancel: Abort playback (choose a different device from Audio menu)";
                const int choice = MessageBoxW(hwnd, mismatch.str().c_str(), L"Sample Rate Mismatch", MB_YESNOCANCEL | MB_ICONWARNING);
                if (choice == IDYES) {
                    core.project.projectSampleRate = devSR;
                } else if (choice == IDCANCEL) {
                    DeviceStopWasapiAudio(audio);
                    audio.playing = false;
                    return false;
                }
            }
            if (!audio.lastPlaybackInitError.empty()) {
                MessageBoxW(hwnd, audio.lastPlaybackInitError.c_str(), L"Playback warning", MB_OK | MB_ICONWARNING);
            }
            return true;
        }

        audio.audioStopRequested.store(true);
        WaitForSingleObject(audio.audioThread, INFINITE);
        CloseHandle(audio.audioThread);
        audio.audioThread = nullptr;
    }

    audio.playing = false;
    audio.playingViaWasapi = false;
    audio.audioStopRequested.store(false);
    audio.audioThreadRunning.store(false);
    return false;
}

void DeviceStopWasapiAudio(AudioRuntimeState& audio) {
    audio.audioStopRequested.store(true);

    if (audio.audioThread != nullptr) {
        WaitForSingleObject(audio.audioThread, INFINITE);
        CloseHandle(audio.audioThread);
        audio.audioThread = nullptr;
    }

    audio.playingViaWasapi = false;
    audio.audioThreadRunning.store(false);
}

bool DeviceStartWasapiRecording(HWND hwnd, CoreState& core, AudioRuntimeState& audio, int armedTrack, bool wasPlaying, float playheadBeat) {
    audio.hwnd = hwnd;
    audio.coreContext = &core;

    audio.recordedInputPcm.clear();
    audio.monitorInputPcm.clear();
    audio.monitorInputReadPos = 0;
    audio.recordTrackIndex = armedTrack;
    audio.recordCaptureStartTickMs = GetTickCount64();
    const std::uint64_t timelineStartFrame = audio.playing
        ? DeviceGetRenderedPlaybackFrame(core, audio)
        : TimelineFramesFromBeats(core, std::max(0.0f, playheadBeat));

    audio.recordPrerollFrames = 0;
    if (!wasPlaying && audio.countInEnabled) {
        audio.recordPrerollFrames = TimelineFramesFromBeats(core, 4.0f * static_cast<float>(audio.countInBars));
    }
    audio.recordStartFrame = timelineStartFrame + audio.recordPrerollFrames;
    audio.countingIn = (audio.recordPrerollFrames > 0);

    audio.recordUsingWasapi = true;
    audio.recordStopRequested.store(false);
    audio.recordInitState.store(0);
    audio.lastRecordInitError.clear();
    audio.recordThread = CreateThread(nullptr, 0, WasapiRecordThreadProc, &audio, 0, nullptr);
    if (audio.recordThread == nullptr) {
        audio.countingIn = false;
        audio.recordUsingWasapi = false;
        return false;
    }

    for (int i = 0; i < 60 && audio.recordInitState.load() == 0; ++i) {
        Sleep(10);
    }

    if (audio.recordInitState.load() != 1) {
        audio.recordStopRequested.store(true);
        WaitForSingleObject(audio.recordThread, INFINITE);
        CloseHandle(audio.recordThread);
        audio.recordThread = nullptr;
        audio.countingIn = false;
        audio.recordUsingWasapi = false;
        if (!audio.lastRecordInitError.empty()) {
            MessageBoxW(hwnd, (L"WASAPI capture failed: " + audio.lastRecordInitError + L"\nFalling back to MME.").c_str(), L"Record", MB_OK | MB_ICONWARNING);
        }
        return false;
    }

    {
        const std::uint64_t captureNow = audio.playing ? DeviceGetRenderedPlaybackFrame(core, audio) : 0;
        const std::uint64_t scheduledStart = audio.recordStartFrame;
        const std::uint64_t actualSkip = (captureNow < scheduledStart) ? (scheduledStart - captureNow) : 0;
        audio.recordPrerollFrames = actualSkip;
        audio.recordStartFrame = captureNow + actualSkip;
    }

    audio.recording = true;
    return true;
}

void DeviceStopWasapiRecording(AudioRuntimeState& audio) {
    audio.recordStopRequested.store(true);

    if (audio.recordThread != nullptr) {
        WaitForSingleObject(audio.recordThread, INFINITE);
        CloseHandle(audio.recordThread);
        audio.recordThread = nullptr;
    }

    audio.recordUsingWasapi = false;
    audio.recordInitState.store(0);
}
