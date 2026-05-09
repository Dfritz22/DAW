#include "automix_bridge.h"
#include "core/internal_app_services.h"
#include "io/wav_io.h"

#include <fstream>
#include <iterator>

bool RenderTrackToStereoLocked(const AppState& state, int trackIndex,
                               std::vector<float>* outStereo, int* outSampleRate);
using daw::internal::core::DefaultInsertBypass;
using daw::internal::core::DefaultInsertConfig;
using daw::internal::core::DefaultInsertEffects;
using daw::internal::core::FindRepoRoot;
using daw::internal::core::QuoteArg;

// ── File-private helpers ──────────────────────────────────────────────────

static std::wstring ToLowerCopy(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(::towlower(c));
    });
    return s;
}

static bool ParseJsonFloatLine(const std::string& line, const char* key, float* outValue) {
    if (outValue == nullptr) return false;
    const std::string token = std::string("\"") + key + "\"";
    const size_t pos = line.find(token);
    if (pos == std::string::npos) return false;
    const size_t colon = line.find(':', pos + token.size());
    if (colon == std::string::npos) return false;
    try {
        *outValue = std::stof(line.substr(colon + 1));
        return true;
    } catch (...) {
        return false;
    }
}

static bool ParseJsonIntLine(const std::string& line, const char* key, int* outValue) {
    if (outValue == nullptr) return false;
    const std::string token = std::string("\"") + key + "\"";
    const size_t pos = line.find(token);
    if (pos == std::string::npos) return false;
    const size_t colon = line.find(':', pos + token.size());
    if (colon == std::string::npos) return false;
    try {
        *outValue = std::stoi(line.substr(colon + 1));
        return true;
    } catch (...) {
        return false;
    }
}

static bool ParseJsonStringLine(const std::string& line, const char* key, std::string* outValue) {
    if (outValue == nullptr) return false;
    const std::string token = std::string("\"") + key + "\"";
    const size_t pos = line.find(token);
    if (pos == std::string::npos) return false;
    const size_t colon = line.find(':', pos + token.size());
    if (colon == std::string::npos) return false;
    const size_t q1 = line.find('"', colon + 1);
    if (q1 == std::string::npos) return false;
    const size_t q2 = line.find('"', q1 + 1);
    if (q2 == std::string::npos || q2 <= q1 + 1) return false;
    *outValue = line.substr(q1 + 1, q2 - q1 - 1);
    return true;
}

static void AutoMixAppendInsert(
    InsertEffectArray* effects,
    InsertBypassArray* bypass,
    InsertConfigArray* params,
    int* slotCount,
    int effectType,
    const InsertConfig& p)
{
    if (effects == nullptr || bypass == nullptr || params == nullptr || slotCount == nullptr) return;
    if (*slotCount < 0 || *slotCount >= kMaxInsertSlots) return;
    const int s = *slotCount;
    (*effects)[static_cast<size_t>(s)] = static_cast<std::uint8_t>(std::clamp(effectType, 0, kInsertEffectTypeCount - 1));
    (*bypass)[static_cast<size_t>(s)] = false;
    (*params)[static_cast<size_t>(s)] = p;
    *slotCount = s + 1;
}

static std::wstring ReadSmallUtf8Text(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) {
        return L"";
    }
    std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (s.empty()) {
        return L"";
    }
    const int needed = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (needed <= 0) {
        return std::wstring(s.begin(), s.end());
    }
    std::wstring w(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), needed);
    return w;
}

// ── Public implementations ────────────────────────────────────────────────

