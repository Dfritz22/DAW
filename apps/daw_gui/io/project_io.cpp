#include "project_io.h"

#include "wav_io.h"

void UpdateWindowTitle(HWND hwnd, const UiState& state);
InsertEffectArray DefaultInsertEffects();
InsertBypassArray DefaultInsertBypass();
InsertConfigArray DefaultInsertConfig();

// ── JSON project serialization ───────────────────────────────────────────────
// Minimal hand-written JSON - no external dependency required.

static std::string WstrToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

static std::wstring Utf8ToWstr(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

static std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 4);
    for (const char c : s) {
        if (c == '"')  { out += "\\\""; }
        else if (c == '\\') { out += "\\\\"; }
        else if (c == '\n') { out += "\\n"; }
        else if (c == '\r') { out += "\\r"; }
        else { out += c; }
    }
    return out;
}

static std::string JsonUnescape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c != '\\' || i + 1 >= s.size()) {
            out += c;
            continue;
        }

        const char n = s[i + 1];
        switch (n) {
        case '\\': out += '\\'; break;
        case '"': out += '"'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        default:
            // Preserve unknown escapes literally for best-effort compatibility.
            out += n;
            break;
        }
        ++i;
    }
    return out;
}

// Encode all params for all slots as a flat pipe-delimited CSV string.
// Format: slot0_val0,val1,...valN|slot1_val0,...|...
// The float layout per slot is fixed and must match DecodeInsertConfigCsv.
static std::string EncodeInsertConfigCsv(const InsertConfigArray& params, int slotCount) {
    std::ostringstream out;
    out << std::fixed;
    out.precision(4);
    const int count = std::clamp(slotCount, 0, kMaxInsertSlots);
    for (int s = 0; s < count; ++s) {
        if (s > 0) out << '|';
        const InsertConfig& p = params[static_cast<size_t>(s)];
        // EQ bands: freq,gain,q,type  x kEqBandCount
        for (int b = 0; b < kEqBandCount; ++b) {
            out << p.eq[b].freq_hz << ',' << p.eq[b].gain_db << ','
                << p.eq[b].q      << ',' << p.eq[b].type;
            out << ',';
        }
        // Compressor
        out << p.cmp_threshold_db << ',' << p.cmp_ratio << ','
            << p.cmp_attack_ms    << ',' << p.cmp_release_ms << ','
            << p.cmp_knee_db      << ',' << p.cmp_makeup_db  << ',';
        // Saturation
        out << p.sat_drive << ',' << p.sat_mix << ',';
        // Delay
        out << p.dly_time_ms << ',' << p.dly_feedback << ',' << p.dly_mix << ',';
        // Reverb
        out << p.rev_room_size << ',' << p.rev_damping << ',' << p.rev_mix << ',';
        // Gate
        out << p.gate_threshold_db << ',' << p.gate_attack_ms << ','
            << p.gate_release_ms   << ',' << p.gate_hold_ms   << ',';
        // De-esser
        out << p.dee_threshold_db << ',' << p.dee_freq_hz << ','
            << p.dee_bandwidth_hz << ',' << p.dee_reduction_db << ',';
        // Limiter
        out << p.lim_ceiling_db << ',' << p.lim_release_ms;
    }
    return out.str();
}

