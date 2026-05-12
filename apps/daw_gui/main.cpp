#include "daw_sdk.h"
#include <windowsx.h>
#include <algorithm>
#include "core/internal_app_services.h"
#include "ui/draw.h"
#include "ui/layout.h"
#include "ui/dpi.h"
#include "ui/dock.h"
#include "ui/dock_persist.h"
#include "ui/panel.h"
#include "ai/automix_bridge.h"
#include "audio/engine_utils.h"
#include "audio/transport_adapter.h"
#include "vm/timeline_zoom.h"
#include "ui/wndproc/wheel.h"
#include "ui/wndproc/timer.h"
#include "ui/wndproc/keys.h"
#include "ui/wndproc/rbutton.h"
#include "ui/wndproc/lbutton.h"
#include "ui/wndproc/mousemove.h"
#include "ui/floating.h"

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

void ImportWavFiles(HWND hwnd, AppState& state);
void ConvertImportedAudioToProjectSampleRate(HWND hwnd, AppState& state);

// Forward declarations for audio orchestration (defined after DoAutoMaster/DoExportMix)
void StopPlayback(AppState& state, bool rewind);
void StopRecording(AppState& state, bool commitTake);
bool StartRecording(HWND hwnd, AppState& state);
bool StartPlayback(HWND hwnd, AppState& state);

// ============================================================
// Audio Settings Dialog
// ============================================================

struct AudioSettingsDlgData {
    AppState*         appState    {nullptr};
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
    AppState& state = *d.appState;
    const int beIdx = (int)SendDlgItemMessageW(hwnd, kAsDlgBackend, CB_GETCURSEL, 0, 0);
    if      (beIdx == 0) state.audio.audioBackend = AudioBackend::MME;
    else if (beIdx == 1) state.audio.audioBackend = AudioBackend::WasapiShared;
    else if (beIdx == 2) state.audio.audioBackend = AudioBackend::WasapiExclusive;

    const int outIdx = (int)SendDlgItemMessageW(hwnd, kAsDlgOutputDev, CB_GETCURSEL, 0, 0);
    if (outIdx >= 0 && outIdx < (int)state.audio.outputDeviceIds.size()) {
        state.audio.selectedOutputDeviceId   = state.audio.outputDeviceIds[outIdx];
        state.audio.selectedOutputDeviceName = state.audio.outputDeviceNames[outIdx];
    }
    const int inIdx = (int)SendDlgItemMessageW(hwnd, kAsDlgInputDev, CB_GETCURSEL, 0, 0);
    if (inIdx >= 0 && inIdx < (int)state.audio.inputDeviceIds.size()) {
        state.audio.selectedInputDeviceId   = state.audio.inputDeviceIds[inIdx];
        state.audio.selectedInputDeviceName = state.audio.inputDeviceNames[inIdx];
    }
    const int srIdx = (int)SendDlgItemMessageW(hwnd, kAsDlgSampleRate, CB_GETCURSEL, 0, 0);
    if (srIdx >= 0 && srIdx < (int)d.sampleRates.size()) {
        state.audio.preferredSampleRate = d.sampleRates[srIdx];
    }
    const int bufIdx = (int)SendDlgItemMessageW(hwnd, kAsDlgBufferSize, CB_GETCURSEL, 0, 0);
    if (bufIdx >= 0 && bufIdx < (int)d.bufferSizes.size()) {
        state.audio.preferredBufferFrames = d.bufferSizes[bufIdx];
    }
}

