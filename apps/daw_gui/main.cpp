#include "daw_sdk.h"
#include "core/internal_app_services.h"
#include "ui/draw.h"
#include "ui/layout.h"
#include "ai/automix_bridge.h"

using daw::internal::core::DefaultInsertBypass;
using daw::internal::core::DefaultInsertConfig;
using daw::internal::core::DefaultInsertEffects;
using daw::internal::core::FindRepoRoot;
using daw::internal::core::QuoteArg;
using daw::internal::core::UpdateWindowTitle;

const wchar_t* BusName(int busIndex) {
    static const wchar_t* kNames[kBusCount] = {L"Drums", L"Music", L"Vocals", L"Master"};
    if (busIndex < 0 || busIndex >= kBusCount) {
        return L"Music";
    }
    return kNames[busIndex];
}

void ApplyBalancePan(float pan, float* left, float* right) {
    const float p = std::clamp(pan, -1.0f, 1.0f);
    if (p < 0.0f) {
        *right *= (1.0f + p);
    } else if (p > 0.0f) {
        *left *= (1.0f - p);
    }
}

void ImportWavFiles(HWND hwnd, UiState& state);

// Forward declarations for audio orchestration (defined after DoAutoMaster/DoExportMix)
void StopPlayback(UiState& state, bool rewind);
void StopRecording(UiState& state, bool commitTake);
bool StartRecording(HWND hwnd, UiState& state);
bool StartPlayback(HWND hwnd, UiState& state);

// ============================================================
// Audio Settings Dialog
// ============================================================

struct AudioSettingsDlgData {
    UiState*         appState    {nullptr};
    std::vector<int> sampleRates;
    std::vector<int> bufferSizes;
    // Originals preserved for Cancel
    AudioBackend     origBackend          {AudioBackend::WasapiShared};
    int              origSampleRate       {0};
    int              origBufferFrames     {0};
    UINT             origOutputDeviceId   {WAVE_MAPPER};
    std::wstring     origOutputDeviceName;
    UINT             origInputDeviceId    {WAVE_MAPPER};
    std::wstring     origInputDeviceName;
};

static constexpr int kAsDlgBackend    = 1001;
static constexpr int kAsDlgOutputDev  = 1002;
static constexpr int kAsDlgInputDev   = 1003;
static constexpr int kAsDlgSampleRate = 1004;
static constexpr int kAsDlgBufferSize = 1005;
static constexpr int kAsDlgStatus     = 1006;
static constexpr int kAsDlgApply      = 1010;
// IDOK = 1, IDCANCEL = 2

static void AsDlgReadFields(HWND hwnd, AudioSettingsDlgData& d) {
    UiState& state = *d.appState;
    const int beIdx = (int)SendDlgItemMessageW(hwnd, kAsDlgBackend, CB_GETCURSEL, 0, 0);
    if      (beIdx == 0) state.audioBackend = AudioBackend::MME;
    else if (beIdx == 1) state.audioBackend = AudioBackend::WasapiShared;
    else if (beIdx == 2) state.audioBackend = AudioBackend::WasapiExclusive;

    const int outIdx = (int)SendDlgItemMessageW(hwnd, kAsDlgOutputDev, CB_GETCURSEL, 0, 0);
    if (outIdx >= 0 && outIdx < (int)state.outputDeviceIds.size()) {
        state.selectedOutputDeviceId   = state.outputDeviceIds[outIdx];
        state.selectedOutputDeviceName = state.outputDeviceNames[outIdx];
    }
    const int inIdx = (int)SendDlgItemMessageW(hwnd, kAsDlgInputDev, CB_GETCURSEL, 0, 0);
    if (inIdx >= 0 && inIdx < (int)state.inputDeviceIds.size()) {
        state.selectedInputDeviceId   = state.inputDeviceIds[inIdx];
        state.selectedInputDeviceName = state.inputDeviceNames[inIdx];
    }
    const int srIdx = (int)SendDlgItemMessageW(hwnd, kAsDlgSampleRate, CB_GETCURSEL, 0, 0);
    if (srIdx >= 0 && srIdx < (int)d.sampleRates.size()) {
        state.preferredSampleRate = d.sampleRates[srIdx];
    }
    const int bufIdx = (int)SendDlgItemMessageW(hwnd, kAsDlgBufferSize, CB_GETCURSEL, 0, 0);
    if (bufIdx >= 0 && bufIdx < (int)d.bufferSizes.size()) {
        state.preferredBufferFrames = d.bufferSizes[bufIdx];
    }
}

static void AsDlgUpdateStatus(HWND hwnd, const UiState& state) {
    wchar_t buf[256]{};
    const int sr  = state.activeDeviceSampleRate   > 0 ? state.activeDeviceSampleRate   : state.project.projectSampleRate;
    const int bsz = state.activeDeviceBufferFrames > 0 ? state.activeDeviceBufferFrames : state.preferredBufferFrames;
    if (sr > 0) {
        const double ms = bsz > 0 ? static_cast<double>(bsz) / static_cast<double>(sr) * 1000.0 : 0.0;
        swprintf_s(buf, L"Active: %d Hz  /  %d frames  (~%.1f ms latency)", sr, bsz, ms);
    } else {
        wcscpy_s(buf, L"Active: (device not yet opened)");
    }
    SetDlgItemTextW(hwnd, kAsDlgStatus, buf);
}

