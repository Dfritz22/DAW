#include "ui/wndproc/command.h"

#include "core/internal_app_services.h"
#include "daw_audio.h"
#include "daw_project.h"
#include "ui/UiRuntimeState.h"
#include "ui/dialogs.h"
#include "ui/dock.h"
#include "ui/dock_persist.h"
#include "ui/panel.h"
#include "vm/timeline_zoom.h"

#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <iterator>
#include <string>
#include <vector>

void WndProcOnMenuCommand(HWND hwnd, AppState& state, UINT cmd) {
    using daw::internal::core::UpdateWindowTitle;

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
        UiShowProjectSampleRateDialog(hwnd, state);
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
        UiShowAudioSettingsDialog(hwnd, state);
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