static void AsDlgUpdateStatus(HWND hwnd, const AppState& state) {
    wchar_t buf[256]{};
    // Status reflects the actual open device only -- never the project SR,
    // which is independent of any device (managed via Project > Sample Rate).
    const int sr  = state.audio.activeDeviceSampleRate;
    const int bsz = state.audio.activeDeviceBufferFrames > 0 ? state.audio.activeDeviceBufferFrames : state.audio.preferredBufferFrames;
    if (sr > 0) {
        const double ms = bsz > 0 ? static_cast<double>(bsz) / static_cast<double>(sr) * 1000.0 : 0.0;
        const int projSR = state.core.project.projectSampleRate;
        if (projSR > 0 && projSR != sr) {
            swprintf_s(buf, L"Active: %d Hz  /  %d frames  (~%.1f ms)   |   Project: %d Hz (real-time SRC)",
                       sr, bsz, ms, projSR);
        } else {
            swprintf_s(buf, L"Active: %d Hz  /  %d frames  (~%.1f ms latency)", sr, bsz, ms);
        }
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
        AppState& state = *d->appState;

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
        makeLabel(L"Device Sample Rate:", LX, y + 2, 140, ROW_H); makeCombo(kAsDlgSampleRate, CX, y, CW, 180);  y += GAP;
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
            L"Device settings only. Project sample rate: Project > Sample Rate. SRC applied if they differ.",
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
        if (state.audio.audioBackend == AudioBackend::MME)              beIdx = 0;
        else if (state.audio.audioBackend == AudioBackend::WasapiExclusive) beIdx = 2;
        SendMessageW(hBe, CB_SETCURSEL, beIdx, 0);

        // Output Device
        HWND hOut = GetDlgItem(hwnd, kAsDlgOutputDev);
        int selOut = 0;
        for (size_t i = 0; i < state.audio.outputDeviceNames.size(); ++i) {
            SendMessageW(hOut, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(state.audio.outputDeviceNames[i].c_str()));
            if (state.audio.outputDeviceIds[i] == state.audio.selectedOutputDeviceId) selOut = static_cast<int>(i);
        }
        if (!state.audio.outputDeviceNames.empty()) SendMessageW(hOut, CB_SETCURSEL, selOut, 0);

        // Input Device
        HWND hIn = GetDlgItem(hwnd, kAsDlgInputDev);
        int selIn = 0;
        for (size_t i = 0; i < state.audio.inputDeviceNames.size(); ++i) {
            SendMessageW(hIn, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(state.audio.inputDeviceNames[i].c_str()));
            if (state.audio.inputDeviceIds[i] == state.audio.selectedInputDeviceId) selIn = static_cast<int>(i);
        }
        if (!state.audio.inputDeviceNames.empty()) SendMessageW(hIn, CB_SETCURSEL, selIn, 0);

        // Sample Rate
        {
            const int stdRates[] = {22050, 44100, 48000, 88200, 96000, 176400, 192000};
            for (int r : stdRates) d->sampleRates.push_back(r);
            // Device-side SRs only. The project's SR is independent and is
            // managed via Project > Sample Rate -- it must not appear in the
            // device list (selecting it here would not change the project).
            auto addSR = [&](int sr) {
                if (sr > 0 && std::find(d->sampleRates.begin(), d->sampleRates.end(), sr) == d->sampleRates.end())
                    d->sampleRates.push_back(sr);
            };
            addSR(state.audio.preferredSampleRate);
            addSR(state.audio.activeDeviceSampleRate);
            std::sort(d->sampleRates.begin(), d->sampleRates.end());
            HWND hSr = GetDlgItem(hwnd, kAsDlgSampleRate);
            int selSR = -1;
            for (size_t i = 0; i < d->sampleRates.size(); ++i) {
                wchar_t rbuf[32]{};
                swprintf_s(rbuf, L"%d Hz", d->sampleRates[i]);
                SendMessageW(hSr, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(rbuf));
                if (state.audio.preferredSampleRate > 0 && d->sampleRates[i] == state.audio.preferredSampleRate)
                    selSR = static_cast<int>(i);
            }
            if (selSR < 0) {
                // Fallback: prefer 48000 if present, then 44100, else first.
                for (size_t i = 0; i < d->sampleRates.size(); ++i) {
                    if (d->sampleRates[i] == 48000) { selSR = static_cast<int>(i); break; }
                }
                if (selSR < 0) {
                    for (size_t i = 0; i < d->sampleRates.size(); ++i) {
                        if (d->sampleRates[i] == 44100) { selSR = static_cast<int>(i); break; }
                    }
                }
                if (selSR < 0 && !d->sampleRates.empty()) selSR = 0;
            }
            SendMessageW(hSr, CB_SETCURSEL, selSR, 0);
        }

        // Buffer Size
        {
            const int stdBufSizes[] = {64, 128, 256, 512, 1024, 2048};
            for (int b : stdBufSizes) d->bufferSizes.push_back(b);
            HWND hBuf = GetDlgItem(hwnd, kAsDlgBufferSize);
            int selBuf = 2; // default 256
            for (size_t i = 0; i < d->bufferSizes.size(); ++i) {
                wchar_t rbuf[32]{};
                swprintf_s(rbuf, L"%d frames", d->bufferSizes[i]);
                SendMessageW(hBuf, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(rbuf));
                if (d->bufferSizes[i] == state.audio.preferredBufferFrames) selBuf = static_cast<int>(i);
            }
            SendMessageW(hBuf, CB_SETCURSEL, selBuf, 0);
        }

        AsDlgUpdateStatus(hwnd, state);
        return 0;
    }
    case WM_COMMAND: {
        
        const int ctrlId = LOWORD(wParam);
        if (ctrlId == IDOK) {
            AsDlgReadFields(hwnd, *d);
            // Many devices (USB class-compliant interfaces, the AXE-FX II,
            // etc.) cannot change their sample rate from the host: the rate
            // is hard-locked in the device firmware / driver control panel.
            // If the user just picked a rate that the device has already
            // demonstrated it won't honor, warn here and snap the field back
            // to the truth -- otherwise the dialog would lie about what's
            // actually going to happen on the next Play.
            AppState& stateOK = *d->appState;
            const int wantSR = stateOK.audio.preferredSampleRate;
            const int actSR  = stateOK.audio.activeDeviceSampleRate;
            if (wantSR > 0 && actSR > 0 && wantSR != actSR) {
                wchar_t srMsg[512]{};
                swprintf_s(srMsg,
                    L"The selected device is currently open at %d Hz and is not accepting a sample-rate change to %d Hz.\n\n"
                    L"Many USB audio interfaces are hard-locked to a single rate; the rate must be changed in the device's own driver / control panel (and the device may need to be reconnected).\n\n"
                    L"Audio Settings will keep the device's actual sample rate (%d Hz).",
                    actSR, wantSR, actSR);
                MessageBoxW(hwnd, srMsg, L"Device sample rate not changed", MB_OK | MB_ICONWARNING);
                stateOK.audio.preferredSampleRate = actSR;
            }
            DestroyWindow(hwnd);
        } else if (ctrlId == IDCANCEL) {
            AppState& state = *d->appState;
            state.audio.audioBackend            = d->origBackend;
            state.audio.preferredSampleRate     = d->origSampleRate;
            state.audio.preferredBufferFrames   = d->origBufferFrames;
            state.audio.selectedOutputDeviceId   = d->origOutputDeviceId;
            state.audio.selectedOutputDeviceName = d->origOutputDeviceName;
            state.audio.selectedInputDeviceId    = d->origInputDeviceId;
            state.audio.selectedInputDeviceName  = d->origInputDeviceName;
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
            AppState& state = *d->appState;
            state.audio.audioBackend            = d->origBackend;
            state.audio.preferredSampleRate     = d->origSampleRate;
            state.audio.preferredBufferFrames   = d->origBufferFrames;
            state.audio.selectedOutputDeviceId   = d->origOutputDeviceId;
            state.audio.selectedOutputDeviceName = d->origOutputDeviceName;
            state.audio.selectedInputDeviceId    = d->origInputDeviceId;
            state.audio.selectedInputDeviceName  = d->origInputDeviceName;
        }
        DestroyWindow(hwnd);
        return 0;
    }
    case WM_DESTROY:
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void ShowAudioSettingsDialog(HWND hwndParent, AppState& state) {
    DeviceRefreshInputDevices(state);
    DeviceRefreshOutputDevices(state);

    AudioSettingsDlgData dlgData;
    dlgData.appState             = &state;
    dlgData.origBackend          = state.audio.audioBackend;
    dlgData.origSampleRate       = state.audio.preferredSampleRate;
    dlgData.origBufferFrames     = state.audio.preferredBufferFrames;
    dlgData.origOutputDeviceId   = state.audio.selectedOutputDeviceId;
    dlgData.origOutputDeviceName = state.audio.selectedOutputDeviceName;
    dlgData.origInputDeviceId    = state.audio.selectedInputDeviceId;
    dlgData.origInputDeviceName  = state.audio.selectedInputDeviceName;

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

// ── Project Sample Rate dialog ───────────────────────────────────────────────
// Stop-gap UI for setting the project's sample rate. The project SR is a
// property of the project file (default 48000) and is independent of any
// audio device's sample rate. It can only be changed here. Eventually this
// will move into a proper Project Settings dialog and the New Project flow.

struct ProjectSampleRateDlgData {
    AppState* appState {nullptr};
    int       result   {0};   // 0 = cancelled, otherwise the new SR
};

static constexpr int kPsrDlgEdit  = 2001;
static constexpr int kPsrDlgCombo = 2002;

static LRESULT CALLBACK ProjectSampleRateDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    ProjectSampleRateDlgData* d = reinterpret_cast<ProjectSampleRateDlgData*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
    case WM_CREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        d = reinterpret_cast<ProjectSampleRateDlgData*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(d));

        HINSTANCE hInst = GetModuleHandleW(nullptr);
        HFONT hFont = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));

        auto makeLabel = [&](const wchar_t* text, int x, int y, int w, int h) {
            HWND hc = CreateWindowExW(0, L"STATIC", text,
                WS_CHILD | WS_VISIBLE | SS_LEFT,
                x, y, w, h, hwnd, nullptr, hInst, nullptr);
            SendMessageW(hc, WM_SETFONT, reinterpret_cast<WPARAM>(hFont), TRUE);
        };
        auto makeButton = [&](const wchar_t* text, int id, int x, int y, int w, int h) {
            HWND hc = CreateWindowExW(0, L"BUTTON", text,
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                x, y, w, h, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), hInst, nullptr);
            SendMessageW(hc, WM_SETFONT, reinterpret_cast<WPARAM>(hFont), TRUE);
        };

        const int LX = 12, ROW_H = 22;
        int y = 14;

        makeLabel(L"Common rates:", LX, y + 2, 100, ROW_H);
        HWND hCb = CreateWindowExW(WS_EX_CLIENTEDGE, L"COMBOBOX", nullptr,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
            120, y, 240, 200, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPsrDlgCombo)),
            hInst, nullptr);
        SendMessageW(hCb, WM_SETFONT, reinterpret_cast<WPARAM>(hFont), TRUE);
        const int presets[] = {44100, 48000, 88200, 96000, 176400, 192000};
        for (int sr : presets) {
            wchar_t label[32]{};
            swprintf_s(label, L"%d Hz", sr);
            SendMessageW(hCb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
        }
        y += 32;

        makeLabel(L"Or custom:", LX, y + 2, 100, ROW_H);
        HWND hEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", nullptr,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_NUMBER,
            120, y, 240, ROW_H, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPsrDlgEdit)),
            hInst, nullptr);
        SendMessageW(hEdit, WM_SETFONT, reinterpret_cast<WPARAM>(hFont), TRUE);
        // Pre-fill with current project SR
        const int currentSR = d->appState ? d->appState->core.project.projectSampleRate : 48000;
        wchar_t curBuf[16]{};
        swprintf_s(curBuf, L"%d", currentSR);
        SetWindowTextW(hEdit, curBuf);
        // Match combo selection if current SR is a preset
        for (size_t i = 0; i < std::size(presets); ++i) {
            if (presets[i] == currentSR) {
                SendMessageW(hCb, CB_SETCURSEL, static_cast<int>(i), 0);
                break;
            }
        }
        y += 32;

        // Note
        HWND hNote = CreateWindowExW(0, L"STATIC",
            L"Range: 8000 - 384000 Hz. Affects timeline and storage rate.",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            LX, y, 360, ROW_H, hwnd, nullptr, hInst, nullptr);
        SendMessageW(hNote, WM_SETFONT, reinterpret_cast<WPARAM>(hFont), TRUE);
        y += 30;

        makeButton(L"Cancel", IDCANCEL, 178, y, 90, 26);
        makeButton(L"OK",     IDOK,     276, y, 90, 26);
        return 0;
    }
    case WM_COMMAND: {
        const int ctrlId = LOWORD(wParam);
        const int notifyCode = HIWORD(wParam);
        if (ctrlId == kPsrDlgCombo && notifyCode == CBN_SELCHANGE) {
            const int sel = static_cast<int>(SendDlgItemMessageW(hwnd, kPsrDlgCombo, CB_GETCURSEL, 0, 0));
            const int presets[] = {44100, 48000, 88200, 96000, 176400, 192000};
            if (sel >= 0 && sel < static_cast<int>(std::size(presets))) {
                wchar_t buf[16]{};
                swprintf_s(buf, L"%d", presets[sel]);
                SetDlgItemTextW(hwnd, kPsrDlgEdit, buf);
            }
            return 0;
        }
        if (ctrlId == IDOK) {
            wchar_t buf[16]{};
            GetDlgItemTextW(hwnd, kPsrDlgEdit, buf, static_cast<int>(std::size(buf)));
            const int sr = _wtoi(buf);
            if (sr < 8000 || sr > 384000) {
                MessageBoxW(hwnd, L"Sample rate must be between 8000 and 384000 Hz.",
                    L"Invalid Sample Rate", MB_OK | MB_ICONWARNING);
                return 0;
            }
            if (d != nullptr) d->result = sr;
            DestroyWindow(hwnd);
            return 0;
        }
        if (ctrlId == IDCANCEL) {
            DestroyWindow(hwnd);
            return 0;
        }
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void ShowProjectSampleRateDialog(HWND hwndParent, AppState& state) {
    ProjectSampleRateDlgData dlgData;
    dlgData.appState = &state;
    dlgData.result   = 0;

    static bool sClassRegistered = false;
    if (!sClassRegistered) {
        WNDCLASSEXW wc{};
        wc.cbSize        = sizeof(wc);
        wc.style         = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc   = ProjectSampleRateDlgProc;
        wc.hInstance     = GetModuleHandleW(nullptr);
        wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"DawProjectSampleRateDlg";
        if (RegisterClassExW(&wc)) sClassRegistered = true;
    }

    const int DLG_W = 380, DLG_H = 170;
    RECT wr{0, 0, DLG_W, DLG_H};
    AdjustWindowRectEx(&wr, WS_POPUP | WS_CAPTION | WS_SYSMENU, FALSE, 0);
    const int ww = wr.right - wr.left;
    const int wh = wr.bottom - wr.top;

    RECT parentRect{};
    GetWindowRect(hwndParent, &parentRect);
    const int cx = (parentRect.left + parentRect.right) / 2;
    const int cy = (parentRect.top  + parentRect.bottom) / 2;

    HWND hwndDlg = CreateWindowExW(
        WS_EX_DLGMODALFRAME,
        L"DawProjectSampleRateDlg",
        L"Project Sample Rate",
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

    if (dlgData.result > 0 && dlgData.result != state.core.project.projectSampleRate) {
        EnterCriticalSection(&state.audio.audioStateLock);
        state.core.project.projectSampleRate = dlgData.result;
        LeaveCriticalSection(&state.audio.audioStateLock);
        state.core.projectModified = true;
    }

    InvalidateRect(hwndParent, nullptr, FALSE);
}

// ── Native Win32 menu bar ────────────────────────────────────────────────────
// The application used to draw its own File/View/Audio/Track "tabs" in the top
// bar and trigger custom popup menus on click. That worked but it didn't behave
// like a native Windows menu (no Alt-key access, no keyboard navigation, no
// theme), and the hit-tested tab rects ate horizontal space on the toolbar.
//
// Now we build a real HMENU at startup and attach it via SetMenu(). Menu items
// dispatch through WM_COMMAND -> HandleMenuCommand. The Audio submenu has
// dynamic content (enumerated devices, sample rates), so we rebuild that
// submenu in WM_INITMENUPOPUP every time it opens.

static HMENU g_hMenuFile  = nullptr;
static HMENU g_hMenuView  = nullptr;
static HMENU g_hMenuAudio = nullptr;
static HMENU g_hMenuTrack = nullptr;
static HMENU g_hMenuWindow = nullptr;

static void ClearMenu(HMENU m) {
    if (m == nullptr) return;
    while (GetMenuItemCount(m) > 0) {
        DeleteMenu(m, 0, MF_BYPOSITION);
    }
}

static void PopulateFileMenu(HMENU menu, AppState& /*state*/) {
    AppendMenuW(menu, MF_STRING, kCmdFileOpen,         L"Open Project...\tCtrl+O");
    AppendMenuW(menu, MF_STRING, kCmdFileSave,         L"Save Project\tCtrl+S");
    AppendMenuW(menu, MF_STRING, kCmdFileSaveAs,       L"Save Project As...");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdFileImportWav,    L"Import WAV...\tI");
    AppendMenuW(menu, MF_STRING, kCmdFileExportWav,    L"Export Mix as WAV...");
    AppendMenuW(menu, MF_STRING, kCmdAutoMaster,       L"Auto Master...");
    AppendMenuW(menu, MF_STRING, kCmdMixReadiness,     L"Mix Readiness Check...");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdProjectSampleRate, L"Project Sample Rate...");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdFileExit, L"Exit");
}

static void PopulateViewMenu(HMENU menu, AppState& /*state*/) {
    AppendMenuW(menu, MF_STRING, kCmdViewZoomIn,  L"Zoom In");
    AppendMenuW(menu, MF_STRING, kCmdViewZoomOut, L"Zoom Out");
    AppendMenuW(menu, MF_STRING, kCmdViewReset,   L"Reset View");
}

