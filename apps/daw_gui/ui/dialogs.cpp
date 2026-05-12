#include "ui/dialogs.h"

#include "daw_audio.h"   // DeviceRefreshInputDevices/OutputDevices

#include <algorithm>
#include <iterator>
#include <vector>
#include <string>
#include <cstdio>
#include <cwchar>
#include <cstdlib>

// ============================================================
// Audio Settings Dialog
// ============================================================

namespace {

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

constexpr int kAsDlgBackend    = 1001;
constexpr int kAsDlgOutputDev  = 1002;
constexpr int kAsDlgInputDev   = 1003;
constexpr int kAsDlgSampleRate = 1004;
constexpr int kAsDlgBufferSize = 1005;
constexpr int kAsDlgStatus     = 1006;
constexpr int kAsDlgApply      = 1010;
// IDOK = 1, IDCANCEL = 2

void AsDlgReadFields(HWND hwnd, AudioSettingsDlgData& d) {
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

void AsDlgUpdateStatus(HWND hwnd, const AppState& state) {
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

LRESULT CALLBACK AudioSettingsDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
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

// ── Project Sample Rate dialog ───────────────────────────────────────────────
// Stop-gap UI for setting the project's sample rate. The project SR is a
// property of the project file (default 48000) and is independent of any
// audio device's sample rate. It can only be changed here. Eventually this
// will move into a proper Project Settings dialog and the New Project flow.

struct ProjectSampleRateDlgData {
    AppState* appState {nullptr};
    int       result   {0};   // 0 = cancelled, otherwise the new SR
};

constexpr int kPsrDlgEdit  = 2001;
constexpr int kPsrDlgCombo = 2002;

LRESULT CALLBACK ProjectSampleRateDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
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

}  // namespace

void UiShowAudioSettingsDialog(HWND hwndParent, AppState& state) {
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

void UiShowProjectSampleRateDialog(HWND hwndParent, AppState& state) {
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