static LRESULT CALLBACK AudioSettingsDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* d = reinterpret_cast<AudioSettingsDlgData*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_CREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        d = reinterpret_cast<AudioSettingsDlgData*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(d));
        UiState& state = *d->appState;

        const HFONT hFont = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        const HINSTANCE hInst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(hwnd, GWLP_HINSTANCE));

        auto makeLabel = [&](const wchar_t* text, int x, int y, int w, int h) {
            HWND hc = CreateWindowExW(0, L"STATIC", text,
                WS_CHILD | WS_VISIBLE | SS_LEFT,
                x, y, w, h, hwnd, nullptr, hInst, nullptr);
            SendMessageW(hc, WM_SETFONT, reinterpret_cast<WPARAM>(hFont), TRUE);
        };
        auto makeCombo = [&](int id, int x, int y, int w, int dropH) -> HWND {
            HWND hc = CreateWindowExW(WS_EX_CLIENTEDGE, L"COMBOBOX", nullptr,
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                x, y, w, dropH, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), hInst, nullptr);
            SendMessageW(hc, WM_SETFONT, reinterpret_cast<WPARAM>(hFont), TRUE);
            return hc;
        };
        auto makeButton = [&](const wchar_t* text, int id, int x, int y, int w, int h) {
            HWND hc = CreateWindowExW(0, L"BUTTON", text,
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                x, y, w, h, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), hInst, nullptr);
            SendMessageW(hc, WM_SETFONT, reinterpret_cast<WPARAM>(hFont), TRUE);
        };

        const int LX = 12, CX = 148, CW = 308, ROW_H = 22, GAP = 36;
        int y = 14;
        makeLabel(L"Backend:",        LX, y + 2, 130, ROW_H); makeCombo(kAsDlgBackend,    CX, y, CW, 120);  y += GAP;
        makeLabel(L"Output Device:",  LX, y + 2, 130, ROW_H); makeCombo(kAsDlgOutputDev,  CX, y, CW, 200);  y += GAP;
        makeLabel(L"Input Device:",   LX, y + 2, 130, ROW_H); makeCombo(kAsDlgInputDev,   CX, y, CW, 200);  y += GAP;
        makeLabel(L"Sample Rate:",    LX, y + 2, 130, ROW_H); makeCombo(kAsDlgSampleRate, CX, y, CW, 180);  y += GAP;
        makeLabel(L"Buffer Size:",    LX, y + 2, 130, ROW_H); makeCombo(kAsDlgBufferSize, CX, y, CW, 180);  y += GAP + 6;

        // Horizontal separator
        CreateWindowExW(0, L"STATIC", nullptr,
            WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ,
            LX, y, 452, 2, hwnd, nullptr, hInst, nullptr);
        y += 10;

        // Status line
        HWND hStatus = CreateWindowExW(0, L"STATIC", L"Active: \u2014",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            LX, y, 452, ROW_H, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAsDlgStatus)), hInst, nullptr);
        SendMessageW(hStatus, WM_SETFONT, reinterpret_cast<WPARAM>(hFont), TRUE);
        y += 26;

        // Note
        HWND hNote = CreateWindowExW(0, L"STATIC",
            L"Note: Changes take effect on next Play or Record.",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            LX, y, 452, ROW_H, hwnd, nullptr, hInst, nullptr);
        SendMessageW(hNote, WM_SETFONT, reinterpret_cast<WPARAM>(hFont), TRUE);
        y += 34;

        // Buttons: Apply left, Cancel + OK right
        makeButton(L"Apply",  kAsDlgApply, LX,  y, 82, 26);
        makeButton(L"Cancel", IDCANCEL,   270,  y, 90, 26);
        makeButton(L"OK",     IDOK,       368,  y, 90, 26);

        // ---- Populate combos ----
        // Backend
        HWND hBe = GetDlgItem(hwnd, kAsDlgBackend);
        SendMessageW(hBe, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"MME (Legacy)"));
        SendMessageW(hBe, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"WASAPI Shared"));
        SendMessageW(hBe, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"WASAPI Exclusive"));
        int beIdx = 1;
        if (state.audioBackend == AudioBackend::MME)              beIdx = 0;
        else if (state.audioBackend == AudioBackend::WasapiExclusive) beIdx = 2;
        SendMessageW(hBe, CB_SETCURSEL, beIdx, 0);

        // Output Device
        HWND hOut = GetDlgItem(hwnd, kAsDlgOutputDev);
        int selOut = 0;
        for (size_t i = 0; i < state.outputDeviceNames.size(); ++i) {
            SendMessageW(hOut, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(state.outputDeviceNames[i].c_str()));
            if (state.outputDeviceIds[i] == state.selectedOutputDeviceId) selOut = static_cast<int>(i);
        }
        if (!state.outputDeviceNames.empty()) SendMessageW(hOut, CB_SETCURSEL, selOut, 0);

        // Input Device
        HWND hIn = GetDlgItem(hwnd, kAsDlgInputDev);
        int selIn = 0;
        for (size_t i = 0; i < state.inputDeviceNames.size(); ++i) {
            SendMessageW(hIn, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(state.inputDeviceNames[i].c_str()));
            if (state.inputDeviceIds[i] == state.selectedInputDeviceId) selIn = static_cast<int>(i);
        }
        if (!state.inputDeviceNames.empty()) SendMessageW(hIn, CB_SETCURSEL, selIn, 0);

        // Sample Rate
        {
            const int stdRates[] = {22050, 44100, 48000, 88200, 96000, 176400, 192000};
            for (int r : stdRates) d->sampleRates.push_back(r);
            auto addSR = [&](int sr) {
                if (sr > 0 && std::find(d->sampleRates.begin(), d->sampleRates.end(), sr) == d->sampleRates.end())
                    d->sampleRates.push_back(sr);
            };
            addSR(state.preferredSampleRate);
            addSR(state.project.projectSampleRate);
            addSR(state.activeDeviceSampleRate);
            std::sort(d->sampleRates.begin(), d->sampleRates.end());
            HWND hSr = GetDlgItem(hwnd, kAsDlgSampleRate);
            int selSR = 1; // default 44100
            for (size_t i = 0; i < d->sampleRates.size(); ++i) {
                wchar_t rbuf[32]{};
                swprintf_s(rbuf, L"%d Hz", d->sampleRates[i]);
                SendMessageW(hSr, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(rbuf));
                const int target = (state.preferredSampleRate > 0) ? state.preferredSampleRate : state.project.projectSampleRate;
                if (d->sampleRates[i] == target) selSR = static_cast<int>(i);
            }
            SendMessageW(hSr, CB_SETCURSEL, selSR, 0);
        }

        // Buffer Size
        {
            const int bufOpts[] = {64, 128, 256, 512, 1024, 2048};
            for (int b : bufOpts) d->bufferSizes.push_back(b);
            HWND hBuf = GetDlgItem(hwnd, kAsDlgBufferSize);
            int selBuf = 2; // default 256
            for (size_t i = 0; i < d->bufferSizes.size(); ++i) {
                wchar_t rbuf[32]{};
                swprintf_s(rbuf, L"%d frames", d->bufferSizes[i]);
                SendMessageW(hBuf, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(rbuf));
                if (d->bufferSizes[i] == state.preferredBufferFrames) selBuf = static_cast<int>(i);
            }
            SendMessageW(hBuf, CB_SETCURSEL, selBuf, 0);
        }

        AsDlgUpdateStatus(hwnd, state);
        return 0;
    }
    case WM_COMMAND: {
        if (d == nullptr) return 0;
        const int ctrlId = LOWORD(wParam);
        if (ctrlId == IDOK) {
            AsDlgReadFields(hwnd, *d);
            DestroyWindow(hwnd);
        } else if (ctrlId == IDCANCEL) {
            UiState& state = *d->appState;
            state.audioBackend            = d->origBackend;
            state.preferredSampleRate     = d->origSampleRate;
            state.preferredBufferFrames   = d->origBufferFrames;
            state.selectedOutputDeviceId   = d->origOutputDeviceId;
            state.selectedOutputDeviceName = d->origOutputDeviceName;
            state.selectedInputDeviceId    = d->origInputDeviceId;
            state.selectedInputDeviceName  = d->origInputDeviceName;
            DestroyWindow(hwnd);
        } else if (ctrlId == kAsDlgApply) {
            AsDlgReadFields(hwnd, *d);
            AsDlgUpdateStatus(hwnd, *d->appState);
        }
        return 0;
    }
    case WM_CLOSE: {
        // Treat X as Cancel
        if (d != nullptr) {
            UiState& state = *d->appState;
            state.audioBackend            = d->origBackend;
            state.preferredSampleRate     = d->origSampleRate;
            state.preferredBufferFrames   = d->origBufferFrames;
            state.selectedOutputDeviceId   = d->origOutputDeviceId;
            state.selectedOutputDeviceName = d->origOutputDeviceName;
            state.selectedInputDeviceId    = d->origInputDeviceId;
            state.selectedInputDeviceName  = d->origInputDeviceName;
        }
        DestroyWindow(hwnd);
        return 0;
    }
    case WM_DESTROY:
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void ShowAudioSettingsDialog(HWND hwndParent, UiState& state) {
    DeviceRefreshInputDevices(state);
    DeviceRefreshOutputDevices(state);

    AudioSettingsDlgData dlgData;
    dlgData.appState             = &state;
    dlgData.origBackend          = state.audioBackend;
    dlgData.origSampleRate       = state.preferredSampleRate;
    dlgData.origBufferFrames     = state.preferredBufferFrames;
    dlgData.origOutputDeviceId   = state.selectedOutputDeviceId;
    dlgData.origOutputDeviceName = state.selectedOutputDeviceName;
    dlgData.origInputDeviceId    = state.selectedInputDeviceId;
    dlgData.origInputDeviceName  = state.selectedInputDeviceName;

    // Register window class once
    static bool sClassRegistered = false;
    if (!sClassRegistered) {
        WNDCLASSEXW wc{};
        wc.cbSize        = sizeof(wc);
        wc.style         = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc   = AudioSettingsDlgProc;
        wc.hInstance     = GetModuleHandleW(nullptr);
        wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"DawAudioSettingsDlg";
        if (RegisterClassExW(&wc)) sClassRegistered = true;
    }

    // Client area size
    const int DLG_W = 470, DLG_H = 290;
    RECT wr{0, 0, DLG_W, DLG_H};
    AdjustWindowRectEx(&wr, WS_POPUP | WS_CAPTION | WS_SYSMENU, FALSE, 0);
    const int ww = wr.right - wr.left;
    const int wh = wr.bottom - wr.top;

    // Center on parent
    RECT parentRect{};
    GetWindowRect(hwndParent, &parentRect);
    const int cx = (parentRect.left + parentRect.right)  / 2;
    const int cy = (parentRect.top  + parentRect.bottom) / 2;

    HWND hwndDlg = CreateWindowExW(
        WS_EX_DLGMODALFRAME,
        L"DawAudioSettingsDlg",
        L"Audio Settings",
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        cx - ww / 2, cy - wh / 2, ww, wh,
        hwndParent, nullptr, GetModuleHandleW(nullptr),
        &dlgData);
    if (hwndDlg == nullptr) return;

    ShowWindow(hwndDlg, SW_SHOW);
    UpdateWindow(hwndDlg);
    EnableWindow(hwndParent, FALSE);

    MSG msg{};
    while (IsWindow(hwndDlg)) {
        const BOOL got = GetMessageW(&msg, nullptr, 0, 0);
        if (got == 0 || got == -1) break;
        if (!IsDialogMessageW(hwndDlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    EnableWindow(hwndParent, TRUE);
    SetForegroundWindow(hwndParent);
    InvalidateRect(hwndParent, nullptr, FALSE);
}

void ShowTopMenu(HWND hwnd, UiState& state, int menuKind, const RECT& menuRect) {
    HMENU menu = CreatePopupMenu();
    if (menu == nullptr) {
        return;
    }

    if (menuKind == 0) {
        AppendMenuW(menu, MF_STRING, kCmdFileOpen,      L"Open Project...\tCtrl+O");
        AppendMenuW(menu, MF_STRING, kCmdFileSave,      L"Save Project\tCtrl+S");
        AppendMenuW(menu, MF_STRING, kCmdFileSaveAs,    L"Save Project As...");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kCmdFileImportWav, L"Import WAV...\tI");
        AppendMenuW(menu, MF_STRING, kCmdFileExportWav,  L"Export Mix as WAV...");
        AppendMenuW(menu, MF_STRING, kCmdAutoMaster,     L"Auto Master...");
        AppendMenuW(menu, MF_STRING, kCmdMixReadiness,    L"Mix Readiness Check...");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kCmdFileExit, L"Exit");
    } else if (menuKind == 1) {
        AppendMenuW(menu, MF_STRING, kCmdViewZoomIn, L"Zoom In");
        AppendMenuW(menu, MF_STRING, kCmdViewZoomOut, L"Zoom Out");
        AppendMenuW(menu, MF_STRING, kCmdViewReset, L"Reset View");
    } else if (menuKind == 2) {
        DeviceRefreshInputDevices(state);
        DeviceRefreshOutputDevices(state);
        HMENU inputSub = CreatePopupMenu();
        if (inputSub != nullptr) {
            for (size_t i = 0; i < state.inputDeviceNames.size(); ++i) {
                const UINT cmdId = kCmdAudioInputBase + static_cast<UINT>(i);
                UINT flags = MF_STRING;
                if (state.inputDeviceIds[i] == state.selectedInputDeviceId) {
                    flags |= MF_CHECKED;
                }
                AppendMenuW(inputSub, flags, cmdId, state.inputDeviceNames[i].c_str());
            }
            AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(inputSub), L"Input Device");
        }
        HMENU outputSub = CreatePopupMenu();
        if (outputSub != nullptr) {
            for (size_t i = 0; i < state.outputDeviceNames.size(); ++i) {
                const UINT cmdId = kCmdAudioOutputBase + static_cast<UINT>(i);
                UINT flags = MF_STRING;
                if (state.outputDeviceIds[i] == state.selectedOutputDeviceId) {
                    flags |= MF_CHECKED;
                }
                AppendMenuW(outputSub, flags, cmdId, state.outputDeviceNames[i].c_str());
            }
            AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(outputSub), L"Output Device");
        }
        HMENU backendSub = CreatePopupMenu();
        if (backendSub != nullptr) {
            AppendMenuW(backendSub, MF_STRING | (state.audioBackend == AudioBackend::MME ? MF_CHECKED : 0), kCmdAudioBackendMME, L"MME (Legacy)");
            AppendMenuW(backendSub, MF_STRING | (state.audioBackend == AudioBackend::WasapiShared ? MF_CHECKED : 0), kCmdAudioBackendWasapiShared, L"WASAPI Shared (Default devices)");
            AppendMenuW(backendSub, MF_STRING | (state.audioBackend == AudioBackend::WasapiExclusive ? MF_CHECKED : 0), kCmdAudioBackendWasapiExclusive, L"WASAPI Exclusive");
            AppendMenuW(backendSub, MF_STRING | MF_GRAYED | (state.audioBackend == AudioBackend::Asio ? MF_CHECKED : 0), kCmdAudioBackendAsio, L"ASIO (Future)");
            AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(backendSub), L"Backend");
        }

        HMENU sampleRateSub = CreatePopupMenu();
        if (sampleRateSub != nullptr) {
            std::vector<int> sampleRates;
            auto addRate = [&](int sr) {
                if (sr > 0 && std::find(sampleRates.begin(), sampleRates.end(), sr) == sampleRates.end()) {
                    sampleRates.push_back(sr);
                }
            };
            addRate(state.preferredSampleRate);
            addRate(state.project.projectSampleRate);
            addRate(state.activeDeviceSampleRate);
            addRate(state.lastOpenedOutputSampleRate);
            for (const LoadedAudio& a : state.project.audio) {
                addRate(a.sampleRate);
            }
            std::sort(sampleRates.begin(), sampleRates.end());
            if (sampleRates.empty()) {
                AppendMenuW(sampleRateSub, MF_STRING | MF_GRAYED, kCmdAudioSampleRateBase, L"No sample rates available yet");
            } else {
                for (size_t i = 0; i < sampleRates.size(); ++i) {
                    const int sr = sampleRates[i];
                    const UINT cmdId = kCmdAudioSampleRateBase + static_cast<UINT>(i);
                    wchar_t label[64]{};
                    swprintf_s(label, L"%d Hz", sr);
                    const UINT flags = MF_STRING | ((state.preferredSampleRate == sr) ? MF_CHECKED : 0);
                    AppendMenuW(sampleRateSub, flags, cmdId, label);
                }
            }
            AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(sampleRateSub), L"Sample Rate");
        }

        HMENU bufferSub = CreatePopupMenu();
        if (bufferSub != nullptr) {
            const int bufferOptions[] = {64, 128, 256, 512, 1024, 2048};
            for (size_t i = 0; i < std::size(bufferOptions); ++i) {
                const int frames = bufferOptions[i];
                const UINT cmdId = kCmdAudioBufferSizeBase + static_cast<UINT>(i);
                wchar_t label[64]{};
                swprintf_s(label, L"%d frames", frames);
                const UINT flags = MF_STRING | ((state.preferredBufferFrames == frames) ? MF_CHECKED : 0);
                AppendMenuW(bufferSub, flags, cmdId, label);
            }
            AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(bufferSub), L"Buffer Size");
        }

        AppendMenuW(menu, MF_STRING, kCmdAudioRefreshInputs, L"Refresh Inputs");
        AppendMenuW(menu, MF_STRING, kCmdAudioDiagnostics, L"Audio Diagnostics...");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kCmdAudioSettings, L"Audio Settings...");
    } else {
        AppendMenuW(menu, MF_STRING, kCmdTrackNew, L"New Track");
    }

    POINT p{menuRect.left, menuRect.bottom};
    ClientToScreen(hwnd, &p);
    const UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, p.x, p.y, 0, hwnd, nullptr);

    if (cmd == kCmdFileOpen) {
        DoOpen(hwnd, state);
        if (state.trackInsertDspState.size() != state.project.tracks.size()) state.trackInsertDspState.resize(state.project.tracks.size());
    } else if (cmd == kCmdFileSave) {
        DoSave(hwnd, state);
    } else if (cmd == kCmdFileSaveAs) {
        DoSaveAs(hwnd, state);
    } else if (cmd == kCmdFileImportWav) {
        ImportWavFiles(hwnd, state);
        state.projectModified = true;
        UpdateWindowTitle(hwnd, state);
    } else if (cmd == kCmdFileExportWav) {
        DoExportMix(hwnd, state);
    } else if (cmd == kCmdAutoMaster) {
        DoAutoMaster(hwnd, state);
    } else if (cmd == kCmdMixReadiness) {
        DoMixReadiness(hwnd, state);
    } else if (cmd == kCmdFileExit) {
        PostMessage(hwnd, WM_CLOSE, 0, 0);
    } else if (cmd == kCmdViewZoomIn) {
        state.viewBeatsVisible = std::max(4.0f, state.viewBeatsVisible * 0.85f);
    } else if (cmd == kCmdViewZoomOut) {
        state.viewBeatsVisible = std::min(128.0f, state.viewBeatsVisible * 1.15f);
    } else if (cmd == kCmdViewReset) {
        state.viewStartBeat = 0.0f;
        state.viewBeatsVisible = 32.0f;
    } else if (cmd == kCmdAudioRefreshInputs) {
        DeviceRefreshInputDevices(state);
        DeviceRefreshOutputDevices(state);
    } else if (cmd == kCmdAudioDiagnostics) {
        const std::wstring diag = DeviceBuildAudioDiagnosticsReport(state);
        MessageBoxW(hwnd, diag.c_str(), L"Audio Diagnostics", MB_OK | MB_ICONINFORMATION);
    } else if (cmd == kCmdAudioSettings) {
        ShowAudioSettingsDialog(hwnd, state);
    } else if (cmd == kCmdAudioBackendMME) {
        state.audioBackend = AudioBackend::MME;
    } else if (cmd == kCmdAudioBackendWasapiShared) {
        state.audioBackend = AudioBackend::WasapiShared;
    } else if (cmd == kCmdAudioBackendWasapiExclusive) {
        state.audioBackend = AudioBackend::WasapiExclusive;
    } else if (cmd == kCmdAudioBackendAsio) {
        MessageBoxW(hwnd, L"ASIO backend is planned but not implemented yet.", L"Audio Backend", MB_OK | MB_ICONINFORMATION);
    } else if (cmd >= kCmdAudioSampleRateBase && cmd < kCmdAudioSampleRateBase + 64) {
        std::vector<int> sampleRates;
        auto addRate = [&](int sr) {
            if (sr > 0 && std::find(sampleRates.begin(), sampleRates.end(), sr) == sampleRates.end()) {
                sampleRates.push_back(sr);
            }
        };
        addRate(state.preferredSampleRate);
        addRate(state.project.projectSampleRate);
        addRate(state.activeDeviceSampleRate);
        addRate(state.lastOpenedOutputSampleRate);
        for (const LoadedAudio& a : state.project.audio) addRate(a.sampleRate);
        std::sort(sampleRates.begin(), sampleRates.end());
        const size_t idx = static_cast<size_t>(cmd - kCmdAudioSampleRateBase);
        if (idx < sampleRates.size()) {
            state.preferredSampleRate = sampleRates[idx];
        }
    } else if (cmd >= kCmdAudioBufferSizeBase && cmd < kCmdAudioBufferSizeBase + 16) {
        const int bufferOptions[] = {64, 128, 256, 512, 1024, 2048};
        const size_t idx = static_cast<size_t>(cmd - kCmdAudioBufferSizeBase);
        if (idx < std::size(bufferOptions)) {
            state.preferredBufferFrames = bufferOptions[idx];
        }
    } else if (cmd == kCmdTrackNew) {
        AddNewTrack(state);
    } else if (cmd >= kCmdAudioInputBase && cmd < kCmdAudioInputBase + static_cast<UINT>(state.inputDeviceIds.size())) {
        const size_t idx = static_cast<size_t>(cmd - kCmdAudioInputBase);
        state.selectedInputDeviceId = state.inputDeviceIds[idx];
        state.selectedInputDeviceName = state.inputDeviceNames[idx];
    } else if (cmd >= kCmdAudioOutputBase && cmd < kCmdAudioOutputBase + static_cast<UINT>(state.outputDeviceIds.size())) {
        const size_t idx = static_cast<size_t>(cmd - kCmdAudioOutputBase);
        state.selectedOutputDeviceId = state.outputDeviceIds[idx];
        state.selectedOutputDeviceName = state.outputDeviceNames[idx];
    }

    DestroyMenu(menu);
    InvalidateRect(hwnd, nullptr, FALSE);
}