static void PopulateAudioMenu(HMENU menu, AppState& state) {
    DeviceRefreshInputDevices(state);
    DeviceRefreshOutputDevices(state);

    HMENU inputSub = CreatePopupMenu();
    if (inputSub != nullptr) {
        for (size_t i = 0; i < state.audio.inputDeviceNames.size(); ++i) {
            const UINT cmdId = kCmdAudioInputBase + static_cast<UINT>(i);
            UINT flags = MF_STRING;
            if (state.audio.inputDeviceIds[i] == state.audio.selectedInputDeviceId) {
                flags |= MF_CHECKED;
            }
            AppendMenuW(inputSub, flags, cmdId, state.audio.inputDeviceNames[i].c_str());
        }
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(inputSub), L"Input Device");
    }
    HMENU outputSub = CreatePopupMenu();
    if (outputSub != nullptr) {
        for (size_t i = 0; i < state.audio.outputDeviceNames.size(); ++i) {
            const UINT cmdId = kCmdAudioOutputBase + static_cast<UINT>(i);
            UINT flags = MF_STRING;
            if (state.audio.outputDeviceIds[i] == state.audio.selectedOutputDeviceId) {
                flags |= MF_CHECKED;
            }
            AppendMenuW(outputSub, flags, cmdId, state.audio.outputDeviceNames[i].c_str());
        }
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(outputSub), L"Output Device");
    }
    HMENU backendSub = CreatePopupMenu();
    if (backendSub != nullptr) {
        AppendMenuW(backendSub, MF_STRING | (state.audio.audioBackend == AudioBackend::MME ? MF_CHECKED : 0), kCmdAudioBackendMME, L"MME (Legacy)");
        AppendMenuW(backendSub, MF_STRING | (state.audio.audioBackend == AudioBackend::WasapiShared ? MF_CHECKED : 0), kCmdAudioBackendWasapiShared, L"WASAPI Shared (Default devices)");
        AppendMenuW(backendSub, MF_STRING | (state.audio.audioBackend == AudioBackend::WasapiExclusive ? MF_CHECKED : 0), kCmdAudioBackendWasapiExclusive, L"WASAPI Exclusive");
        AppendMenuW(backendSub, MF_STRING | MF_GRAYED | (state.audio.audioBackend == AudioBackend::Asio ? MF_CHECKED : 0), kCmdAudioBackendAsio, L"ASIO (Future)");
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
        const int stdRates[] = {44100, 48000, 88200, 96000, 176400, 192000};
        for (int r : stdRates) addRate(r);
        addRate(state.audio.preferredSampleRate);
        addRate(state.audio.activeDeviceSampleRate);
        addRate(state.audio.lastOpenedOutputSampleRate);
        std::sort(sampleRates.begin(), sampleRates.end());
        if (sampleRates.empty()) {
            AppendMenuW(sampleRateSub, MF_STRING | MF_GRAYED, kCmdAudioSampleRateBase, L"No sample rates available yet");
        } else {
            for (size_t i = 0; i < sampleRates.size(); ++i) {
                const int sr = sampleRates[i];
                const UINT cmdId = kCmdAudioSampleRateBase + static_cast<UINT>(i);
                wchar_t label[64]{};
                swprintf_s(label, L"%d Hz", sr);
                const UINT flags = MF_STRING | ((state.audio.preferredSampleRate == sr) ? MF_CHECKED : 0);
                AppendMenuW(sampleRateSub, flags, cmdId, label);
            }
        }
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(sampleRateSub), L"Device Sample Rate");
    }

    HMENU bufferSub = CreatePopupMenu();
    if (bufferSub != nullptr) {
        const int bufferOptions[] = {64, 128, 256, 512, 1024, 2048};
        for (size_t i = 0; i < std::size(bufferOptions); ++i) {
            const int frames = bufferOptions[i];
            const UINT cmdId = kCmdAudioBufferSizeBase + static_cast<UINT>(i);
            wchar_t label[64]{};
            swprintf_s(label, L"%d frames", frames);
            const UINT flags = MF_STRING | ((state.audio.preferredBufferFrames == frames) ? MF_CHECKED : 0);
            AppendMenuW(bufferSub, flags, cmdId, label);
        }
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(bufferSub), L"Buffer Size");
    }

    AppendMenuW(menu, MF_STRING, kCmdAudioRefreshInputs,    L"Refresh Inputs");
    AppendMenuW(menu, MF_STRING, kCmdAudioDiagnostics,      L"Audio Diagnostics...");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdAudioConvertImported,  L"Convert Imported Audio to Project Sample Rate...");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdAudioSettings,         L"Audio Settings...");
}

static void PopulateTrackMenu(HMENU menu, AppState& /*state*/) {
    AppendMenuW(menu, MF_STRING, kCmdTrackNew, L"New Track");
}

// Window menu lists every panel with a checkmark when it's currently visible
// somewhere in the dock tree. Toggling unchecks/removes a visible panel, or
// re-adds a hidden one as a tab in the first non-primary leaf (so users can
// always recover panels they accidentally dragged out of existence).
static void PopulateWindowMenu(HMENU menu, AppState& state) {
    using namespace daw::ui;
    for (int i = 0; i < PanelCount(); ++i) {
        const PanelKind   k   = static_cast<PanelKind>(i);
        const PanelDef&   def = PanelGet(k);
        const bool visible = (state.ui.dockRoot &&
                              DockFindLeafContaining(state.ui.dockRoot.get(), k) != nullptr);
        UINT flags = MF_STRING;
        if (visible)      flags |= MF_CHECKED;
        if (def.primary)  flags |= MF_GRAYED;   // primary panels can't be hidden
        AppendMenuW(menu, flags, kCmdWindowPanelBase + static_cast<UINT>(i), def.title);
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdWindowResetLayout, L"Reset Layout");
}

static HMENU BuildMainMenuBar(AppState& state) {
    HMENU bar = CreateMenu();
    if (bar == nullptr) return nullptr;
    g_hMenuFile  = CreatePopupMenu();
    g_hMenuView  = CreatePopupMenu();
    g_hMenuAudio = CreatePopupMenu();
    g_hMenuTrack = CreatePopupMenu();
    g_hMenuWindow = CreatePopupMenu();
    PopulateFileMenu (g_hMenuFile,  state);
    PopulateViewMenu (g_hMenuView,  state);
    PopulateAudioMenu(g_hMenuAudio, state);
    PopulateTrackMenu(g_hMenuTrack, state);
    PopulateWindowMenu(g_hMenuWindow, state);
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(g_hMenuFile),  L"&File");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(g_hMenuView),  L"&View");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(g_hMenuAudio), L"&Audio");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(g_hMenuTrack), L"&Track");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(g_hMenuWindow), L"&Window");
    return bar;
}

// Rebuild the contents of one of the four top-level popups. Called from
// WM_INITMENUPOPUP just before the menu becomes visible so dynamic content
// (device list, current sample rate / backend / buffer-size checkmarks)
// always reflects current state.
static bool RefreshTopLevelPopup(HMENU popup, AppState& state) {
    if (popup == g_hMenuFile)  { ClearMenu(popup); PopulateFileMenu (popup, state); return true; }
    if (popup == g_hMenuView)  { ClearMenu(popup); PopulateViewMenu (popup, state); return true; }
    if (popup == g_hMenuAudio) { ClearMenu(popup); PopulateAudioMenu(popup, state); return true; }
    if (popup == g_hMenuTrack) { ClearMenu(popup); PopulateTrackMenu(popup, state); return true; }
    if (popup == g_hMenuWindow){ ClearMenu(popup); PopulateWindowMenu(popup, state); return true; }
    return false;
}

