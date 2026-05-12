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

// INITGUID must be defined BEFORE the propkey headers so the property key
// constants get a definition (not just a declaration) in this TU.
#define INITGUID
#include <initguid.h>
#include <propsys.h>
#include <propvarutil.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>
// Define PKEY_AudioEngine_DeviceFormat ourselves (the SDK only declares it).
extern "C" const PROPERTYKEY DECLSPEC_SELECTANY PKEY_AudioEngine_DeviceFormat_Local = {
    { 0xf19f064d, 0x082c, 0x4e27, { 0xbc, 0x73, 0x68, 0x82, 0xa1, 0xbb, 0x8e, 0x4c } }, 0
};

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
    AUDCLNT_SHAREMODE   openedShareMode  = AUDCLNT_SHAREMODE_SHARED;

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

            // 1) Try the format Windows has configured for this endpoint
            //    (Sound Control Panel → Properties → Advanced → Default Format).
            //    For Exclusive mode this is virtually always the format the
            //    driver actually wants. Read PKEY_AudioEngine_DeviceFormat.
            WAVEFORMATEX* deviceFmt = nullptr;
            {
                IPropertyStore* propStore = nullptr;
                if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &propStore)) && propStore != nullptr) {
                    PROPVARIANT pv; PropVariantInit(&pv);
                    if (SUCCEEDED(propStore->GetValue(PKEY_AudioEngine_DeviceFormat_Local, &pv))
                        && pv.vt == VT_BLOB && pv.blob.cbSize >= sizeof(WAVEFORMATEX)
                        && pv.blob.pBlobData != nullptr) {
                        const WAVEFORMATEX* src = reinterpret_cast<const WAVEFORMATEX*>(pv.blob.pBlobData);
                        const size_t need = sizeof(WAVEFORMATEX) + src->cbSize;
                        if (pv.blob.cbSize >= need) {
                            deviceFmt = reinterpret_cast<WAVEFORMATEX*>(CoTaskMemAlloc(need));
                            if (deviceFmt) std::memcpy(deviceFmt, src, need);
                        }
                    }
                    PropVariantClear(&pv);
                    propStore->Release();
                }
            }

            std::wstring exclusiveFailReason;
            bool exclusiveOk = false;

            // Reject any negotiated/closest format the render loop can't write.
            // We only support IEEE float (any bit depth, but typically 32) and
            // 16-bit PCM. Anything else (24-bit packed, 24-in-32, 32-bit int)
            // would silently fall into the memset-zero branch below.
            auto formatIsRenderable = [](const WAVEFORMATEX* f) -> bool {
                if (!f) return false;
                const bool fl = (f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) ||
                                (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                                 reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(f)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
                const bool i16 = (f->wBitsPerSample == 16) &&
                                 ((f->wFormatTag == WAVE_FORMAT_PCM) ||
                                  (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                                   reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(f)->SubFormat == KSDATAFORMAT_SUBTYPE_PCM));
                return fl || i16;
            };

            auto tryFormat = [&](WAVEFORMATEX* fmt, const wchar_t* label) -> bool {
                if (!fmt) return false;
                WAVEFORMATEX* closest = nullptr;
                HRESULT hrSup = audioClient->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, fmt, &closest);
                if (hrSup == S_FALSE && closest != nullptr) {
                    if (!formatIsRenderable(closest)) {
                        CoTaskMemFree(closest);
                        wchar_t buf[160]{};
                        swprintf_s(buf, L"  - %s: closest=non-renderable\n", label);
                        exclusiveFailReason += buf;
                        return false;
                    }
                    const size_t cBytes = sizeof(WAVEFORMATEX) + static_cast<size_t>(closest->cbSize);
                    if (exclusiveFmt) { CoTaskMemFree(exclusiveFmt); exclusiveFmt = nullptr; }
                    exclusiveFmt = reinterpret_cast<WAVEFORMATEX*>(CoTaskMemAlloc(cBytes));
                    if (exclusiveFmt) std::memcpy(exclusiveFmt, closest, cBytes);
                    CoTaskMemFree(closest);
                    return exclusiveFmt != nullptr;
                }
                if (closest != nullptr) { CoTaskMemFree(closest); closest = nullptr; }
                if (SUCCEEDED(hrSup)) {
                    if (!formatIsRenderable(fmt)) {
                        wchar_t buf[160]{};
                        swprintf_s(buf, L"  - %s: non-renderable\n", label);
                        exclusiveFailReason += buf;
                        return false;
                    }
                    const size_t fBytes = sizeof(WAVEFORMATEX) + static_cast<size_t>(fmt->cbSize);
                    if (exclusiveFmt) { CoTaskMemFree(exclusiveFmt); exclusiveFmt = nullptr; }
                    exclusiveFmt = reinterpret_cast<WAVEFORMATEX*>(CoTaskMemAlloc(fBytes));
                    if (exclusiveFmt) std::memcpy(exclusiveFmt, fmt, fBytes);
                    return exclusiveFmt != nullptr;
                }
                wchar_t buf[160]{};
                swprintf_s(buf, L"  - %s: 0x%08lX\n", label, static_cast<unsigned long>(hrSup));
                exclusiveFailReason += buf;
                return false;
            };

            // Try the device's configured format first.
            if (deviceFmt) {
                wchar_t lbl[96]{};
                swprintf_s(lbl, L"DeviceFormat %luHz %dch %dbit", deviceFmt->nSamplesPerSec,
                           deviceFmt->nChannels, deviceFmt->wBitsPerSample);
                if (tryFormat(deviceFmt, lbl)) exclusiveOk = true;
            }

            // 2) Build SR candidates and try several bit depths / subtypes.
            if (!exclusiveOk) {
                int candidates[8]{};
                int candCount = 0;
                auto pushCand = [&](int sr) {
                    if (sr <= 0) return;
                    for (int i = 0; i < candCount; ++i) if (candidates[i] == sr) return;
                    if (candCount < 8) candidates[candCount++] = sr;
                };
                pushCand(requestedSR);
                if (deviceFmt) pushCand(static_cast<int>(deviceFmt->nSamplesPerSec));
                pushCand(static_cast<int>(mixFmt->nSamplesPerSec));
                pushCand(48000);
                pushCand(44100);
                pushCand(96000);
                pushCand(88200);

                struct FmtSpec { WORD bits; WORD validBits; bool isFloat; const wchar_t* name; };
                // The render loop only knows how to write IEEE float and 16-bit
                // PCM. Don't try anything else here; otherwise we'd "succeed"
                // and then write silence to the device buffer.
                const FmtSpec specs[] = {
                    {32, 32, true,  L"f32"},
                    {16, 16, false, L"i16"},
                };

                // Use a WAVEFORMATEXTENSIBLE on the stack for trials.
                WAVEFORMATEXTENSIBLE wfx{};
                for (const auto& spec : specs) {
                    if (exclusiveOk) break;
                    for (int ci = 0; ci < candCount && !exclusiveOk; ++ci) {
                        std::memset(&wfx, 0, sizeof(wfx));
                        wfx.Format.wFormatTag      = WAVE_FORMAT_EXTENSIBLE;
                        wfx.Format.nChannels       = static_cast<WORD>(deviceFmt ? deviceFmt->nChannels : mixFmt->nChannels);
                        if (wfx.Format.nChannels < 2) wfx.Format.nChannels = 2;
                        wfx.Format.nSamplesPerSec  = static_cast<DWORD>(candidates[ci]);
                        wfx.Format.wBitsPerSample  = spec.bits;
                        wfx.Format.nBlockAlign     = static_cast<WORD>(wfx.Format.nChannels * (wfx.Format.wBitsPerSample / 8));
                        wfx.Format.nAvgBytesPerSec = wfx.Format.nSamplesPerSec * wfx.Format.nBlockAlign;
                        wfx.Format.cbSize          = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
                        wfx.Samples.wValidBitsPerSample = spec.validBits;
                        wfx.dwChannelMask = (wfx.Format.nChannels == 2) ? (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT) : 0;
                        wfx.SubFormat = spec.isFloat ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT : KSDATAFORMAT_SUBTYPE_PCM;

                        wchar_t lbl[96]{};
                        swprintf_s(lbl, L"%s %dHz %dch", spec.name, candidates[ci], wfx.Format.nChannels);
                        if (tryFormat(reinterpret_cast<WAVEFORMATEX*>(&wfx), lbl)) {
                            exclusiveOk = true;
                            break;
                        }
                    }
                }
            }

            if (deviceFmt) { CoTaskMemFree(deviceFmt); deviceFmt = nullptr; }

            if (exclusiveOk && exclusiveFmt != nullptr) {
                shareMode = AUDCLNT_SHAREMODE_EXCLUSIVE;
                openFmt = exclusiveFmt;
            } else {
                fellBackToShared = true;
                std::wstring msg = L"WASAPI Exclusive: device rejected all candidate formats. Falling back to Shared.\nDetails:\n";
                msg += exclusiveFailReason;
                audio->lastPlaybackInitError = msg;
            }
        }

        if (audio->preferredBufferFrames > 0 && openFmt->nSamplesPerSec > 0) {
            hnsBuffer = static_cast<REFERENCE_TIME>((10000000LL * static_cast<long long>(audio->preferredBufferFrames)) / static_cast<long long>(openFmt->nSamplesPerSec));
            hnsBuffer = std::max<REFERENCE_TIME>(hnsBuffer, 10000);
        }
        // For Exclusive mode, the requested buffer must be at least the
        // device's minimum period or Initialize will fail. Honor that.
        if (shareMode == AUDCLNT_SHAREMODE_EXCLUSIVE) {
            REFERENCE_TIME devDefault = 0, devMin = 0;
            if (SUCCEEDED(audioClient->GetDevicePeriod(&devDefault, &devMin))) {
                if (hnsBuffer < devMin) hnsBuffer = devMin;
            }
        }

        hr = audioClient->Initialize(shareMode, 0, hnsBuffer, (shareMode == AUDCLNT_SHAREMODE_EXCLUSIVE) ? hnsBuffer : 0, openFmt, nullptr);
        if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED && shareMode == AUDCLNT_SHAREMODE_EXCLUSIVE) {
            // The driver wants a specific aligned buffer size; ask for it,
            // release the client, recreate, and re-Initialize with the
            // aligned period (per MSDN guidance).
            UINT32 alignedFrames = 0;
            if (SUCCEEDED(audioClient->GetBufferSize(&alignedFrames)) && alignedFrames > 0) {
                hnsBuffer = static_cast<REFERENCE_TIME>(
                    (10000000.0 * alignedFrames) / static_cast<double>(openFmt->nSamplesPerSec) + 0.5);
                audioClient->Release();
                audioClient = nullptr;
                hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&audioClient));
                if (SUCCEEDED(hr) && audioClient != nullptr) {
                    hr = audioClient->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, 0, hnsBuffer, hnsBuffer, openFmt, nullptr);
                }
            }
        }
        if (FAILED(hr)) {
            if (shareMode == AUDCLNT_SHAREMODE_EXCLUSIVE) {
                fellBackToShared = true;
                wchar_t msg[160]{};
                swprintf_s(msg, L"WASAPI Exclusive Initialize failed (0x%08lX). Falling back to Shared.",
                           static_cast<unsigned long>(hr));
                audio->lastPlaybackInitError = msg;
                if (audioClient == nullptr) {
                    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&audioClient));
                }
                if (SUCCEEDED(hr) && audioClient != nullptr) {
                    hr = audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, hnsBuffer, 0, mixFmt, nullptr);
                    openFmt = mixFmt;
                    shareMode = AUDCLNT_SHAREMODE_SHARED;
                }
            }
            if (FAILED(hr)) {
                fail(L"Initialize");
                return 0;
            }
        }

        hr = audioClient->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&renderClient));
        if (FAILED(hr) || renderClient == nullptr) { fail(L"GetService"); return 0; }
        openedShareMode = shareMode;
    } while (false);

    {
        WAVEFORMATEX fmtCopy{};
        // Use the format we ACTUALLY opened with (openFmt), not always mixFmt.
        // Otherwise the engine will think the device opened at the shared-mode
        // mix rate even when Exclusive negotiated a different rate, which
        // makes srcDeviceActive lie and the realtime SRC math go wrong.
        WAVEFORMATEX* effectiveFmt = (openedShareMode == AUDCLNT_SHAREMODE_EXCLUSIVE && exclusiveFmt != nullptr)
            ? exclusiveFmt : mixFmt;
        fmtCopy = *effectiveFmt;
        EnterCriticalSection(&audio->audioStateLock);
        audio->wasapiOutFormat              = fmtCopy;
        audio->lastOpenedOutputSampleRate   = static_cast<int>(fmtCopy.nSamplesPerSec);
        audio->lastOpenedOutputChannels     = static_cast<int>(fmtCopy.nChannels);
        audio->activeDeviceSampleRate       = static_cast<int>(fmtCopy.nSamplesPerSec);
        LeaveCriticalSection(&audio->audioStateLock);
    }

    // Pick the effective format we actually opened with for all downstream
    // sample-format / channel decisions. Was always reading mixFmt before,
    // which is wrong in Exclusive mode if we negotiated a different SR or
    // format from the device's shared mix format.
    WAVEFORMATEX* activeFmt = (openedShareMode == AUDCLNT_SHAREMODE_EXCLUSIVE && exclusiveFmt != nullptr)
        ? exclusiveFmt : mixFmt;
    const UINT32 nChannels   = activeFmt->nChannels;
    const bool   isFloat     = (activeFmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) ||
                               (activeFmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                                reinterpret_cast<WAVEFORMATEXTENSIBLE*>(activeFmt)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
    const bool   isPcm16     = (activeFmt->wBitsPerSample == 16) &&
                               ((activeFmt->wFormatTag == WAVE_FORMAT_PCM) ||
                                (activeFmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                                 reinterpret_cast<WAVEFORMATEXTENSIBLE*>(activeFmt)->SubFormat == KSDATAFORMAT_SUBTYPE_PCM));

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
    (void)core;

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
        // Project sample rate is owned by ProjectData; do not mutate from device code.

        if (FAILED(audioClient->Start())) {
            audio->lastRecordInitError = L"WASAPI capture start failed";
            break;
        }

        audio->recordInitState.store(1);

        // True once we've drained any queued capture packets that were filled
        // (in whole or in part) during count-in. Without this drain, the very
        // first packet seen after countingIn flips false would prepend up to
        // ~hundreds of ms of stale audio captured during the click-in,
        // shifting the user's performance later in the recorded clip.
        bool prerollDrained = false;

        while (!audio->recordStopRequested.load()) {
            // On the count-in -> capture transition, throw away whatever the
            // capture client has queued so the first appended packet contains
            // only post-count-in audio.
            if (!prerollDrained && !audio->countingIn) {
                UINT32 stalePacket = 0;
                while (SUCCEEDED(captureClient->GetNextPacketSize(&stalePacket)) && stalePacket > 0) {
                    BYTE* sd = nullptr;
                    UINT32 sf = 0;
                    DWORD sflags = 0;
                    if (FAILED(captureClient->GetBuffer(&sd, &sf, &sflags, nullptr, nullptr))) {
                        break;
                    }
                    captureClient->ReleaseBuffer(sf);
                }
                prerollDrained = true;
            }

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
                if (!silent && data != nullptr && frames > 0 && !audio->countingIn && prerollDrained) {
                    // Skip recording during count-in (only record after count-in ends)
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
    audio.engineSrcPrimed = false;
    audio.engineSrcPhase  = 0.0;
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

            // The device may silently ignore the requested sample rate (USB
            // class-compliant interfaces, the AXE-FX II, etc. are hard-locked
            // to their native rate). Detect that and tell the user once, then
            // snap `preferredSampleRate` to the rate the device actually
            // opened at so the UI (Audio Settings combo, Audio menu
            // checkmark, project file save) reflects reality instead of the
            // unmet wish.
            if (audio.preferredSampleRate > 0 && devSR > 0 && audio.preferredSampleRate != devSR) {
                std::wstringstream warn;
                warn << L"The audio device did not accept the requested sample rate ("
                     << audio.preferredSampleRate << L" Hz).\n\n"
                     << L"It opened at its native rate (" << devSR << L" Hz) instead. "
                     << L"Many USB audio interfaces are hard-locked to a single sample rate "
                     << L"and the rate must be changed in the device's own driver / control panel.\n\n"
                     << L"The Audio Settings will now show the device's actual sample rate.";
                MessageBoxW(hwnd, warn.str().c_str(), L"Device sample rate not changed", MB_OK | MB_ICONWARNING);
                audio.preferredSampleRate = devSR;
            }

            const bool hasTimelineAudio = !core.project.clips.empty() || !core.project.audio.empty();
            if (hasTimelineAudio && core.project.projectSampleRate > 0 && devSR > 0 && core.project.projectSampleRate != devSR) {
                // Suppress the dialog if the user has already acknowledged
                // this exact (project SR, device SR) pair. Many devices
                // (USB-class, AXE-FX II, etc.) refuse SR changes, so popping
                // this on every Play would be a constant nuisance once the
                // user has accepted the SRC tradeoff.
                const bool alreadyAcknowledged =
                    audio.acknowledgedMismatchProjectSR == core.project.projectSampleRate &&
                    audio.acknowledgedMismatchDeviceSR  == devSR;
                if (!alreadyAcknowledged) {
                    std::wstringstream mismatch;
                    mismatch
                        << L"Project sample rate (" << core.project.projectSampleRate << L" Hz) does not match the device's sample rate (" << devSR << L" Hz).\n\n"
                        << L"The audio device did not accept a sample-rate change, so real-time sample rate conversion will be applied to keep playback and recording at the project rate.\n\n"
                        << L"This dialog will not appear again until the project or device sample rate changes.\n\n"
                        << L"Continue?";
                    const int choice = MessageBoxW(hwnd, mismatch.str().c_str(), L"Sample Rate Mismatch", MB_OKCANCEL | MB_ICONINFORMATION);
                    if (choice == IDCANCEL) {
                        DeviceStopWasapiAudio(audio);
                        audio.playing = false;
                        return false;
                    }
                    audio.acknowledgedMismatchProjectSR = core.project.projectSampleRate;
                    audio.acknowledgedMismatchDeviceSR  = devSR;
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

    // Ensure project sample rate is set before computing preroll duration
    // Will be set from input device format or from output device when playback starts
    // Project sample rate is owned by ProjectData (default 48000). Device code
    // does not write to it; SRC will reconcile any device/project mismatch.

    const std::uint64_t timelineStartFrame = audio.playing
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
    // Mark recording active BEFORE starting the playback backend so the very
    // first engine callback sees recording=true and keeps the render loop
    // alive (otherwise an empty-project + metronome-only or count-in-only
    // session can race and immediately tear down).
    audio.recording = true;

    // Always start the playback backend during recording. The render thread
    // owns playbackFrameCursor advance (and therefore the visible playhead);
    // without it, recording with no clips, no metronome, and no count-in
    // would capture audio fine but the playhead would never move.
    if (!audio.playing) {
        if (!DeviceStartWasapiAudio(hwnd, core, audio, playheadBeat)) {
            audio.countingIn = false;
            audio.recording = false;
            return false;
        }
    }

    audio.recordUsingWasapi = true;
    audio.recordStopRequested.store(false);
    audio.recordInitState.store(0);
    audio.lastRecordInitError.clear();
    audio.recordThread = CreateThread(nullptr, 0, WasapiRecordThreadProc, &audio, 0, nullptr);
    if (audio.recordThread == nullptr) {
        audio.countingIn = false;
        audio.recordUsingWasapi = false;
        audio.recording = false;
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
        audio.recording = false;
        if (!audio.lastRecordInitError.empty()) {
            MessageBoxW(hwnd, (L"WASAPI capture failed: " + audio.lastRecordInitError + L"\nFalling back to MME.").c_str(), L"Record", MB_OK | MB_ICONWARNING);
        }
        return false;
    }

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