static std::wstring PickSingleWavFile(HWND hwnd, const wchar_t* title) {
    wchar_t filePath[MAX_PATH] = {};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = L"WAV Files (*.wav)\0*.wav\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = filePath;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&ofn)) return L"";
    return filePath;
}

static bool ChooseAutoMasterSettings(HWND hwnd, float* outTargetLufs, float* outCeilingDb, float* outWidth) {
    if (outTargetLufs == nullptr || outCeilingDb == nullptr || outWidth == nullptr) {
        return false;
    }

    // LUFS preset
    int r = MessageBoxW(
        hwnd,
        L"Auto Master Loudness Preset\n\nYes = Spotify/YouTube (-14 LUFS)\nNo = More options\nCancel = Abort",
        L"Auto Master Settings",
        MB_YESNOCANCEL | MB_ICONQUESTION);
    if (r == IDCANCEL) return false;
    if (r == IDYES) {
        *outTargetLufs = -14.0f;
    } else {
        r = MessageBoxW(
            hwnd,
            L"Choose alternate loudness\n\nYes = Apple Music (-16 LUFS)\nNo = CD/Offline (-12 LUFS)\nCancel = Abort",
            L"Auto Master Settings",
            MB_YESNOCANCEL | MB_ICONQUESTION);
        if (r == IDCANCEL) return false;
        *outTargetLufs = (r == IDYES) ? -16.0f : -12.0f;
    }

    // Ceiling preset
    r = MessageBoxW(
        hwnd,
        L"True-Peak Ceiling\n\nYes = -1.0 dBFS (streaming-safe)\nNo = More options\nCancel = Abort",
        L"Auto Master Settings",
        MB_YESNOCANCEL | MB_ICONQUESTION);
    if (r == IDCANCEL) return false;
    if (r == IDYES) {
        *outCeilingDb = -1.0f;
    } else {
        r = MessageBoxW(
            hwnd,
            L"Alternate ceiling\n\nYes = -0.3 dBFS (louder)\nNo = -2.0 dBFS (extra headroom)\nCancel = Abort",
            L"Auto Master Settings",
            MB_YESNOCANCEL | MB_ICONQUESTION);
        if (r == IDCANCEL) return false;
        *outCeilingDb = (r == IDYES) ? -0.3f : -2.0f;
    }

    // Width preset
    r = MessageBoxW(
        hwnd,
        L"Stereo Width\n\nYes = 1.15 (wider, recommended)\nNo = More options\nCancel = Abort",
        L"Auto Master Settings",
        MB_YESNOCANCEL | MB_ICONQUESTION);
    if (r == IDCANCEL) return false;
    if (r == IDYES) {
        *outWidth = 1.15f;
    } else {
        r = MessageBoxW(
            hwnd,
            L"Alternate width\n\nYes = 1.00 (keep original)\nNo = 1.25 (extra wide)\nCancel = Abort",
            L"Auto Master Settings",
            MB_YESNOCANCEL | MB_ICONQUESTION);
        if (r == IDCANCEL) return false;
        *outWidth = (r == IDYES) ? 1.00f : 1.25f;
    }

    return true;
}

static bool ReplaceProjectWithSingleWav(UiState& state, const std::wstring& wavPath, std::wstring* outError) {
    LoadedAudio audio{};
    std::wstring error;
    if (!LoadWavStereo(wavPath, &audio, &error)) {
        if (outError) *outError = error;
        return false;
    }

    EnterCriticalSection(&state.audioStateLock);
    state.project.tracks.clear();
    state.project.audio.clear();
    state.project.clips.clear();

    // Reset buses to defaults
    state.project.buses.assign(kBusCount, BusData{});

    state.project.projectSampleRate = audio.sampleRate;

    {
        TrackData t{};
        t.name = audio.displayName;
        t.busIndex = 3; // mastered stereo routes directly to Master
        state.project.tracks.push_back(std::move(t));
    }
        state.project.audio.push_back(std::move(audio));

        const float lengthBeats = BeatsFromFrames(state, state.project.audio.back().frames);
        state.project.clips.push_back(ClipItem{
            0,
            0,
            0.0f,
            std::max(0.25f, lengthBeats),
            kPalette.clip1,
            state.project.tracks.back().name,
        });

    state.selectedTrackIndex = 0;
    state.selectedClipIndex = 0;
    state.playheadBeat = 0.0f;
    state.viewStartBeat = 0.0f;
    state.viewBeatsVisible = std::clamp(std::max(16.0f, lengthBeats + 4.0f), 16.0f, 128.0f);
    state.projectFilePath.clear();
    state.projectModified = true;
    LeaveCriticalSection(&state.audioStateLock);
    return true;
}

bool DoAutoMaster(HWND hwnd, UiState& state) {
    float targetLufs = -14.0f;
    float ceilingDb = -1.0f;
    float width = 1.15f;
    if (!ChooseAutoMasterSettings(hwnd, &targetLufs, &ceilingDb, &width)) {
        return false;
    }

    std::wstring sourceWav;
    {
        EnterCriticalSection(&state.audioStateLock);
        if (!state.project.clips.empty() && state.selectedClipIndex >= 0 && state.selectedClipIndex < static_cast<int>(state.project.clips.size())) {
            const ClipItem& c = state.project.clips[static_cast<size_t>(state.selectedClipIndex)];
            if (c.audioIndex >= 0 && c.audioIndex < static_cast<int>(state.project.audio.size())) {
                sourceWav = state.project.audio[static_cast<size_t>(c.audioIndex)].sourcePath;
            }
        }
        if (sourceWav.empty() && state.project.audio.size() == 1) {
            sourceWav = state.project.audio[0].sourcePath;
        }
        LeaveCriticalSection(&state.audioStateLock);
    }

    if (sourceWav.empty() || !std::filesystem::exists(sourceWav)) {
        sourceWav = PickSingleWavFile(hwnd, L"Auto Master - Select Mix WAV");
        if (sourceWav.empty()) return false;
    }
    if (!std::filesystem::exists(sourceWav)) {
        MessageBoxW(hwnd, L"Selected source WAV does not exist.", L"Auto Master", MB_OK | MB_ICONERROR);
        return false;
    }

    const std::filesystem::path repoRoot = FindRepoRoot();
    if (repoRoot.empty()) {
        MessageBoxW(hwnd, L"Could not locate project root (.venv and src/daw_ai).", L"Auto Master", MB_OK | MB_ICONERROR);
        return false;
    }
    const std::filesystem::path pythonExe = repoRoot / L".venv" / L"Scripts" / L"python.exe";
    if (!std::filesystem::exists(pythonExe)) {
        MessageBoxW(hwnd, L"Python venv executable not found at .venv\\Scripts\\python.exe", L"Auto Master", MB_OK | MB_ICONERROR);
        return false;
    }

    const std::filesystem::path outputDir = repoRoot / L"analysis_out" / L"mastered";
    std::error_code ec;
    std::filesystem::create_directories(outputDir, ec);

    const std::filesystem::path srcPath(sourceWav);
    const std::filesystem::path srcDir = srcPath.parent_path();
    const std::wstring srcName = srcPath.filename().wstring();

    const std::wstring cmd =
        QuoteArg(pythonExe.wstring()) +
        L" -m daw_ai.cli --input-dir " + QuoteArg(srcDir.wstring()) +
        L" --output-dir " + QuoteArg(outputDir.wstring()) +
        L" --select " + QuoteArg(srcName) +
        L" --master --master-input " + QuoteArg(srcPath.wstring()) +
        L" --target-lufs " + std::to_wstring(targetLufs) +
        L" --master-ceiling-db " + std::to_wstring(ceilingDb) +
        L" --master-width " + std::to_wstring(width);

    STARTUPINFOW si{};
    PROCESS_INFORMATION pi{};
    si.cb = sizeof(si);

    std::vector<wchar_t> mutableCmd(cmd.begin(), cmd.end());
    mutableCmd.push_back(L'\0');

    const BOOL ok = CreateProcessW(
        nullptr,
        mutableCmd.data(),
        nullptr,
        nullptr,
        FALSE,
        CREATE_NO_WINDOW,
        nullptr,
        (repoRoot / L"src").wstring().c_str(),
        &si,
        &pi
    );
    if (!ok) {
        MessageBoxW(hwnd, L"Failed to launch Auto Master process.", L"Auto Master", MB_OK | MB_ICONERROR);
        return false;
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    if (exitCode != 0) {
        MessageBoxW(hwnd, L"Auto Master failed. Check Python logs/output.", L"Auto Master", MB_OK | MB_ICONERROR);
        return false;
    }

    const std::filesystem::path masteredPath = outputDir / (srcPath.stem().wstring() + L"_master.wav");
    if (!std::filesystem::exists(masteredPath)) {
        MessageBoxW(hwnd, L"Auto Master completed but mastered WAV was not found.", L"Auto Master", MB_OK | MB_ICONERROR);
        return false;
    }

    std::wstring msg = L"Auto Master complete:\n" + masteredPath.wstring() +
        L"\n\nOpen mastered file in a new empty project?";
    const int choice = MessageBoxW(hwnd, msg.c_str(), L"Auto Master", MB_YESNO | MB_ICONINFORMATION);
    if (choice == IDYES) {
        if (state.recording) {
            MessageBoxW(hwnd, L"Stop recording before loading the mastered file.", L"Auto Master", MB_OK | MB_ICONWARNING);
            return true;
        }
        StopPlayback(state, true);

        std::wstring err;
        if (!ReplaceProjectWithSingleWav(state, masteredPath.wstring(), &err)) {
            const std::wstring em = err.empty() ? L"Failed to load mastered WAV into project." : (L"Failed to load mastered WAV: " + err);
            MessageBoxW(hwnd, em.c_str(), L"Auto Master", MB_OK | MB_ICONERROR);
            return false;
        }
        UpdateWindowTitle(hwnd, state);
        InvalidateRect(hwnd, nullptr, FALSE);
    }

    return true;
}

bool DoMixReadiness(HWND hwnd, UiState& state) {
    if (state.project.tracks.empty() || state.project.clips.empty()) {
        MessageBoxW(hwnd, L"Nothing to analyse - add tracks and clips first.", L"Mix Readiness", MB_OK | MB_ICONINFORMATION);
        return false;
    }

    // Create a temp directory for bus stems
    wchar_t tempBase[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tempBase);
    wchar_t stemsDir[MAX_PATH] = {};
    swprintf_s(stemsDir, L"%sdaw_readiness_%u", tempBase, GetCurrentProcessId());
    std::filesystem::create_directories(stemsDir);

    // Render + write each bus stem
    static const wchar_t* kBusWavNames[kBusCount] = {L"Drums", L"Music", L"Vocals", L"Master"};
    int exported = 0;
    for (int b = 0; b < kBusCount; ++b) {
        std::vector<float> stereo;
        int sr = 0;
        EnterCriticalSection(&state.audioStateLock);
        const bool ok = RenderBusStemToStereoLocked(state, b, &stereo, &sr);
        LeaveCriticalSection(&state.audioStateLock);
        if (!ok || stereo.empty()) continue;
        const std::wstring wavPath = std::wstring(stemsDir) + L"\\" + kBusWavNames[b] + L".wav";
        if (WriteWavPcm16Stereo(wavPath, stereo, sr)) ++exported;
    }

    if (exported == 0) {
        MessageBoxW(hwnd, L"Could not render any bus stems. Check that tracks are assigned to buses and clips are present.", L"Mix Readiness", MB_OK | MB_ICONERROR);
        return false;
    }

    // Locate Python interpreter (.venv next to the executable)
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    const auto exeDir   = std::filesystem::path(exePath).parent_path();
    // Walk up until we find .venv or reach drive root
    std::filesystem::path searchDir = exeDir;
    std::filesystem::path venvPy;
    for (int depth = 0; depth < 8; ++depth) {
        const auto candidate = searchDir / L".venv" / L"Scripts" / L"python.exe";
        if (std::filesystem::exists(candidate)) { venvPy = candidate; break; }
        const auto parent = searchDir.parent_path();
        if (parent == searchDir) break;
        searchDir = parent;
    }
    if (venvPy.empty()) {
        MessageBoxW(hwnd, L"Could not find .venv\\Scripts\\python.exe. Activate the project virtual environment.", L"Mix Readiness", MB_OK | MB_ICONERROR);
        return false;
    }

    // Build command: python -m daw_ai --mix-readiness <stemsDir>
    // We need to set cwd to the project src root so the module is importable
    const std::wstring srcDir = (searchDir / L"src").wstring();
    const std::wstring cmd = L"\"" + venvPy.wstring() + L"\" -m daw_ai --mix-readiness \"" + std::wstring(stemsDir) + L"\"";

    // Run via CreateProcess, capture stdout
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE hReadPipe = nullptr, hWritePipe = nullptr;
    if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) {
        MessageBoxW(hwnd, L"Failed to create output pipe for Python process.", L"Mix Readiness", MB_OK | MB_ICONERROR);
        return false;
    }
    SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hWritePipe;
    si.hStdError  = hWritePipe;
    si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi{};
    std::wstring cmdMutable = cmd;
    const BOOL created = CreateProcessW(
        nullptr, cmdMutable.data(), nullptr, nullptr,
        TRUE, CREATE_NO_WINDOW, nullptr,
        srcDir.c_str(),
        &si, &pi);
    CloseHandle(hWritePipe);

    if (!created) {
        CloseHandle(hReadPipe);
        MessageBoxW(hwnd, L"Failed to launch Python mix-readiness analysis.", L"Mix Readiness", MB_OK | MB_ICONERROR);
        return false;
    }

    // Read output
    std::string output;
    {
        char buf[1024];
        DWORD bytesRead = 0;
        while (ReadFile(hReadPipe, buf, sizeof(buf) - 1, &bytesRead, nullptr) && bytesRead > 0) {
            buf[bytesRead] = '\0';
            output += buf;
        }
    }
    CloseHandle(hReadPipe);
    WaitForSingleObject(pi.hProcess, 10000);
    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    if (output.empty()) {
        MessageBoxW(hwnd, L"Mix Readiness analysis returned no output. Check the Python environment.", L"Mix Readiness", MB_OK | MB_ICONERROR);
        return false;
    }

    // First line is "GATE_PASSED=true" or "GATE_PASSED=false"; rest is the human-readable report.
    const bool gatePassed = (output.find("GATE_PASSED=true") != std::string::npos);

    // Find where the human text starts (after the first newline)
    std::string reportText = output;
    const size_t nl = output.find('\n');
    if (nl != std::string::npos) {
        reportText = output.substr(nl + 1);
    }
    // Strip trailing whitespace
    while (!reportText.empty() && (reportText.back() == '\n' || reportText.back() == '\r' || reportText.back() == ' ')) {
        reportText.pop_back();
    }

    // Convert UTF-8 report text to wide string for MessageBox
    std::wstring msg;
    if (!reportText.empty()) {
        const int needed = MultiByteToWideChar(CP_UTF8, 0, reportText.c_str(), static_cast<int>(reportText.size()), nullptr, 0);
        if (needed > 0) {
            msg.resize(static_cast<size_t>(needed));
            MultiByteToWideChar(CP_UTF8, 0, reportText.c_str(), static_cast<int>(reportText.size()), msg.data(), needed);
        }
    }
    if (msg.empty()) {
        msg = L"(no report text received)";
    }

    const wchar_t* title = gatePassed ? L"Mix Readiness - PASSED" : L"Mix Readiness - NOT PASSED";
    MessageBoxW(hwnd, msg.c_str(), title, MB_OK | (gatePassed ? MB_ICONINFORMATION : MB_ICONWARNING));

    // Clean up temp stems
    std::filesystem::remove_all(stemsDir);
    return gatePassed;
}