static void DecodeInsertConfigCsv(const std::string& csv, int slotCount, InsertConfigArray* params) {
    if (!params) return;
    *params = DefaultInsertConfig();
    if (csv.empty()) return;
    const int count = std::clamp(slotCount, 0, kMaxInsertSlots);
    // Split by '|' into per-slot strings
    std::vector<std::string> slotStrs;
    {
        std::string tok;
        for (char c : csv) {
            if (c == '|') { slotStrs.push_back(tok); tok.clear(); }
            else tok += c;
        }
        slotStrs.push_back(tok);
    }
    for (int s = 0; s < count && s < static_cast<int>(slotStrs.size()); ++s) {
        InsertConfig& p = (*params)[static_cast<size_t>(s)];
        std::vector<std::string> vals;
        {
            std::string tok;
            for (char c : slotStrs[static_cast<size_t>(s)]) {
                if (c == ',') { vals.push_back(tok); tok.clear(); }
                else tok += c;
            }
            vals.push_back(tok);
        }
        auto getf = [&](int idx, float fallback) -> float {
            if (idx < 0 || idx >= static_cast<int>(vals.size()) || vals[static_cast<size_t>(idx)].empty()) return fallback;
            try { return std::stof(vals[static_cast<size_t>(idx)]); } catch (...) { return fallback; }
        };
        auto geti = [&](int idx, int fallback) -> int {
            if (idx < 0 || idx >= static_cast<int>(vals.size()) || vals[static_cast<size_t>(idx)].empty()) return fallback;
            try { return std::stoi(vals[static_cast<size_t>(idx)]); } catch (...) { return fallback; }
        };
        int i = 0;
        // EQ bands
        for (int b = 0; b < kEqBandCount; ++b) {
            p.eq[b].freq_hz = getf(i++, p.eq[b].freq_hz);
            p.eq[b].gain_db = getf(i++, p.eq[b].gain_db);
            p.eq[b].q       = getf(i++, p.eq[b].q);
            p.eq[b].type    = geti(i++, p.eq[b].type);
        }
        // Compressor
        p.cmp_threshold_db = getf(i++, p.cmp_threshold_db);
        p.cmp_ratio        = getf(i++, p.cmp_ratio);
        p.cmp_attack_ms    = getf(i++, p.cmp_attack_ms);
        p.cmp_release_ms   = getf(i++, p.cmp_release_ms);
        p.cmp_knee_db      = getf(i++, p.cmp_knee_db);
        p.cmp_makeup_db    = getf(i++, p.cmp_makeup_db);
        // Saturation
        p.sat_drive = getf(i++, p.sat_drive);
        p.sat_mix   = getf(i++, p.sat_mix);
        // Delay
        p.dly_time_ms  = getf(i++, p.dly_time_ms);
        p.dly_feedback = getf(i++, p.dly_feedback);
        p.dly_mix      = getf(i++, p.dly_mix);
        // Reverb
        p.rev_room_size = getf(i++, p.rev_room_size);
        p.rev_damping   = getf(i++, p.rev_damping);
        p.rev_mix       = getf(i++, p.rev_mix);
        // Gate
        p.gate_threshold_db = getf(i++, p.gate_threshold_db);
        p.gate_attack_ms    = getf(i++, p.gate_attack_ms);
        p.gate_release_ms   = getf(i++, p.gate_release_ms);
        p.gate_hold_ms      = getf(i++, p.gate_hold_ms);
        // De-esser
        p.dee_threshold_db  = getf(i++, p.dee_threshold_db);
        p.dee_freq_hz       = getf(i++, p.dee_freq_hz);
        p.dee_bandwidth_hz  = getf(i++, p.dee_bandwidth_hz);
        p.dee_reduction_db  = getf(i++, p.dee_reduction_db);
        // Limiter
        p.lim_ceiling_db = getf(i++, p.lim_ceiling_db);
        p.lim_release_ms = getf(i++, p.lim_release_ms);
    }
}

static std::string EncodeInsertEffectsCsv(const InsertEffectArray& effects, int slotCount) {
    std::ostringstream out;
    const int count = std::clamp(slotCount, 0, kMaxInsertSlots);
    for (int i = 0; i < count; ++i) {
        if (i > 0) out << ',';
        out << static_cast<int>(effects[static_cast<size_t>(i)]);
    }
    return out.str();
}

static std::string EncodeInsertBypassCsv(const InsertBypassArray& bypass, int slotCount) {
    std::ostringstream out;
    const int count = std::clamp(slotCount, 0, kMaxInsertSlots);
    for (int i = 0; i < count; ++i) {
        if (i > 0) out << ',';
        out << (bypass[static_cast<size_t>(i)] ? 1 : 0);
    }
    return out.str();
}