bool ParseAutoMixGains(const std::filesystem::path& jsonPath, std::map<std::wstring, float>* outGains) {
    std::ifstream in(jsonPath, std::ios::binary);
    if (!in) {
        return false;
    }

    std::map<std::wstring, float> gains;
    std::string line;
    std::string currentName;

    while (std::getline(in, line)) {
        auto npos = line.find("\"name\"");
        if (npos != std::string::npos) {
            auto q1 = line.find('"', npos + 6);
            if (q1 != std::string::npos) {
                auto q2 = line.find('"', q1 + 1);
                if (q2 != std::string::npos && q2 > q1 + 1) {
                    currentName = line.substr(q1 + 1, q2 - q1 - 1);
                }
            }
            continue;
        }

        auto gpos = line.find("\"gain_db\"");
        if (gpos != std::string::npos && !currentName.empty()) {
            auto colon = line.find(':', gpos);
            if (colon != std::string::npos) {
                const std::string valuePart = line.substr(colon + 1);
                try {
                    const float gain = std::stof(valuePart);
                    std::filesystem::path p(currentName);
                    std::wstring key = ToLowerCopy(p.filename().wstring());
                    gains[key] = gain;
                } catch (...) {
                }
            }
            currentName.clear();
        }
    }

    if (gains.empty()) {
        return false;
    }
    *outGains = std::move(gains);
    return true;
}

bool ParseAutoMixSettings(
    const std::filesystem::path& jsonPath,
    std::map<std::wstring, AutoMixTrackSettings>* outTracks,
    AutoMixMasterSettings* outMaster)
{
    if (outTracks == nullptr || outMaster == nullptr) return false;

    std::ifstream in(jsonPath, std::ios::binary);
    if (!in) return false;

    std::map<std::wstring, AutoMixTrackSettings> tracks;
    AutoMixMasterSettings master{};

    bool inTracks = false;
    bool inMaster = false;
    bool haveCurrentTrack = false;
    std::string currentName;
    AutoMixTrackSettings current{};

    std::string line;
    while (std::getline(in, line)) {
        if (line.find("\"tracks\"") != std::string::npos) {
            inTracks = true;
            continue;
        }
        if (line.find("\"master_bus\"") != std::string::npos) {
            inMaster = true;
            continue;
        }

        if (inTracks) {
            std::string name;
            if (ParseJsonStringLine(line, "name", &name)) {
                if (haveCurrentTrack && !currentName.empty()) {
                    std::filesystem::path p(currentName);
                    tracks[ToLowerCopy(p.filename().wstring())] = current;
                }
                currentName = name;
                current = AutoMixTrackSettings{};
                haveCurrentTrack = true;
                continue;
            }

            if (haveCurrentTrack) {
                ParseJsonFloatLine(line, "gain_db", &current.gainDb);
                ParseJsonFloatLine(line, "pan", &current.pan);
                ParseJsonIntLine(line, "bus_index", &current.busIndex);

                ParseJsonFloatLine(line, "highpass_hz", &current.highpassHz);
                ParseJsonFloatLine(line, "low_shelf_db", &current.lowShelfDb);
                ParseJsonFloatLine(line, "low_mid_db", &current.lowMidDb);
                ParseJsonFloatLine(line, "presence_gain_db", &current.presenceDb);
                ParseJsonFloatLine(line, "air_gain_db", &current.airDb);
                ParseJsonFloatLine(line, "deesser_db", &current.deesserDb);

                ParseJsonFloatLine(line, "comp_threshold_db", &current.compThresholdDb);
                ParseJsonFloatLine(line, "comp_ratio", &current.compRatio);
                ParseJsonFloatLine(line, "comp_makeup_db", &current.compMakeupDb);

                ParseJsonFloatLine(line, "reverb_decay_s", &current.reverbDecayS);
                ParseJsonFloatLine(line, "reverb_predelay_ms", &current.reverbPreDelayMs);
                ParseJsonFloatLine(line, "reverb_mix", &current.reverbMix);
                ParseJsonFloatLine(line, "delay_time_ms", &current.delayTimeMs);
                ParseJsonFloatLine(line, "delay_feedback", &current.delayFeedback);
                ParseJsonFloatLine(line, "delay_mix", &current.delayMix);
                ParseJsonFloatLine(line, "saturation_drive_db", &current.saturationDriveDb);
                ParseJsonFloatLine(line, "saturation_blend", &current.saturationBlend);

                if (line.find("}") != std::string::npos) {
                    if (!currentName.empty()) {
                        std::filesystem::path p(currentName);
                        tracks[ToLowerCopy(p.filename().wstring())] = current;
                    }
                    haveCurrentTrack = false;
                    currentName.clear();
                    current = AutoMixTrackSettings{};
                }
            }

            if (line.find("]") != std::string::npos) {
                inTracks = false;
            }
            continue;
        }

        if (inMaster) {
            if (ParseJsonFloatLine(line, "comp_threshold_db", &master.compThresholdDb)) {
                master.hasCompressor = true;
            }
            if (ParseJsonFloatLine(line, "comp_ratio", &master.compRatio)) {
                master.hasCompressor = true;
            }
            if (ParseJsonFloatLine(line, "comp_makeup_db", &master.compMakeupDb)) {
                master.hasCompressor = true;
            }
            if (line.find("}") != std::string::npos) {
                inMaster = false;
            }
        }
    }

    if (haveCurrentTrack && !currentName.empty()) {
        std::filesystem::path p(currentName);
        tracks[ToLowerCopy(p.filename().wstring())] = current;
    }

    if (tracks.empty()) return false;
    *outTracks = std::move(tracks);
    *outMaster = master;
    return true;
}