bool DoExportMix(HWND hwnd, UiState& state) {
    if (state.project.tracks.empty() || state.project.clips.empty()) {
        MessageBoxW(hwnd, L"Nothing to export -- add some tracks and clips first.", L"Export Mix", MB_OK | MB_ICONINFORMATION);
        return false;
    }

    // Prompt for output file
    wchar_t filePath[MAX_PATH] = {};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = hwnd;
    ofn.lpstrFilter = L"WAV Audio (*.wav)\0*.wav\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile   = filePath;
    ofn.nMaxFile    = MAX_PATH;
    ofn.lpstrTitle  = L"Export Mix as WAV";
    ofn.Flags       = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    ofn.lpstrDefExt = L"wav";

    // Suggest a default filename next to the project file
    if (!state.projectFilePath.empty()) {
        const auto stem = std::filesystem::path(state.projectFilePath).stem().wstring();
        const auto dir  = std::filesystem::path(state.projectFilePath).parent_path().wstring();
        const std::wstring suggested = dir + L"\\" + stem + L"_mix.wav";
        wcsncpy_s(filePath, MAX_PATH, suggested.c_str(), _TRUNCATE);
        ofn.lpstrInitialDir = dir.c_str();
    }

    if (!GetSaveFileNameW(&ofn)) {
        return false;  // User cancelled
    }

    // Render
    std::vector<float> stereo;
    int sampleRate = 0;
    EnterCriticalSection(&state.audioStateLock);
    const bool ok = RenderFullMixToStereoLocked(state, &stereo, &sampleRate);
    LeaveCriticalSection(&state.audioStateLock);

    if (!ok || stereo.empty()) {
        MessageBoxW(hwnd, L"Render failed - no audio could be mixed.", L"Export Mix", MB_OK | MB_ICONERROR);
        return false;
    }

    if (!WriteWavPcm16Stereo(filePath, stereo, sampleRate)) {
        MessageBoxW(hwnd, L"Could not write WAV file. Check the output path.", L"Export Mix", MB_OK | MB_ICONERROR);
        return false;
    }

    // Report duration
    const double durationSec = static_cast<double>(stereo.size() / 2) / static_cast<double>(sampleRate);
    wchar_t msg[256] = {};
    swprintf_s(msg, L"Mix exported successfully.\n\n%s\n\nDuration: %.1f seconds, %d Hz",
               filePath, durationSec, sampleRate);
    MessageBoxW(hwnd, msg, L"Export Mix", MB_OK | MB_ICONINFORMATION);
    return true;
}

std::vector<std::wstring> PickWavFiles(HWND hwnd) {
    wchar_t buffer[65536] = {};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = L"WAV Files (*.wav)\0*.wav\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = buffer;
    ofn.nMaxFile = static_cast<DWORD>(std::size(buffer));
    ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_ALLOWMULTISELECT;

    if (!GetOpenFileNameW(&ofn)) {
        return {};
    }

    std::vector<std::wstring> result;
    const std::wstring first = buffer;
    const wchar_t* p = buffer + first.size() + 1;

    if (*p == L'\0') {
        result.push_back(first);
        return result;
    }

    const std::wstring dir = first;
    while (*p != L'\0') {
        std::wstring file = p;
        result.push_back(dir + L"\\" + file);
        p += file.size() + 1;
    }

    return result;
}

void ImportWavFiles(HWND hwnd, UiState& state) {
    const std::vector<std::wstring> files = PickWavFiles(hwnd);
    if (files.empty()) {
        return;
    }

    const COLORREF clipColors[4] = {kPalette.clip1, kPalette.clip2, kPalette.clip3, kPalette.clip4};
    std::wstring skipped;

    for (const std::wstring& path : files) {
        LoadedAudio audio{};
        std::wstring error;
        if (!LoadWavStereo(path, &audio, &error)) {
            skipped += std::filesystem::path(path).filename().wstring() + L": " + error + L"\n";
            continue;
        }

        if (state.project.projectSampleRate == 0) {
            state.project.projectSampleRate = audio.sampleRate;
        } else if (audio.sampleRate != state.project.projectSampleRate) {
            skipped += std::filesystem::path(path).filename().wstring() + L": sample rate mismatch\n";
            continue;
        }

        const int trackIndex = static_cast<int>(state.project.tracks.size());
        const int audioIndex = static_cast<int>(state.project.audio.size());

        EnterCriticalSection(&state.audioStateLock);
        {
            TrackData t{};
            t.name = audio.displayName;
            t.busIndex = 1;
            state.project.tracks.push_back(std::move(t));
        }
        state.project.audio.push_back(std::move(audio));

        const float lengthBeats = BeatsFromFrames(state, state.project.audio.back().frames);
        state.project.clips.push_back(ClipItem{
            trackIndex,
            audioIndex,
            0.0f,
            std::max(0.25f, lengthBeats),
            clipColors[trackIndex % 4],
            state.project.tracks.back().name,
        });
        LeaveCriticalSection(&state.audioStateLock);
    }

    if (!state.project.clips.empty()) {
        float endBeat = 0.0f;
        EnterCriticalSection(&state.audioStateLock);
        for (const ClipItem& clip : state.project.clips) {
            endBeat = std::max(endBeat, clip.startBeat + clip.lengthBeats);
        }
        LeaveCriticalSection(&state.audioStateLock);
        state.viewStartBeat = 0.0f;
        state.viewBeatsVisible = std::clamp(std::max(16.0f, endBeat + 4.0f), 16.0f, 128.0f);
    }

    if (!skipped.empty()) {
        MessageBoxW(hwnd, skipped.c_str(), L"Some files were skipped", MB_OK | MB_ICONWARNING);
    }
}

// ── Audio orchestration layer ─────────────────────────────────────────────────
// These functions coordinate both the MME and WASAPI backends.

void StopPlayback(UiState& state, bool rewind) {
    DeviceStopPlaybackBackend(state);

    state.playing = false;
    state.audioThreadRunning.store(false);
    if (rewind) {
        state.playheadBeat = 0.0f;
        state.playbackFrameCursor.store(0);
    }
}