static void DecodeInsertEffectsCsv(const std::string& csv, int slotCount, InsertEffectArray* effects) {
    if (effects == nullptr) return;
    *effects = DefaultInsertEffects();
    std::stringstream ss(csv);
    std::string token;
    int i = 0;
    const int count = std::clamp(slotCount, 0, kMaxInsertSlots);
    while (i < count && std::getline(ss, token, ',')) {
        try {
            const int v = std::stoi(token);
            (*effects)[static_cast<size_t>(i)] = static_cast<std::uint8_t>(std::clamp(v, 0, kInsertEffectTypeCount - 1));
        } catch (...) {
        }
        ++i;
    }
}

static void DecodeInsertBypassCsv(const std::string& csv, int slotCount, InsertBypassArray* bypass) {
    if (bypass == nullptr) return;
    *bypass = DefaultInsertBypass();
    std::stringstream ss(csv);
    std::string token;
    int i = 0;
    const int count = std::clamp(slotCount, 0, kMaxInsertSlots);
    while (i < count && std::getline(ss, token, ',')) {
        try {
            const int v = std::stoi(token);
            (*bypass)[static_cast<size_t>(i)] = (v != 0);
        } catch (...) {
        }
        ++i;
    }
}

static AudioBackend AudioBackendFromJson(const std::string& value) {
    if (value == "mme") return AudioBackend::MME;
    if (value == "wasapi_exclusive") return AudioBackend::WasapiExclusive;
    if (value == "asio") return AudioBackend::Asio;
    return AudioBackend::WasapiShared;
}