bool ApplyAutoMixToFaders(HWND hwnd, AppState& state) {
    if (state.core.project.audio.empty()) {
        MessageBoxW(hwnd, L"Import tracks first before applying AutoMix.", L"AutoMix", MB_OK | MB_ICONINFORMATION);
        return false;
    }

    const std::filesystem::path repoRoot = FindRepoRoot();
    if (repoRoot.empty()) {
        MessageBoxW(hwnd, L"Could not locate project root (.venv and src/daw_ai).", L"AutoMix", MB_OK | MB_ICONERROR);
        return false;
    }

    const std::filesystem::path pythonExe = repoRoot / L".venv" / L"Scripts" / L"python.exe";
    const std::filesystem::path outputDir = repoRoot / L"analysis_out";
    const std::filesystem::path inputDir = outputDir / L"automix_inputs";
    const std::filesystem::path outJson = outputDir / L"auto_mix_best.json";
    const std::filesystem::path outApplyJson = outputDir / L"auto_mix_apply.json";

    if (!std::filesystem::exists(pythonExe)) {
        MessageBoxW(hwnd, L"Python venv executable not found at .venv\\Scripts\\python.exe", L"AutoMix", MB_OK | MB_ICONERROR);
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(outputDir, ec);
    std::filesystem::remove_all(inputDir, ec);
    std::filesystem::create_directories(inputDir, ec);

    std::vector<std::pair<std::wstring, int>> exportedTracks;
    for (int ti = 0; ti < static_cast<int>(state.core.project.tracks.size()); ++ti) {
        std::vector<float> trackStereo;
        int trackSr = 0;
        EnterCriticalSection(&state.audio.audioStateLock);
        const bool hasTrackAudio = RenderTrackToStereoLocked(state, ti, &trackStereo, &trackSr);
        const std::wstring trackName = state.core.project.tracks[static_cast<size_t>(ti)].name;
        LeaveCriticalSection(&state.audio.audioStateLock);
        if (!hasTrackAudio || trackStereo.empty() || trackSr <= 0) {
            continue;
        }

        std::wstring safeTrackName = trackName;
        for (wchar_t& c : safeTrackName) {
            if (c == L' ' || c == L'\\' || c == L'/' || c == L':' || c == L'*' || c == L'?' || c == L'"' || c == L'<' || c == L'>' || c == L'|') {
                c = L'_';
            }
        }

        const std::wstring fileName = L"track_" + std::to_wstring(ti + 1) + L"_" + safeTrackName + L".wav";
        const std::filesystem::path wavPath = inputDir / fileName;
        if (!IoWriteWavPcm16Stereo(wavPath.wstring(), trackStereo, trackSr)) {
            continue;
        }
        exportedTracks.push_back({fileName, ti});
    }

    if (exportedTracks.empty()) {
        MessageBoxW(hwnd, L"No track audio available for AutoMix analysis.", L"AutoMix", MB_OK | MB_ICONWARNING);
        return false;
    }

    std::wstring cmd =
        QuoteArg(pythonExe.wstring()) +
        L" -m daw_ai.cli --input-dir " + QuoteArg(inputDir.wstring()) +
        L" --output-dir " + QuoteArg(outputDir.wstring()) +
        L" --auto-mix --mix-mode best --iterations 8 --preview-sec 8 --select";

    for (const auto& e : exportedTracks) {
        cmd += L" " + QuoteArg(e.first);
    }

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
        repoRoot.wstring().c_str(),
        &si,
        &pi
    );
    if (!ok) {
        MessageBoxW(hwnd, L"Failed to launch AutoMix process.", L"AutoMix", MB_OK | MB_ICONERROR);
        return false;
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    if (exitCode != 0) {
        MessageBoxW(hwnd, L"AutoMix process failed. Check terminal outputs/logs.", L"AutoMix", MB_OK | MB_ICONERROR);
        return false;
    }

    std::map<std::wstring, AutoMixTrackSettings> settings;
    AutoMixMasterSettings master{};
    const bool hasFullSettings = ParseAutoMixSettings(outApplyJson, &settings, &master);

    std::map<std::wstring, float> gains;
    if (!hasFullSettings && !ParseAutoMixGains(outJson, &gains)) {
        MessageBoxW(hwnd, L"Could not parse AutoMix output for settings.", L"AutoMix", MB_OK | MB_ICONERROR);
        return false;
    }

    int applied = 0;
    int appliedFx = 0;
    int appliedBusRoute = 0;
    EnterCriticalSection(&state.audio.audioStateLock);
    for (const auto& e : exportedTracks) {
        const int trackIndex = e.second;
        if (trackIndex < 0 || trackIndex >= static_cast<int>(state.core.project.tracks.size())) {
            continue;
        }
        const std::wstring key = ToLowerCopy(e.first);

        if (hasFullSettings) {
            auto it = settings.find(key);
            if (it == settings.end()) continue;
            const AutoMixTrackSettings& s = it->second;

            state.core.project.tracks[static_cast<size_t>(trackIndex)].gainDb = std::clamp(s.gainDb, kFaderMinDb, kFaderMaxDb);
            if (trackIndex < static_cast<int>(state.core.project.tracks.size())) {
                state.core.project.tracks[static_cast<size_t>(trackIndex)].pan = std::clamp(s.pan, -1.0f, 1.0f);
            }
            if (trackIndex < static_cast<int>(state.core.project.tracks.size())) {
                state.core.project.tracks[static_cast<size_t>(trackIndex)].busIndex = std::clamp(s.busIndex, 0, kBusCount - 1);
                ++appliedBusRoute;
            }

            if (trackIndex < static_cast<int>(state.core.project.tracks.size()) &&
                trackIndex < static_cast<int>(state.core.project.tracks.size()) &&
                trackIndex < static_cast<int>(state.core.project.tracks.size()) &&
                trackIndex < static_cast<int>(state.core.project.tracks.size())) {
                InsertEffectArray fx = DefaultInsertEffects();
                InsertBypassArray by = DefaultInsertBypass();
                InsertConfigArray pp = DefaultInsertConfig();
                int slots = 0;

                const bool hasEq =
                    s.highpassHz > 5.0f ||
                    std::fabs(s.lowShelfDb) > 0.05f ||
                    std::fabs(s.lowMidDb) > 0.05f ||
                    std::fabs(s.presenceDb) > 0.05f ||
                    std::fabs(s.airDb) > 0.05f;
                if (hasEq) {
                    InsertConfig p = DefaultInsertConfig()[0];
                    p.eq[0].type = (s.highpassHz > 5.0f) ? kEqHighPass : kEqLowShelf;
                    p.eq[0].freq_hz = std::clamp(s.highpassHz > 5.0f ? s.highpassHz : 120.0f, 20.0f, 20000.0f);
                    p.eq[0].gain_db = (s.highpassHz > 5.0f) ? 0.0f : std::clamp(s.lowShelfDb, -18.0f, 18.0f);
                    p.eq[0].q = 0.707f;

                    p.eq[1].type = kEqPeak;
                    p.eq[1].freq_hz = 350.0f;
                    p.eq[1].gain_db = std::clamp(s.lowMidDb, -18.0f, 18.0f);
                    p.eq[1].q = 1.0f;

                    p.eq[2].type = kEqPeak;
                    p.eq[2].freq_hz = 3200.0f;
                    p.eq[2].gain_db = std::clamp(s.presenceDb, -18.0f, 18.0f);
                    p.eq[2].q = 1.0f;

                    p.eq[3].type = kEqHighShelf;
                    p.eq[3].freq_hz = 9000.0f;
                    p.eq[3].gain_db = std::clamp(s.airDb, -18.0f, 18.0f);
                    p.eq[3].q = 0.707f;

                    AutoMixAppendInsert(&fx, &by, &pp, &slots, kFxEQ, p);
                }

                if (s.compRatio > 1.05f) {
                    InsertConfig p = DefaultInsertConfig()[0];
                    p.cmp_threshold_db = std::clamp(s.compThresholdDb, -60.0f, 0.0f);
                    p.cmp_ratio = std::clamp(s.compRatio, 1.0f, 20.0f);
                    p.cmp_makeup_db = std::clamp(s.compMakeupDb, 0.0f, 24.0f);
                    p.cmp_attack_ms = 10.0f;
                    p.cmp_release_ms = 120.0f;
                    p.cmp_knee_db = 3.0f;
                    AutoMixAppendInsert(&fx, &by, &pp, &slots, kFxCMP, p);
                }

                if (s.deesserDb < -0.05f) {
                    InsertConfig p = DefaultInsertConfig()[0];
                    p.dee_threshold_db = std::clamp(-22.0f + s.deesserDb * 2.0f, -40.0f, 0.0f);
                    p.dee_freq_hz = 7000.0f;
                    p.dee_bandwidth_hz = 3500.0f;
                    p.dee_reduction_db = std::clamp(std::fabs(s.deesserDb) * 6.0f, 0.0f, 24.0f);
                    AutoMixAppendInsert(&fx, &by, &pp, &slots, kFxDEE, p);
                }

                if (s.saturationBlend > 0.01f && s.saturationDriveDb > 0.01f) {
                    InsertConfig p = DefaultInsertConfig()[0];
                    p.sat_drive = std::clamp(s.saturationDriveDb / 8.0f, 0.0f, 1.0f);
                    p.sat_mix = std::clamp(s.saturationBlend, 0.0f, 1.0f);
                    AutoMixAppendInsert(&fx, &by, &pp, &slots, kFxSAT, p);
                }

                if (s.delayMix > 0.01f && s.delayTimeMs > 1.0f) {
                    InsertConfig p = DefaultInsertConfig()[0];
                    p.dly_time_ms = std::clamp(s.delayTimeMs, 10.0f, 2000.0f);
                    p.dly_feedback = std::clamp(s.delayFeedback, 0.0f, 0.95f);
                    p.dly_mix = std::clamp(s.delayMix, 0.0f, 1.0f);
                    AutoMixAppendInsert(&fx, &by, &pp, &slots, kFxDLY, p);
                }

                if (s.reverbMix > 0.01f) {
                    InsertConfig p = DefaultInsertConfig()[0];
                    p.rev_mix = std::clamp(s.reverbMix, 0.0f, 1.0f);
                    p.rev_room_size = std::clamp((s.reverbDecayS - 0.2f) / 2.3f, 0.0f, 1.0f);
                    p.rev_damping = std::clamp(0.35f + s.reverbPreDelayMs / 80.0f, 0.0f, 1.0f);
                    AutoMixAppendInsert(&fx, &by, &pp, &slots, kFxREV, p);
                }

                state.core.project.tracks[static_cast<size_t>(trackIndex)].insertEffects = fx;
                state.core.project.tracks[static_cast<size_t>(trackIndex)].insertBypass = by;
                state.core.project.tracks[static_cast<size_t>(trackIndex)].insertConfig = pp;
                state.core.project.tracks[static_cast<size_t>(trackIndex)].insertSlots = slots;
                if (slots > 0) ++appliedFx;
            }

            ++applied;
        } else {
            auto it = gains.find(key);
            if (it == gains.end()) continue;
            state.core.project.tracks[static_cast<size_t>(trackIndex)].gainDb = std::clamp(it->second, kFaderMinDb, kFaderMaxDb);
            ++applied;
        }
    }

    if (hasFullSettings && master.hasCompressor &&
        state.core.project.buses.size() > 3 &&
        state.core.project.buses.size() > 3 &&
        state.core.project.buses.size() > 3 &&
        state.core.project.buses.size() > 3) {
        InsertEffectArray fx = DefaultInsertEffects();
        InsertBypassArray by = DefaultInsertBypass();
        InsertConfigArray pp = DefaultInsertConfig();
        int slots = 0;

        InsertConfig p = DefaultInsertConfig()[0];
        p.cmp_threshold_db = std::clamp(master.compThresholdDb, -60.0f, 0.0f);
        p.cmp_ratio = std::clamp(master.compRatio, 1.0f, 20.0f);
        p.cmp_makeup_db = std::clamp(master.compMakeupDb, 0.0f, 24.0f);
        p.cmp_attack_ms = 25.0f;
        p.cmp_release_ms = 150.0f;
        p.cmp_knee_db = 4.0f;
        AutoMixAppendInsert(&fx, &by, &pp, &slots, kFxCMP, p);

        state.core.project.buses[3].insertEffects = fx;
        state.core.project.buses[3].insertBypass = by;
        state.core.project.buses[3].insertConfig = pp;
        state.core.project.buses[3].insertSlots = slots;
    }

    LeaveCriticalSection(&state.audio.audioStateLock);

    if (applied == 0) {
        MessageBoxW(hwnd, L"AutoMix completed but no matching track settings were found.", L"AutoMix", MB_OK | MB_ICONWARNING);
        return false;
    }

    InvalidateRect(hwnd, nullptr, FALSE);
    std::wstringstream status;
    status << L"AutoMix applied to " << applied << L" track(s).";
    if (hasFullSettings) {
        status << L"\nBus routes updated: " << appliedBusRoute << L".";
        status << L"\nTrack FX chains updated: " << appliedFx << L".";
        if (master.hasCompressor) {
            status << L"\nMaster bus compressor updated.";
        }
    } else {
        status << L"\n(legacy gain-only apply mode)";
    }
    MessageBoxW(hwnd, status.str().c_str(), L"AutoMix", MB_OK | MB_ICONINFORMATION);
    return true;
}

bool AnalyzeSelectedTrackQuality(HWND hwnd, AppState& state) {
    const int trackIndex = state.ui.selectedTrackIndex;
    if (trackIndex < 0 || trackIndex >= static_cast<int>(state.core.project.tracks.size())) {
        MessageBoxW(hwnd, L"Select a track first, then run Vocal Check.", L"Vocal Check", MB_OK | MB_ICONINFORMATION);
        return false;
    }

    std::vector<float> stereo;
    std::vector<float> referenceStereo;
    int sampleRate = 0;
    int referenceSampleRate = 0;
    EnterCriticalSection(&state.audio.audioStateLock);
    const bool hasAudio = RenderTrackToStereoLocked(state, trackIndex, &stereo, &sampleRate);
    bool hasReference = false;
    if (trackIndex >= 0 && trackIndex < static_cast<int>(state.core.project.tracks.size())) {
        const bool prevMute = state.core.project.tracks[static_cast<size_t>(trackIndex)].mute;
        state.core.project.tracks[static_cast<size_t>(trackIndex)].mute = true;
        hasReference = RenderFullMixToStereoLocked(state, &referenceStereo, &referenceSampleRate);
        state.core.project.tracks[static_cast<size_t>(trackIndex)].mute = prevMute;
    }
    const std::wstring trackName = state.core.project.tracks[static_cast<size_t>(trackIndex)].name;
    LeaveCriticalSection(&state.audio.audioStateLock);

    if (!hasAudio || stereo.empty() || sampleRate <= 0) {
        MessageBoxW(hwnd, L"Selected track has no audio clips to analyze.", L"Vocal Check", MB_OK | MB_ICONWARNING);
        return false;
    }

    const std::filesystem::path repoRoot = FindRepoRoot();
    if (repoRoot.empty()) {
        MessageBoxW(hwnd, L"Could not locate project root (.venv and src/daw_ai).", L"Vocal Check", MB_OK | MB_ICONERROR);
        return false;
    }

    const std::filesystem::path pythonExe = repoRoot / L".venv" / L"Scripts" / L"python.exe";
    if (!std::filesystem::exists(pythonExe)) {
        MessageBoxW(hwnd, L"Python venv executable not found at .venv\\Scripts\\python.exe", L"Vocal Check", MB_OK | MB_ICONERROR);
        return false;
    }

    const std::filesystem::path outputDir = repoRoot / L"analysis_out";
    std::error_code ec;
    std::filesystem::create_directories(outputDir, ec);

    std::wstring safeTrackName = trackName;
    for (wchar_t& c : safeTrackName) {
        if (c == L' ' || c == L'\\' || c == L'/' || c == L':' || c == L'*' || c == L'?' || c == L'"' || c == L'<' || c == L'>' || c == L'|') {
            c = L'_';
        }
    }

    const std::filesystem::path wavPath = outputDir / (safeTrackName + L"_vocal_check.wav");
    const std::filesystem::path referencePath = outputDir / (safeTrackName + L"_vocal_check_reference.wav");
    const std::filesystem::path txtPath = outputDir / (safeTrackName + L"_vocal_check.txt");

    if (!IoWriteWavPcm16Stereo(wavPath.wstring(), stereo, sampleRate)) {
        MessageBoxW(hwnd, L"Failed to write temporary audio for analysis.", L"Vocal Check", MB_OK | MB_ICONERROR);
        return false;
    }

    bool wroteReference = false;
    if (hasReference && !referenceStereo.empty() && referenceSampleRate == sampleRate) {
        wroteReference = IoWriteWavPcm16Stereo(referencePath.wstring(), referenceStereo, referenceSampleRate);
    }

    std::wstring cmd =
        QuoteArg(pythonExe.wstring()) +
        L" -m daw_ai.vocal_check --input " + QuoteArg(wavPath.wstring()) +
        L" --output " + QuoteArg(txtPath.wstring()) +
        L" --bpm " + std::to_wstring(static_cast<int>(state.core.project.bpm)) +
        L" --track-name " + QuoteArg(trackName);

    if (wroteReference) {
        cmd += L" --reference " + QuoteArg(referencePath.wstring());
    }

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
        repoRoot.wstring().c_str(),
        &si,
        &pi
    );
    if (!ok) {
        MessageBoxW(hwnd, L"Failed to launch Vocal Check analyzer process.", L"Vocal Check", MB_OK | MB_ICONERROR);
        return false;
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    if (exitCode != 0) {
        MessageBoxW(hwnd, L"Vocal Check failed. See terminal/log output.", L"Vocal Check", MB_OK | MB_ICONERROR);
        return false;
    }

    const std::wstring report = ReadSmallUtf8Text(txtPath);
    if (report.empty()) {
        MessageBoxW(hwnd, L"Vocal Check completed, but no report text was found.", L"Vocal Check", MB_OK | MB_ICONWARNING);
        return false;
    }

    MessageBoxW(hwnd, report.c_str(), L"Vocal Check Report", MB_OK | MB_ICONINFORMATION);
    return true;
}

DWORD WINAPI AutoMixThreadProc(LPVOID param) {
    auto* state = reinterpret_cast<AppState*>(param);
    if (state == nullptr || state->ui.hwnd == nullptr) {
        return 0;
    }

    const bool ok = ApplyAutoMixToFaders(state->ui.hwnd, *state);
    state->audio.automixRunning.store(false);
    PostMessage(state->ui.hwnd, kMsgAutoMixFinished, ok ? 1 : 0, 0);
    return 0;
}

void StartAutoMixAsync(HWND hwnd, AppState& state) {
    if (state.audio.automixRunning.load()) {
        MessageBoxW(hwnd, L"AutoMix is already running.", L"AutoMix", MB_OK | MB_ICONINFORMATION);
        return;
    }

    state.audio.automixRunning.store(true);
    state.audio.automixThread = CreateThread(nullptr, 0, AutoMixThreadProc, &state, 0, nullptr);
    if (state.audio.automixThread == nullptr) {
        state.audio.automixRunning.store(false);
        MessageBoxW(hwnd, L"Could not start AutoMix worker thread.", L"AutoMix", MB_OK | MB_ICONERROR);
        return;
    }

    InvalidateRect(hwnd, nullptr, FALSE);
}
