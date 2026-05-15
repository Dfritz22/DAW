// import.cpp — extracted from main.cpp Phase 16r.
//
// WAV import + project-wide sample-rate conversion. Public entry points are
// declared in include/daw_project.h (have been since Phase 16l-pre). The
// PickWavFiles helper is private to this TU.

#include "AppState.h"
#include "audio/engine_utils.h"        // ResampleStereoFloatSincHQ
#include "core/internal_app_services.h" // UpdateWindowTitle
#include "daw_audio.h"                  // IoLoadWavStereo, StopPlayback
#include "daw_project.h"                // public decls
#include "ui/UiRuntimeState.h"          // kPalette
#include "vm/timeline_zoom.h"           // FitVisibleToContent

#include <algorithm>
#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>
#include <windows.h>

using daw::internal::core::UpdateWindowTitle;

namespace {

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

} // namespace

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
        state.ui.view.viewStartBeat = 0.0f;
        state.ui.view.viewBeatsVisible = daw::vm::FitVisibleToContent(endBeat);
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