bool SaveProject(const std::wstring& path, UiState& state) {

    // Materialize recorded in-memory takes so they survive save/load round-trips.
    {
        const std::filesystem::path projectPath(path);
        const std::filesystem::path takeDir = projectPath.parent_path() / (projectPath.stem().wstring() + L"_audio");
        std::error_code ec;
        std::filesystem::create_directories(takeDir, ec);
        if (ec) {
            return false;
        }

        for (size_t i = 0; i < state.project.audio.size(); ++i) {
            auto& a = state.project.audio[i];
            const bool needsMaterialize = a.sourcePath.empty() || a.sourcePath == L"[recording]";
            if (!needsMaterialize) {
                continue;
            }
            if (a.sampleRate <= 0 || a.stereo.empty()) {
                return false;
            }
            const std::filesystem::path takePath = takeDir / (L"take_" + std::to_wstring(i + 1) + L".wav");
            if (!WriteWavPcm16Stereo(takePath.wstring(), a.stereo, a.sampleRate)) {
                return false;
            }
            a.sourcePath = takePath.wstring();
        }
    }

    const auto backendJson = [&]() -> const char* {
        switch (state.audioBackend) {
        case AudioBackend::MME:
            return "mme";
        case AudioBackend::WasapiExclusive:
            return "wasapi_exclusive";
        case AudioBackend::Asio:
            return "asio";
        case AudioBackend::WasapiShared:
        default:
            return "wasapi_shared";
        }
    };

    std::ostringstream js;
    js << "{\n";
    js << "  \"version\": 1,\n";
    js << "  \"bpm\": " << state.project.bpm << ",\n";
    js << "  \"sample_rate\": " << state.project.projectSampleRate << ",\n";
    js << "  \"audio_backend\": \"" << backendJson() << "\",\n";
    js << "  \"audio_preferred_sample_rate\": " << state.preferredSampleRate << ",\n";
    js << "  \"audio_preferred_buffer_frames\": " << state.preferredBufferFrames << ",\n";
    js << "  \"audio_input_device_name\": \"" << JsonEscape(WstrToUtf8(state.selectedInputDeviceName)) << "\",\n";
    js << "  \"audio_output_device_name\": \"" << JsonEscape(WstrToUtf8(state.selectedOutputDeviceName)) << "\",\n";
    js << "  \"view_start_beat\": " << state.viewStartBeat << ",\n";
    js << "  \"view_beats_visible\": " << state.viewBeatsVisible << ",\n";

    // Fixed buses
    js << "  \"buses\": [\n";
    for (int b = 0; b < kBusCount; ++b) {
        if (b >= static_cast<int>(state.project.buses.size())) break;
        const BusData& bus = state.project.buses[static_cast<size_t>(b)];
        const std::string insertEffects = JsonEscape(EncodeInsertEffectsCsv(bus.insertEffects, bus.insertSlots));
        const std::string insertBypass = JsonEscape(EncodeInsertBypassCsv(bus.insertBypass, bus.insertSlots));
        const std::string insertConfig = JsonEscape(EncodeInsertConfigCsv(bus.insertConfig, bus.insertSlots));
        js << "    {";
        js << "\"name\":\"" << JsonEscape(WstrToUtf8(bus.name)) << "\",";
        js << "\"gain_db\":" << bus.gainDb << ",";
        js << "\"mute\":" << (bus.mute ? "true" : "false") << ",";
        js << "\"pan\":" << bus.pan << ",";
        js << "\"insert_slots\":" << bus.insertSlots << ",";
        js << "\"insert_effects\":\"" << insertEffects << "\",";
        js << "\"insert_bypass\":\"" << insertBypass << "\",";
        js << "\"insert_params\":\"" << insertConfig << "\"";
        js << "}";
        if (b + 1 < kBusCount) js << ",";
        js << "\n";
    }
    js << "  ],\n";

    // Tracks
    js << "  \"tracks\": [\n";
    for (size_t i = 0; i < state.project.tracks.size(); ++i) {
        const TrackData& track = state.project.tracks[i];
        const std::string name = JsonEscape(WstrToUtf8(track.name));
        const std::string insertEffects = JsonEscape(EncodeInsertEffectsCsv(track.insertEffects, track.insertSlots));
        const std::string insertBypass = JsonEscape(EncodeInsertBypassCsv(track.insertBypass, track.insertSlots));
        const std::string insertConfig = JsonEscape(EncodeInsertConfigCsv(track.insertConfig, track.insertSlots));
        js << "    {";
        js << "\"name\":\"" << name << "\",";
        js << "\"gain_db\":" << track.gainDb << ",";
        js << "\"mute\":" << (track.mute ? "true" : "false") << ",";
        js << "\"solo\":" << (track.solo ? "true" : "false") << ",";
        js << "\"record_arm\":" << (track.recordArm ? "true" : "false") << ",";
        js << "\"bus_index\":" << track.busIndex << ",";
        js << "\"pan\":" << track.pan << ",";
        js << "\"insert_slots\":" << track.insertSlots << ",";
        js << "\"insert_effects\":\"" << insertEffects << "\",";
        js << "\"insert_bypass\":\"" << insertBypass << "\",";
        js << "\"insert_params\":\"" << insertConfig << "\"";
        js << "}";
        if (i + 1 < state.project.tracks.size()) js << ",";
        js << "\n";
    }
    js << "  ],\n";

    // Audio files (deduplicated source paths)
    js << "  \"audio_files\": [\n";
    for (size_t i = 0; i < state.project.audio.size(); ++i) {
        const std::string src = JsonEscape(WstrToUtf8(state.project.audio[i].sourcePath));
        js << "    \"" << src << "\"";
        if (i + 1 < state.project.audio.size()) js << ",";
        js << "\n";
    }
    js << "  ],\n";

    // Clips
    js << "  \"clips\": [\n";
    for (size_t i = 0; i < state.project.clips.size(); ++i) {
        const ClipItem& c = state.project.clips[i];
        const std::string cname = JsonEscape(WstrToUtf8(c.name));
        js << "    {";
        js << "\"track_index\":" << c.trackIndex << ",";
        js << "\"audio_index\":" << c.audioIndex << ",";
        js << "\"start_beat\":"  << c.startBeat  << ",";
        js << "\"length_beats\":" << c.lengthBeats << ",";
        js << "\"source_offset_frames\":" << c.sourceOffsetFrames << ",";
        js << "\"name\":\"" << cname << "\"";
        js << "}";
        if (i + 1 < state.project.clips.size()) js << ",";
        js << "\n";
    }
    js << "  ]\n";
    js << "}\n";

    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    const std::string text = js.str();
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    return out.good();
}

