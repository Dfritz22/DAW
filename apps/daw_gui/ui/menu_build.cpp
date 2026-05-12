#include "ui/menu_build.h"

#include "ui/UiRuntimeState.h"   // kCmd* command IDs
#include "ui/dock.h"              // DockFindLeafContaining
#include "ui/panel.h"             // PanelKind, PanelCount, PanelGet
#include "daw_audio.h"            // DeviceRefreshInputDevices/OutputDevices

#include <algorithm>
#include <iterator>
#include <vector>
#include <cstdio>

// File-private cached HMENU handles for the five top-level popups so
// WM_INITMENUPOPUP can identify which one is opening (popup HMENU equality
// against these statics) and rebuild it from scratch.
static HMENU g_hMenuFile   = nullptr;
static HMENU g_hMenuView   = nullptr;
static HMENU g_hMenuAudio  = nullptr;
static HMENU g_hMenuTrack  = nullptr;
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

HMENU UiBuildMainMenuBar(AppState& state) {
    HMENU bar = CreateMenu();
    if (bar == nullptr) return nullptr;
    g_hMenuFile   = CreatePopupMenu();
    g_hMenuView   = CreatePopupMenu();
    g_hMenuAudio  = CreatePopupMenu();
    g_hMenuTrack  = CreatePopupMenu();
    g_hMenuWindow = CreatePopupMenu();
    PopulateFileMenu  (g_hMenuFile,   state);
    PopulateViewMenu  (g_hMenuView,   state);
    PopulateAudioMenu (g_hMenuAudio,  state);
    PopulateTrackMenu (g_hMenuTrack,  state);
    PopulateWindowMenu(g_hMenuWindow, state);
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(g_hMenuFile),   L"&File");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(g_hMenuView),   L"&View");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(g_hMenuAudio),  L"&Audio");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(g_hMenuTrack),  L"&Track");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(g_hMenuWindow), L"&Window");
    return bar;
}

bool UiRefreshTopLevelPopup(HMENU popup, AppState& state) {
    if (popup == g_hMenuFile)   { ClearMenu(popup); PopulateFileMenu  (popup, state); return true; }
    if (popup == g_hMenuView)   { ClearMenu(popup); PopulateViewMenu  (popup, state); return true; }
    if (popup == g_hMenuAudio)  { ClearMenu(popup); PopulateAudioMenu (popup, state); return true; }
    if (popup == g_hMenuTrack)  { ClearMenu(popup); PopulateTrackMenu (popup, state); return true; }
    if (popup == g_hMenuWindow) { ClearMenu(popup); PopulateWindowMenu(popup, state); return true; }
    return false;
}