void StopRecording(UiState& state, bool commitTake) {
    if (!state.recording && state.recordThread == nullptr) {
        return;
    }

    DeviceStopRecordingBackend(state);

    if (commitTake && state.recordTrackIndex >= 0 && !state.recordedInputPcm.empty()) {
        const int channels = std::max(1, state.recordInputChannels);
        const std::uint32_t totalFrames = static_cast<std::uint32_t>(state.recordedInputPcm.size() / static_cast<size_t>(channels));
        const ULONGLONG nowTick = GetTickCount64();
        const ULONGLONG elapsedMsUll = (state.recordCaptureStartTickMs > 0 && nowTick > state.recordCaptureStartTickMs)
            ? (nowTick - state.recordCaptureStartTickMs)
            : 0ULL;
        const int elapsedMs = static_cast<int>(std::min<ULONGLONG>(elapsedMsUll, static_cast<ULONGLONG>(std::numeric_limits<int>::max())));
        const int captureSampleRate = (state.lastOpenedInputSampleRate > 0)
            ? state.lastOpenedInputSampleRate
            : state.project.projectSampleRate;
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

        state.lastCaptureElapsedMs = elapsedMs;
        state.lastCaptureObservedRateRatio = observedRatio;
        state.lastCaptureFrameStride = frameStride;

        const std::uint32_t skipFramesRaw = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(state.recordPrerollFrames * static_cast<std::uint64_t>(frameStride), totalFrames));
        const std::uint32_t frames = (totalFrames > skipFramesRaw)
            ? ((totalFrames - skipFramesRaw) / static_cast<std::uint32_t>(frameStride))
            : 0;

        if (frames > 0) {
            std::vector<float> stereo(static_cast<size_t>(frames) * 2, 0.0f);
            for (std::uint32_t f = 0; f < frames; ++f) {
                float l = 0.0f;
                float r = 0.0f;
                const std::uint32_t srcFrame = skipFramesRaw + f * static_cast<std::uint32_t>(frameStride);
                if (channels == 1) {
                    const float v = static_cast<float>(state.recordedInputPcm[static_cast<size_t>(srcFrame)]) / 32768.0f;
                    l = v;
                    r = v;
                } else {
                    const size_t base = static_cast<size_t>(srcFrame) * static_cast<size_t>(channels);
                    l = static_cast<float>(state.recordedInputPcm[base]) / 32768.0f;
                    r = static_cast<float>(state.recordedInputPcm[base + 1]) / 32768.0f;
                }
                stereo[static_cast<size_t>(f) * 2] = l;
                stereo[static_cast<size_t>(f) * 2 + 1] = r;
            }

            LoadedAudio take{};
            take.sourcePath = L"[recording]";
            take.displayName = L"Take " + std::to_wstring(static_cast<int>(state.project.audio.size()) + 1);
            take.sampleRate = captureSampleRate;
            take.frames = frames;
            take.stereo = std::move(stereo);

            state.lastCommittedTakeSampleRate = take.sampleRate;
            state.lastCommittedTakeFrames = static_cast<int>(take.frames);
            state.lastCommittedTakeChannels = 2;

            const COLORREF clipColors[4] = {kPalette.clip1, kPalette.clip2, kPalette.clip3, kPalette.clip4};
            EnterCriticalSection(&state.audioStateLock);
            const int audioIndex = static_cast<int>(state.project.audio.size());
            state.project.audio.push_back(std::move(take));

            const float startBeat = BeatsFromFrames(state, state.recordStartFrame);
            const float lengthBeats = BeatsFromFrames(state, frames);

            if (state.recordTrackIndex >= 0 && state.recordTrackIndex < static_cast<int>(state.project.tracks.size())) {
                state.project.clips.push_back(ClipItem{
                    state.recordTrackIndex,
                    audioIndex,
                    startBeat,
                    std::max(0.25f, lengthBeats),
                    clipColors[state.recordTrackIndex % 4],
                    state.project.tracks[static_cast<size_t>(state.recordTrackIndex)].name + L" Rec",
                });
                state.projectModified = true;
                if (state.hwnd) UpdateWindowTitle(state.hwnd, state);
            }
            LeaveCriticalSection(&state.audioStateLock);
        }
    }

    state.recordedInputPcm.clear();
    state.monitorInputPcm.clear();
    state.monitorInputReadPos = 0;
    state.recordInputChannels = 0;
    state.recordTrackIndex = -1;
    state.recordCaptureStartTickMs = 0;
    state.recordStartFrame = 0;
    state.recordPrerollFrames = 0;
    state.countingIn = false;
    state.recordUsingWasapi = false;
    state.recordInitState.store(0);
    state.recording = false;
}

bool StartRecording(HWND hwnd, UiState& state) {
    if (state.recording) {
        return true;
    }

    int armedTrack = -1;
    for (size_t i = 0; i < state.project.tracks.size(); ++i) {
        if (state.project.tracks[i].recordArm) {
            armedTrack = static_cast<int>(i);
            break;
        }
    }
    if (armedTrack < 0) {
        MessageBoxW(hwnd, L"Arm a track first using the R button.", L"Record", MB_OK | MB_ICONINFORMATION);
        return false;
    }

    const bool wasPlaying = state.playing;

    if (!state.playing && (!state.project.clips.empty() || state.metronomeRecord || state.countInEnabled)) {
        if (!StartPlayback(hwnd, state)) {
            return false;
        }
    }

    return DeviceStartRecordingBackend(hwnd, state, armedTrack, wasPlaying);
}

bool StartPlayback(HWND hwnd, UiState& state) {
    state.lastPlaybackInitError.clear();

    DeviceRefreshOutputDevices(state);
    if (state.outputDeviceIds.empty()) {
        MessageBoxW(hwnd, L"No audio output devices detected.", L"Playback error", MB_OK | MB_ICONERROR);
        return false;
    }

    if (state.project.clips.empty() && !state.metronomePlay && !state.metronomeRecord && !state.countInEnabled) {
        MessageBoxW(hwnd, L"Import at least one supported WAV file first.", L"No audio to play", MB_OK | MB_ICONINFORMATION);
        return false;
    }

    StopPlayback(state, false);

    return DeviceStartPlaybackBackend(hwnd, state);
}


LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<UiState*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));

    switch (msg) {
    case WM_CREATE: {
        auto* initial = new UiState();
        initial->hwnd = hwnd;
        DeviceRefreshInputDevices(*initial);
        DeviceRefreshOutputDevices(*initial);
        InitializeCriticalSection(&initial->audioStateLock);
        SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(initial));
        SetTimer(hwnd, kPlaybackTimerId, kPlaybackTimerMs, nullptr);
        return 0;
    }
    case WM_DESTROY:
        KillTimer(hwnd, kPlaybackTimerId);
        if (state != nullptr) {
            StopRecording(*state, false);
            if (state->automixThread != nullptr) {
                WaitForSingleObject(state->automixThread, INFINITE);
                CloseHandle(state->automixThread);
                state->automixThread = nullptr;
            }
            StopPlayback(*state, false);
            DeleteCriticalSection(&state->audioStateLock);
            delete state;
        }
        PostQuitMessage(0);
        return 0;
    case kMsgPlaybackFinished:
        if (state != nullptr) {
            if (state->recording) {
                StopRecording(*state, true);
            }
            StopPlayback(*state, false);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case kMsgAutoMixFinished:
        if (state != nullptr) {
            if (state->automixThread != nullptr) {
                CloseHandle(state->automixThread);
                state->automixThread = nullptr;
            }
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_TIMER:
        if (state != nullptr && wParam == kPlaybackTimerId && state->playing) {
            const std::uint64_t absoluteFrame = DeviceGetRenderedPlaybackFrame(*state);
            state->playheadBeat = BeatsFromFrames(*state, absoluteFrame);

            const float viewRight = state->viewStartBeat + state->viewBeatsVisible;
            if (state->playheadBeat > viewRight - 1.0f) {
                state->viewStartBeat = state->playheadBeat - (state->viewBeatsVisible * 0.75f);
            }
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_KEYDOWN:
        if (state == nullptr) {
            return 0;
        }
        if (wParam == VK_SPACE) {
            if (state->playing) {
                if (state->recording) {
                    StopRecording(*state, true);
                }
                StopPlayback(*state, false);
            } else {
                StartPlayback(hwnd, *state);
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (wParam == VK_HOME) {
            StopPlayback(*state, true);
            state->viewStartBeat = 0.0f;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (wParam == 'I') {
            ImportWavFiles(hwnd, *state);
            state->projectModified = true;
            UpdateWindowTitle(hwnd, *state);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (wParam == 'O' && (GetKeyState(VK_CONTROL) & 0x8000)) {
            DoOpen(hwnd, *state);
            if (state->trackInsertDspState.size() != state->project.tracks.size()) state->trackInsertDspState.resize(state->project.tracks.size());
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (wParam == 'S' && (GetKeyState(VK_CONTROL) & 0x8000)) {
            if (GetKeyState(VK_SHIFT) & 0x8000) {
                DoSaveAs(hwnd, *state);
            } else {
                DoSave(hwnd, *state);
            }
            return 0;
        }
        if (wParam == 'Z' && (GetKeyState(VK_CONTROL) & 0x8000)) {
            if (state->playing || state->recording) return 0;
            ApplyUndo(hwnd, *state);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if ((wParam == 'Y' && (GetKeyState(VK_CONTROL) & 0x8000)) ||
            (wParam == 'Z' && (GetKeyState(VK_CONTROL) & 0x8000) && (GetKeyState(VK_SHIFT) & 0x8000))) {
            if (state->playing || state->recording) return 0;
            ApplyRedo(hwnd, *state);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (wParam == 'S' && !(GetKeyState(VK_CONTROL) & 0x8000)) {
            // Split selected clip at playhead
            if (state->playing || state->recording) return 0;
            SplitSelectedClip(*state);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (wParam == 'D' && (GetKeyState(VK_CONTROL) & 0x8000)) {
            if (state->playing || state->recording) return 0;
            DuplicateSelectedClip(*state);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (wParam == VK_LEFT && !(GetKeyState(VK_CONTROL) & 0x8000)) {
            if (state->playing || state->recording) return 0;
            NudgeSelectedClip(*state, -0.25f);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (wParam == VK_RIGHT && !(GetKeyState(VK_CONTROL) & 0x8000)) {
            if (state->playing || state->recording) return 0;
            NudgeSelectedClip(*state, 0.25f);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (wParam == 'A') {
            StartAutoMixAsync(hwnd, *state);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (wParam == 'V') {
            AnalyzeSelectedTrackQuality(hwnd, *state);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (wParam == 'R') {
            if (state->recording) {
                StopRecording(*state, true);
                StopPlayback(*state, true);  // rewind so next take starts from beat 0
            } else {
                StartRecording(hwnd, *state);
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (wParam == VK_ESCAPE) {
            if (state->fxInspectorOpen) {
                state->fxInspectorOpen = false;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
        }
        if (wParam == VK_DELETE) {
            if (state->recording || state->playing) {
                MessageBoxW(hwnd, L"Stop playback/recording before deleting.", L"Delete", MB_OK | MB_ICONINFORMATION);
                return 0;
            }

            if (state->selectedClipIndex >= 0 && state->selectedClipIndex < static_cast<int>(state->project.clips.size())) {
                DeleteSelectedClip(*state);
                state->projectModified = true;
                UpdateWindowTitle(hwnd, *state);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }

            if (state->selectedTrackIndex >= 0 && state->selectedTrackIndex < static_cast<int>(state->project.tracks.size())) {
                DeleteTrackAt(*state, state->selectedTrackIndex);
                state->projectModified = true;
                UpdateWindowTitle(hwnd, *state);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        if (wParam == VK_OEM_PLUS || wParam == VK_ADD) {
            state->viewBeatsVisible = std::max(4.0f, state->viewBeatsVisible * 0.85f);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (wParam == VK_OEM_MINUS || wParam == VK_SUBTRACT) {
            state->viewBeatsVisible = std::min(128.0f, state->viewBeatsVisible * 1.15f);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        return 0;
    case WM_LBUTTONDOWN:
        if (state == nullptr) {
            return 0;
        }
        {
            const POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            if (PtInRect(&state->fileMenuRect, pt)) {
                ShowTopMenu(hwnd, *state, 0, state->fileMenuRect);
                return 0;
            }
            if (PtInRect(&state->viewMenuRect, pt)) {
                ShowTopMenu(hwnd, *state, 1, state->viewMenuRect);
                return 0;
            }
            if (PtInRect(&state->audioMenuRect, pt)) {
                ShowTopMenu(hwnd, *state, 2, state->audioMenuRect);
                return 0;
            }
            if (PtInRect(&state->trackMenuRect, pt)) {
                ShowTopMenu(hwnd, *state, 3, state->trackMenuRect);
                return 0;
            }

            if (PtInRect(&state->playRect, pt)) {
                if (state->playing) {
                    StopPlayback(*state, false);
                } else {
                    StartPlayback(hwnd, *state);
                }
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (PtInRect(&state->stopRect, pt)) {
                if (state->recording) {
                    StopRecording(*state, true);
                }
                StopPlayback(*state, true);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (PtInRect(&state->recordRect, pt)) {
                if (state->recording) {
                    StopRecording(*state, true);
                    StopPlayback(*state, true);  // rewind so next take starts from beat 0
                } else {
                    StartRecording(hwnd, *state);
                }
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (PtInRect(&state->importRect, pt)) {
                ImportWavFiles(hwnd, *state);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (PtInRect(&state->automixRect, pt)) {
                StartAutoMixAsync(hwnd, *state);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (PtInRect(&state->vocalCheckRect, pt)) {
                AnalyzeSelectedTrackQuality(hwnd, *state);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (PtInRect(&state->autoMasterRect, pt)) {
                DoAutoMaster(hwnd, *state);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (PtInRect(&state->metPlayRect, pt)) {
                state->metronomePlay = !state->metronomePlay;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (PtInRect(&state->metRecRect, pt)) {
                state->metronomeRecord = !state->metronomeRecord;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (PtInRect(&state->monitorRect, pt)) {
                state->inputMonitoring = !state->inputMonitoring;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (PtInRect(&state->bpmDownRect, pt)) {
                const bool coarse = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                state->project.bpm = static_cast<float>(std::max(40, static_cast<int>(state->project.bpm) - (coarse ? 5 : 1)));
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (PtInRect(&state->bpmUpRect, pt)) {
                const bool coarse = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                state->project.bpm = static_cast<float>(std::min(260, static_cast<int>(state->project.bpm) + (coarse ? 5 : 1)));
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (PtInRect(&state->countInRect, pt)) {
                state->countInEnabled = !state->countInEnabled;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }

            RECT client{};
            GetClientRect(hwnd, &client);
            const LayoutRects layout = UiLayoutComputeLayout(client);

            // ── Insert inspector click handling ──────────────────────────
            if (state->fxInspectorOpen && state->fxInspectorIndex >= 0) {
                const RECT inspPanel = UiDrawGetInspectorPanelRect(client, *state);
                if (!PtInRect(&inspPanel, pt)) {
                    // Click outside → close
                    state->fxInspectorOpen = false;
                    InvalidateRect(hwnd, nullptr, FALSE);
                    // Don't return - let the click fall through to normal handling
                } else {
                    // Click inside inspector - handle controls
                    const int idx = state->fxInspectorIndex;
                    int* pSlots       = nullptr;
                    InsertEffectArray* pEffects = nullptr;
                    InsertBypassArray* pBypass  = nullptr;
                    InsertConfigArray* pParams  = nullptr;
                    if (state->fxInspectorIsTrack) {
                        if (idx < static_cast<int>(state->project.tracks.size()))
                            pSlots = &state->project.tracks[static_cast<size_t>(idx)].insertSlots;
                        if (idx < static_cast<int>(state->project.tracks.size()))
                            pEffects = &state->project.tracks[static_cast<size_t>(idx)].insertEffects;
                        if (idx < static_cast<int>(state->project.tracks.size()))
                            pBypass = &state->project.tracks[static_cast<size_t>(idx)].insertBypass;
                        if (idx < static_cast<int>(state->project.tracks.size()))
                            pParams = &state->project.tracks[static_cast<size_t>(idx)].insertConfig;
                    } else {
                        if (idx < static_cast<int>(state->project.buses.size()))
                            pSlots = &state->project.buses[static_cast<size_t>(idx)].insertSlots;
                        if (idx < static_cast<int>(state->project.buses.size()))
                            pEffects = &state->project.buses[static_cast<size_t>(idx)].insertEffects;
                        if (idx < static_cast<int>(state->project.buses.size()))
                            pBypass = &state->project.buses[static_cast<size_t>(idx)].insertBypass;
                        if (idx < static_cast<int>(state->project.buses.size()))
                            pParams = &state->project.buses[static_cast<size_t>(idx)].insertConfig;
                    }

                    const int slotCount = pSlots ? std::clamp(*pSlots, 0, kMaxInsertSlots) : 0;

                    // Close button
                    RECT closeBtn{inspPanel.right - 24, inspPanel.top + 4, inspPanel.right - 4, inspPanel.top + kUiDrawInspHeaderH - 4};
                    if (PtInRect(&closeBtn, pt)) {
                        state->fxInspectorOpen = false;
                        InvalidateRect(hwnd, nullptr, FALSE);
                        return 0;
                    }

                    // ADD / REM buttons
                    const int ctrlTop = inspPanel.top + kUiDrawInspHeaderH;
                    RECT addBtn{inspPanel.left + 6,  ctrlTop + 4, inspPanel.left + 66,  ctrlTop + kUiDrawInspCtrlH - 4};
                    RECT remBtn{inspPanel.left + 72, ctrlTop + 4, inspPanel.left + 132, ctrlTop + kUiDrawInspCtrlH - 4};
                    if (PtInRect(&addBtn, pt) && pSlots && slotCount < kMaxInsertSlots) {
                        EnterCriticalSection(&state->audioStateLock);
                        (*pSlots)++;
                        state->projectModified = true;
                        UpdateWindowTitle(hwnd, *state);
                        LeaveCriticalSection(&state->audioStateLock);
                        InvalidateRect(hwnd, nullptr, FALSE);
                        return 0;
                    }
                    if (PtInRect(&remBtn, pt) && pSlots && slotCount > 0) {
                        EnterCriticalSection(&state->audioStateLock);
                        (*pSlots)--;
                        // Clear removed slot's bypass so it doesn't persist
                        if (pBypass) (*pBypass)[static_cast<size_t>(*pSlots)] = false;
                        state->projectModified = true;
                        UpdateWindowTitle(hwnd, *state);
                        LeaveCriticalSection(&state->audioStateLock);
                        InvalidateRect(hwnd, nullptr, FALSE);
                        return 0;
                    }

                    // Per-slot rows
                    const int slotsTop = ctrlTop + kUiDrawInspCtrlH;
                    for (int s = 0; s < slotCount; ++s) {
                        const int rowTop = slotsTop + s * kUiDrawInspSlotH;
                        const int rowBot = rowTop + kUiDrawInspSlotH;
                        RECT typeBtn  {inspPanel.left + 26, rowTop + 2, inspPanel.left + 84, rowBot - 2};
                        RECT bypassBtn{inspPanel.left + 88, rowTop + 2, inspPanel.left + 130, rowBot - 2};
                        RECT arrowBtn {inspPanel.left + 134, rowTop + 2, inspPanel.left + 154, rowBot - 2};
                        if (PtInRect(&typeBtn, pt) && pEffects) {
                            EnterCriticalSection(&state->audioStateLock);
                            std::uint8_t& fx = (*pEffects)[static_cast<size_t>(s)];
                            fx = static_cast<std::uint8_t>((fx + 1) % kInsertEffectTypeCount);
                            state->projectModified = true;
                            UpdateWindowTitle(hwnd, *state);
                            LeaveCriticalSection(&state->audioStateLock);
                            InvalidateRect(hwnd, nullptr, FALSE);
                            return 0;
                        }
                        if (PtInRect(&bypassBtn, pt) && pBypass) {
                            EnterCriticalSection(&state->audioStateLock);
                            (*pBypass)[static_cast<size_t>(s)] = !(*pBypass)[static_cast<size_t>(s)];
                            state->projectModified = true;
                            UpdateWindowTitle(hwnd, *state);
                            LeaveCriticalSection(&state->audioStateLock);
                            InvalidateRect(hwnd, nullptr, FALSE);
                            return 0;
                        }
                        if (PtInRect(&arrowBtn, pt)) {
                            state->fxInspectorSelectedSlot = (state->fxInspectorSelectedSlot == s) ? -1 : s;
                            InvalidateRect(hwnd, nullptr, FALSE);
                            return 0;
                        }
                    }

                    // Param knob clicks in the expanded strip
                    if (state->fxInspectorSelectedSlot >= 0 && state->fxInspectorSelectedSlot < slotCount && pParams && pEffects) {
                        const int selSlot = state->fxInspectorSelectedSlot;
                        const int paramTop = slotsTop + slotCount * kUiDrawInspSlotH;
                        const int ky = paramTop + 16;
                        const int kw = (kUiDrawInspW - 12) / 4;
                        const int fxT = std::clamp(static_cast<int>((*pEffects)[static_cast<size_t>(selSlot)]), 0, kInsertEffectTypeCount - 1);

                        // Build same knob list as in Draw
                        struct KnobDef2 { float lo; float hi; int paramId; };
                        KnobDef2 knobs[4] = {};
                        int knobCount = 0;
                        switch (fxT) {
                        case kFxEQ:  knobs[0]={20,20000,0}; knobs[1]={-18,18,1}; knobs[2]={20,20000,2}; knobs[3]={-18,18,3}; knobCount=4; break;
                        case kFxCMP: knobs[0]={-60,0,10}; knobs[1]={1,20,11}; knobs[2]={0.1f,200,12}; knobs[3]={0,24,13}; knobCount=4; break;
                        case kFxSAT: knobs[0]={0,1,20}; knobs[1]={0,1,21}; knobCount=2; break;
                        case kFxDLY: knobs[0]={10,2000,30}; knobs[1]={0,0.95f,31}; knobs[2]={0,1,32}; knobCount=3; break;
                        case kFxREV: knobs[0]={0,1,40}; knobs[1]={0,1,41}; knobs[2]={0,1,42}; knobCount=3; break;
                        case kFxGATE:knobs[0]={-80,0,50}; knobs[1]={0.1f,200,51}; knobs[2]={10,500,52}; knobCount=3; break;
                        case kFxDEE: knobs[0]={-40,0,60}; knobs[1]={2000,16000,61}; knobs[2]={200,8000,62}; knobs[3]={0,24,63}; knobCount=4; break;
                        case kFxLIM: knobs[0]={-12,0,70}; knobs[1]={1,500,71}; knobCount=2; break;
                        }

                        for (int k = 0; k < knobCount; ++k) {
                            const int kx = inspPanel.left + 6 + k * kw;
                            const RECT kRect{kx, ky, kx + kw - 2, ky + kUiDrawInspParamH - 18};
                            if (PtInRect(&kRect, pt)) {
                                // Get current value for this paramId
                                const InsertConfig& P = (*pParams)[static_cast<size_t>(selSlot)];
                                float curVal = 0.0f;
                                switch (knobs[k].paramId) {
                                case 0: curVal=P.eq[0].freq_hz; break; case 1: curVal=P.eq[0].gain_db; break;
                                case 2: curVal=P.eq[1].freq_hz; break; case 3: curVal=P.eq[1].gain_db; break;
                                case 10:curVal=P.cmp_threshold_db; break; case 11:curVal=P.cmp_ratio; break;
                                case 12:curVal=P.cmp_attack_ms; break; case 13:curVal=P.cmp_makeup_db; break;
                                case 20:curVal=P.sat_drive; break; case 21:curVal=P.sat_mix; break;
                                case 30:curVal=P.dly_time_ms; break; case 31:curVal=P.dly_feedback; break; case 32:curVal=P.dly_mix; break;
                                case 40:curVal=P.rev_room_size; break; case 41:curVal=P.rev_damping; break; case 42:curVal=P.rev_mix; break;
                                case 50:curVal=P.gate_threshold_db; break; case 51:curVal=P.gate_attack_ms; break; case 52:curVal=P.gate_release_ms; break;
                                case 60:curVal=P.dee_threshold_db; break; case 61:curVal=P.dee_freq_hz; break; case 62:curVal=P.dee_bandwidth_hz; break; case 63:curVal=P.dee_reduction_db; break;
                                case 70:curVal=P.lim_ceiling_db; break; case 71:curVal=P.lim_release_ms; break;
                                }
                                state->draggingParamKnob = true;
                                state->paramKnobParamId  = knobs[k].paramId * 100 + selSlot;  // encode slot in lower 2 digits
                                state->paramKnobDragStartY   = pt.y;
                                state->paramKnobDragStartVal = curVal;
                                SetCapture(hwnd);
                                return 0;
                            }
                        }
                    }
                    // Consumed by inspector (click on non-interactive area inside)
                    return 0;
                }
            }

            // ── Ruler click → set playhead ───────────────────────────────
            if (PtInRect(&layout.ruler, pt)) {
                const float beat = std::max(0.0f, UiLayoutXToBeat(layout.ruler, *state, pt.x));
                state->playheadBeat = UiLayoutSnapBeat(beat);
                state->draggingPlayhead = true;
                SetCapture(hwnd);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }

            if (PtInRect(&layout.leftPanel, pt) && pt.y > layout.leftPanel.top + kRulerHeight && !state->project.tracks.empty()) {
                const int trackIndex = UiLayoutTrackIndexFromY(layout.arrange, *state, pt.y);
                if (trackIndex >= 0 && trackIndex < static_cast<int>(state->project.tracks.size())) {
                    state->selectedTrackIndex = trackIndex;
                    state->selectedClipIndex = -1;

                    RECT busRect{};
                    RECT panKnobRect{};
                    RECT panValRect{};
                    RECT fxRect{};
                    UiLayoutGetTrackRoutingRects(layout.leftPanel, trackIndex, &busRect, &panKnobRect, &panValRect, &fxRect);
                    if (PtInRect(&busRect, pt)) {
                        EnterCriticalSection(&state->audioStateLock);
                        if (trackIndex < static_cast<int>(state->project.tracks.size())) {
                            const int cur = TrackBusIndexAt(*state, trackIndex);
                            state->project.tracks[static_cast<size_t>(trackIndex)].busIndex = (cur + 1) % kBusCount;
                            state->projectModified = true;
                            UpdateWindowTitle(hwnd, *state);
                        }
                        LeaveCriticalSection(&state->audioStateLock);
                        InvalidateRect(hwnd, nullptr, FALSE);
                        return 0;
                    }
                    if (PtInRect(&fxRect, pt)) {
                        // Open insert-chain inspector for this track
                        state->fxInspectorOpen    = true;
                        state->fxInspectorIsTrack = true;
                        state->fxInspectorIndex   = trackIndex;
                        InvalidateRect(hwnd, nullptr, FALSE);
                        return 0;
                    }
                    if (PtInRect(&panKnobRect, pt) || PtInRect(&panValRect, pt)) {
                        if (PtInRect(&panValRect, pt) && (GetKeyState(VK_LBUTTON) & 0x8000)) {
                            // Double-click on value label resets to center
                            EnterCriticalSection(&state->audioStateLock);
                            if (trackIndex < static_cast<int>(state->project.tracks.size()))
                                state->project.tracks[static_cast<size_t>(trackIndex)].pan = 0.0f;
                            state->projectModified = true;
                            UpdateWindowTitle(hwnd, *state);
                            LeaveCriticalSection(&state->audioStateLock);
                        } else if (trackIndex < static_cast<int>(state->project.tracks.size())) {
                            // Start drag
                            state->draggingPan    = true;
                            state->dragPanIsBus   = false;
                            state->dragPanIndex   = trackIndex;
                            state->dragPanStartY  = pt.y;
                            state->dragPanStartVal = state->project.tracks[static_cast<size_t>(trackIndex)].pan;
                            SetCapture(hwnd);
                        }
                        InvalidateRect(hwnd, nullptr, FALSE);
                        return 0;
                    }

                    RECT muteRect{};
                    RECT soloRect{};
                    RECT recRect{};
                    UiLayoutGetTrackButtonRects(layout.leftPanel, trackIndex, &muteRect, &soloRect, &recRect);
                    if (PtInRect(&muteRect, pt)) {
                        EnterCriticalSection(&state->audioStateLock);
                        if (trackIndex < static_cast<int>(state->project.tracks.size())) {
                            state->project.tracks[static_cast<size_t>(trackIndex)].mute = !state->project.tracks[static_cast<size_t>(trackIndex)].mute;
                        }
                        LeaveCriticalSection(&state->audioStateLock);
                        InvalidateRect(hwnd, nullptr, FALSE);
                        return 0;
                    }
                    if (PtInRect(&recRect, pt)) {
                        EnterCriticalSection(&state->audioStateLock);
                        if (trackIndex < static_cast<int>(state->project.tracks.size())) {
                            state->project.tracks[static_cast<size_t>(trackIndex)].recordArm = !state->project.tracks[static_cast<size_t>(trackIndex)].recordArm;
                        }
                        LeaveCriticalSection(&state->audioStateLock);
                        InvalidateRect(hwnd, nullptr, FALSE);
                        return 0;
                    }
                    if (PtInRect(&soloRect, pt)) {
                        EnterCriticalSection(&state->audioStateLock);
                        if (trackIndex < static_cast<int>(state->project.tracks.size())) {
                            state->project.tracks[static_cast<size_t>(trackIndex)].solo = !state->project.tracks[static_cast<size_t>(trackIndex)].solo;
                        }
                        LeaveCriticalSection(&state->audioStateLock);
                        InvalidateRect(hwnd, nullptr, FALSE);
                        return 0;
                    }

                    RECT rail{};
                    RECT knob{};
                    UiLayoutGetTrackFaderRects(layout.leftPanel, trackIndex, &rail, &knob);
                    RECT hitRect{rail.left - 12, rail.top, rail.right + 12, rail.bottom};
                    if (PtInRect(&hitRect, pt)) {
                        PushUndo(*state);
                        state->draggingFader = true;
                        state->dragFaderTrack = trackIndex;
                        state->dragFaderStartY = pt.y;
                        EnterCriticalSection(&state->audioStateLock);
                        state->dragFaderStartDb = UiLayoutGainFromFaderY(rail, pt.y);
                        state->project.tracks[static_cast<size_t>(trackIndex)].gainDb = state->dragFaderStartDb;
                        LeaveCriticalSection(&state->audioStateLock);
                        SetCapture(hwnd);
                        InvalidateRect(hwnd, nullptr, FALSE);
                        return 0;
                    }
                }

                const int busTop = UiLayoutBusPanelTop(layout.leftPanel, *state);
                if (pt.y >= busTop + 18) {
                    for (int b = 0; b < kBusCount; ++b) {
                        RECT rowRect{};
                        RECT muteRect{};
                        RECT gainDownRect{};
                        RECT gainUpRect{};
                        RECT panKnobRect{};
                        RECT panValRect{};
                        RECT fxRect{};
                        UiLayoutGetBusControlRects(layout.leftPanel, *state, b, &rowRect, &muteRect, &gainDownRect, &gainUpRect, &panKnobRect, &panValRect, &fxRect);
                        if (!PtInRect(&rowRect, pt)) {
                            continue;
                        }

                        EnterCriticalSection(&state->audioStateLock);
                        if (PtInRect(&muteRect, pt) && b < static_cast<int>(state->project.buses.size())) {
                            state->project.buses[static_cast<size_t>(b)].mute = !state->project.buses[static_cast<size_t>(b)].mute;
                        } else if (PtInRect(&gainDownRect, pt) && b < static_cast<int>(state->project.buses.size())) {
                            state->project.buses[static_cast<size_t>(b)].gainDb = std::max(kFaderMinDb, state->project.buses[static_cast<size_t>(b)].gainDb - 1.0f);
                        } else if (PtInRect(&gainUpRect, pt) && b < static_cast<int>(state->project.buses.size())) {
                            state->project.buses[static_cast<size_t>(b)].gainDb = std::min(kFaderMaxDb, state->project.buses[static_cast<size_t>(b)].gainDb + 1.0f);
                        } else if ((PtInRect(&panKnobRect, pt) || PtInRect(&panValRect, pt)) && b < static_cast<int>(state->project.buses.size())) {
                            LeaveCriticalSection(&state->audioStateLock);
                            state->draggingPan    = true;
                            state->dragPanIsBus   = true;
                            state->dragPanIndex   = b;
                            state->dragPanStartY  = pt.y;
                            state->dragPanStartVal = state->project.buses[static_cast<size_t>(b)].pan;
                            SetCapture(hwnd);
                            InvalidateRect(hwnd, nullptr, FALSE);
                            return 0;
                        } else if (PtInRect(&fxRect, pt) && b < static_cast<int>(state->project.buses.size())) {
                            // Open insert-chain inspector for this bus
                            LeaveCriticalSection(&state->audioStateLock);
                            state->fxInspectorOpen    = true;
                            state->fxInspectorIsTrack = false;
                            state->fxInspectorIndex   = b;
                            InvalidateRect(hwnd, nullptr, FALSE);
                            return 0;
                        }
                        state->projectModified = true;
                        UpdateWindowTitle(hwnd, *state);
                        LeaveCriticalSection(&state->audioStateLock);

                        InvalidateRect(hwnd, nullptr, FALSE);
                        return 0;
                    }
                }
            }

            if (PtInRect(&layout.arrange, pt)) {
                state->selectedClipIndex = -1;
                for (int i = static_cast<int>(state->project.clips.size()) - 1; i >= 0; --i) {
                    RECT r{};
                    if (!UiLayoutClipRectForDraw(layout.arrange, *state, state->project.clips[static_cast<size_t>(i)], &r)) {
                        continue;
                    }
                    if (PtInRect(&r, pt)) {
                        state->selectedClipIndex = i;
                        state->selectedTrackIndex = state->project.clips[static_cast<size_t>(i)].trackIndex;

                        constexpr int kEdgeThresh = 7;
                        const int fullLeft  = UiLayoutBeatToX(layout.arrange, *state, state->project.clips[static_cast<size_t>(i)].startBeat);
                        const int fullRight = UiLayoutBeatToX(layout.arrange, *state, state->project.clips[static_cast<size_t>(i)].startBeat + state->project.clips[static_cast<size_t>(i)].lengthBeats);
                        const bool nearLeft  = (pt.x - fullLeft)  <= kEdgeThresh && (pt.x - fullLeft)  >= 0;
                        const bool nearRight = (fullRight - pt.x)  <= kEdgeThresh && (fullRight - pt.x) >= 0;

                        if (nearLeft || nearRight) {
                            // Trim
                            state->trimmingClip         = true;
                            state->trimClipIndex        = i;
                            state->trimIsLeft           = nearLeft;
                            state->trimOrigStart        = state->project.clips[static_cast<size_t>(i)].startBeat;
                            state->trimOrigLen          = state->project.clips[static_cast<size_t>(i)].lengthBeats;
                            state->trimOrigSourceOffset = state->project.clips[static_cast<size_t>(i)].sourceOffsetFrames;
                            PushUndo(*state);
                            SetCapture(hwnd);
                        } else {
                            // Drag
                            PushUndo(*state);
                            state->draggingClip = true;
                            state->dragClipIndex = i;
                            state->dragOffsetBeats = UiLayoutXToBeat(layout.arrange, *state, pt.x) - state->project.clips[static_cast<size_t>(i)].startBeat;
                            SetCapture(hwnd);
                        }
                        break;
                    }
                }
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        }
        return 0;
    case WM_RBUTTONUP:
        if (state == nullptr) {
            return 0;
        }
        {
            RECT client{};
            GetClientRect(hwnd, &client);
            const LayoutRects layout = UiLayoutComputeLayout(client);
            const POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};

            if (!PtInRect(&layout.leftPanel, pt) && !PtInRect(&layout.arrange, pt)) {
                return 0;
            }

            HMENU menu = CreatePopupMenu();
            if (menu == nullptr) {
                return 0;
            }

            AppendMenuW(menu, MF_STRING, kCmdTrackNew, L"New Track");

            int trackIndex = -1;
            if (pt.y > layout.leftPanel.top + kRulerHeight && !state->project.tracks.empty()) {
                trackIndex = UiLayoutTrackIndexFromY(layout.arrange, *state, pt.y);
            }
            if (trackIndex >= 0 && trackIndex < static_cast<int>(state->project.tracks.size())) {
                const bool armed = state->project.tracks[static_cast<size_t>(trackIndex)].recordArm;
                AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
                AppendMenuW(menu, MF_STRING, kCmdTrackNew + 1000, armed ? L"Disarm Track" : L"Arm Track");
            }

            POINT screenPt{pt.x, pt.y};
            ClientToScreen(hwnd, &screenPt);
            const UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, screenPt.x, screenPt.y, 0, hwnd, nullptr);
            DestroyMenu(menu);

            if (cmd == kCmdTrackNew) {
                AddNewTrack(*state);
                state->projectModified = true;
                UpdateWindowTitle(hwnd, *state);
                InvalidateRect(hwnd, nullptr, FALSE);
            } else if (cmd == kCmdTrackNew + 1000 && trackIndex >= 0 && trackIndex < static_cast<int>(state->project.tracks.size())) {
                EnterCriticalSection(&state->audioStateLock);
                state->project.tracks[static_cast<size_t>(trackIndex)].recordArm = !state->project.tracks[static_cast<size_t>(trackIndex)].recordArm;
                LeaveCriticalSection(&state->audioStateLock);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        }
        return 0;
    case WM_MOUSEMOVE:
        if (state == nullptr) {
            return 0;
        }

        if (state->draggingPlayhead) {
            RECT client{};
            GetClientRect(hwnd, &client);
            const LayoutRects layout = UiLayoutComputeLayout(client);
            const float beat = std::max(0.0f, UiLayoutXToBeat(layout.ruler, *state, GET_X_LPARAM(lParam)));
            state->playheadBeat = UiLayoutSnapBeat(beat);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        if (state->trimmingClip && state->trimClipIndex >= 0 &&
            state->trimClipIndex < static_cast<int>(state->project.clips.size())) {
            RECT client{};
            GetClientRect(hwnd, &client);
            const LayoutRects layout = UiLayoutComputeLayout(client);
            const float mouseBeat = UiLayoutSnapBeat(std::max(0.0f, UiLayoutXToBeat(layout.arrange, *state, GET_X_LPARAM(lParam))));
            ClipItem& clip = state->project.clips[static_cast<size_t>(state->trimClipIndex)];
            if (state->trimIsLeft) {
                const float newStart = std::min(mouseBeat, state->trimOrigStart + state->trimOrigLen - 0.25f);
                const float delta = newStart - state->trimOrigStart;
                clip.startBeat   = std::max(0.0f, state->trimOrigStart + delta);
                clip.lengthBeats = std::max(0.25f, state->trimOrigLen   - delta);
                const float spb = SamplesPerBeat(*state);
                const std::int64_t offsetDelta = static_cast<std::int64_t>(delta * spb);
                const std::int64_t newOff = static_cast<std::int64_t>(state->trimOrigSourceOffset) + offsetDelta;
                clip.sourceOffsetFrames = static_cast<std::uint64_t>(std::max<std::int64_t>(0, newOff));
            } else {
                const float newEnd = std::max(mouseBeat, state->trimOrigStart + 0.25f);
                clip.lengthBeats = newEnd - clip.startBeat;
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        if (state->draggingFader && state->dragFaderTrack >= 0) {
            RECT client{};
            GetClientRect(hwnd, &client);
            const LayoutRects layout = UiLayoutComputeLayout(client);
            RECT rail{};
            RECT knob{};
            UiLayoutGetTrackFaderRects(layout.leftPanel, state->dragFaderTrack, &rail, &knob);
            const int mouseY = GET_Y_LPARAM(lParam);
            const bool shiftFine = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            float newDb;
            if (shiftFine) {
                // Fine mode: 1px = 0.05 dB (drag relative from start point)
                const int dy = mouseY - state->dragFaderStartY;
                newDb = std::clamp(state->dragFaderStartDb - dy * 0.05f, kFaderMinDb, kFaderMaxDb);
            } else {
                newDb = UiLayoutGainFromFaderY(rail, mouseY);
            }
            EnterCriticalSection(&state->audioStateLock);
            state->project.tracks[static_cast<size_t>(state->dragFaderTrack)].gainDb = newDb;
            LeaveCriticalSection(&state->audioStateLock);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        if (state->draggingPan && state->dragPanIndex >= 0) {
            const int mouseY = GET_Y_LPARAM(lParam);
            const bool shiftFine = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            // Drag up = more right (+), drag down = more left (-).
            // Normal: full range (-1..+1) over ~200px. Fine (Shift): 10x slower.
            const int dy = mouseY - state->dragPanStartY;
            const float sensitivity = shiftFine ? 0.001f : 0.01f;
            const float newPan = std::clamp(state->dragPanStartVal - dy * sensitivity, -1.0f, 1.0f);
            EnterCriticalSection(&state->audioStateLock);
            if (state->dragPanIsBus) {
                if (state->dragPanIndex < static_cast<int>(state->project.buses.size()))
                    state->project.buses[static_cast<size_t>(state->dragPanIndex)].pan = newPan;
            } else {
                if (state->dragPanIndex < static_cast<int>(state->project.tracks.size()))
                    state->project.tracks[static_cast<size_t>(state->dragPanIndex)].pan = newPan;
            }
            LeaveCriticalSection(&state->audioStateLock);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        if (state->draggingParamKnob && state->paramKnobParamId >= 0) {
            const int dy = GET_Y_LPARAM(lParam) - state->paramKnobDragStartY;
            const bool shiftFine = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            // paramKnobParamId = paramId * 100 + slotIndex
            const int paramId = state->paramKnobParamId / 100;
            const int slotIdx = state->paramKnobParamId % 100;
            const int inspIdx = state->fxInspectorIndex;
            InsertConfigArray* pPA = nullptr;
            if (state->fxInspectorIsTrack) {
                if (inspIdx >= 0 && inspIdx < static_cast<int>(state->project.tracks.size()))
                    pPA = &state->project.tracks[static_cast<size_t>(inspIdx)].insertConfig;
            } else {
                if (inspIdx >= 0 && inspIdx < static_cast<int>(state->project.buses.size()))
                    pPA = &state->project.buses[static_cast<size_t>(inspIdx)].insertConfig;
            }
            if (pPA && slotIdx >= 0 && slotIdx < kMaxInsertSlots) {
                InsertConfig& P = (*pPA)[static_cast<size_t>(slotIdx)];
                // Sensitivity: dragging 100px covers full range; shift = 10x finer
                // We encode the range in the lo/hi from original knob definitions
                // Instead store range implicitly: use paramKnobDragStartVal + delta * range/100
                // Map by paramId:
                auto applyDrag = [&](float lo, float hi) -> float {
                    const float range = hi - lo;
                    const float sens = shiftFine ? 0.001f : 0.01f;
                    return std::clamp(state->paramKnobDragStartVal - dy * range * sens, lo, hi);
                };
                EnterCriticalSection(&state->audioStateLock);
                switch (paramId) {
                case 0: P.eq[0].freq_hz  = applyDrag(20.0f, 20000.0f); break;
                case 1: P.eq[0].gain_db  = applyDrag(-18.0f, 18.0f);   break;
                case 2: P.eq[1].freq_hz  = applyDrag(20.0f, 20000.0f); break;
                case 3: P.eq[1].gain_db  = applyDrag(-18.0f, 18.0f);   break;
                case 10: P.cmp_threshold_db = applyDrag(-60.0f, 0.0f);  break;
                case 11: P.cmp_ratio        = applyDrag(1.0f, 20.0f);   break;
                case 12: P.cmp_attack_ms    = applyDrag(0.1f, 200.0f);  break;
                case 13: P.cmp_makeup_db    = applyDrag(0.0f, 24.0f);   break;
                case 20: P.sat_drive = applyDrag(0.0f, 1.0f); break;
                case 21: P.sat_mix   = applyDrag(0.0f, 1.0f); break;
                case 30: P.dly_time_ms   = applyDrag(10.0f, 2000.0f);  break;
                case 31: P.dly_feedback  = applyDrag(0.0f, 0.95f);     break;
                case 32: P.dly_mix       = applyDrag(0.0f, 1.0f);      break;
                case 40: P.rev_room_size = applyDrag(0.0f, 1.0f); break;
                case 41: P.rev_damping   = applyDrag(0.0f, 1.0f); break;
                case 42: P.rev_mix       = applyDrag(0.0f, 1.0f); break;
                case 50: P.gate_threshold_db = applyDrag(-80.0f, 0.0f);   break;
                case 51: P.gate_attack_ms    = applyDrag(0.1f, 200.0f);   break;
                case 52: P.gate_release_ms   = applyDrag(10.0f, 500.0f);  break;
                case 60: P.dee_threshold_db  = applyDrag(-40.0f, 0.0f);   break;
                case 61: P.dee_freq_hz       = applyDrag(2000.0f, 16000.0f); break;
                case 62: P.dee_bandwidth_hz  = applyDrag(200.0f, 8000.0f); break;
                case 63: P.dee_reduction_db  = applyDrag(0.0f, 24.0f);    break;
                case 70: P.lim_ceiling_db = applyDrag(-12.0f, 0.0f);  break;
                case 71: P.lim_release_ms = applyDrag(1.0f, 500.0f);  break;
                }
                LeaveCriticalSection(&state->audioStateLock);
                state->projectModified = true;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }

        if (!state->draggingClip || state->dragClipIndex < 0) {
            return 0;
        }

        {
            RECT client{};
            GetClientRect(hwnd, &client);
            const LayoutRects layout = UiLayoutComputeLayout(client);

            const int mouseX = GET_X_LPARAM(lParam);
            const int mouseY = GET_Y_LPARAM(lParam);
            float newStart = UiLayoutXToBeat(layout.arrange, *state, mouseX) - state->dragOffsetBeats;
            newStart = std::max(0.0f, UiLayoutSnapBeat(newStart));

            EnterCriticalSection(&state->audioStateLock);
            ClipItem& clip = state->project.clips[static_cast<size_t>(state->dragClipIndex)];
            clip.startBeat = newStart;
            clip.trackIndex = UiLayoutTrackIndexFromY(layout.arrange, *state, mouseY);
            LeaveCriticalSection(&state->audioStateLock);

            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONUP:
        if (state != nullptr) {
            bool changed = false;
            if (state->draggingPlayhead) {
                state->draggingPlayhead = false;
                changed = true;
            }
            if (state->trimmingClip) {
                state->trimmingClip = false;
                state->trimClipIndex = -1;
                state->projectModified = true;
                changed = true;
            }
            if (state->draggingClip) {
                state->draggingClip = false;
                state->dragClipIndex = -1;
                state->projectModified = true;
                changed = true;
            }
            if (state->draggingFader) {
                state->draggingFader = false;
                state->dragFaderTrack = -1;
                state->projectModified = true;
                changed = true;
            }
            if (state->draggingPan) {
                state->draggingPan = false;
                state->dragPanIndex = -1;
                state->projectModified = true;
                changed = true;
            }
            if (state->draggingParamKnob) {
                state->draggingParamKnob = false;
                state->paramKnobParamId  = -1;
                state->projectModified = true;
                changed = true;
            }
            if (changed) {
                UpdateWindowTitle(hwnd, *state);
                ReleaseCapture();
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        }
        return 0;
    case WM_MOUSEWHEEL:
        if (state == nullptr) {
            return 0;
        }
        {
            const short delta = GET_WHEEL_DELTA_WPARAM(wParam);
            const bool ctrl = (GET_KEYSTATE_WPARAM(wParam) & MK_CONTROL) != 0;
            if (ctrl) {
                const float oldVisible = state->viewBeatsVisible;
                state->viewBeatsVisible = (delta > 0)
                    ? std::max(4.0f, state->viewBeatsVisible * 0.9f)
                    : std::min(128.0f, state->viewBeatsVisible * 1.1f);

                RECT client{};
                GetClientRect(hwnd, &client);
                const LayoutRects layout = UiLayoutComputeLayout(client);
                POINT p{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                ScreenToClient(hwnd, &p);
                const int focusX = std::clamp(p.x, layout.arrange.left, layout.arrange.right);
                const float beatAtCursor = UiLayoutXToBeat(layout.arrange, *state, focusX);
                const float ratio = (beatAtCursor - state->viewStartBeat) / oldVisible;
                state->viewStartBeat = beatAtCursor - ratio * state->viewBeatsVisible;
            } else {
                const float step = state->viewBeatsVisible * 0.08f;
                state->viewStartBeat += (delta > 0) ? -step : step;
            }
            state->viewStartBeat = std::max(0.0f, state->viewStartBeat);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_MOUSEHWHEEL:
        if (state == nullptr) {
            return 0;
        }
        {
            const short delta = GET_WHEEL_DELTA_WPARAM(wParam);
            const float step = state->viewBeatsVisible * 0.08f;
            state->viewStartBeat += (delta > 0) ? -step : step;
            state->viewStartBeat = std::max(0.0f, state->viewStartBeat);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_CAPTURECHANGED:
        if (state != nullptr) {
            state->draggingClip = false;
            state->dragClipIndex = -1;
            state->draggingFader = false;
            state->dragFaderTrack = -1;
            state->draggingPan = false;
            state->dragPanIndex = -1;
            state->draggingPlayhead = false;
            state->trimmingClip = false;
            state->trimClipIndex = -1;
        }
        return 0;
    case WM_ERASEBKGND:
        // Fully repainted in WM_PAINT using backbuffer.
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(hwnd, &ps);

        RECT client{};
        GetClientRect(hwnd, &client);

        HDC memDc = CreateCompatibleDC(hdc);
        const int backWidth = std::max(1, static_cast<int>(client.right - client.left));
        const int backHeight = std::max(1, static_cast<int>(client.bottom - client.top));
        HBITMAP backBmp = CreateCompatibleBitmap(hdc, backWidth, backHeight);
        HGDIOBJ oldBmp = SelectObject(memDc, backBmp);

        {
            HBRUSH bgBrush = CreateSolidBrush(kPalette.windowBg);
            FillRect(memDc, &client, bgBrush);
            DeleteObject(bgBrush);
        }

        if (state != nullptr) {
            HFONT uiFont = CreateFontW(
                18, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI"
            );
            HFONT smallFont = CreateFontW(
                15, 0, 0, 0, FW_MEDIUM, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI"
            );

            HGDIOBJ oldFont = SelectObject(memDc, uiFont);
            UiDrawTopBar(memDc, client, *state);

            SelectObject(memDc, smallFont);

            const LayoutRects layout = UiLayoutComputeLayout(client);
            UiDrawLeftTrackPanel(memDc, layout.leftPanel, *state);
            UiDrawRuler(memDc, layout.ruler, *state);
            UiDrawArrangeLanes(memDc, layout.arrange, *state);

            // Inspector panel floats on top of everything
            if (state->fxInspectorOpen)
                UiDrawInsertInspector(memDc, client, *state);

            SelectObject(memDc, oldFont);
            DeleteObject(uiFont);
            DeleteObject(smallFont);
        }

        BitBlt(hdc, 0, 0, client.right - client.left, client.bottom - client.top, memDc, 0, 0, SRCCOPY);
        SelectObject(memDc, oldBmp);
        DeleteObject(backBmp);
        DeleteDC(memDc);

        EndPaint(hwnd, &ps);
        return 0;
    }
    default:
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR lpCmdLine, int nCmdShow) {
    daw_core::Project project;
    project.name = "scratch";
    daw_core::Engine engine(project);
    (void)engine;

    // Optional: path to .dawproj passed as first command-line argument
    std::wstring startupProjectPath;
    if (lpCmdLine && lpCmdLine[0] != L'\0') {
        std::wstring arg = lpCmdLine;
        // Strip surrounding quotes if present
        if (!arg.empty() && arg.front() == L'"') arg = arg.substr(1);
        if (!arg.empty() && arg.back()  == L'"') arg.pop_back();
        if (std::filesystem::exists(arg)) startupProjectPath = arg;
    }

    WNDCLASS wc{};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = kWindowClassName;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);

    RegisterClass(&wc);

    HWND hwnd = CreateWindowEx(
        0,
        kWindowClassName,
        L"DAW GUI (C++ Bare Bones)",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        1200,
        700,
        nullptr,
        nullptr,
        hInstance,
        nullptr
    );

    if (hwnd == nullptr) {
        return 0;
    }

    // Load project file if one was specified at launch
    if (!startupProjectPath.empty()) {
        auto* initialState = reinterpret_cast<UiState*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
        if (initialState != nullptr) {
            EnterCriticalSection(&initialState->audioStateLock);
            LoadProject(startupProjectPath, *initialState);
            LeaveCriticalSection(&initialState->audioStateLock);
            if (initialState->trackInsertDspState.size() != initialState->project.tracks.size()) initialState->trackInsertDspState.resize(initialState->project.tracks.size());
            UpdateWindowTitle(hwnd, *initialState);
        }
    }

    SetFocus(hwnd);
    ShowWindow(hwnd, nCmdShow);

    MSG msg{};
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return 0;
}