// Minimal JSON string-value extractor (no nesting support needed for flat fields)
static bool JsonReadString(const std::string& json, const std::string& key, std::string* val) {
    const std::string search = '"' + key + "\":\"";
    const size_t pos = json.find(search);
    if (pos == std::string::npos) return false;
    const size_t start = pos + search.size();
    size_t end = start;
    while (end < json.size() && !(json[end] == '"' && (end == 0 || json[end-1] != '\\'))) ++end;
    *val = JsonUnescape(json.substr(start, end - start));
    return true;
}

static bool JsonReadDouble(const std::string& json, const std::string& key, double* val) {
    const std::string search = '"' + key + "\":";
    const size_t pos = json.find(search);
    if (pos == std::string::npos) return false;
    try { *val = std::stod(json.substr(pos + search.size())); return true; } catch (...) { return false; }
}

bool LoadProject(const std::wstring& path, UiState& state) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    const std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (json.empty()) return false;

    // Clear current session
    state.project.tracks.clear();
    state.project.buses.assign(kBusCount, BusData{});
    state.project.audio.clear();
    state.project.clips.clear();
    state.selectedTrackIndex = -1;
    state.selectedClipIndex  = -1;
    state.playheadBeat  = 0.0f;
    state.viewStartBeat = 0.0f;

    // Top-level scalars
    double dval = 0.0;
    std::string sval;
    if (JsonReadDouble(json, "bpm", &dval))                state.project.bpm               = static_cast<float>(dval);
    if (JsonReadDouble(json, "sample_rate", &dval))        state.project.projectSampleRate  = static_cast<int>(dval);
    if (JsonReadString(json, "audio_backend", &sval))      state.audioBackend       = AudioBackendFromJson(sval);
    if (JsonReadDouble(json, "audio_preferred_sample_rate", &dval)) state.preferredSampleRate = std::max(0, static_cast<int>(dval));
    if (JsonReadDouble(json, "audio_preferred_buffer_frames", &dval)) state.preferredBufferFrames = std::max(64, static_cast<int>(dval));
    if (JsonReadString(json, "audio_input_device_name", &sval)) state.selectedInputDeviceName = Utf8ToWstr(sval);
    if (JsonReadString(json, "audio_output_device_name", &sval)) state.selectedOutputDeviceName = Utf8ToWstr(sval);
    if (JsonReadDouble(json, "view_start_beat", &dval))    state.viewStartBeat      = static_cast<float>(dval);
    if (JsonReadDouble(json, "view_beats_visible", &dval)) state.viewBeatsVisible   = static_cast<float>(dval);

    // Parse fixed buses array (optional for backward compatibility)
    {
        size_t pos = json.find("\"buses\"");
        size_t arrStart = (pos != std::string::npos) ? json.find('[', pos) : std::string::npos;
        size_t arrEnd   = (arrStart != std::string::npos) ? json.find(']', arrStart) : std::string::npos;
        if (arrStart != std::string::npos && arrEnd != std::string::npos) {
            int busIdx = 0;
            size_t cur = arrStart + 1;
            while (cur < arrEnd && busIdx < kBusCount) {
                size_t obOpen  = json.find('{', cur);
                if (obOpen == std::string::npos || obOpen > arrEnd) break;
                size_t obClose = json.find('}', obOpen);
                if (obClose == std::string::npos) break;
                const std::string obj = json.substr(obOpen + 1, obClose - obOpen - 1);

                auto numVal = [&](const std::string& k, double& out) {
                    const std::string s2 = '"' + k + "\":";
                    size_t p2 = obj.find(s2);
                    if (p2 == std::string::npos) return;
                    try { out = std::stod(obj.substr(p2 + s2.size())); } catch (...) {}
                };
                auto boolVal = [&](const std::string& k) -> bool {
                    const std::string s2 = '"' + k + "\":true";
                    return obj.find(s2) != std::string::npos;
                };
                auto strVal = [&](const std::string& k, std::string& out) {
                    const std::string s2 = '"' + k + "\":\"";
                    size_t p2 = obj.find(s2);
                    if (p2 == std::string::npos) return;
                    size_t st = p2 + s2.size();
                    size_t en = st;
                    while (en < obj.size() && !(obj[en] == '"' && (en == 0 || obj[en-1] != '\\'))) ++en;
                    out = JsonUnescape(obj.substr(st, en - st));
                };

                double gain = 0.0;
                double pan = 0.0;
                double inserts = 0.0;
                numVal("gain_db", gain);
                numVal("pan", pan);
                numVal("insert_slots", inserts);
                const bool mute = boolVal("mute");
                std::string busName;
                std::string insertEffectsCsv;
                std::string insertBypassCsv;
                std::string insertConfigCsv;
                strVal("name", busName);
                strVal("insert_effects", insertEffectsCsv);
                strVal("insert_bypass", insertBypassCsv);
                strVal("insert_params", insertConfigCsv);

                BusData& bus = state.project.buses[static_cast<size_t>(busIdx)];
                bus.name = Utf8ToWstr(busName);
                bus.gainDb = static_cast<float>(gain);
                bus.pan = static_cast<float>(pan);
                bus.mute = mute;
                const int slotCount = std::clamp(static_cast<int>(inserts), 0, 8);
                bus.insertSlots = slotCount;
                DecodeInsertEffectsCsv(insertEffectsCsv, slotCount, &bus.insertEffects);
                DecodeInsertBypassCsv(insertBypassCsv, slotCount, &bus.insertBypass);
                DecodeInsertConfigCsv(insertConfigCsv, slotCount, &bus.insertConfig);

                cur = obClose + 1;
                ++busIdx;
            }
        }
    }

    // Parse tracks array
    {
        size_t pos = json.find("\"tracks\"");
        size_t arrStart = (pos != std::string::npos) ? json.find('[', pos) : std::string::npos;
        size_t arrEnd   = (arrStart != std::string::npos) ? json.find(']', arrStart) : std::string::npos;
        if (arrStart != std::string::npos && arrEnd != std::string::npos) {
            size_t cur = arrStart + 1;
            while (cur < arrEnd) {
                size_t obOpen  = json.find('{', cur);
                if (obOpen == std::string::npos || obOpen > arrEnd) break;
                size_t obClose = json.find('}', obOpen);
                if (obClose == std::string::npos) break;
                const std::string obj = json.substr(obOpen + 1, obClose - obOpen - 1);

                auto strVal = [&](const std::string& k, std::string& out) {
                    const std::string s2 = '"' + k + "\":\"";
                    size_t p2 = obj.find(s2);
                    if (p2 == std::string::npos) return;
                    size_t st = p2 + s2.size();
                    size_t en = st;
                    while (en < obj.size() && !(obj[en] == '"' && (en == 0 || obj[en-1] != '\\'))) ++en;
                    out = JsonUnescape(obj.substr(st, en - st));
                };
                auto numVal = [&](const std::string& k, double& out) {
                    const std::string s2 = '"' + k + "\":";
                    size_t p2 = obj.find(s2);
                    if (p2 == std::string::npos) return;
                    try { out = std::stod(obj.substr(p2 + s2.size())); } catch (...) {}
                };
                auto boolVal = [&](const std::string& k) -> bool {
                    const std::string s2 = '"' + k + "\":true";
                    return obj.find(s2) != std::string::npos;
                };

                std::string tname; strVal("name", tname);
                double gain = 0.0;  numVal("gain_db", gain);
                double busIndex = 1.0; numVal("bus_index", busIndex);
                double pan = 0.0; numVal("pan", pan);
                double inserts = 0.0; numVal("insert_slots", inserts);
                std::string insertEffectsCsv; strVal("insert_effects", insertEffectsCsv);
                std::string insertBypassCsv; strVal("insert_bypass", insertBypassCsv);
                std::string insertConfigCsv; strVal("insert_params", insertConfigCsv);
                const bool mute = boolVal("mute");
                const bool solo = boolVal("solo");
                const bool arm  = boolVal("record_arm");

                TrackData track{};
                track.name = Utf8ToWstr(tname);
                track.gainDb = static_cast<float>(gain);
                track.mute = mute;
                track.solo = solo;
                track.recordArm = arm;
                track.busIndex = std::clamp(static_cast<int>(busIndex), 0, kBusCount - 1);
                track.pan = std::clamp(static_cast<float>(pan), -1.0f, 1.0f);
                const int slotCount = std::clamp(static_cast<int>(inserts), 0, 8);
                track.insertSlots = slotCount;
                DecodeInsertEffectsCsv(insertEffectsCsv, slotCount, &track.insertEffects);
                DecodeInsertBypassCsv(insertBypassCsv, slotCount, &track.insertBypass);
                DecodeInsertConfigCsv(insertConfigCsv, slotCount, &track.insertConfig);
                state.project.tracks.push_back(track);

                cur = obClose + 1;
            }
        }
    }

    // Parse audio_files array and load each WAV
    {
        size_t pos = json.find("\"audio_files\"");
        size_t arrStart = (pos != std::string::npos) ? json.find('[', pos) : std::string::npos;
        size_t arrEnd   = (arrStart != std::string::npos) ? json.find(']', arrStart) : std::string::npos;
        if (arrStart != std::string::npos && arrEnd != std::string::npos) {
            size_t cur = arrStart + 1;
            while (cur < arrEnd) {
                size_t q1 = json.find('"', cur);
                if (q1 == std::string::npos || q1 > arrEnd) break;
                size_t q2 = q1 + 1;
                while (q2 < arrEnd && !(json[q2] == '"' && json[q2-1] != '\\')) ++q2;
                const std::wstring srcPath = Utf8ToWstr(JsonUnescape(json.substr(q1 + 1, q2 - q1 - 1)));
                LoadedAudio audio{};
                std::wstring err;
                if (std::filesystem::exists(srcPath) && LoadWavStereo(srcPath, &audio, &err)) {
                    if (state.project.projectSampleRate == 0) state.project.projectSampleRate = audio.sampleRate;
                    state.project.audio.push_back(std::move(audio));
                } else {
                    // Push placeholder so clip audio_index references stay valid
                    LoadedAudio placeholder{};
                    placeholder.sourcePath = srcPath;
                    placeholder.displayName = std::filesystem::path(srcPath).filename().wstring() + L" [missing]";
                    state.project.audio.push_back(std::move(placeholder));
                }
                cur = q2 + 1;
            }
        }
    }

    // Parse clips array
    {
        size_t pos = json.find("\"clips\"");
        size_t arrStart = (pos != std::string::npos) ? json.find('[', pos) : std::string::npos;
        size_t arrEnd   = (arrStart != std::string::npos) ? json.find(']', arrStart) : std::string::npos;
        if (arrStart != std::string::npos && arrEnd != std::string::npos) {
            size_t cur = arrStart + 1;
            while (cur < arrEnd) {
                size_t obOpen  = json.find('{', cur);
                if (obOpen == std::string::npos || obOpen > arrEnd) break;
                size_t obClose = json.find('}', obOpen);
                if (obClose == std::string::npos) break;
                const std::string obj = json.substr(obOpen + 1, obClose - obOpen - 1);

                auto numVal = [&](const std::string& k, double& out) {
                    const std::string s2 = '"' + k + "\":";
                    size_t p2 = obj.find(s2);
                    if (p2 == std::string::npos) return;
                    try { out = std::stod(obj.substr(p2 + s2.size())); } catch (...) {}
                };
                auto strVal2 = [&](const std::string& k, std::string& out) {
                    const std::string s2 = '"' + k + "\":\"";
                    size_t p2 = obj.find(s2);
                    if (p2 == std::string::npos) return;
                    size_t st = p2 + s2.size();
                    size_t en = st;
                    while (en < obj.size() && !(obj[en] == '"' && (en == 0 || obj[en-1] != '\\'))) ++en;
                    out = JsonUnescape(obj.substr(st, en - st));
                };

                double trackIdx = -1, audioIdx = -1, startBeat = 0, lengthBeats = 4;
                std::string cname;
                numVal("track_index",  trackIdx);
                numVal("audio_index",  audioIdx);
                numVal("start_beat",   startBeat);
                numVal("length_beats", lengthBeats);
                double srcOffsetFrames = 0;
                numVal("source_offset_frames", srcOffsetFrames);
                strVal2("name", cname);

                static const COLORREF kClipColors[4] = {
                    RGB(88, 131, 199), RGB(97, 163, 122),
                    RGB(193, 125, 91), RGB(169, 118, 188)
                };
                ClipItem clip{};
                clip.trackIndex  = static_cast<int>(trackIdx);
                clip.audioIndex  = static_cast<int>(audioIdx);
                clip.startBeat   = static_cast<float>(startBeat);
                clip.lengthBeats = static_cast<float>(lengthBeats);
                clip.sourceOffsetFrames = static_cast<std::uint64_t>(std::max(0.0, srcOffsetFrames));
                clip.name        = Utf8ToWstr(cname);
                clip.color = (clip.trackIndex >= 0)
                    ? kClipColors[static_cast<size_t>(clip.trackIndex) % 4]
                    : kClipColors[0];
                state.project.clips.push_back(clip);

                cur = obClose + 1;
            }
        }
    }

    state.projectFilePath = path;
    state.projectModified = false;

    // Runtime DSP state is not serialized; reset to defaults after load.
    state.trackInsertDspState.assign(state.project.tracks.size(), InsertDspStateArray{});
    state.busInsertDspState = {};


    return true;
}

