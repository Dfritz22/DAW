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

// Forward declarations for audio orchestration (defined after DoAutoMaster/DoExportMix)
void StopPlayback(AppState& state, bool rewind);
void StopRecording(AppState& state, bool commitTake);
bool StartRecording(HWND hwnd, AppState& state);
bool StartPlayback(HWND hwnd, AppState& state);


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
