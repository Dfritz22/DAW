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
#include "ui/wndproc/paint.h"
#include "ui/wndproc/command.h"
#include "ui/wndproc/lbuttondown.h"
#include "ui/menu_build.h"
#include "ui/dialogs.h"
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

// ── Dock-aware layout for hit-tests ─────────────────────────────────────────
// FindDockLeafRect / ComputeHitTestLayout were hoisted to ui/layout.{h,cpp}
// (UiLayoutFindDockLeafRect / UiLayoutComputeHitTestLayout) so per-message
// handler files extracted from WindowProc can share the same dock-aware
// layout without depending on main.cpp internals.

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
        if (HMENU bar = UiBuildMainMenuBar(*initial); bar != nullptr) {
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
        if (UiRefreshTopLevelPopup(popup, *state)) {
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
        WndProcOnMenuCommand(hwnd, *state, cmd);
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
        if (state == nullptr) return 0;
        return WndProcOnLButtonDown(hwnd, lParam, *state);
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
    case WM_PAINT:
        if (state == nullptr) return DefWindowProc(hwnd, msg, wParam, lParam);
        return WndProcOnPaint(hwnd, *state);
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