bool DoSaveAs(HWND hwnd, UiState& state) {
    wchar_t buf[MAX_PATH] = {};
    if (!state.projectFilePath.empty()) {
        wcsncpy_s(buf, state.projectFilePath.c_str(), MAX_PATH - 1);
    } else {
        wcsncpy_s(buf, L"Untitled.dawproj", MAX_PATH - 1);
    }
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = hwnd;
    ofn.lpstrFilter = L"DAW Project (*.dawproj)\0*.dawproj\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile   = buf;
    ofn.nMaxFile    = MAX_PATH;
    ofn.lpstrDefExt = L"dawproj";
    ofn.Flags       = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameW(&ofn)) return false;
    if (!SaveProject(buf, state)) {
        MessageBoxW(hwnd, L"Failed to save project.", L"Save", MB_OK | MB_ICONERROR);
        return false;
    }
    state.projectFilePath = buf;
    state.projectModified = false;
    UpdateWindowTitle(hwnd, state);
    return true;
}

bool DoSave(HWND hwnd, UiState& state) {
    if (state.projectFilePath.empty()) return DoSaveAs(hwnd, state);
    if (!SaveProject(state.projectFilePath, state)) {
        MessageBoxW(hwnd, L"Failed to save project.", L"Save", MB_OK | MB_ICONERROR);
        return false;
    }
    state.projectModified = false;
    UpdateWindowTitle(hwnd, state);
    return true;
}

bool DoOpen(HWND hwnd, UiState& state) {
    wchar_t buf[MAX_PATH] = {};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = hwnd;
    ofn.lpstrFilter = L"DAW Project (*.dawproj)\0*.dawproj\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile   = buf;
    ofn.nMaxFile    = MAX_PATH;
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&ofn)) return false;
    EnterCriticalSection(&state.audioStateLock);
    const bool ok = LoadProject(buf, state);
    LeaveCriticalSection(&state.audioStateLock);
    if (!ok) {
        MessageBoxW(hwnd, L"Failed to open project.", L"Open", MB_OK | MB_ICONERROR);
        return false;
    }
    UpdateWindowTitle(hwnd, state);
    return true;
}