// Dispatch a WM_COMMAND ID coming from the menu bar.
static void HandleMenuCommand(HWND hwnd, AppState& state, UINT cmd) {
    if (cmd == kCmdFileOpen) {
        DoOpen(hwnd, state);
        if (state.audio.trackInsertDspState.size() != state.core.project.tracks.size()) state.audio.trackInsertDspState.resize(state.core.project.tracks.size());
    } else if (cmd == kCmdFileSave) {
        DoSave(hwnd, state);
    } else if (cmd == kCmdFileSaveAs) {
        DoSaveAs(hwnd, state);
    } else if (cmd == kCmdFileImportWav) {
        ImportWavFiles(hwnd, state);
        state.core.projectModified = true;
        UpdateWindowTitle(hwnd, state.core);
    } else if (cmd == kCmdFileExportWav) {
        DoExportMix(hwnd, state);
    } else if (cmd == kCmdAutoMaster) {
        DoAutoMaster(hwnd, state);
    } else if (cmd == kCmdMixReadiness) {
        DoMixReadiness(hwnd, state);
    } else if (cmd == kCmdProjectSampleRate) {
        ShowProjectSampleRateDialog(hwnd, state);
        UpdateWindowTitle(hwnd, state.core);
    } else if (cmd == kCmdFileExit) {
        PostMessage(hwnd, WM_CLOSE, 0, 0);
    } else if (cmd == kCmdViewZoomIn) {
        state.ui.viewBeatsVisible = daw::vm::ZoomVisible(state.ui.viewBeatsVisible, daw::vm::kKeyZoomInFactor);
    } else if (cmd == kCmdViewZoomOut) {
        state.ui.viewBeatsVisible = daw::vm::ZoomVisible(state.ui.viewBeatsVisible, daw::vm::kKeyZoomOutFactor);
    } else if (cmd == kCmdViewReset) {
        const auto reset = daw::vm::ResetView();
        state.ui.viewStartBeat    = reset.viewStartBeat;
        state.ui.viewBeatsVisible = reset.viewBeatsVisible;
    } else if (cmd == kCmdWindowResetLayout) {
        // Rebuild the dock tree from scratch. Splitter ratios reset too.
        // Also wipe the persisted layout so a crash before the next save
        // can't resurrect the old custom arrangement.
        state.ui.dockRoot = daw::ui::DockBuildDefault();
        daw::ui::DockDeleteLayoutFile();
        InvalidateRect(hwnd, nullptr, FALSE);
    } else if (cmd >= kCmdWindowPanelBase &&
               cmd <  kCmdWindowPanelBase + static_cast<UINT>(daw::ui::PanelCount())) {
        // Toggle a panel: hide if currently visible, show as a tab in the
        // first non-primary leaf otherwise. Primary panels are MF_GRAYED so
        // this branch is unreachable for them, but we guard anyway.
        const daw::ui::PanelKind k =
            static_cast<daw::ui::PanelKind>(cmd - kCmdWindowPanelBase);
        if (!daw::ui::PanelGet(k).primary && state.ui.dockRoot) {
            daw::ui::DockNode* leaf =
                daw::ui::DockFindLeafContaining(state.ui.dockRoot.get(), k);
            if (leaf != nullptr) {
                // Hide: remove every tab matching this panel from the leaf.
                for (int i = static_cast<int>(leaf->panels.size()) - 1; i >= 0; --i) {
                    if (leaf->panels[static_cast<size_t>(i)] == k) {
                        daw::ui::DockRemoveTab(state.ui.dockRoot, leaf, i);
                    }
                }
            } else {
                // Show: prefer tab-merging into the Tracks leaf (it's the
                // natural left-rail home for utility panels). Fall back to
                // any non-primary leaf, then to splitting off Arrange if
                // every leaf is primary and single-tabbed.
                daw::ui::DockNode* host = daw::ui::DockFindLeafContaining(
                    state.ui.dockRoot.get(), daw::ui::PanelKind::Tracks);
                if (host == nullptr) {
                    host = daw::ui::DockFindNonPrimaryLeaf(state.ui.dockRoot.get());
                }
                if (host != nullptr) {
                    daw::ui::DockInsertTab(host, k,
                        static_cast<int>(host->panels.size()));
                } else {
                    daw::ui::DockNode* arrange = daw::ui::DockFindLeafContaining(
                        state.ui.dockRoot.get(), daw::ui::PanelKind::Arrange);
                    if (arrange != nullptr) {
                        daw::ui::DockSplitWith(state.ui.dockRoot, arrange,
                            daw::ui::DockDropSide::Left, k, 0.25f);
                    }
                }
            }
            InvalidateRect(hwnd, nullptr, FALSE);
        }
    } else if (cmd == kCmdAudioRefreshInputs) {
        DeviceRefreshInputDevices(state);
        DeviceRefreshOutputDevices(state);
    } else if (cmd == kCmdAudioDiagnostics) {
        const std::wstring diag = DeviceBuildAudioDiagnosticsReport(state);
        MessageBoxW(hwnd, diag.c_str(), L"Audio Diagnostics", MB_OK | MB_ICONINFORMATION);
    } else if (cmd == kCmdAudioSettings) {
        ShowAudioSettingsDialog(hwnd, state);
    } else if (cmd == kCmdAudioConvertImported) {
        ConvertImportedAudioToProjectSampleRate(hwnd, state);
    } else if (cmd == kCmdAudioBackendMME) {
        state.audio.audioBackend = AudioBackend::MME;
    } else if (cmd == kCmdAudioBackendWasapiShared) {
        state.audio.audioBackend = AudioBackend::WasapiShared;
    } else if (cmd == kCmdAudioBackendWasapiExclusive) {
        state.audio.audioBackend = AudioBackend::WasapiExclusive;
    } else if (cmd == kCmdAudioBackendAsio) {
        MessageBoxW(hwnd, L"ASIO backend is planned but not implemented yet.", L"Audio Backend", MB_OK | MB_ICONINFORMATION);
    } else if (cmd >= kCmdAudioSampleRateBase && cmd < kCmdAudioSampleRateBase + 64) {
        std::vector<int> sampleRates;
        auto addRate = [&](int sr) {
            if (sr > 0 && std::find(sampleRates.begin(), sampleRates.end(), sr) == sampleRates.end()) {
                sampleRates.push_back(sr);
            }
        };
        const int stdRates[] = {44100, 48000, 88200, 96000, 176400, 192000};
        for (int r : stdRates) addRate(r);
        addRate(state.audio.preferredSampleRate);
        addRate(state.audio.activeDeviceSampleRate);
        addRate(state.audio.lastOpenedOutputSampleRate);
        std::sort(sampleRates.begin(), sampleRates.end());
        const size_t idx = static_cast<size_t>(cmd - kCmdAudioSampleRateBase);
        if (idx < sampleRates.size()) {
            const int wantSR = sampleRates[idx];
            const int actSR  = state.audio.activeDeviceSampleRate;
            if (wantSR > 0 && actSR > 0 && wantSR != actSR) {
                wchar_t srMsg[512]{};
                swprintf_s(srMsg,
                    L"The selected device is currently open at %d Hz and is not accepting a sample-rate change to %d Hz.\n\n"
                    L"Many USB audio interfaces are hard-locked to a single rate; the rate must be changed in the device's own driver / control panel.\n\n"
                    L"The device's actual sample rate (%d Hz) will be kept.",
                    actSR, wantSR, actSR);
                MessageBoxW(hwnd, srMsg, L"Device sample rate not changed", MB_OK | MB_ICONWARNING);
                state.audio.preferredSampleRate = actSR;
            } else {
                state.audio.preferredSampleRate = wantSR;
            }
        }
    } else if (cmd >= kCmdAudioBufferSizeBase && cmd < kCmdAudioBufferSizeBase + 16) {
        const int bufferOptions[] = {64, 128, 256, 512, 1024, 2048};
        const size_t idx = static_cast<size_t>(cmd - kCmdAudioBufferSizeBase);
        if (idx < std::size(bufferOptions)) {
            state.audio.preferredBufferFrames = bufferOptions[idx];
        }
    } else if (cmd == kCmdTrackNew) {
        AddNewTrack(state);
    } else if (cmd >= kCmdAudioInputBase && cmd < kCmdAudioInputBase + static_cast<UINT>(state.audio.inputDeviceIds.size())) {
        const size_t idx = static_cast<size_t>(cmd - kCmdAudioInputBase);
        state.audio.selectedInputDeviceId = state.audio.inputDeviceIds[idx];
        state.audio.selectedInputDeviceName = state.audio.inputDeviceNames[idx];
    } else if (cmd >= kCmdAudioOutputBase && cmd < kCmdAudioOutputBase + static_cast<UINT>(state.audio.outputDeviceIds.size())) {
        const size_t idx = static_cast<size_t>(cmd - kCmdAudioOutputBase);
        state.audio.selectedOutputDeviceId = state.audio.outputDeviceIds[idx];
        state.audio.selectedOutputDeviceName = state.audio.outputDeviceNames[idx];
    }

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

static bool ReplaceProjectWithSingleWav(AppState& state, const std::wstring& wavPath, std::wstring* outError) {
    LoadedAudio audio{};
    std::wstring error;
    if (!IoLoadWavStereo(wavPath, &audio, &error)) {
        if (outError) *outError = error;
        return false;
    }

    EnterCriticalSection(&state.audio.audioStateLock);
    state.core.project.tracks.clear();
    state.core.project.audio.clear();
    state.core.project.clips.clear();

    // Reset buses to defaults
    state.core.project.buses.assign(kBusCount, BusData{});

    state.core.project.projectSampleRate = audio.sampleRate;

    {
        TrackData t{};
        t.name = audio.displayName;
        t.busIndex = 3; // mastered stereo routes directly to Master
        state.core.project.tracks.push_back(std::move(t));
    }
        state.core.project.audio.push_back(std::move(audio));

        const float lengthBeats = BeatsFromFrames(state, state.core.project.audio.back().frames);
        state.core.project.clips.push_back(ClipItem{
            0,
            0,
            0.0f,
            std::max(0.25f, lengthBeats),
            kPalette.clip1,
            state.core.project.tracks.back().name,
        });

    state.ui.selectedTrackIndex = 0;
    state.ui.selectedClipIndex = 0;
    state.ui.playheadBeat = 0.0f;
    state.ui.viewStartBeat = 0.0f;
    state.ui.viewBeatsVisible = daw::vm::FitVisibleToContent(lengthBeats);
    state.core.projectFilePath.clear();
    state.core.projectModified = true;
    LeaveCriticalSection(&state.audio.audioStateLock);
    return true;
}

bool DoAutoMaster(HWND hwnd, AppState& state) {
    float targetLufs = -14.0f;
    float ceilingDb = -1.0f;
    float width = 1.15f;
    if (!ChooseAutoMasterSettings(hwnd, &targetLufs, &ceilingDb, &width)) {
        return false;
    }

    std::wstring sourceWav;
    {
        EnterCriticalSection(&state.audio.audioStateLock);
        if (!state.core.project.clips.empty() && state.ui.selectedClipIndex >= 0 && state.ui.selectedClipIndex < static_cast<int>(state.core.project.clips.size())) {
            const ClipItem& c = state.core.project.clips[static_cast<size_t>(state.ui.selectedClipIndex)];
            if (c.audioIndex >= 0 && c.audioIndex < static_cast<int>(state.core.project.audio.size())) {
                sourceWav = state.core.project.audio[static_cast<size_t>(c.audioIndex)].sourcePath;
            }
        }
        if (sourceWav.empty() && state.core.project.audio.size() == 1) {
            sourceWav = state.core.project.audio[0].sourcePath;
        }
        LeaveCriticalSection(&state.audio.audioStateLock);
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
        if (state.audio.recording) {
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
        UpdateWindowTitle(hwnd, state.core);
        InvalidateRect(hwnd, nullptr, FALSE);
    }

    return true;
}

bool DoMixReadiness(HWND hwnd, AppState& state) {
    if (state.core.project.tracks.empty() || state.core.project.clips.empty()) {
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
        EnterCriticalSection(&state.audio.audioStateLock);
        const bool ok = RenderBusStemToStereoLocked(state, b, &stereo, &sr);
        LeaveCriticalSection(&state.audio.audioStateLock);
        if (!ok || stereo.empty()) continue;
        const std::wstring wavPath = std::wstring(stemsDir) + L"\\" + kBusWavNames[b] + L".wav";
        if (IoWriteWavPcm16Stereo(wavPath, stereo, sr)) ++exported;
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

bool DoExportMix(HWND hwnd, AppState& state) {
    if (state.core.project.tracks.empty() || state.core.project.clips.empty()) {
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
    if (!state.core.projectFilePath.empty()) {
        const auto stem = std::filesystem::path(state.core.projectFilePath).stem().wstring();
        const auto dir  = std::filesystem::path(state.core.projectFilePath).parent_path().wstring();
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
    EnterCriticalSection(&state.audio.audioStateLock);
    const bool ok = RenderFullMixToStereoLocked(state, &stereo, &sampleRate);
    LeaveCriticalSection(&state.audio.audioStateLock);

    if (!ok || stereo.empty()) {
        MessageBoxW(hwnd, L"Render failed - no audio could be mixed.", L"Export Mix", MB_OK | MB_ICONERROR);
        return false;
    }

    if (!IoWriteWavPcm16Stereo(filePath, stereo, sampleRate)) {
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

void ImportWavFiles(HWND hwnd, AppState& state) {
    const std::vector<std::wstring> files = PickWavFiles(hwnd);
    if (files.empty()) {
        return;
    }

    const COLORREF clipColors[4] = {kPalette.clip1, kPalette.clip2, kPalette.clip3, kPalette.clip4};
    std::wstring skipped;
    int convertedAtImport = 0;
    int alreadyMatched    = 0;
    const int projSRForReport = state.core.project.projectSampleRate;

    for (const std::wstring& path : files) {
        LoadedAudio audio{};
        std::wstring error;
        if (!IoLoadWavStereo(path, &audio, &error)) {
            skipped += std::filesystem::path(path).filename().wstring() + L": " + error + L"\n";
            continue;
        }

        if (audio.sampleRate == state.core.project.projectSampleRate) {
            ++alreadyMatched;
        }

        if (audio.sampleRate != state.core.project.projectSampleRate) {
            // Sample-rate convert on import. The project owns its SR; imported
            // files are resampled to match using a high-quality windowed-sinc
            // (Kaiser) resampler so the on-disk file is faithfully represented
            // at the project rate. Linear interpolation was previously used
            // and produced audible high-frequency loss / aliasing.
            const int srcSR = audio.sampleRate;
            const int dstSR = state.core.project.projectSampleRate;
            if (srcSR <= 0 || dstSR <= 0 || audio.frames == 0) {
                skipped += std::filesystem::path(path).filename().wstring() + L": invalid sample rate or empty file\n";
                continue;
            }
            const std::uint64_t dstFrames64 =
                (static_cast<std::uint64_t>(audio.frames) * static_cast<std::uint64_t>(dstSR)
                 + static_cast<std::uint64_t>(srcSR) / 2)
                / static_cast<std::uint64_t>(srcSR);
            if (dstFrames64 == 0 || dstFrames64 > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
                skipped += std::filesystem::path(path).filename().wstring() + L": resample size out of range\n";
                continue;
            }
            const int dstFrames = static_cast<int>(dstFrames64);
            std::vector<float> resampled(static_cast<size_t>(dstFrames) * 2, 0.0f);
            ResampleStereoFloatSincHQ(audio.stereo.data(), static_cast<int>(audio.frames), srcSR,
                                      resampled.data(), dstFrames, dstSR);
            audio.stereo     = std::move(resampled);
            audio.frames     = static_cast<std::uint32_t>(dstFrames);
            audio.sampleRate = dstSR;
            ++convertedAtImport;
        }

        const int trackIndex = static_cast<int>(state.core.project.tracks.size());
        const int audioIndex = static_cast<int>(state.core.project.audio.size());

        EnterCriticalSection(&state.audio.audioStateLock);
        {
            TrackData t{};
            t.name = audio.displayName;
            t.busIndex = 1;
            state.core.project.tracks.push_back(std::move(t));
        }
        state.core.project.audio.push_back(std::move(audio));

        const float lengthBeats = BeatsFromFrames(state, state.core.project.audio.back().frames);
        state.core.project.clips.push_back(ClipItem{
            trackIndex,
            audioIndex,
            0.0f,
            std::max(0.25f, lengthBeats),
            clipColors[trackIndex % 4],
            state.core.project.tracks.back().name,
        });
        LeaveCriticalSection(&state.audio.audioStateLock);
    }

    if (!state.core.project.clips.empty()) {
        float endBeat = 0.0f;
        EnterCriticalSection(&state.audio.audioStateLock);
        for (const ClipItem& clip : state.core.project.clips) {
            endBeat = std::max(endBeat, clip.startBeat + clip.lengthBeats);
        }
        LeaveCriticalSection(&state.audio.audioStateLock);
        state.ui.viewStartBeat = 0.0f;
        state.ui.viewBeatsVisible = daw::vm::FitVisibleToContent(endBeat);
    }

    if (!skipped.empty()) {
        MessageBoxW(hwnd, skipped.c_str(), L"Some files were skipped", MB_OK | MB_ICONWARNING);
    }

    if (convertedAtImport > 0 || alreadyMatched > 0) {
        wchar_t summary[384]{};
        swprintf_s(summary,
            L"Imported %d file(s).\n"
            L"  - %d converted to project rate (%d Hz) using high-quality sinc resampler.\n"
            L"  - %d already at project rate.",
            convertedAtImport + alreadyMatched, convertedAtImport, projSRForReport, alreadyMatched);
        MessageBoxW(hwnd, summary, L"Import Complete", MB_OK | MB_ICONINFORMATION);
    }
}

void ConvertImportedAudioToProjectSampleRate(HWND hwnd, AppState& state) {
    const int dstSR = state.core.project.projectSampleRate;
    if (dstSR <= 0) {
        MessageBoxW(hwnd, L"Project sample rate is not set.", L"Convert Audio", MB_OK | MB_ICONWARNING);
        return;
    }

    // Snapshot which clips need conversion (without holding the lock).
    int needCount = 0;
    int totalCount = static_cast<int>(state.core.project.audio.size());
    for (const LoadedAudio& a : state.core.project.audio) {
        if (a.sampleRate > 0 && a.sampleRate != dstSR && a.frames > 0) {
            ++needCount;
        }
    }
    if (totalCount == 0) {
        MessageBoxW(hwnd, L"No audio clips have been imported.", L"Convert Audio", MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (needCount == 0) {
        wchar_t msg[256]{};
        swprintf_s(msg, L"All %d imported clip(s) are already at the project sample rate (%d Hz).",
                   totalCount, dstSR);
        MessageBoxW(hwnd, msg, L"Convert Audio", MB_OK | MB_ICONINFORMATION);
        return;
    }

    wchar_t prompt[512]{};
    swprintf_s(prompt,
        L"%d of %d imported clip(s) are not at the project sample rate (%d Hz).\n\n"
        L"Convert them now? Playback will stop. The original WAV files on disk will not be modified — only the in-memory audio used by this project.",
        needCount, totalCount, dstSR);
    if (MessageBoxW(hwnd, prompt, L"Convert Imported Audio", MB_OKCANCEL | MB_ICONQUESTION) != IDOK) {
        return;
    }

    // Stop playback so the audio thread isn't iterating while we resample.
    if (state.audio.playing) {
        StopPlayback(state, false);
    }

    int converted = 0;
    int failed = 0;
    EnterCriticalSection(&state.audio.audioStateLock);
    for (LoadedAudio& a : state.core.project.audio) {
        if (a.sampleRate <= 0 || a.sampleRate == dstSR || a.frames == 0) continue;
        const std::uint64_t dstFrames64 =
            (static_cast<std::uint64_t>(a.frames) * static_cast<std::uint64_t>(dstSR)
             + static_cast<std::uint64_t>(a.sampleRate) / 2)
            / static_cast<std::uint64_t>(a.sampleRate);
        if (dstFrames64 == 0 || dstFrames64 > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
            ++failed;
            continue;
        }
        const int dstFrames = static_cast<int>(dstFrames64);
        std::vector<float> resampled(static_cast<size_t>(dstFrames) * 2, 0.0f);
        ResampleStereoFloatSincHQ(a.stereo.data(), static_cast<int>(a.frames), a.sampleRate,
                                  resampled.data(), dstFrames, dstSR);
        a.stereo     = std::move(resampled);
        a.frames     = static_cast<std::uint32_t>(dstFrames);
        a.sampleRate = dstSR;
        a.peakSummary.clear(); // force redraw cache rebuild
        ++converted;
    }
    LeaveCriticalSection(&state.audio.audioStateLock);

    state.core.projectModified = true;
    UpdateWindowTitle(hwnd, state.core);

    wchar_t done[256]{};
    if (failed == 0) {
        swprintf_s(done, L"Converted %d clip(s) to %d Hz.", converted, dstSR);
        MessageBoxW(hwnd, done, L"Convert Audio", MB_OK | MB_ICONINFORMATION);
    } else {
        swprintf_s(done, L"Converted %d clip(s) to %d Hz.\n%d clip(s) could not be converted (invalid size).",
                   converted, dstSR, failed);
        MessageBoxW(hwnd, done, L"Convert Audio", MB_OK | MB_ICONWARNING);
    }
    InvalidateRect(hwnd, nullptr, FALSE);
}

// ── Audio orchestration layer ─────────────────────────────────────────────────
// These functions coordinate both the MME and WASAPI backends.

void StopPlayback(AppState& state, bool rewind) {
    DeviceStopPlaybackBackend(state);

    state.audio.playing = false;
    state.audio.audioThreadRunning.store(false);
    if (rewind) {
        state.ui.playheadBeat = 0.0f;
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
                if (state.ui.hwnd) UpdateWindowTitle(state.ui.hwnd, state.core);
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

// ── Dock-aware layout for hit-tests ─────────────────────────────────────────
// FindDockLeafRect / ComputeHitTestLayout were hoisted to ui/layout.{h,cpp}
// (UiLayoutFindDockLeafRect / UiLayoutComputeHitTestLayout) so per-message
// handler files extracted from WindowProc can share the same dock-aware
// layout without depending on main.cpp internals.

// ── Tab drag drop-target resolution (Phase 2.2b) ────────────────────────────
// Activate-distance threshold for promoting a tab click into a tab drag.
constexpr int kDragTabThresholdPx = 4;

// Hit-test `pt` against current dockLayout. Writes the resolved drop target
// to `state.ui.drop*` fields and returns true if a target was found.
//
// Per-leaf zones (Unity/VS-style):
//   * Outer 1/4 band on each edge → split (Left/Right/Top/Bottom).
//   * Center → tab insert at the cursor's X within the leaf's tab strip
//     (or end-of-list if past the last tab). Forbidden on primary leaves.
// Non-static so ui/wndproc/mousemove.cpp can forward-declare and call it.
bool ResolveDropTarget(AppState& state, POINT pt) {
    state.ui.dropTargetLeaf  = nullptr;
    state.ui.dropTargetSide  = daw::ui::DockDropSide::Center;
    state.ui.dropTargetTabAt = -1;
    state.ui.dropPreviewRect = RECT{0, 0, 0, 0};

    // ── Outer drop zone (split the root) ────────────────────────────────
    // Compute the dock area as the union of all current leaves. A thin band
    // along each edge means "split the WHOLE dock against this side", so a
    // panel can be pinned to the full bottom (under both Tracks AND Arrange)
    // by dropping it on the bottom outer band — without first needing a
    // single leaf that already spans the full width.
    if (!state.ui.dockLayout.empty() && state.ui.dockRoot) {
        RECT dockBounds = state.ui.dockLayout.front().rect;
        for (const auto& leaf : state.ui.dockLayout) {
            dockBounds.left   = std::min<LONG>(dockBounds.left,   leaf.rect.left);
            dockBounds.top    = std::min<LONG>(dockBounds.top,    leaf.rect.top);
            dockBounds.right  = std::max<LONG>(dockBounds.right,  leaf.rect.right);
            dockBounds.bottom = std::max<LONG>(dockBounds.bottom, leaf.rect.bottom);
        }
        const int outerBand = Dpi(16);
        if (PtInRect(&dockBounds, pt)) {
            daw::ui::DockDropSide outer = daw::ui::DockDropSide::Center;
            if      (pt.x < dockBounds.left   + outerBand) outer = daw::ui::DockDropSide::Left;
            else if (pt.x > dockBounds.right  - outerBand) outer = daw::ui::DockDropSide::Right;
            else if (pt.y < dockBounds.top    + outerBand) outer = daw::ui::DockDropSide::Top;
            else if (pt.y > dockBounds.bottom - outerBand) outer = daw::ui::DockDropSide::Bottom;
            if (outer != daw::ui::DockDropSide::Center) {
                state.ui.dropTargetLeaf = state.ui.dockRoot.get();
                state.ui.dropTargetSide = outer;
                const int w = dockBounds.right  - dockBounds.left;
                const int h = dockBounds.bottom - dockBounds.top;
                RECT preview = dockBounds;
                if      (outer == daw::ui::DockDropSide::Left)   preview.right  = dockBounds.left   + w / 4;
                else if (outer == daw::ui::DockDropSide::Right)  preview.left   = dockBounds.right  - w / 4;
                else if (outer == daw::ui::DockDropSide::Top)    preview.bottom = dockBounds.top    + h / 4;
                else /* Bottom */                                 preview.top    = dockBounds.bottom - h / 4;
                state.ui.dropPreviewRect = preview;
                return true;
            }
        }
    }

    for (const auto& leaf : state.ui.dockLayout) {
        if (!PtInRect(&leaf.rect, pt)) continue;
        const RECT r = leaf.rect;
        const int  w = r.right  - r.left;
        const int  h = r.bottom - r.top;
        const int  edge = std::min({w / 4, h / 4, Dpi(60)});

        // Don't allow a single-tab leaf to split against itself (no-op).
        const bool sameAsSource = (leaf.node == state.ui.dragTabSource);

        // Edge bands (split)
        daw::ui::DockDropSide side = daw::ui::DockDropSide::Center;
        if      (pt.x < r.left   + edge) side = daw::ui::DockDropSide::Left;
        else if (pt.x > r.right  - edge) side = daw::ui::DockDropSide::Right;
        else if (pt.y < r.top    + edge) side = daw::ui::DockDropSide::Top;
        else if (pt.y > r.bottom - edge) side = daw::ui::DockDropSide::Bottom;

        if (side != daw::ui::DockDropSide::Center) {
            if (sameAsSource && state.ui.dragTabSource != nullptr &&
                state.ui.dragTabSource->panels.size() <= 1) {
                // Splitting a leaf containing only the dragged tab against
                // itself would be a no-op; skip and let center handle it.
                side = daw::ui::DockDropSide::Center;
            } else {
                state.ui.dropTargetLeaf = leaf.node;
                state.ui.dropTargetSide = side;
                RECT preview = r;
                if      (side == daw::ui::DockDropSide::Left)   preview.right  = r.left   + w / 2;
                else if (side == daw::ui::DockDropSide::Right)  preview.left   = r.right  - w / 2;
                else if (side == daw::ui::DockDropSide::Top)    preview.bottom = r.top    + h / 2;
                else /* Bottom */                                preview.top    = r.bottom - h / 2;
                state.ui.dropPreviewRect = preview;
                return true;
            }
        }

        // Center (tab insert) — allowed on any leaf, including primary,
        // so users can dock new tabs alongside Ruler/Tracks/Arrange.
        state.ui.dropTargetLeaf = leaf.node;
        state.ui.dropTargetSide = daw::ui::DockDropSide::Center;

        // Find insertion index by scanning the tabs that belong to this leaf.
        int insertAt = static_cast<int>(leaf.node->panels.size());
        for (const auto& tab : state.ui.dockTabs) {
            if (tab.node != leaf.node) continue;
            const int mid = (tab.rect.left + tab.rect.right) / 2;
            if (pt.x < mid) { insertAt = tab.tabIndex; break; }
        }
        // If dragging within the same leaf, account for the tab being removed
        // before re-insertion so the visual index matches.
        if (sameAsSource && insertAt > state.ui.dragTabIndex) insertAt -= 1;
        state.ui.dropTargetTabAt = insertAt;

        // Center preview = full leaf rect (signals "join this leaf as a tab").
        // Edge previews fill half the leaf, so the two are visually distinct.
        state.ui.dropPreviewRect = r;
        return true;
    }
    return false;
}

// ── Floating tear-off windows (Phase 4a) ────────────────────────────────────
// A floating window owns a single panel (no nested dock tree yet). Closing
// the window re-docks the panel back into the main DockNode tree so the
// panel is never lost. Mouse interaction inside floating windows is not
// wired in 4a — only paint + close. Phase 4b will route mouse messages
// through a per-panel hit dispatcher so the panel is fully usable while
// floating.

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);


LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<AppState*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));

    switch (msg) {
    case WM_CREATE: {
        // Pick up the monitor's DPI before the first paint so all layout math
        // produces correctly-sized rects on HiDPI displays from frame zero.
        g_uiDpi = static_cast<int>(GetDpiForWindow(hwnd));
        if (g_uiDpi <= 0) g_uiDpi = 96;

        auto* initial = new AppState();
        initial->ui.hwnd = hwnd;
        // Single-call audio engine bring-up: device enumeration, SR probe,
        // critical-section init, and engineState transition to Ready.
        AudioInitializeRuntime(hwnd, initial->core, initial->audio);
        SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(initial));

        // Attach the native Win32 menu bar. SetMenu() takes ownership of the
        // HMENU; Windows destroys it when the window is destroyed.
        if (HMENU bar = BuildMainMenuBar(*initial); bar != nullptr) {
            SetMenu(hwnd, bar);
            DrawMenuBar(hwnd);
        }

        SetTimer(hwnd, kPlaybackTimerId, kPlaybackTimerMs, nullptr);
        return 0;
    }
    case WM_SIZE: {
        // Default WM_SIZE only invalidates newly-exposed area, so when you
        // maximize / restore / drag-resize the window the existing content
        // (Arrange grid, ruler, etc.) shows stretched stale pixels until
        // the next click triggers a repaint. Force a full redraw on every
        // size change so the layout walker re-runs against the new client.
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_DPICHANGED: {
        // Per-monitor DPI v2: Windows tells us the new DPI in HIWORD(wParam)
        // and a recommended new bounding rect in lParam (already adjusted for
        // the new scale on the destination monitor).
        g_uiDpi = HIWORD(wParam);
        if (g_uiDpi <= 0) g_uiDpi = 96;
        const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
        if (suggested != nullptr) {
            SetWindowPos(hwnd, nullptr,
                         suggested->left, suggested->top,
                         suggested->right  - suggested->left,
                         suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_INITMENUPOPUP: {
        // Refresh dynamic content (device list, current sample rate / backend
        // / buffer-size checkmarks) just before each top-level popup opens.
        // HIWORD(lParam) is non-zero for the system (window) menu, which we
        // skip so Windows can render the standard window menu.
        if (HIWORD(lParam) != 0 || state == nullptr) {
            return DefWindowProc(hwnd, msg, wParam, lParam);
        }
        HMENU popup = reinterpret_cast<HMENU>(wParam);
        if (RefreshTopLevelPopup(popup, *state)) {
            return 0;
        }
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    case WM_COMMAND: {
        // Native menu items dispatch here. notification code (HIWORD) is 0 for
        // menu items, 1 for accelerators; we treat both the same way.
        if (state == nullptr) {
            return DefWindowProc(hwnd, msg, wParam, lParam);
        }
        const UINT cmd = LOWORD(wParam);
        HandleMenuCommand(hwnd, *state, cmd);
        return 0;
    }
    case WM_DESTROY:
        KillTimer(hwnd, kPlaybackTimerId);
        if (state != nullptr) {
            // Persist the dock layout AND the geometry of every floating
            // panel before tearing down so the next launch reopens with
            // the same arrangement of panels, tabs, splitter ratios, and
            // torn-off windows.
            if (state->ui.dockRoot) {
                std::vector<daw::ui::DockFloatingPanel> floats;
                floats.reserve(state->ui.floatingPanels.size());
                for (const auto& fp : state->ui.floatingPanels) {
                    if (!fp.hwnd || !IsWindow(fp.hwnd)) continue;
                    RECT wr{};
                    if (!GetWindowRect(fp.hwnd, &wr)) continue;
                    daw::ui::DockFloatingPanel out{};
                    out.panel = fp.panel;
                    out.x = wr.left;
                    out.y = wr.top;
                    out.w = wr.right  - wr.left;
                    out.h = wr.bottom - wr.top;
                    floats.push_back(out);
                }
                daw::ui::DockSaveLayout(state->ui.dockRoot.get(), floats);
            }
            StopRecording(*state, false);
            if (state->audio.automixThread != nullptr) {
                WaitForSingleObject(state->audio.automixThread, INFINITE);
                CloseHandle(state->audio.automixThread);
                state->audio.automixThread = nullptr;
            }
            StopPlayback(*state, false);
            DeleteCriticalSection(&state->audio.audioStateLock);
            delete state;
        }
        PostQuitMessage(0);
        return 0;
    case kMsgPlaybackFinished:
        if (state != nullptr) {
            // Engine signaled natural end-of-song. Route through the FSM so
            // it stays the single source of truth for transport transitions.
            // From Recording → StopRecording (commits take + stops). From
            // Playing/CountingIn → StopPlayback / StopRecording respectively.
            // From Stopped (race) → no-op.
            daw::app::DispatchTransportEvent(hwnd, *state,
                daw::services::TransportEvent::StopPressed,
                /*rewindOnStop=*/false);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case kMsgCountInComplete:
        if (state != nullptr) {
            // Engine signaled the count-in preroll has elapsed. The audio
            // thread already cleared audio.countingIn for tight timing; this
            // message lets the FSM observe the CountingIn → Recording
            // transition. The resulting StartRecording action is idempotent
            // (recording was armed at the start of count-in). Repaint so the
            // transport buttons reflect the new state.
            daw::app::DispatchTransportEvent(hwnd, *state,
                daw::services::TransportEvent::CountInComplete,
                /*rewindOnStop=*/false);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case kMsgAutoMixFinished:
        if (state != nullptr) {
            if (state->audio.automixThread != nullptr) {
                CloseHandle(state->audio.automixThread);
                state->audio.automixThread = nullptr;
            }
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_TIMER:
        if (state != nullptr && wParam == kPlaybackTimerId) {
            return WndProcOnPlaybackTimer(hwnd, *state);
        }
        return 0;
    case WM_KEYDOWN:
        if (state == nullptr) return 0;
        return WndProcOnKeyDown(hwnd, wParam, *state);
    case WM_LBUTTONDOWN:
        if (state == nullptr) {
            return 0;
        }
        {
            const POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            // (File/View/Audio/Track tab hit-tests removed: those menus are now
            //  served by the native Win32 menu bar attached via SetMenu().)

            // Floating tear-off windows reuse this WindowProc for panel
            // hit-testing (rects were set in floating client coords during
            // the floating WM_PAINT). They have no dock tree of their own,
            // so skip splitter / tab hit-tests in that case to avoid
            // spurious matches against the main window's dock layout.
            const bool isMainHwnd = (hwnd == state->ui.hwnd);

            // ── Dock splitter drag start ────────────────────────────────
            // Check splitters before anything else so the user can grab a
            // divider even if its hit zone overlaps a panel's controls.
            if (isMainHwnd) {
                for (const auto& sp : state->ui.dockSplitters) {
                    if (PtInRect(&sp.rect, pt)) {
                        state->ui.draggingSplitter       = true;
                        state->ui.dragSplitterNode       = sp.node;
                        state->ui.dragSplitterHorizontal = sp.horizontal;
                        SetCapture(hwnd);
                        return 0;
                    }
                }
            }

            // ── Dock tab click → activate that tab + arm potential drag ──
            if (isMainHwnd) {
                for (const auto& tab : state->ui.dockTabs) {
                    if (PtInRect(&tab.rect, pt)) {
                        if (tab.node != nullptr) {
                            tab.node->activeTab = tab.tabIndex;
                            // Arm a tab drag — promoted to active in WM_MOUSEMOVE
                            // once cursor moves past kDragTabThresholdPx. Primary
                            // panels can't be dragged out of their leaf (they're
                            // pinned to the layout), but can still be clicked to
                            // activate when sharing a tab strip with others.
                            const daw::ui::PanelKind pk =
                                tab.node->panels[static_cast<size_t>(tab.tabIndex)];
                            if (!daw::ui::PanelGet(pk).primary) {
                                state->ui.dragTabArmed   = true;
                                state->ui.dragTabActive  = false;
                                state->ui.dragTabSource  = tab.node;
                                state->ui.dragTabIndex   = tab.tabIndex;
                                state->ui.dragTabPanel   = pk;
                                state->ui.dragTabStartPt = pt;
                                state->ui.dragTabCurPt   = pt;
                                SetCapture(hwnd);
                            }
                            InvalidateRect(hwnd, nullptr, FALSE);
                        }
                        return 0;
                    }
                }
            }

            if (PtInRect(&state->ui.playRect, pt)) {
                using daw::services::TransportEvent;
                const auto ev = state->audio.playing ? TransportEvent::StopPressed
                                                     : TransportEvent::PlayPressed;
                daw::app::DispatchTransportEvent(hwnd, *state, ev, /*rewindOnStop=*/false);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (PtInRect(&state->ui.stopRect, pt)) {
                // Stop button always rewinds.
                daw::app::DispatchTransportEvent(hwnd, *state,
                    daw::services::TransportEvent::StopPressed, /*rewindOnStop=*/true);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (PtInRect(&state->ui.recordRect, pt)) {
                using daw::services::TransportEvent;
                TransportEvent ev;
                if (state->audio.recording || state->audio.countingIn) {
                    ev = TransportEvent::StopPressed;
                } else if (daw::app::WillCountIn(state->audio)) {
                    ev = TransportEvent::RecordPressedWithCountIn;
                } else {
                    ev = TransportEvent::RecordPressed;
                }
                daw::app::DispatchTransportEvent(hwnd, *state, ev, /*rewindOnStop=*/true);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (PtInRect(&state->ui.importRect, pt)) {
                ImportWavFiles(hwnd, *state);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (PtInRect(&state->ui.automixRect, pt)) {
                StartAutoMixAsync(hwnd, *state);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (PtInRect(&state->ui.vocalCheckRect, pt)) {
                AnalyzeSelectedTrackQuality(hwnd, *state);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (PtInRect(&state->ui.autoMasterRect, pt)) {
                DoAutoMaster(hwnd, *state);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (PtInRect(&state->ui.metPlayRect, pt)) {
                state->audio.metronomePlay = !state->audio.metronomePlay;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (PtInRect(&state->ui.metRecRect, pt)) {
                state->audio.metronomeRecord = !state->audio.metronomeRecord;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (PtInRect(&state->ui.monitorRect, pt)) {
                state->audio.inputMonitoring = !state->audio.inputMonitoring;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (PtInRect(&state->ui.bpmDownRect, pt)) {
                const bool coarse = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                state->core.project.bpm = static_cast<float>(std::max(40, static_cast<int>(state->core.project.bpm) - (coarse ? 5 : 1)));
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (PtInRect(&state->ui.bpmUpRect, pt)) {
                const bool coarse = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                state->core.project.bpm = static_cast<float>(std::min(260, static_cast<int>(state->core.project.bpm) + (coarse ? 5 : 1)));
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (PtInRect(&state->ui.countInRect, pt)) {
                state->audio.countInEnabled = !state->audio.countInEnabled;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }

            RECT client{};
            GetClientRect(hwnd, &client);
            const LayoutRects layout = UiLayoutComputeHitTestLayout(hwnd, *state);

            // ── Insert inspector click handling ──────────────────────────
            if (state->ui.fxInspectorOpen && state->ui.fxInspectorIndex >= 0) {
                const RECT inspPanel = UiDrawGetInspectorPanelRect(client, *state);
                if (!PtInRect(&inspPanel, pt)) {
                    // Click outside → close
                    state->ui.fxInspectorOpen = false;
                    InvalidateRect(hwnd, nullptr, FALSE);
                    // Don't return - let the click fall through to normal handling
                } else {
                    // Click inside inspector - handle controls
                    const int idx = state->ui.fxInspectorIndex;
                    int* pSlots       = nullptr;
                    InsertEffectArray* pEffects = nullptr;
                    InsertBypassArray* pBypass  = nullptr;
                    InsertConfigArray* pParams  = nullptr;
                    if (state->ui.fxInspectorIsTrack) {
                        if (idx < static_cast<int>(state->core.project.tracks.size()))
                            pSlots = &state->core.project.tracks[static_cast<size_t>(idx)].insertSlots;
                        if (idx < static_cast<int>(state->core.project.tracks.size()))
                            pEffects = &state->core.project.tracks[static_cast<size_t>(idx)].insertEffects;
                        if (idx < static_cast<int>(state->core.project.tracks.size()))
                            pBypass = &state->core.project.tracks[static_cast<size_t>(idx)].insertBypass;
                        if (idx < static_cast<int>(state->core.project.tracks.size()))
                            pParams = &state->core.project.tracks[static_cast<size_t>(idx)].insertConfig;
                    } else {
                        if (idx < static_cast<int>(state->core.project.buses.size()))
                            pSlots = &state->core.project.buses[static_cast<size_t>(idx)].insertSlots;
                        if (idx < static_cast<int>(state->core.project.buses.size()))
                            pEffects = &state->core.project.buses[static_cast<size_t>(idx)].insertEffects;
                        if (idx < static_cast<int>(state->core.project.buses.size()))
                            pBypass = &state->core.project.buses[static_cast<size_t>(idx)].insertBypass;
                        if (idx < static_cast<int>(state->core.project.buses.size()))
                            pParams = &state->core.project.buses[static_cast<size_t>(idx)].insertConfig;
                    }

                    const int slotCount = pSlots ? std::clamp(*pSlots, 0, kMaxInsertSlots) : 0;

                    // Close button
                    RECT closeBtn{inspPanel.right - 24, inspPanel.top + 4, inspPanel.right - 4, inspPanel.top + kUiDrawInspHeaderH - 4};
                    if (PtInRect(&closeBtn, pt)) {
                        state->ui.fxInspectorOpen = false;
                        InvalidateRect(hwnd, nullptr, FALSE);
                        return 0;
                    }

                    // ADD / REM buttons
                    const int ctrlTop = inspPanel.top + kUiDrawInspHeaderH;
                    RECT addBtn{inspPanel.left + 6,  ctrlTop + 4, inspPanel.left + 66,  ctrlTop + kUiDrawInspCtrlH - 4};
                    RECT remBtn{inspPanel.left + 72, ctrlTop + 4, inspPanel.left + 132, ctrlTop + kUiDrawInspCtrlH - 4};
                    if (PtInRect(&addBtn, pt) && pSlots && slotCount < kMaxInsertSlots) {
                        EnterCriticalSection(&state->audio.audioStateLock);
                        (*pSlots)++;
                        state->core.projectModified = true;
                        UpdateWindowTitle(hwnd, state->core);
                        LeaveCriticalSection(&state->audio.audioStateLock);
                        InvalidateRect(hwnd, nullptr, FALSE);
                        return 0;
                    }
                    if (PtInRect(&remBtn, pt) && pSlots && slotCount > 0) {
                        EnterCriticalSection(&state->audio.audioStateLock);
                        (*pSlots)--;
                        // Clear removed slot's bypass so it doesn't persist
                        if (pBypass) (*pBypass)[static_cast<size_t>(*pSlots)] = false;
                        state->core.projectModified = true;
                        UpdateWindowTitle(hwnd, state->core);
                        LeaveCriticalSection(&state->audio.audioStateLock);
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
                            EnterCriticalSection(&state->audio.audioStateLock);
                            std::uint8_t& fx = (*pEffects)[static_cast<size_t>(s)];
                            fx = static_cast<std::uint8_t>((fx + 1) % kInsertEffectTypeCount);
                            state->core.projectModified = true;
                            UpdateWindowTitle(hwnd, state->core);
                            LeaveCriticalSection(&state->audio.audioStateLock);
                            InvalidateRect(hwnd, nullptr, FALSE);
                            return 0;
                        }
                        if (PtInRect(&bypassBtn, pt) && pBypass) {
                            EnterCriticalSection(&state->audio.audioStateLock);
                            (*pBypass)[static_cast<size_t>(s)] = !(*pBypass)[static_cast<size_t>(s)];
                            state->core.projectModified = true;
                            UpdateWindowTitle(hwnd, state->core);
                            LeaveCriticalSection(&state->audio.audioStateLock);
                            InvalidateRect(hwnd, nullptr, FALSE);
                            return 0;
                        }
                        if (PtInRect(&arrowBtn, pt)) {
                            state->ui.fxInspectorSelectedSlot = (state->ui.fxInspectorSelectedSlot == s) ? -1 : s;
                            InvalidateRect(hwnd, nullptr, FALSE);
                            return 0;
                        }
                    }

                    // Param knob clicks in the expanded strip
                    if (state->ui.fxInspectorSelectedSlot >= 0 && state->ui.fxInspectorSelectedSlot < slotCount && pParams && pEffects) {
                        const int selSlot = state->ui.fxInspectorSelectedSlot;
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
                                state->ui.draggingParamKnob = true;
                                state->ui.paramKnobParamId  = knobs[k].paramId * 100 + selSlot;  // encode slot in lower 2 digits
                                state->ui.paramKnobDragStartY   = pt.y;
                                state->ui.paramKnobDragStartVal = curVal;
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
                state->ui.playheadBeat = UiLayoutSnapBeat(beat);
                state->ui.draggingPlayhead = true;
                SetCapture(hwnd);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }

            // Tracks panel + Buses panel hit-tests. Inner blocks gate by
            // their own panel's rect, so it's safe to enter on any click in
            // either leaf (the leaf rects are siblings in the dock tree).
            const bool inTracksLeaf = (PtInRect(&layout.leftPanel, pt) && pt.y > layout.leftPanel.top + Dpi(kRulerHeight));
            bool inBusesLeaf = false;
            for (const auto& leaf : state->ui.dockLayout) {
                if (leaf.activePanel == daw::ui::PanelKind::Buses && PtInRect(&leaf.rect, pt)) {
                    inBusesLeaf = true;
                    break;
                }
            }
            if ((inTracksLeaf || inBusesLeaf) && !state->core.project.tracks.empty()) {
                const bool inTracksRegion = inTracksLeaf && (pt.y < UiLayoutTracksRegionBottom(layout.leftPanel));
                const int trackIndex = UiLayoutTrackIndexFromY(layout.arrange, *state, pt.y);
                if (inTracksRegion && trackIndex >= 0 && trackIndex < static_cast<int>(state->core.project.tracks.size())) {
                    state->ui.selectedTrackIndex = trackIndex;
                    state->ui.selectedClipIndex = -1;

                    RECT busRect{};
                    RECT panKnobRect{};
                    RECT panValRect{};
                    RECT fxRect{};
                    UiLayoutGetTrackRoutingRects(layout.leftPanel, trackIndex, &busRect, &panKnobRect, &panValRect, &fxRect, state->ui.tracksScrollY);
                    if (PtInRect(&busRect, pt)) {
                        EnterCriticalSection(&state->audio.audioStateLock);
                        if (trackIndex < static_cast<int>(state->core.project.tracks.size())) {
                            const int cur = TrackBusIndexAt(*state, trackIndex);
                            state->core.project.tracks[static_cast<size_t>(trackIndex)].busIndex = (cur + 1) % kBusCount;
                            state->core.projectModified = true;
                            UpdateWindowTitle(hwnd, state->core);
                        }
                        LeaveCriticalSection(&state->audio.audioStateLock);
                        InvalidateRect(hwnd, nullptr, FALSE);
                        return 0;
                    }
                    if (PtInRect(&fxRect, pt)) {
                        // Open insert-chain inspector for this track
                        state->ui.fxInspectorOpen    = true;
                        state->ui.fxInspectorIsTrack = true;
                        state->ui.fxInspectorIndex   = trackIndex;
                        InvalidateRect(hwnd, nullptr, FALSE);
                        return 0;
                    }
                    if (PtInRect(&panKnobRect, pt) || PtInRect(&panValRect, pt)) {
                        if (PtInRect(&panValRect, pt) && (GetKeyState(VK_LBUTTON) & 0x8000)) {
                            // Double-click on value label resets to center
                            EnterCriticalSection(&state->audio.audioStateLock);
                            if (trackIndex < static_cast<int>(state->core.project.tracks.size()))
                                state->core.project.tracks[static_cast<size_t>(trackIndex)].pan = 0.0f;
                            state->core.projectModified = true;
                            UpdateWindowTitle(hwnd, state->core);
                            LeaveCriticalSection(&state->audio.audioStateLock);
                        } else if (trackIndex < static_cast<int>(state->core.project.tracks.size())) {
                            // Start drag
                            state->ui.draggingPan    = true;
                            state->ui.dragPanIsBus   = false;
                            state->ui.dragPanIndex   = trackIndex;
                            state->ui.dragPanStartY  = pt.y;
                            state->ui.dragPanStartVal = state->core.project.tracks[static_cast<size_t>(trackIndex)].pan;
                            SetCapture(hwnd);
                        }
                        InvalidateRect(hwnd, nullptr, FALSE);
                        return 0;
                    }

                    RECT muteRect{};
                    RECT soloRect{};
                    RECT recRect{};
                    UiLayoutGetTrackButtonRects(layout.leftPanel, trackIndex, &muteRect, &soloRect, &recRect, state->ui.tracksScrollY);
                    if (PtInRect(&muteRect, pt)) {
                        EnterCriticalSection(&state->audio.audioStateLock);
                        if (trackIndex < static_cast<int>(state->core.project.tracks.size())) {
                            state->core.project.tracks[static_cast<size_t>(trackIndex)].mute = !state->core.project.tracks[static_cast<size_t>(trackIndex)].mute;
                        }
                        LeaveCriticalSection(&state->audio.audioStateLock);
                        InvalidateRect(hwnd, nullptr, FALSE);
                        return 0;
                    }
                    if (PtInRect(&recRect, pt)) {
                        EnterCriticalSection(&state->audio.audioStateLock);
                        if (trackIndex < static_cast<int>(state->core.project.tracks.size())) {
                            state->core.project.tracks[static_cast<size_t>(trackIndex)].recordArm = !state->core.project.tracks[static_cast<size_t>(trackIndex)].recordArm;
                        }
                        LeaveCriticalSection(&state->audio.audioStateLock);
                        InvalidateRect(hwnd, nullptr, FALSE);
                        return 0;
                    }
                    if (PtInRect(&soloRect, pt)) {
                        EnterCriticalSection(&state->audio.audioStateLock);
                        if (trackIndex < static_cast<int>(state->core.project.tracks.size())) {
                            state->core.project.tracks[static_cast<size_t>(trackIndex)].solo = !state->core.project.tracks[static_cast<size_t>(trackIndex)].solo;
                        }
                        LeaveCriticalSection(&state->audio.audioStateLock);
                        InvalidateRect(hwnd, nullptr, FALSE);
                        return 0;
                    }

                    RECT rail{};
                    RECT knob{};
                    UiLayoutGetTrackFaderRects(layout.leftPanel, trackIndex, &rail, &knob, state->ui.tracksScrollY);
                    RECT hitRect{rail.left - 12, rail.top, rail.right + 12, rail.bottom};
                    if (PtInRect(&hitRect, pt)) {
                        PushUndo(*state);
                        state->ui.draggingFader = true;
                        state->ui.dragFaderTrack = trackIndex;
                        state->ui.dragFaderStartY = pt.y;
                        EnterCriticalSection(&state->audio.audioStateLock);
                        state->ui.dragFaderStartDb = UiLayoutGainFromFaderY(rail, pt.y);
                        state->core.project.tracks[static_cast<size_t>(trackIndex)].gainDb = state->ui.dragFaderStartDb;
                        LeaveCriticalSection(&state->audio.audioStateLock);
                        SetCapture(hwnd);
                        InvalidateRect(hwnd, nullptr, FALSE);
                        return 0;
                    }
                }

                // Bus hit-test: use the Buses dock leaf's actual rect so it
                // works regardless of where the panel has been moved/sized.
                RECT busPanelRect{};
                bool hasBusPanel = false;
                for (const auto& leaf : state->ui.dockLayout) {
                    if (leaf.activePanel == daw::ui::PanelKind::Buses) {
                        busPanelRect = leaf.rect;
                        hasBusPanel = true;
                        break;
                    }
                }
                const int busTop = hasBusPanel ? (busPanelRect.top + Dpi(kBusPanelTopMargin))
                                               : (layout.leftPanel.bottom - Dpi(kBusPanelHeight) + Dpi(kBusPanelTopMargin));
                if (hasBusPanel && PtInRect(&busPanelRect, pt) && pt.y >= busTop + Dpi(kBusPanelHeaderHeight)) {
                    for (int b = 0; b < kBusCount; ++b) {
                        RECT rowRect{};
                        RECT muteRect{};
                        RECT gainDownRect{};
                        RECT gainUpRect{};
                        RECT panKnobRect{};
                        RECT panValRect{};
                        RECT fxRect{};
                        UiLayoutGetBusControlRectsInPanel(busPanelRect, b, &rowRect, &muteRect, &gainDownRect, &gainUpRect, &panKnobRect, &panValRect, &fxRect);                        if (!PtInRect(&rowRect, pt)) {
                            continue;
                        }

                        EnterCriticalSection(&state->audio.audioStateLock);
                        if (PtInRect(&muteRect, pt) && b < static_cast<int>(state->core.project.buses.size())) {
                            state->core.project.buses[static_cast<size_t>(b)].mute = !state->core.project.buses[static_cast<size_t>(b)].mute;
                        } else if (PtInRect(&gainDownRect, pt) && b < static_cast<int>(state->core.project.buses.size())) {
                            state->core.project.buses[static_cast<size_t>(b)].gainDb = std::max(kFaderMinDb, state->core.project.buses[static_cast<size_t>(b)].gainDb - 1.0f);
                        } else if (PtInRect(&gainUpRect, pt) && b < static_cast<int>(state->core.project.buses.size())) {
                            state->core.project.buses[static_cast<size_t>(b)].gainDb = std::min(kFaderMaxDb, state->core.project.buses[static_cast<size_t>(b)].gainDb + 1.0f);
                        } else if ((PtInRect(&panKnobRect, pt) || PtInRect(&panValRect, pt)) && b < static_cast<int>(state->core.project.buses.size())) {
                            LeaveCriticalSection(&state->audio.audioStateLock);
                            state->ui.draggingPan    = true;
                            state->ui.dragPanIsBus   = true;
                            state->ui.dragPanIndex   = b;
                            state->ui.dragPanStartY  = pt.y;
                            state->ui.dragPanStartVal = state->core.project.buses[static_cast<size_t>(b)].pan;
                            SetCapture(hwnd);
                            InvalidateRect(hwnd, nullptr, FALSE);
                            return 0;
                        } else if (PtInRect(&fxRect, pt) && b < static_cast<int>(state->core.project.buses.size())) {
                            // Open insert-chain inspector for this bus
                            LeaveCriticalSection(&state->audio.audioStateLock);
                            state->ui.fxInspectorOpen    = true;
                            state->ui.fxInspectorIsTrack = false;
                            state->ui.fxInspectorIndex   = b;
                            InvalidateRect(hwnd, nullptr, FALSE);
                            return 0;
                        }
                        state->core.projectModified = true;
                        UpdateWindowTitle(hwnd, state->core);
                        LeaveCriticalSection(&state->audio.audioStateLock);

                        InvalidateRect(hwnd, nullptr, FALSE);
                        return 0;
                    }
                }
            }

            if (PtInRect(&layout.arrange, pt)) {
                state->ui.selectedClipIndex = -1;
                for (int i = static_cast<int>(state->core.project.clips.size()) - 1; i >= 0; --i) {
                    RECT r{};
                    if (!UiLayoutClipRectForDraw(layout.arrange, *state, state->core.project.clips[static_cast<size_t>(i)], &r)) {
                        continue;
                    }
                    if (PtInRect(&r, pt)) {
                        state->ui.selectedClipIndex = i;
                        state->ui.selectedTrackIndex = state->core.project.clips[static_cast<size_t>(i)].trackIndex;

                        constexpr int kEdgeThresh = 7;
                        const int fullLeft  = UiLayoutBeatToX(layout.arrange, *state, state->core.project.clips[static_cast<size_t>(i)].startBeat);
                        const int fullRight = UiLayoutBeatToX(layout.arrange, *state, state->core.project.clips[static_cast<size_t>(i)].startBeat + state->core.project.clips[static_cast<size_t>(i)].lengthBeats);
                        const bool nearLeft  = (pt.x - fullLeft)  <= kEdgeThresh && (pt.x - fullLeft)  >= 0;
                        const bool nearRight = (fullRight - pt.x)  <= kEdgeThresh && (fullRight - pt.x) >= 0;

                        if (nearLeft || nearRight) {
                            // Trim
                            state->ui.trimmingClip         = true;
                            state->ui.trimClipIndex        = i;
                            state->ui.trimIsLeft           = nearLeft;
                            state->ui.trimOrigStart        = state->core.project.clips[static_cast<size_t>(i)].startBeat;
                            state->ui.trimOrigLen          = state->core.project.clips[static_cast<size_t>(i)].lengthBeats;
                            state->ui.trimOrigSourceOffset = state->core.project.clips[static_cast<size_t>(i)].sourceOffsetFrames;
                            PushUndo(*state);
                            SetCapture(hwnd);
                        } else {
                            // Drag
                            PushUndo(*state);
                            state->ui.draggingClip = true;
                            state->ui.dragClipIndex = i;
                            state->ui.dragOffsetBeats = UiLayoutXToBeat(layout.arrange, *state, pt.x) - state->core.project.clips[static_cast<size_t>(i)].startBeat;
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
        if (state == nullptr) return 0;
        return WndProcOnRButtonUp(hwnd, lParam, *state);
    case WM_MOUSEMOVE:
        if (state == nullptr) return 0;
        return WndProcOnMouseMove(hwnd, lParam, *state);
    case WM_LBUTTONUP:
        if (state == nullptr) return 0;
        return WndProcOnLButtonUp(hwnd, *state);
    case WM_MOUSEWHEEL:
        if (state == nullptr) return 0;
        return WndProcOnMouseWheel(hwnd, wParam, lParam, *state);
    case WM_MOUSEHWHEEL:
        if (state == nullptr) return 0;
        return WndProcOnMouseHWheel(hwnd, wParam, *state);
    case WM_CAPTURECHANGED:
        if (state != nullptr) WndProcOnCaptureChanged(*state);
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

            // ── Top bar (still a fixed strip) ────────────────────────────
            UiDrawTopBar(memDc, client, *state);

            // ── Dock-driven paint of the body below the top bar ──────────
            // Lazily build the default dock tree on first paint. The dock
            // walker emits one entry per leaf + one per splitter so we can
            // both dispatch to the panel registry and render draggable
            // dividers.
            if (!state->ui.dockRoot) {
                // Try to restore the user's last layout from
                // %APPDATA%\DAW\layout.json. Falls back to the built-in
                // default if missing/invalid. Floating panels persisted
                // alongside the dock tree are spawned now (one per entry)
                // so a torn-off Mixer survives a restart in the same spot.
                daw::ui::DockLayoutDocument doc = daw::ui::DockLoadLayout();
                if (doc.root) {
                    state->ui.dockRoot = std::move(doc.root);
                    for (const auto& f : doc.floating) {
                        SpawnFloatingPanelAt(*state, f.panel,
                                             f.x, f.y, f.w, f.h,
                                             /*restoreOnFail=*/false);
                    }
                } else {
                    state->ui.dockRoot = daw::ui::DockBuildDefault();
                }
            }
            RECT bodyRect{client.left, client.top + Dpi(kTopBarHeight), client.right, client.bottom - Dpi(kStatusBarHeight)};
            state->ui.dockLayout.clear();
            state->ui.dockSplitters.clear();
            state->ui.dockTabs.clear();
            daw::ui::DockLayout(state->ui.dockRoot.get(), bodyRect,
                                state->ui.dockLayout, &state->ui.dockSplitters);

            // Render dock leaves (panel content + per-leaf tab strip) and
            // splitter dividers. Side effect: populates state.ui.dockTabs
            // for the tab hit-test in WM_LBUTTONDOWN.
            UiDrawDockLeavesAndSplitters(memDc, *state, smallFont);

            // ── Tab-drag drop preview (Phase 2.2c) ──────────────────────
            // Translucent accent fill over the resolved drop region plus a
            // Unity-style 5-position compass centered on the target leaf.
            // Self-contained GDI; lives in ui/draw.cpp.
            UiDrawDockDropOverlay(memDc, *state);

            // Inspector panel floats on top of everything
            if (state->ui.fxInspectorOpen)
                UiDrawInsertInspector(memDc, client, *state);

            // ── Status bar (fixed bottom strip, not dockable) ────────────
            {
                RECT statusRect{client.left,
                                client.bottom - Dpi(kStatusBarHeight),
                                client.right,
                                client.bottom};
                UiDrawStatusBar(memDc, statusRect, *state);
            }

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
    case WM_SETCURSOR: {
        // Show resize cursor when hovering a dock splitter or while dragging
        // one. For all other regions fall through to the default arrow.
        if (state != nullptr && reinterpret_cast<HWND>(wParam) == hwnd && LOWORD(lParam) == HTCLIENT) {
            POINT pt{};
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            for (const auto& sp : state->ui.dockSplitters) {
                if (PtInRect(&sp.rect, pt) || (state->ui.draggingSplitter && state->ui.dragSplitterNode == sp.node)) {
                    SetCursor(LoadCursor(nullptr, sp.horizontal ? IDC_SIZENS : IDC_SIZEWE));
                    return TRUE;
                }
            }
        }
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    default:
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR lpCmdLine, int nCmdShow) {
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

    // Phase 4 floating tear-off windows: separate WindowProc, same hInstance.
    WNDCLASS fwc{};
    fwc.lpfnWndProc   = FloatingWindowProc;
    fwc.hInstance     = hInstance;
    fwc.lpszClassName = kFloatingClassName;
    fwc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    fwc.hbrBackground = nullptr; // We paint the background ourselves.
    RegisterClass(&fwc);

    // Per-monitor DPI v2: each monitor reports its own DPI, WM_DPICHANGED fires
    // when the window crosses monitors with different scales (essential for the
    // multi-monitor / tear-off-window workflows planned for Phase 4).
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

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
        auto* initialState = reinterpret_cast<AppState*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
        if (initialState != nullptr) {
            EnterCriticalSection(&initialState->audio.audioStateLock);
            LoadProject(startupProjectPath, *initialState);
            LeaveCriticalSection(&initialState->audio.audioStateLock);
            if (initialState->audio.trackInsertDspState.size() != initialState->core.project.tracks.size()) initialState->audio.trackInsertDspState.resize(initialState->core.project.tracks.size());
            UpdateWindowTitle(hwnd, initialState->core);
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
