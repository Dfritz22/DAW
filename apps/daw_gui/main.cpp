#include "draw.h"
#include "io/wav_io.h"
#include "dsp/chain.h"

void UpdateWindowTitle(HWND hwnd, const UiState& state) {
    std::wstring name = state.projectFilePath.empty()
        ? L"Untitled"
        : std::filesystem::path(state.projectFilePath).stem().wstring();
    std::wstring title = L"DAW  -  " + name + (state.projectModified ? L" *" : L"");
    SetWindowTextW(hwnd, title.c_str());
}

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


InsertEffectArray DefaultInsertEffects() {
    InsertEffectArray effects{};
    for (int i = 0; i < kMaxInsertSlots; ++i) {
        effects[static_cast<size_t>(i)] = static_cast<std::uint8_t>(i % kInsertEffectTypeCount);
    }
    return effects;
}

InsertBypassArray DefaultInsertBypass() {
    InsertBypassArray bypass{};
    for (int i = 0; i < kMaxInsertSlots; ++i) {
        bypass[static_cast<size_t>(i)] = false;
    }
    return bypass;
}

InsertParamsArray DefaultInsertParams() {
    InsertParamsArray arr;
    // arr is value-initialized; InsertParams uses in-class defaults, so nothing extra needed.
    return arr;
}

// Encode all params for all slots as a flat pipe-delimited CSV string.
// Format: slot0_val0,val1,...valN|slot1_val0,...|...
// The float layout per slot is fixed and must match DecodeInsertParamsCsv.
std::string EncodeInsertParamsCsv(const InsertParamsArray& params, int slotCount) {
    std::ostringstream out;
    out << std::fixed;
    out.precision(4);
    const int count = std::clamp(slotCount, 0, kMaxInsertSlots);
    for (int s = 0; s < count; ++s) {
        if (s > 0) out << '|';
        const InsertParams& p = params[static_cast<size_t>(s)];
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

void DecodeInsertParamsCsv(const std::string& csv, int slotCount, InsertParamsArray* params) {
    if (!params) return;
    *params = DefaultInsertParams();
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
        InsertParams& p = (*params)[static_cast<size_t>(s)];
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

std::string EncodeInsertEffectsCsv(const InsertEffectArray& effects, int slotCount) {
    std::ostringstream out;
    const int count = std::clamp(slotCount, 0, kMaxInsertSlots);
    for (int i = 0; i < count; ++i) {
        if (i > 0) out << ',';
        out << static_cast<int>(effects[static_cast<size_t>(i)]);
    }
    return out.str();
}

std::string EncodeInsertBypassCsv(const InsertBypassArray& bypass, int slotCount) {
    std::ostringstream out;
    const int count = std::clamp(slotCount, 0, kMaxInsertSlots);
    for (int i = 0; i < count; ++i) {
        if (i > 0) out << ',';
        out << (bypass[static_cast<size_t>(i)] ? 1 : 0);
    }
    return out.str();
}

void DecodeInsertEffectsCsv(const std::string& csv, int slotCount, InsertEffectArray* effects) {
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

void DecodeInsertBypassCsv(const std::string& csv, int slotCount, InsertBypassArray* bypass) {
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

static AudioBackend AudioBackendFromJson(const std::string& value);

bool SaveProject(const std::wstring& path, UiState& state) {
    // Sync legacy UiState fields to ProjectData before saving
    state.project.bpm = static_cast<float>(state.bpm);
    state.project.projectSampleRate = state.projectSampleRate;
    state.project.audio = state.audio;
    state.project.clips = state.clips;
    
    // Reconstruct ProjectData tracks from legacy track vectors
    state.project.tracks.clear();
    for (size_t i = 0; i < state.tracks.size(); ++i) {
        TrackData track{};
        track.name = state.tracks[i];
        track.gainDb = (i < state.trackGainDb.size()) ? state.trackGainDb[i] : 0.0f;
        track.mute = (i < state.trackMute.size()) ? state.trackMute[i] : false;
        track.solo = (i < state.trackSolo.size()) ? state.trackSolo[i] : false;
        track.recordArm = (i < state.trackRecordArm.size()) ? state.trackRecordArm[i] : false;
        track.busIndex = (i < state.trackBusIndex.size()) ? state.trackBusIndex[i] : 1;
        track.pan = (i < state.trackPan.size()) ? state.trackPan[i] : 0.0f;
        track.insertSlots = (i < state.trackInsertSlots.size()) ? state.trackInsertSlots[i] : 0;
        if (i < state.trackInsertEffects.size()) track.insertEffects = state.trackInsertEffects[i];
        if (i < state.trackInsertBypass.size()) track.insertBypass = state.trackInsertBypass[i];
        if (i < state.trackInsertParams.size()) track.insertParams = state.trackInsertParams[i];
        state.project.tracks.push_back(track);
    }
    
    // Reconstruct ProjectData buses from legacy bus vectors
    for (int b = 0; b < kBusCount; ++b) {
        if (b >= static_cast<int>(state.project.buses.size())) {
            state.project.buses.push_back(BusData{});
        }
        auto& bus = state.project.buses[static_cast<size_t>(b)];
        bus.name = Utf8ToWstr(WstrToUtf8(BusName(b)));
        bus.gainDb = BusGainDbAt(state, b);
        bus.mute = BusMuteAt(state, b);
        bus.pan = BusPanAt(state, b);
        bus.insertSlots = (b < static_cast<int>(state.busInsertSlots.size())) ? state.busInsertSlots[static_cast<size_t>(b)] : 0;
        if (b < static_cast<int>(state.busInsertEffects.size())) bus.insertEffects = state.busInsertEffects[static_cast<size_t>(b)];
        if (b < static_cast<int>(state.busInsertBypass.size())) bus.insertBypass = state.busInsertBypass[static_cast<size_t>(b)];
        if (b < static_cast<int>(state.busInsertParams.size())) bus.insertParams = state.busInsertParams[static_cast<size_t>(b)];
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
        const std::string insertParams = JsonEscape(EncodeInsertParamsCsv(bus.insertParams, bus.insertSlots));
        js << "    {";
        js << "\"name\":\"" << JsonEscape(WstrToUtf8(bus.name)) << "\",";
        js << "\"gain_db\":" << bus.gainDb << ",";
        js << "\"mute\":" << (bus.mute ? "true" : "false") << ",";
        js << "\"pan\":" << bus.pan << ",";
        js << "\"insert_slots\":" << bus.insertSlots << ",";
        js << "\"insert_effects\":\"" << insertEffects << "\",";
        js << "\"insert_bypass\":\"" << insertBypass << "\",";
        js << "\"insert_params\":\"" << insertParams << "\"";
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
        const std::string insertParams = JsonEscape(EncodeInsertParamsCsv(track.insertParams, track.insertSlots));
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
        js << "\"insert_params\":\"" << insertParams << "\"";
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
                std::string insertParamsCsv;
                strVal("name", busName);
                strVal("insert_effects", insertEffectsCsv);
                strVal("insert_bypass", insertBypassCsv);
                strVal("insert_params", insertParamsCsv);

                BusData& bus = state.project.buses[static_cast<size_t>(busIdx)];
                bus.name = Utf8ToWstr(busName);
                bus.gainDb = static_cast<float>(gain);
                bus.pan = static_cast<float>(pan);
                bus.mute = mute;
                const int slotCount = std::clamp(static_cast<int>(inserts), 0, 8);
                bus.insertSlots = slotCount;
                DecodeInsertEffectsCsv(insertEffectsCsv, slotCount, &bus.insertEffects);
                DecodeInsertBypassCsv(insertBypassCsv, slotCount, &bus.insertBypass);
                DecodeInsertParamsCsv(insertParamsCsv, slotCount, &bus.insertParams);

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
                std::string insertParamsCsv; strVal("insert_params", insertParamsCsv);
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
                DecodeInsertParamsCsv(insertParamsCsv, slotCount, &track.insertParams);
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

    // Sync ProjectData back to legacy UiState fields for backward compatibility
    state.bpm = static_cast<int>(state.project.bpm);
    state.projectSampleRate = state.project.projectSampleRate;
    state.audio = state.project.audio;
    state.clips = state.project.clips;
    state.tracks.clear();
    state.trackGainDb.clear();
    state.trackMute.clear();
    state.trackSolo.clear();
    state.trackRecordArm.clear();
    state.trackBusIndex.clear();
    state.trackPan.clear();
    state.trackInsertSlots.clear();
    state.trackInsertEffects.clear();
    state.trackInsertBypass.clear();
    state.trackInsertParams.clear();
    for (const auto& track : state.project.tracks) {
        state.tracks.push_back(track.name);
        state.trackGainDb.push_back(track.gainDb);
        state.trackMute.push_back(track.mute);
        state.trackSolo.push_back(track.solo);
        state.trackRecordArm.push_back(track.recordArm);
        state.trackBusIndex.push_back(track.busIndex);
        state.trackPan.push_back(track.pan);
        state.trackInsertSlots.push_back(track.insertSlots);
        state.trackInsertEffects.push_back(track.insertEffects);
        state.trackInsertBypass.push_back(track.insertBypass);
        state.trackInsertParams.push_back(track.insertParams);
    }
    state.busGainDb.clear();
    state.busMute.clear();
    state.busPan.clear();
    state.busInsertSlots.clear();
    state.busInsertEffects.clear();
    state.busInsertBypass.clear();
    state.busInsertParams.clear();
    for (int i = 0; i < kBusCount && i < static_cast<int>(state.project.buses.size()); ++i) {
        const auto& bus = state.project.buses[static_cast<size_t>(i)];
        state.busGainDb.push_back(bus.gainDb);
        state.busMute.push_back(bus.mute);
        state.busPan.push_back(bus.pan);
        state.busInsertSlots.push_back(bus.insertSlots);
        state.busInsertEffects.push_back(bus.insertEffects);
        state.busInsertBypass.push_back(bus.insertBypass);
        state.busInsertParams.push_back(bus.insertParams);
    }

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

float DbToLinear(float db) {
    return std::pow(10.0f, db / 20.0f);
}

// ── Undo / redo ──────────────────────────────────────────────────────────────
static constexpr int kMaxUndoLevels = 50;

void PushUndo(UiState& state) {
    state.undoStack.push_back({state.clips, state.trackGainDb});
    state.redoStack.clear();
    if (static_cast<int>(state.undoStack.size()) > kMaxUndoLevels) {
        state.undoStack.erase(state.undoStack.begin());
    }
}

void ApplyUndo(HWND hwnd, UiState& state) {
    if (state.undoStack.empty()) return;
    state.redoStack.push_back({state.clips, state.trackGainDb});
    state.clips       = state.undoStack.back().clips;
    state.trackGainDb = state.undoStack.back().trackGainDb;
    state.undoStack.pop_back();
    state.selectedClipIndex = -1;
    state.projectModified = true;
    UpdateWindowTitle(hwnd, state);
}

void ApplyRedo(HWND hwnd, UiState& state) {
    if (state.redoStack.empty()) return;
    state.undoStack.push_back({state.clips, state.trackGainDb});
    state.clips       = state.redoStack.back().clips;
    state.trackGainDb = state.redoStack.back().trackGainDb;
    state.redoStack.pop_back();
    state.selectedClipIndex = -1;
    state.projectModified = true;
    UpdateWindowTitle(hwnd, state);
}

// ── Clip editing helpers ─────────────────────────────────────────────────────
void SplitSelectedClip(UiState& state) {
    if (state.selectedClipIndex < 0 ||
        state.selectedClipIndex >= static_cast<int>(state.clips.size())) return;
    const float splitBeat = state.playheadBeat;
    ClipItem& clip = state.clips[static_cast<size_t>(state.selectedClipIndex)];
    if (splitBeat <= clip.startBeat + 0.01f ||
        splitBeat >= clip.startBeat + clip.lengthBeats - 0.01f) return;

    PushUndo(state);
    // Re-acquire reference after PushUndo (it copies to undoStack but does not reallocate state.clips)
    ClipItem& orig = state.clips[static_cast<size_t>(state.selectedClipIndex)];
    ClipItem right = orig;
    const float splitDelta = splitBeat - orig.startBeat;
    const float spb = SamplesPerBeat(state);
    right.startBeat         = splitBeat;
    right.lengthBeats       = orig.lengthBeats - splitDelta;
    right.sourceOffsetFrames = orig.sourceOffsetFrames + static_cast<std::uint64_t>(splitDelta * spb);
    orig.lengthBeats        = splitDelta;
    state.clips.push_back(right);
    state.selectedClipIndex = -1;
    state.projectModified = true;
}

void DuplicateSelectedClip(UiState& state) {
    if (state.selectedClipIndex < 0 ||
        state.selectedClipIndex >= static_cast<int>(state.clips.size())) return;
    PushUndo(state);
    ClipItem dup = state.clips[static_cast<size_t>(state.selectedClipIndex)];
    dup.startBeat += dup.lengthBeats;
    state.clips.push_back(dup);
    state.selectedClipIndex = static_cast<int>(state.clips.size()) - 1;
    state.projectModified = true;
}

void NudgeSelectedClip(UiState& state, float deltaBeats) {
    if (state.selectedClipIndex < 0 ||
        state.selectedClipIndex >= static_cast<int>(state.clips.size())) return;
    PushUndo(state);
    ClipItem& clip = state.clips[static_cast<size_t>(state.selectedClipIndex)];
    clip.startBeat = std::max(0.0f, clip.startBeat + deltaBeats);
    state.projectModified = true;
}

void DeleteSelectedClip(UiState& state) {
    if (state.selectedClipIndex < 0 ||
        state.selectedClipIndex >= static_cast<int>(state.clips.size())) {
        return;
    }
    PushUndo(state);
    state.clips.erase(state.clips.begin() + state.selectedClipIndex);
    state.selectedClipIndex = -1;
    state.projectModified = true;
}

const wchar_t* BusName(int busIndex) {
    static const wchar_t* kNames[kBusCount] = {L"Drums", L"Music", L"Vocals", L"Master"};
    if (busIndex < 0 || busIndex >= kBusCount) {
        return L"Music";
    }
    return kNames[busIndex];
}

float BusGainDbAt(const UiState& state, int busIndex) {
    if (busIndex < 0 || busIndex >= static_cast<int>(state.busGainDb.size())) {
        return 0.0f;
    }
    return state.busGainDb[static_cast<size_t>(busIndex)];
}

float BusPanAt(const UiState& state, int busIndex) {
    if (busIndex < 0 || busIndex >= static_cast<int>(state.busPan.size())) {
        return 0.0f;
    }
    return std::clamp(state.busPan[static_cast<size_t>(busIndex)], -1.0f, 1.0f);
}

bool BusMuteAt(const UiState& state, int busIndex) {
    if (busIndex < 0 || busIndex >= static_cast<int>(state.busMute.size())) {
        return false;
    }
    return state.busMute[static_cast<size_t>(busIndex)];
}

void ApplyBalancePan(float pan, float* left, float* right) {
    const float p = std::clamp(pan, -1.0f, 1.0f);
    if (p < 0.0f) {
        *right *= (1.0f + p);
    } else if (p > 0.0f) {
        *left *= (1.0f - p);
    }
}

int AddNewTrack(UiState& state) {
    const int index = static_cast<int>(state.tracks.size());
    state.tracks.push_back(L"Track " + std::to_wstring(index + 1));
    state.trackGainDb.push_back(0.0f);
    state.trackMute.push_back(false);
    state.trackSolo.push_back(false);
    state.trackRecordArm.push_back(false);
    state.trackBusIndex.push_back(1);
    state.trackPan.push_back(0.0f);
    state.trackInsertSlots.push_back(0);
    state.trackInsertEffects.push_back(DefaultInsertEffects());
    state.trackInsertBypass.push_back(DefaultInsertBypass());
    state.trackInsertParams.push_back(DefaultInsertParams());
    return index;
}

void DeleteTrackAt(UiState& state, int trackIndex) {
    if (trackIndex < 0 || trackIndex >= static_cast<int>(state.tracks.size())) {
        return;
    }

    EnterCriticalSection(&state.audioStateLock);

    state.tracks.erase(state.tracks.begin() + trackIndex);
    if (trackIndex < static_cast<int>(state.trackGainDb.size())) {
        state.trackGainDb.erase(state.trackGainDb.begin() + trackIndex);
    }
    if (trackIndex < static_cast<int>(state.trackMute.size())) {
        state.trackMute.erase(state.trackMute.begin() + trackIndex);
    }
    if (trackIndex < static_cast<int>(state.trackSolo.size())) {
        state.trackSolo.erase(state.trackSolo.begin() + trackIndex);
    }
    if (trackIndex < static_cast<int>(state.trackRecordArm.size())) {
        state.trackRecordArm.erase(state.trackRecordArm.begin() + trackIndex);
    }
    if (trackIndex < static_cast<int>(state.trackBusIndex.size())) {
        state.trackBusIndex.erase(state.trackBusIndex.begin() + trackIndex);
    }
    if (trackIndex < static_cast<int>(state.trackPan.size())) {
        state.trackPan.erase(state.trackPan.begin() + trackIndex);
    }
    if (trackIndex < static_cast<int>(state.trackInsertSlots.size())) {
        state.trackInsertSlots.erase(state.trackInsertSlots.begin() + trackIndex);
    }
    if (trackIndex < static_cast<int>(state.trackInsertEffects.size())) {
        state.trackInsertEffects.erase(state.trackInsertEffects.begin() + trackIndex);
    }
    if (trackIndex < static_cast<int>(state.trackInsertBypass.size())) {
        state.trackInsertBypass.erase(state.trackInsertBypass.begin() + trackIndex);
    }
    if (trackIndex < static_cast<int>(state.trackInsertParams.size())) {
        state.trackInsertParams.erase(state.trackInsertParams.begin() + trackIndex);
    }

    for (int i = static_cast<int>(state.clips.size()) - 1; i >= 0; --i) {
        ClipItem& clip = state.clips[static_cast<size_t>(i)];
        if (clip.trackIndex == trackIndex) {
            state.clips.erase(state.clips.begin() + i);
            continue;
        }
        if (clip.trackIndex > trackIndex) {
            clip.trackIndex -= 1;
        }
    }

    if (state.selectedClipIndex >= static_cast<int>(state.clips.size())) {
        state.selectedClipIndex = -1;
    }

    LeaveCriticalSection(&state.audioStateLock);

    if (state.selectedTrackIndex >= static_cast<int>(state.tracks.size())) {
        state.selectedTrackIndex = static_cast<int>(state.tracks.size()) - 1;
    }
    if (state.dragFaderTrack == trackIndex) {
        state.dragFaderTrack = -1;
        state.draggingFader = false;
    } else if (state.dragFaderTrack > trackIndex) {
        state.dragFaderTrack -= 1;
    }
    if (!state.dragPanIsBus && state.dragPanIndex == trackIndex) {
        state.dragPanIndex = -1;
        state.draggingPan = false;
    } else if (!state.dragPanIsBus && state.dragPanIndex > trackIndex) {
        state.dragPanIndex -= 1;
    }
}

void ImportWavFiles(HWND hwnd, UiState& state);

static bool IsWasapiBackend(AudioBackend backend) {
    return backend == AudioBackend::WasapiShared || backend == AudioBackend::WasapiExclusive;
}

static const wchar_t* AudioBackendLabel(AudioBackend backend) {
    switch (backend) {
    case AudioBackend::MME:
        return L"MME";
    case AudioBackend::WasapiShared:
        return L"WASAPI Shared";
    case AudioBackend::WasapiExclusive:
        return L"WASAPI Exclusive";
    case AudioBackend::Asio:
        return L"ASIO (future)";
    default:
        return L"Unknown";
    }
}

static std::string AudioBackendToJson(AudioBackend backend) {
    switch (backend) {
    case AudioBackend::MME:
        return "mme";
    case AudioBackend::WasapiShared:
        return "wasapi_shared";
    case AudioBackend::WasapiExclusive:
        return "wasapi_exclusive";
    case AudioBackend::Asio:
        return "asio";
    default:
        return "wasapi_shared";
    }
}

static AudioBackend AudioBackendFromJson(const std::string& value) {
    if (value == "mme") return AudioBackend::MME;
    if (value == "wasapi_exclusive") return AudioBackend::WasapiExclusive;
    if (value == "asio") return AudioBackend::Asio;
    return AudioBackend::WasapiShared;
}

std::wstring BuildAudioDiagnosticsReport(const UiState& state) {
    auto queryInputFmt = [](UINT dev, int sr, int ch) {
        WAVEFORMATEX fmt{};
        fmt.wFormatTag = WAVE_FORMAT_PCM;
        fmt.nChannels = static_cast<WORD>(ch);
        fmt.nSamplesPerSec = static_cast<DWORD>(sr);
        fmt.wBitsPerSample = 16;
        fmt.nBlockAlign = static_cast<WORD>((fmt.nChannels * fmt.wBitsPerSample) / 8);
        fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;
        fmt.cbSize = 0;
        const MMRESULT r = waveInOpen(nullptr, dev, &fmt, 0, 0, WAVE_FORMAT_QUERY);
        return r == MMSYSERR_NOERROR;
    };

    auto queryOutputFmt = [](UINT dev, int sr, int ch) {
        WAVEFORMATEX fmt{};
        fmt.wFormatTag = WAVE_FORMAT_PCM;
        fmt.nChannels = static_cast<WORD>(ch);
        fmt.nSamplesPerSec = static_cast<DWORD>(sr);
        fmt.wBitsPerSample = 16;
        fmt.nBlockAlign = static_cast<WORD>((fmt.nChannels * fmt.wBitsPerSample) / 8);
        fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;
        fmt.cbSize = 0;
        const MMRESULT r = waveOutOpen(nullptr, dev, &fmt, 0, 0, WAVE_FORMAT_QUERY);
        return r == MMSYSERR_NOERROR;
    };

    auto yn = [](bool ok) -> const wchar_t* { return ok ? L"OK" : L"NO"; };

    std::wstringstream ss;
    ss << L"Project SR: " << state.projectSampleRate << L"\n";
     ss << L"Audio Backend: " << AudioBackendLabel(state.audioBackend)
       << (state.playingViaWasapi ? L" (output: WASAPI)" : L" (output: MME)") << L"\n";
     ss << L"Preferred SR: " << state.preferredSampleRate << L"\n";
     ss << L"Preferred Buffer: " << state.preferredBufferFrames << L" frames\n";
     ss << L"Active Device SR: " << state.activeDeviceSampleRate << L"\n";
     ss << L"Active Device Buffer: " << state.activeDeviceBufferFrames << L" frames\n";
    ss << L"Selected Input: " << state.selectedInputDeviceName << L" (id=" << state.selectedInputDeviceId << L")\n";
    ss << L"Selected Output: " << state.selectedOutputDeviceName << L" (id=" << state.selectedOutputDeviceId << L")\n\n";

    ss << L"Last Opened Input Format: "
       << state.lastOpenedInputSampleRate << L" Hz, " << state.lastOpenedInputChannels << L" ch\n";
    ss << L"Last Opened Output Format: "
       << state.lastOpenedOutputSampleRate << L" Hz, " << state.lastOpenedOutputChannels << L" ch\n";
    if (!state.lastPlaybackInitError.empty()) {
        ss << L"Last Playback Init Error: " << state.lastPlaybackInitError << L"\n";
    }
    ss << L"Last Committed Take: "
       << state.lastCommittedTakeSampleRate << L" Hz, "
       << state.lastCommittedTakeChannels << L" ch, "
       << state.lastCommittedTakeFrames << L" frames\n\n";
    ss << L"Capture Elapsed: " << state.lastCaptureElapsedMs << L" ms\n";
    ss << L"Observed Frame Ratio: " << state.lastCaptureObservedRateRatio << L"\n";
    ss << L"Applied Capture Stride: " << state.lastCaptureFrameStride << L"\n\n";

    const int probeA = (state.preferredSampleRate > 0) ? state.preferredSampleRate : state.projectSampleRate;
    const int probeB = (state.projectSampleRate > 0 && state.projectSampleRate != probeA) ? state.projectSampleRate : state.lastOpenedOutputSampleRate;

    ss << L"Input Query (16-bit PCM)\n";
    if (probeA > 0) {
        ss << L"  " << probeA << L" mono:   " << yn(queryInputFmt(state.selectedInputDeviceId, probeA, 1)) << L"\n";
        ss << L"  " << probeA << L" stereo: " << yn(queryInputFmt(state.selectedInputDeviceId, probeA, 2)) << L"\n";
    }
    if (probeB > 0 && probeB != probeA) {
        ss << L"  " << probeB << L" mono:   " << yn(queryInputFmt(state.selectedInputDeviceId, probeB, 1)) << L"\n";
        ss << L"  " << probeB << L" stereo: " << yn(queryInputFmt(state.selectedInputDeviceId, probeB, 2)) << L"\n";
    }
    ss << L"\nOutput Query (16-bit PCM)\n";
    if (probeA > 0) {
        ss << L"  " << probeA << L" stereo: " << yn(queryOutputFmt(state.selectedOutputDeviceId, probeA, 2)) << L"\n";
    }
    if (probeB > 0 && probeB != probeA) {
        ss << L"  " << probeB << L" stereo: " << yn(queryOutputFmt(state.selectedOutputDeviceId, probeB, 2)) << L"\n";
    }
    ss << L"\nTip: For MME full-duplex stability, input and output should both report OK at the same sample rate.\n";

    return ss.str();
}

void RefreshInputDevices(UiState& state) {
    state.inputDeviceIds.clear();
    state.inputDeviceNames.clear();

    const std::wstring previousName = state.selectedInputDeviceName;

    const UINT count = waveInGetNumDevs();
    for (UINT i = 0; i < count; ++i) {
        WAVEINCAPSW caps{};
        if (waveInGetDevCapsW(i, &caps, sizeof(caps)) != MMSYSERR_NOERROR) {
            continue;
        }
        state.inputDeviceIds.push_back(i);
        state.inputDeviceNames.push_back(caps.szPname);
    }

    if (state.inputDeviceIds.empty()) {
        state.selectedInputDeviceId = WAVE_MAPPER;
        state.selectedInputDeviceName = L"No Input Devices";
        return;
    }

    bool selectedStillExists = false;
    for (size_t i = 0; i < state.inputDeviceIds.size(); ++i) {
        if (state.inputDeviceIds[i] == state.selectedInputDeviceId) {
            state.selectedInputDeviceName = state.inputDeviceNames[i];
            selectedStillExists = true;
            break;
        }
    }

    if (!selectedStillExists) {
        for (size_t i = 0; i < state.inputDeviceNames.size(); ++i) {
            if (_wcsicmp(state.inputDeviceNames[i].c_str(), previousName.c_str()) == 0) {
                state.selectedInputDeviceId = state.inputDeviceIds[i];
                state.selectedInputDeviceName = state.inputDeviceNames[i];
                selectedStillExists = true;
                break;
            }
        }
    }

    if (!selectedStillExists) {
        state.selectedInputDeviceId = state.inputDeviceIds[0];
        state.selectedInputDeviceName = state.inputDeviceNames[0];
    }
}

void RefreshOutputDevices(UiState& state) {
    state.outputDeviceIds.clear();
    state.outputDeviceNames.clear();

    const std::wstring previousName = state.selectedOutputDeviceName;

    const UINT count = waveOutGetNumDevs();
    for (UINT i = 0; i < count; ++i) {
        WAVEOUTCAPSW caps{};
        if (waveOutGetDevCapsW(i, &caps, sizeof(caps)) != MMSYSERR_NOERROR) {
            continue;
        }
        state.outputDeviceIds.push_back(i);
        state.outputDeviceNames.push_back(caps.szPname);
    }

    if (state.outputDeviceIds.empty()) {
        state.selectedOutputDeviceId = WAVE_MAPPER;
        state.selectedOutputDeviceName = L"No Output Devices";
        return;
    }

    bool selectedStillExists = false;
    for (size_t i = 0; i < state.outputDeviceIds.size(); ++i) {
        if (state.outputDeviceIds[i] == state.selectedOutputDeviceId) {
            state.selectedOutputDeviceName = state.outputDeviceNames[i];
            selectedStillExists = true;
            break;
        }
    }

    if (!selectedStillExists) {
        for (size_t i = 0; i < state.outputDeviceNames.size(); ++i) {
            if (_wcsicmp(state.outputDeviceNames[i].c_str(), previousName.c_str()) == 0) {
                state.selectedOutputDeviceId = state.outputDeviceIds[i];
                state.selectedOutputDeviceName = state.outputDeviceNames[i];
                selectedStillExists = true;
                break;
            }
        }
    }

    if (!selectedStillExists) {
        state.selectedOutputDeviceId = state.outputDeviceIds[0];
        state.selectedOutputDeviceName = state.outputDeviceNames[0];
    }
}

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
    const int sr  = state.activeDeviceSampleRate   > 0 ? state.activeDeviceSampleRate   : state.projectSampleRate;
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
            addSR(state.projectSampleRate);
            addSR(state.activeDeviceSampleRate);
            std::sort(d->sampleRates.begin(), d->sampleRates.end());
            HWND hSr = GetDlgItem(hwnd, kAsDlgSampleRate);
            int selSR = 1; // default 44100
            for (size_t i = 0; i < d->sampleRates.size(); ++i) {
                wchar_t rbuf[32]{};
                swprintf_s(rbuf, L"%d Hz", d->sampleRates[i]);
                SendMessageW(hSr, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(rbuf));
                const int target = (state.preferredSampleRate > 0) ? state.preferredSampleRate : state.projectSampleRate;
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
    RefreshInputDevices(state);
    RefreshOutputDevices(state);

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
        RefreshInputDevices(state);
        RefreshOutputDevices(state);
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
            addRate(state.projectSampleRate);
            addRate(state.activeDeviceSampleRate);
            addRate(state.lastOpenedOutputSampleRate);
            for (const LoadedAudio& a : state.audio) {
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
        RefreshInputDevices(state);
        RefreshOutputDevices(state);
    } else if (cmd == kCmdAudioDiagnostics) {
        const std::wstring diag = BuildAudioDiagnosticsReport(state);
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
        addRate(state.projectSampleRate);
        addRate(state.activeDeviceSampleRate);
        addRate(state.lastOpenedOutputSampleRate);
        for (const LoadedAudio& a : state.audio) addRate(a.sampleRate);
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


std::uint64_t ComputeProjectEndFrameLocked(const UiState& state) {
    const float samplesPerBeat = SamplesPerBeat(state);
    std::uint64_t maxFrame = 0;
    for (const ClipItem& clip : state.clips) {
        if (clip.audioIndex < 0 || clip.audioIndex >= static_cast<int>(state.audio.size())) {
            continue;
        }
        const std::uint64_t clipStartTL   = static_cast<std::uint64_t>(std::llround(std::max(0.0f, clip.startBeat) * samplesPerBeat));
        const std::uint64_t clipLenFrames = static_cast<std::uint64_t>(std::llround(clip.lengthBeats * samplesPerBeat));
        maxFrame = std::max(maxFrame, clipStartTL + clipLenFrames);
    }
    return maxFrame;
}

static bool ReadClipSampleAtProjectFrame(
    const LoadedAudio& audio,
    std::uint64_t clipFrameInProjectRate,
    int projectSampleRate,
    std::uint64_t sourceOffsetFrames,
    float* outL,
    float* outR) {
    if (outL == nullptr || outR == nullptr || audio.frames == 0 || audio.stereo.empty() || projectSampleRate <= 0) {
        return false;
    }

    const int srcRate = (audio.sampleRate > 0) ? audio.sampleRate : projectSampleRate;
    const double ratio = static_cast<double>(srcRate) / static_cast<double>(projectSampleRate);
    const double srcPos = static_cast<double>(sourceOffsetFrames) + static_cast<double>(clipFrameInProjectRate) * ratio;
    if (srcPos < 0.0) {
        return false;
    }

    const double maxSrc = static_cast<double>(audio.frames - 1);
    if (srcPos > maxSrc) {
        return false;
    }

    const std::uint64_t i0 = static_cast<std::uint64_t>(srcPos);
    const std::uint64_t i1 = std::min<std::uint64_t>(i0 + 1, audio.frames - 1);
    const float frac = static_cast<float>(srcPos - static_cast<double>(i0));

    const size_t b0 = static_cast<size_t>(i0) * 2;
    const size_t b1 = static_cast<size_t>(i1) * 2;
    if (b0 + 1 >= audio.stereo.size() || b1 + 1 >= audio.stereo.size()) {
        return false;
    }

    const float l0 = audio.stereo[b0];
    const float r0 = audio.stereo[b0 + 1];
    const float l1 = audio.stereo[b1];
    const float r1 = audio.stereo[b1 + 1];
    *outL = l0 + (l1 - l0) * frac;
    *outR = r0 + (r1 - r0) * frac;
    return true;
}

bool FillRealtimeBufferLocked(UiState& state, std::int16_t* outInterleaved, int frames, bool* reachedEnd) {
    if (state.projectSampleRate <= 0) {
        std::fill(outInterleaved, outInterleaved + (frames * 2), 0);
        *reachedEnd = true;
        return false;
    }

    const bool runMetPlay = state.playing && !state.recording && state.metronomePlay;
    const bool runMetRec = state.recording && state.metronomeRecord;
    // Play count-in click from Record-press until preroll ends (recordStartFrame).
    const bool runCountInClick = state.countingIn
        && state.playbackFrameCursor.load() < state.recordStartFrame;
    const bool allowNoClipPlayback = runMetPlay || runMetRec || runCountInClick || (state.recording && state.inputMonitoring);

    if (state.clips.empty() && !allowNoClipPlayback) {
        std::fill(outInterleaved, outInterleaved + (frames * 2), 0);
        *reachedEnd = true;
        return false;
    }

    const float samplesPerBeat = SamplesPerBeat(state);
    const std::uint64_t endFrame = ComputeProjectEndFrameLocked(state);
    const std::uint64_t startCursor = state.playbackFrameCursor.load();
    const float sampleRate = static_cast<float>(state.projectSampleRate);

    int activeFrames = 0;
    // Keep running while recording, during count-in, or metronome-only playback.
    if (state.recording || state.countingIn || (state.clips.empty() && (runMetPlay || runMetRec))) {
        activeFrames = frames;
        *reachedEnd = false;
    } else {
        activeFrames = static_cast<int>(
            std::min(static_cast<std::uint64_t>(frames), endFrame > startCursor ? endFrame - startCursor : 0u));
        *reachedEnd = (activeFrames < frames);
    }

    // Per-track stereo buffers: collect clips, apply track insert chain
    const int trackCount = static_cast<int>(state.tracks.size());

    // Bus stereo accumulation buffers
    std::vector<float> busBuf[kBusCount];
    for (int b = 0; b < kBusCount; ++b)
        busBuf[b].assign(static_cast<size_t>(frames) * 2, 0.0f);

    for (int ti = 0; ti < trackCount; ++ti) {
        if (!IsTrackAudible(state, ti)) continue;

        const int busIdx = std::clamp(TrackBusIndexAt(state, ti), 0, kBusCount - 1);

        // Fill track buffer from clips
        std::vector<float> trackBuf(static_cast<size_t>(activeFrames) * 2, 0.0f);
        for (const ClipItem& clip : state.clips) {
            if (clip.trackIndex != ti) continue;
            if (clip.audioIndex < 0 || clip.audioIndex >= static_cast<int>(state.audio.size())) continue;
            const LoadedAudio& a = state.audio[static_cast<size_t>(clip.audioIndex)];
            const std::uint64_t clipStart = static_cast<std::uint64_t>(
                std::llround(std::max(0.0f, clip.startBeat) * samplesPerBeat));
            const std::uint64_t clipEnd = clipStart + static_cast<std::uint64_t>(
                std::llround(clip.lengthBeats * samplesPerBeat));

            for (int i = 0; i < activeFrames; ++i) {
                const std::uint64_t gf = startCursor + static_cast<std::uint64_t>(i);
                if (gf < clipStart || gf >= clipEnd) continue;
                float l = 0.0f;
                float r = 0.0f;
                if (!ReadClipSampleAtProjectFrame(a, gf - clipStart, state.projectSampleRate, clip.sourceOffsetFrames, &l, &r)) {
                    continue;
                }
                trackBuf[static_cast<size_t>(i)*2]   += l;
                trackBuf[static_cast<size_t>(i)*2+1] += r;
            }
        }

        // Apply track insert chain
        if (ti < static_cast<int>(state.trackInsertEffects.size()) &&
            ti < static_cast<int>(state.trackInsertBypass.size()) &&
            ti < static_cast<int>(state.trackInsertParams.size()) &&
            ti < static_cast<int>(state.trackInsertSlots.size())) {
            ApplyInsertChain(trackBuf, sampleRate,
                state.trackInsertEffects[static_cast<size_t>(ti)],
                state.trackInsertBypass[static_cast<size_t>(ti)],
                state.trackInsertParams[static_cast<size_t>(ti)],
                state.trackInsertSlots[static_cast<size_t>(ti)]);
        }

        // Apply track gain + pan then mix into bus
        const float trackGain = DbToLinear(TrackGainDbAt(state, ti));
        const float pan = TrackPanAt(state, ti);
        const float panRad = (pan + 1.0f) * 0.5f * 3.14159265f * 0.5f;
        const float gainL = trackGain * std::cos(panRad);
        const float gainR = trackGain * std::sin(panRad);
        for (int i = 0; i < activeFrames; ++i) {
            busBuf[busIdx][static_cast<size_t>(i)*2]   += trackBuf[static_cast<size_t>(i)*2]   * gainL;
            busBuf[busIdx][static_cast<size_t>(i)*2+1] += trackBuf[static_cast<size_t>(i)*2+1] * gainR;
        }
    }

    // Apply bus insert chains, then mix into master
    std::vector<float> masterBuf(static_cast<size_t>(frames) * 2, 0.0f);
    for (int b = 0; b < kBusCount; ++b) {
        if (BusMuteAt(state, b)) continue;

        // Apply bus insert chain (bus 3 = master)
        if (b < static_cast<int>(state.busInsertEffects.size()) &&
            b < static_cast<int>(state.busInsertBypass.size()) &&
            b < static_cast<int>(state.busInsertParams.size()) &&
            b < static_cast<int>(state.busInsertSlots.size())) {
            ApplyInsertChain(busBuf[b], sampleRate,
                state.busInsertEffects[static_cast<size_t>(b)],
                state.busInsertBypass[static_cast<size_t>(b)],
                state.busInsertParams[static_cast<size_t>(b)],
                state.busInsertSlots[static_cast<size_t>(b)]);
        }

        const float busGain = DbToLinear(BusGainDbAt(state, b));
        const float busPan  = BusPanAt(state, b);
        const float panRad  = (busPan + 1.0f) * 0.5f * 3.14159265f * 0.5f;
        const float bGainL  = (b == 3) ? busGain : busGain * std::cos(panRad);
        const float bGainR  = (b == 3) ? busGain : busGain * std::sin(panRad);
        for (int i = 0; i < activeFrames; ++i) {
            masterBuf[static_cast<size_t>(i)*2]   += busBuf[b][static_cast<size_t>(i)*2]   * bGainL;
            masterBuf[static_cast<size_t>(i)*2+1] += busBuf[b][static_cast<size_t>(i)*2+1] * bGainR;
        }
    }

    // Input monitor path for low-latency tracking.
    if (state.recording && state.inputMonitoring && state.recordInputChannels > 0) {
        const int inCh = std::max(1, state.recordInputChannels);
        const size_t availableSamples = (state.monitorInputPcm.size() > state.monitorInputReadPos)
            ? (state.monitorInputPcm.size() - state.monitorInputReadPos)
            : 0;
        const int availableFrames = static_cast<int>(availableSamples / static_cast<size_t>(inCh));
        const int mixFrames = std::min(activeFrames, availableFrames);
        const float monGain = std::clamp(state.inputMonitorGain, 0.0f, 2.0f);
        for (int i = 0; i < mixFrames; ++i) {
            float l = 0.0f;
            float r = 0.0f;
            const size_t base = state.monitorInputReadPos + static_cast<size_t>(i * inCh);
            if (inCh == 1) {
                const float v = static_cast<float>(state.monitorInputPcm[base]) / 32768.0f;
                l = v;
                r = v;
            } else {
                l = static_cast<float>(state.monitorInputPcm[base]) / 32768.0f;
                r = static_cast<float>(state.monitorInputPcm[base + 1]) / 32768.0f;
            }
            masterBuf[static_cast<size_t>(i) * 2] += l * monGain;
            masterBuf[static_cast<size_t>(i) * 2 + 1] += r * monGain;
        }
        state.monitorInputReadPos += static_cast<size_t>(mixFrames * inCh);
        if (state.monitorInputReadPos > 16384 && state.monitorInputReadPos * 2 > state.monitorInputPcm.size()) {
            state.monitorInputPcm.erase(
                state.monitorInputPcm.begin(),
                state.monitorInputPcm.begin() + static_cast<std::vector<std::int16_t>::difference_type>(state.monitorInputReadPos));
            state.monitorInputReadPos = 0;
        }
    }

    // Metronome click (4/4, accented downbeat).
    if (runMetPlay || runMetRec || runCountInClick) {
        const float spb = std::max(1.0f, samplesPerBeat);
        const int clickSamples = std::max(1, static_cast<int>(sampleRate * 0.04f));
        const float metronomeGain = 3.0f;
        for (int i = 0; i < activeFrames; ++i) {
            const std::uint64_t gf = startCursor + static_cast<std::uint64_t>(i);
            const double beatPos = static_cast<double>(gf) / static_cast<double>(spb);
            const int beatIdx = static_cast<int>(std::floor(beatPos));
            const std::uint64_t beatStart = static_cast<std::uint64_t>(std::llround(static_cast<double>(beatIdx) * static_cast<double>(spb)));
            const int since = static_cast<int>(gf - beatStart);
            if (since < 0 || since >= clickSamples) continue;

            const bool accent = (beatIdx % 4) == 0;
            const float freq = accent ? 1800.0f : 1200.0f;
            const float amp = (accent ? 0.22f : 0.14f) * metronomeGain;
            const float env = std::exp(-4.0f * static_cast<float>(since) / static_cast<float>(clickSamples));
            const float t = static_cast<float>(since) / sampleRate;
            const float s = std::sin(2.0f * 3.14159265f * freq * t) * env * amp;
            masterBuf[static_cast<size_t>(i) * 2] += s;
            masterBuf[static_cast<size_t>(i) * 2 + 1] += s;
        }
    }

    // Convert to int16
    for (int i = 0; i < frames; ++i) {
        const float l = std::clamp(masterBuf[static_cast<size_t>(i)*2],   -1.0f, 1.0f);
        const float r = std::clamp(masterBuf[static_cast<size_t>(i)*2+1], -1.0f, 1.0f);
        outInterleaved[i*2]   = static_cast<std::int16_t>(std::lrint(l * 32767.0f));
        outInterleaved[i*2+1] = static_cast<std::int16_t>(std::lrint(r * 32767.0f));
    }

    state.playbackFrameCursor.store(startCursor + static_cast<std::uint64_t>(frames));
    return true;
}

static void ResampleStereoPcm16Linear(const std::int16_t* src, int srcFrames, std::int16_t* dst, int dstFrames) {
    if (src == nullptr || dst == nullptr || srcFrames <= 0 || dstFrames <= 0) {
        return;
    }
    if (srcFrames == 1) {
        const std::int16_t l = src[0];
        const std::int16_t r = src[1];
        for (int i = 0; i < dstFrames; ++i) {
            dst[i * 2] = l;
            dst[i * 2 + 1] = r;
        }
        return;
    }

    const double maxSrcPos = static_cast<double>(srcFrames - 1);
    for (int i = 0; i < dstFrames; ++i) {
        const double t = (dstFrames > 1)
            ? (static_cast<double>(i) * maxSrcPos / static_cast<double>(dstFrames - 1))
            : 0.0;
        const int i0 = std::clamp(static_cast<int>(std::floor(t)), 0, srcFrames - 1);
        const int i1 = std::min(i0 + 1, srcFrames - 1);
        const double frac = t - static_cast<double>(i0);

        const double l = static_cast<double>(src[i0 * 2]) * (1.0 - frac) + static_cast<double>(src[i1 * 2]) * frac;
        const double r = static_cast<double>(src[i0 * 2 + 1]) * (1.0 - frac) + static_cast<double>(src[i1 * 2 + 1]) * frac;
        dst[i * 2] = static_cast<std::int16_t>(std::clamp(std::lrint(l), static_cast<long>(-32768), static_cast<long>(32767)));
        dst[i * 2 + 1] = static_cast<std::int16_t>(std::clamp(std::lrint(r), static_cast<long>(-32768), static_cast<long>(32767)));
    }
}

static bool FillRealtimeForDeviceLocked(
    UiState& state,
    std::int16_t* outInterleaved,
    int deviceFrames,
    int deviceSampleRate,
    bool* reachedEnd)
{
    if (outInterleaved == nullptr || reachedEnd == nullptr || deviceFrames <= 0 || deviceSampleRate <= 0) {
        if (reachedEnd != nullptr) *reachedEnd = true;
        return false;
    }

    if (state.projectSampleRate <= 0) {
        state.projectSampleRate = deviceSampleRate;
    }

    const int projectSampleRate = state.projectSampleRate;
    if (projectSampleRate <= 0 || projectSampleRate == deviceSampleRate) {
        return FillRealtimeBufferLocked(state, outInterleaved, deviceFrames, reachedEnd);
    }

    const double ratio = static_cast<double>(projectSampleRate) / static_cast<double>(deviceSampleRate);
    const int projectFramesNeeded = std::max(1, static_cast<int>(std::ceil(static_cast<double>(deviceFrames) * ratio)) + 2);
    std::vector<std::int16_t> projectPcm(static_cast<size_t>(projectFramesNeeded) * 2, 0);
    bool localReachedEnd = false;
    FillRealtimeBufferLocked(state, projectPcm.data(), projectFramesNeeded, &localReachedEnd);

    ResampleStereoPcm16Linear(projectPcm.data(), projectFramesNeeded, outInterleaved, deviceFrames);
    *reachedEnd = localReachedEnd;
    return true;
}

std::wstring ToLowerCopy(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(::towlower(c)); });
    return s;
}

std::wstring QuoteArg(const std::wstring& s) {
    std::wstring out = L"\"";
    for (wchar_t c : s) {
        if (c == L'\"') {
            out += L"\\\"";
        } else {
            out += c;
        }
    }
    out += L"\"";
    return out;
}

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

struct AutoMixTrackSettings {
    float gainDb {0.0f};
    float pan {0.0f};
    int busIndex {1};

    float highpassHz {0.0f};
    float lowShelfDb {0.0f};
    float lowMidDb {0.0f};
    float presenceDb {0.0f};
    float airDb {0.0f};
    float deesserDb {0.0f};

    float compThresholdDb {-24.0f};
    float compRatio {1.0f};
    float compMakeupDb {0.0f};

    float reverbDecayS {0.0f};
    float reverbPreDelayMs {0.0f};
    float reverbMix {0.0f};
    float delayTimeMs {0.0f};
    float delayFeedback {0.0f};
    float delayMix {0.0f};
    float saturationDriveDb {0.0f};
    float saturationBlend {0.0f};
};

struct AutoMixMasterSettings {
    bool hasCompressor {false};
    float compThresholdDb {-12.0f};
    float compRatio {2.5f};
    float compMakeupDb {1.5f};
};

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

static void AutoMixAppendInsert(
    InsertEffectArray* effects,
    InsertBypassArray* bypass,
    InsertParamsArray* params,
    int* slotCount,
    int effectType,
    const InsertParams& p)
{
    if (effects == nullptr || bypass == nullptr || params == nullptr || slotCount == nullptr) return;
    if (*slotCount < 0 || *slotCount >= kMaxInsertSlots) return;
    const int s = *slotCount;
    (*effects)[static_cast<size_t>(s)] = static_cast<std::uint8_t>(std::clamp(effectType, 0, kInsertEffectTypeCount - 1));
    (*bypass)[static_cast<size_t>(s)] = false;
    (*params)[static_cast<size_t>(s)] = p;
    *slotCount = s + 1;
}

std::filesystem::path FindRepoRoot() {
    auto hasRepoMarkers = [](const std::filesystem::path& p) {
        return std::filesystem::exists(p / L".venv" / L"Scripts" / L"python.exe") &&
               std::filesystem::exists(p / L"src" / L"daw_ai");
    };

    auto scanUp = [&](std::filesystem::path p) -> std::filesystem::path {
        while (!p.empty()) {
            if (hasRepoMarkers(p)) {
                return p;
            }
            const std::filesystem::path parent = p.parent_path();
            if (parent == p) {
                break;
            }
            p = parent;
        }
        return {};
    };

    wchar_t exeBuf[MAX_PATH] = {};
    const DWORD exeLen = GetModuleFileNameW(nullptr, exeBuf, MAX_PATH);
    if (exeLen > 0 && exeLen < MAX_PATH) {
        std::filesystem::path exeDir = std::filesystem::path(exeBuf).parent_path();
        std::filesystem::path fromExe = scanUp(exeDir);
        if (!fromExe.empty()) {
            return fromExe;
        }
    }

    wchar_t cwdBuf[MAX_PATH] = {};
    const DWORD cwdLen = GetCurrentDirectoryW(MAX_PATH, cwdBuf);
    if (cwdLen > 0 && cwdLen < MAX_PATH) {
        std::filesystem::path fromCwd = scanUp(std::filesystem::path(cwdBuf));
        if (!fromCwd.empty()) {
            return fromCwd;
        }
    }

    return {};
}

bool ApplyAutoMixToFaders(HWND hwnd, UiState& state) {
    if (state.audio.empty()) {
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
    for (int ti = 0; ti < static_cast<int>(state.tracks.size()); ++ti) {
        std::vector<float> trackStereo;
        int trackSr = 0;
        EnterCriticalSection(&state.audioStateLock);
        const bool hasTrackAudio = RenderTrackToStereoLocked(state, ti, &trackStereo, &trackSr);
        const std::wstring trackName = state.tracks[static_cast<size_t>(ti)];
        LeaveCriticalSection(&state.audioStateLock);
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
        if (!WriteWavPcm16Stereo(wavPath.wstring(), trackStereo, trackSr)) {
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
    EnterCriticalSection(&state.audioStateLock);
    for (const auto& e : exportedTracks) {
        const int trackIndex = e.second;
        if (trackIndex < 0 || trackIndex >= static_cast<int>(state.trackGainDb.size())) {
            continue;
        }
        const std::wstring key = ToLowerCopy(e.first);

        if (hasFullSettings) {
            auto it = settings.find(key);
            if (it == settings.end()) continue;
            const AutoMixTrackSettings& s = it->second;

            state.trackGainDb[static_cast<size_t>(trackIndex)] = std::clamp(s.gainDb, kFaderMinDb, kFaderMaxDb);
            if (trackIndex < static_cast<int>(state.trackPan.size())) {
                state.trackPan[static_cast<size_t>(trackIndex)] = std::clamp(s.pan, -1.0f, 1.0f);
            }
            if (trackIndex < static_cast<int>(state.trackBusIndex.size())) {
                state.trackBusIndex[static_cast<size_t>(trackIndex)] = std::clamp(s.busIndex, 0, kBusCount - 1);
                ++appliedBusRoute;
            }

            if (trackIndex < static_cast<int>(state.trackInsertEffects.size()) &&
                trackIndex < static_cast<int>(state.trackInsertBypass.size()) &&
                trackIndex < static_cast<int>(state.trackInsertParams.size()) &&
                trackIndex < static_cast<int>(state.trackInsertSlots.size())) {
                InsertEffectArray fx = DefaultInsertEffects();
                InsertBypassArray by = DefaultInsertBypass();
                InsertParamsArray pp = DefaultInsertParams();
                int slots = 0;

                const bool hasEq =
                    s.highpassHz > 5.0f ||
                    std::fabs(s.lowShelfDb) > 0.05f ||
                    std::fabs(s.lowMidDb) > 0.05f ||
                    std::fabs(s.presenceDb) > 0.05f ||
                    std::fabs(s.airDb) > 0.05f;
                if (hasEq) {
                    InsertParams p = DefaultInsertParams()[0];
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
                    InsertParams p = DefaultInsertParams()[0];
                    p.cmp_threshold_db = std::clamp(s.compThresholdDb, -60.0f, 0.0f);
                    p.cmp_ratio = std::clamp(s.compRatio, 1.0f, 20.0f);
                    p.cmp_makeup_db = std::clamp(s.compMakeupDb, 0.0f, 24.0f);
                    p.cmp_attack_ms = 10.0f;
                    p.cmp_release_ms = 120.0f;
                    p.cmp_knee_db = 3.0f;
                    AutoMixAppendInsert(&fx, &by, &pp, &slots, kFxCMP, p);
                }

                if (s.deesserDb < -0.05f) {
                    InsertParams p = DefaultInsertParams()[0];
                    p.dee_threshold_db = std::clamp(-22.0f + s.deesserDb * 2.0f, -40.0f, 0.0f);
                    p.dee_freq_hz = 7000.0f;
                    p.dee_bandwidth_hz = 3500.0f;
                    p.dee_reduction_db = std::clamp(std::fabs(s.deesserDb) * 6.0f, 0.0f, 24.0f);
                    AutoMixAppendInsert(&fx, &by, &pp, &slots, kFxDEE, p);
                }

                if (s.saturationBlend > 0.01f && s.saturationDriveDb > 0.01f) {
                    InsertParams p = DefaultInsertParams()[0];
                    p.sat_drive = std::clamp(s.saturationDriveDb / 8.0f, 0.0f, 1.0f);
                    p.sat_mix = std::clamp(s.saturationBlend, 0.0f, 1.0f);
                    AutoMixAppendInsert(&fx, &by, &pp, &slots, kFxSAT, p);
                }

                if (s.delayMix > 0.01f && s.delayTimeMs > 1.0f) {
                    InsertParams p = DefaultInsertParams()[0];
                    p.dly_time_ms = std::clamp(s.delayTimeMs, 10.0f, 2000.0f);
                    p.dly_feedback = std::clamp(s.delayFeedback, 0.0f, 0.95f);
                    p.dly_mix = std::clamp(s.delayMix, 0.0f, 1.0f);
                    AutoMixAppendInsert(&fx, &by, &pp, &slots, kFxDLY, p);
                }

                if (s.reverbMix > 0.01f) {
                    InsertParams p = DefaultInsertParams()[0];
                    p.rev_mix = std::clamp(s.reverbMix, 0.0f, 1.0f);
                    p.rev_room_size = std::clamp((s.reverbDecayS - 0.2f) / 2.3f, 0.0f, 1.0f);
                    p.rev_damping = std::clamp(0.35f + s.reverbPreDelayMs / 80.0f, 0.0f, 1.0f);
                    AutoMixAppendInsert(&fx, &by, &pp, &slots, kFxREV, p);
                }

                state.trackInsertEffects[static_cast<size_t>(trackIndex)] = fx;
                state.trackInsertBypass[static_cast<size_t>(trackIndex)] = by;
                state.trackInsertParams[static_cast<size_t>(trackIndex)] = pp;
                state.trackInsertSlots[static_cast<size_t>(trackIndex)] = slots;
                if (slots > 0) ++appliedFx;
            }

            ++applied;
        } else {
            auto it = gains.find(key);
            if (it == gains.end()) continue;
            state.trackGainDb[static_cast<size_t>(trackIndex)] = std::clamp(it->second, kFaderMinDb, kFaderMaxDb);
            ++applied;
        }
    }

    if (hasFullSettings && master.hasCompressor &&
        state.busInsertEffects.size() > 3 &&
        state.busInsertBypass.size() > 3 &&
        state.busInsertParams.size() > 3 &&
        state.busInsertSlots.size() > 3) {
        InsertEffectArray fx = DefaultInsertEffects();
        InsertBypassArray by = DefaultInsertBypass();
        InsertParamsArray pp = DefaultInsertParams();
        int slots = 0;

        InsertParams p = DefaultInsertParams()[0];
        p.cmp_threshold_db = std::clamp(master.compThresholdDb, -60.0f, 0.0f);
        p.cmp_ratio = std::clamp(master.compRatio, 1.0f, 20.0f);
        p.cmp_makeup_db = std::clamp(master.compMakeupDb, 0.0f, 24.0f);
        p.cmp_attack_ms = 25.0f;
        p.cmp_release_ms = 150.0f;
        p.cmp_knee_db = 4.0f;
        AutoMixAppendInsert(&fx, &by, &pp, &slots, kFxCMP, p);

        state.busInsertEffects[3] = fx;
        state.busInsertBypass[3] = by;
        state.busInsertParams[3] = pp;
        state.busInsertSlots[3] = slots;
    }

    LeaveCriticalSection(&state.audioStateLock);

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

void StopPlayback(UiState& state, bool rewind) {
    state.audioStopRequested.store(true);

    if (state.audioThread != nullptr) {
        WaitForSingleObject(state.audioThread, INFINITE);
        CloseHandle(state.audioThread);
        state.audioThread = nullptr;
    }

    state.playingViaWasapi = false;

    if (state.waveOut != nullptr) {
        waveOutReset(state.waveOut);
        for (size_t i = 0; i < state.waveHeaders.size(); ++i) {
            waveOutUnprepareHeader(state.waveOut, &state.waveHeaders[i], sizeof(WAVEHDR));
        }
        waveOutClose(state.waveOut);
        state.waveOut = nullptr;
    }

    state.waveHeaders.clear();
    state.waveData.clear();
    state.playing = false;
    state.audioThreadRunning.store(false);
    if (rewind) {
        state.playheadBeat = 0.0f;
        state.playbackFrameCursor.store(0);
    }
}

bool RenderTrackToStereoLocked(const UiState& state, int trackIndex, std::vector<float>* outStereo, int* outSampleRate) {
    if (trackIndex < 0 || trackIndex >= static_cast<int>(state.tracks.size()) || state.projectSampleRate <= 0) {
        return false;
    }

    std::uint64_t startFrame = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t endFrame = 0;
    const float spb = SamplesPerBeat(state);

    for (const ClipItem& clip : state.clips) {
        if (clip.trackIndex != trackIndex || clip.audioIndex < 0 || clip.audioIndex >= static_cast<int>(state.audio.size())) {
            continue;
        }
        const std::uint64_t clipStart = static_cast<std::uint64_t>(std::llround(std::max(0.0f, clip.startBeat) * spb));
        startFrame = std::min(startFrame, clipStart);
        endFrame = std::max(endFrame, clipStart + static_cast<std::uint64_t>(std::llround(clip.lengthBeats * spb)));
    }

    if (endFrame <= startFrame || startFrame == std::numeric_limits<std::uint64_t>::max()) {
        return false;
    }

    const std::uint64_t totalFrames = endFrame - startFrame;
    std::vector<float> stereo(static_cast<size_t>(totalFrames) * 2, 0.0f);

    for (const ClipItem& clip : state.clips) {
        if (clip.trackIndex != trackIndex || clip.audioIndex < 0 || clip.audioIndex >= static_cast<int>(state.audio.size())) {
            continue;
        }
        const LoadedAudio& a = state.audio[static_cast<size_t>(clip.audioIndex)];
        const std::uint64_t clipStartAbs = static_cast<std::uint64_t>(std::llround(std::max(0.0f, clip.startBeat) * spb));
        if (clipStartAbs < startFrame) {
            continue;
        }

        const std::uint64_t writeStart = clipStartAbs - startFrame;
        const std::uint64_t clipFrames = static_cast<std::uint64_t>(std::llround(clip.lengthBeats * spb));
        for (std::uint64_t f = 0; f < clipFrames && (writeStart + f) < totalFrames; ++f) {
            float l = 0.0f;
            float r = 0.0f;
            if (!ReadClipSampleAtProjectFrame(a, f, state.projectSampleRate, clip.sourceOffsetFrames, &l, &r)) {
                continue;
            }
            const size_t dst = static_cast<size_t>(writeStart + f) * 2;
            stereo[dst] += l;
            stereo[dst + 1] += r;
        }
    }

    for (float& s : stereo) {
        s = std::clamp(s, -1.0f, 1.0f);
    }

    *outStereo = std::move(stereo);
    *outSampleRate = state.projectSampleRate;
    return true;
}

bool RenderProjectMixToStereoLocked(const UiState& state, int excludedTrackIndex, std::vector<float>* outStereo, int* outSampleRate) {
    if (state.projectSampleRate <= 0 || state.clips.empty()) {
        return false;
    }

    std::uint64_t startFrame = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t endFrame = 0;
    const float spb = SamplesPerBeat(state);

    for (const ClipItem& clip : state.clips) {
        if (clip.trackIndex == excludedTrackIndex || clip.audioIndex < 0 || clip.audioIndex >= static_cast<int>(state.audio.size())) {
            continue;
        }
        const std::uint64_t clipStart = static_cast<std::uint64_t>(std::llround(std::max(0.0f, clip.startBeat) * spb));
        startFrame = std::min(startFrame, clipStart);
        endFrame = std::max(endFrame, clipStart + static_cast<std::uint64_t>(std::llround(clip.lengthBeats * spb)));
    }

    if (endFrame <= startFrame || startFrame == std::numeric_limits<std::uint64_t>::max()) {
        return false;
    }

    const std::uint64_t totalFrames = endFrame - startFrame;
    std::vector<float> stereo(static_cast<size_t>(totalFrames) * 2, 0.0f);

    for (const ClipItem& clip : state.clips) {
        if (clip.trackIndex == excludedTrackIndex || clip.audioIndex < 0 || clip.audioIndex >= static_cast<int>(state.audio.size())) {
            continue;
        }
        const LoadedAudio& a = state.audio[static_cast<size_t>(clip.audioIndex)];
        const std::uint64_t clipStartAbs = static_cast<std::uint64_t>(std::llround(std::max(0.0f, clip.startBeat) * spb));
        if (clipStartAbs < startFrame) {
            continue;
        }
        const std::uint64_t writeStart = clipStartAbs - startFrame;
        const std::uint64_t clipFrames = static_cast<std::uint64_t>(std::llround(clip.lengthBeats * spb));

        for (std::uint64_t f = 0; f < clipFrames && (writeStart + f) < totalFrames; ++f) {
            float l = 0.0f;
            float r = 0.0f;
            if (!ReadClipSampleAtProjectFrame(a, f, state.projectSampleRate, clip.sourceOffsetFrames, &l, &r)) {
                continue;
            }
            const size_t dst = static_cast<size_t>(writeStart + f) * 2;
            stereo[dst] += l;
            stereo[dst + 1] += r;
        }
    }

    for (float& s : stereo) {
        s = std::clamp(s, -1.0f, 1.0f);
    }

    *outStereo = std::move(stereo);
    *outSampleRate = state.projectSampleRate;
    return true;
}

// Renders the full mix to stereo: all un-muted tracks summed with track gain,
// track pan, bus gain, and bus mute applied. Bus pan is also applied.
bool RenderFullMixToStereoLocked(const UiState& state, std::vector<float>* outStereo, int* outSampleRate) {
    if (state.projectSampleRate <= 0 || state.clips.empty() || state.tracks.empty()) {
        return false;
    }

    // Determine total length across all clips
    std::uint64_t endFrame = 0;
    const float spb = SamplesPerBeat(state);
    for (const ClipItem& clip : state.clips) {
        if (clip.audioIndex < 0 || clip.audioIndex >= static_cast<int>(state.audio.size())) continue;
        const std::uint64_t clipStart = static_cast<std::uint64_t>(std::llround(std::max(0.0f, clip.startBeat) * spb));
        endFrame = std::max(endFrame, clipStart + static_cast<std::uint64_t>(std::llround(clip.lengthBeats * spb)));
    }
    if (endFrame == 0) return false;

    std::vector<float> mix(static_cast<size_t>(endFrame) * 2, 0.0f);

    const int trackCount = static_cast<int>(state.tracks.size());
    for (int ti = 0; ti < trackCount; ++ti) {
        // Track mute check
        const bool trackMuted = (ti < static_cast<int>(state.trackMute.size())) && state.trackMute[static_cast<size_t>(ti)];
        if (trackMuted) continue;

        // Bus mute check
        const int busIdx = (ti < static_cast<int>(state.trackBusIndex.size()))
            ? std::clamp(state.trackBusIndex[static_cast<size_t>(ti)], 0, kBusCount - 1) : 0;
        const bool busMuted = (busIdx < static_cast<int>(state.busMute.size())) && state.busMute[static_cast<size_t>(busIdx)];
        if (busMuted) continue;

        // Gain: track dB + bus dB
        const float trackDb = (ti < static_cast<int>(state.trackGainDb.size()))
            ? state.trackGainDb[static_cast<size_t>(ti)] : 0.0f;
        const float busDb = BusGainDbAt(state, busIdx);
        const float gain = std::pow(10.0f, (trackDb + busDb) / 20.0f);

        // Pan: track pan + bus pan combined (simple additive, clamped)
        const float trackPan = (ti < static_cast<int>(state.trackPan.size()))
            ? state.trackPan[static_cast<size_t>(ti)] : 0.0f;
        const float busPan = BusPanAt(state, busIdx);
        const float pan = std::clamp(trackPan + busPan, -1.0f, 1.0f);
        // Constant-power panning
        const float panRad = (pan + 1.0f) * 0.5f * 3.14159265f * 0.5f;
        const float gainL = gain * std::cos(panRad);
        const float gainR = gain * std::sin(panRad);

        // Build per-track buffer for DSP
        std::vector<float> trackBuf(static_cast<size_t>(endFrame) * 2, 0.0f);
        for (const ClipItem& clip : state.clips) {
            if (clip.trackIndex != ti || clip.audioIndex < 0 || clip.audioIndex >= static_cast<int>(state.audio.size())) continue;
            const LoadedAudio& a = state.audio[static_cast<size_t>(clip.audioIndex)];
            const std::uint64_t clipStart = static_cast<std::uint64_t>(std::llround(std::max(0.0f, clip.startBeat) * spb));
            const std::uint64_t clipFrames = static_cast<std::uint64_t>(std::llround(clip.lengthBeats * spb));
            for (std::uint64_t f = 0; f < clipFrames && (clipStart + f) < endFrame; ++f) {
                float l = 0.0f;
                float r = 0.0f;
                if (!ReadClipSampleAtProjectFrame(a, f, state.projectSampleRate, clip.sourceOffsetFrames, &l, &r)) {
                    continue;
                }
                const size_t dst = static_cast<size_t>(clipStart + f) * 2;
                trackBuf[dst]     += l;
                trackBuf[dst + 1] += r;
            }
        }

        // Apply track insert chain
        if (ti < static_cast<int>(state.trackInsertEffects.size()) &&
            ti < static_cast<int>(state.trackInsertBypass.size()) &&
            ti < static_cast<int>(state.trackInsertParams.size()) &&
            ti < static_cast<int>(state.trackInsertSlots.size())) {
            ApplyInsertChain(trackBuf, static_cast<float>(state.projectSampleRate),
                state.trackInsertEffects[static_cast<size_t>(ti)],
                state.trackInsertBypass[static_cast<size_t>(ti)],
                state.trackInsertParams[static_cast<size_t>(ti)],
                state.trackInsertSlots[static_cast<size_t>(ti)]);
        }

        // Mix into master with gain+pan
        for (std::uint64_t f = 0; f < endFrame; ++f) {
            const size_t i = f * 2;
            mix[i]   += trackBuf[i]   * gainL;
            mix[i+1] += trackBuf[i+1] * gainR;
        }
    }

    // Apply bus insert chains per bus
    for (int b = 0; b < kBusCount; ++b) {
        if (b >= static_cast<int>(state.busInsertEffects.size())) continue;
        if (b >= static_cast<int>(state.busInsertBypass.size()))  continue;
        if (b >= static_cast<int>(state.busInsertParams.size()))  continue;
        if (b >= static_cast<int>(state.busInsertSlots.size()))   continue;
        if (state.busInsertSlots[static_cast<size_t>(b)] <= 0)    continue;
        // Collect all tracks on this bus into a sub-mix
        std::vector<float> busBuf(static_cast<size_t>(endFrame) * 2, 0.0f);
        bool hasContent = false;
        for (int ti2 = 0; ti2 < trackCount; ++ti2) {
            const int tbus = (ti2 < static_cast<int>(state.trackBusIndex.size()))
                ? std::clamp(state.trackBusIndex[static_cast<size_t>(ti2)], 0, kBusCount - 1) : 0;
            if (tbus != b) continue;
            const bool muted = (ti2 < static_cast<int>(state.trackMute.size())) && state.trackMute[static_cast<size_t>(ti2)];
            if (muted) continue;
            hasContent = true;
            for (const ClipItem& clip : state.clips) {
                if (clip.trackIndex != ti2 || clip.audioIndex < 0 || clip.audioIndex >= static_cast<int>(state.audio.size())) continue;
                const LoadedAudio& a2 = state.audio[static_cast<size_t>(clip.audioIndex)];
                const std::uint64_t cs = static_cast<std::uint64_t>(std::llround(std::max(0.0f, clip.startBeat) * spb));
                const std::uint64_t cf = static_cast<std::uint64_t>(std::llround(clip.lengthBeats * spb));
                for (std::uint64_t f = 0; f < cf && (cs + f) < endFrame; ++f) {
                    float l = 0.0f;
                    float r = 0.0f;
                    if (!ReadClipSampleAtProjectFrame(a2, f, state.projectSampleRate, clip.sourceOffsetFrames, &l, &r)) {
                        continue;
                    }
                    const size_t dst = static_cast<size_t>(cs + f) * 2;
                    busBuf[dst]   += l;
                    busBuf[dst+1] += r;
                }
            }
        }
        if (!hasContent) continue;
        // Apply bus inserts — result is a differential that we add back to mix
        // (subtract unprocessed, add processed)
        std::vector<float> busBufPre = busBuf;
        ApplyInsertChain(busBuf, static_cast<float>(state.projectSampleRate),
            state.busInsertEffects[static_cast<size_t>(b)],
            state.busInsertBypass[static_cast<size_t>(b)],
            state.busInsertParams[static_cast<size_t>(b)],
            state.busInsertSlots[static_cast<size_t>(b)]);
        const float busGainLin = std::pow(10.0f, BusGainDbAt(state, b) / 20.0f);
        for (std::uint64_t f = 0; f < endFrame; ++f) {
            const size_t i = f*2;
            mix[i]   += (busBuf[i]   - busBufPre[i])   * busGainLin;
            mix[i+1] += (busBuf[i+1] - busBufPre[i+1]) * busGainLin;
        }
    }

    // Soft clip / clamp
    for (float& s : mix) {
        s = std::clamp(s, -1.0f, 1.0f);
    }

    *outStereo     = std::move(mix);
    *outSampleRate = state.projectSampleRate;
    return true;
}

// Renders all tracks routed to busIndex (with track+bus gain/pan applied) into a stereo buffer.
// Must NOT be called with audioStateLock held.
bool RenderBusStemToStereoLocked(const UiState& state, int busIndex, std::vector<float>* outStereo, int* outSampleRate) {
    if (state.projectSampleRate <= 0 || state.clips.empty() || state.tracks.empty()) return false;

    std::uint64_t endFrame = 0;
    const float spb = SamplesPerBeat(state);
    for (const ClipItem& clip : state.clips) {
        if (clip.audioIndex < 0 || clip.audioIndex >= static_cast<int>(state.audio.size())) continue;
        const std::uint64_t clipStart = static_cast<std::uint64_t>(std::llround(std::max(0.0f, clip.startBeat) * spb));
        endFrame = std::max(endFrame, clipStart + static_cast<std::uint64_t>(std::llround(clip.lengthBeats * spb)));
    }
    if (endFrame == 0) return false;

    std::vector<float> mix(static_cast<size_t>(endFrame) * 2, 0.0f);
    const int trackCount = static_cast<int>(state.tracks.size());

    for (int ti = 0; ti < trackCount; ++ti) {
        const int tbus = (ti < static_cast<int>(state.trackBusIndex.size()))
            ? std::clamp(state.trackBusIndex[static_cast<size_t>(ti)], 0, kBusCount - 1) : 0;
        if (tbus != busIndex) continue;

        const bool trackMuted = (ti < static_cast<int>(state.trackMute.size())) && state.trackMute[static_cast<size_t>(ti)];
        const bool busMuted   = (busIndex < static_cast<int>(state.busMute.size())) && state.busMute[static_cast<size_t>(busIndex)];
        if (trackMuted || busMuted) continue;

        const float trackDb = (ti < static_cast<int>(state.trackGainDb.size())) ? state.trackGainDb[static_cast<size_t>(ti)] : 0.0f;
        const float busDb   = BusGainDbAt(state, busIndex);
        const float gain    = std::pow(10.0f, (trackDb + busDb) / 20.0f);

        const float trackPan = (ti < static_cast<int>(state.trackPan.size())) ? state.trackPan[static_cast<size_t>(ti)] : 0.0f;
        const float busPan   = BusPanAt(state, busIndex);
        const float pan      = std::clamp(trackPan + busPan, -1.0f, 1.0f);
        const float panRad   = (pan + 1.0f) * 0.5f * 3.14159265f * 0.5f;
        const float gainL    = gain * std::cos(panRad);
        const float gainR    = gain * std::sin(panRad);

        // Build per-track buffer for DSP
        std::vector<float> trackBuf(static_cast<size_t>(endFrame) * 2, 0.0f);
        for (const ClipItem& clip : state.clips) {
            if (clip.trackIndex != ti || clip.audioIndex < 0 || clip.audioIndex >= static_cast<int>(state.audio.size())) continue;
            const LoadedAudio& a = state.audio[static_cast<size_t>(clip.audioIndex)];
            const std::uint64_t clipStart = static_cast<std::uint64_t>(std::llround(std::max(0.0f, clip.startBeat) * spb));
            const std::uint64_t clipFrames = static_cast<std::uint64_t>(std::llround(clip.lengthBeats * spb));
            for (std::uint64_t f = 0; f < clipFrames && (clipStart + f) < endFrame; ++f) {
                float l = 0.0f;
                float r = 0.0f;
                if (!ReadClipSampleAtProjectFrame(a, f, state.projectSampleRate, clip.sourceOffsetFrames, &l, &r)) {
                    continue;
                }
                const size_t dst = static_cast<size_t>(clipStart + f) * 2;
                trackBuf[dst]   += l;
                trackBuf[dst+1] += r;
            }
        }

        // Apply track insert chain
        if (ti < static_cast<int>(state.trackInsertEffects.size()) &&
            ti < static_cast<int>(state.trackInsertBypass.size()) &&
            ti < static_cast<int>(state.trackInsertParams.size()) &&
            ti < static_cast<int>(state.trackInsertSlots.size())) {
            ApplyInsertChain(trackBuf, static_cast<float>(state.projectSampleRate),
                state.trackInsertEffects[static_cast<size_t>(ti)],
                state.trackInsertBypass[static_cast<size_t>(ti)],
                state.trackInsertParams[static_cast<size_t>(ti)],
                state.trackInsertSlots[static_cast<size_t>(ti)]);
        }

        // Apply bus insert chain as post-fader processing
        if (busIndex < static_cast<int>(state.busInsertEffects.size()) &&
            busIndex < static_cast<int>(state.busInsertBypass.size()) &&
            busIndex < static_cast<int>(state.busInsertParams.size()) &&
            busIndex < static_cast<int>(state.busInsertSlots.size()) &&
            state.busInsertSlots[static_cast<size_t>(busIndex)] > 0) {
            ApplyInsertChain(trackBuf, static_cast<float>(state.projectSampleRate),
                state.busInsertEffects[static_cast<size_t>(busIndex)],
                state.busInsertBypass[static_cast<size_t>(busIndex)],
                state.busInsertParams[static_cast<size_t>(busIndex)],
                state.busInsertSlots[static_cast<size_t>(busIndex)]);
        }

        for (std::uint64_t f = 0; f < endFrame; ++f) {
            const size_t i = f*2;
            mix[i]   += trackBuf[i]   * gainL;
            mix[i+1] += trackBuf[i+1] * gainR;
        }
    }
    for (float& s : mix) s = std::clamp(s, -1.0f, 1.0f);
    *outStereo     = std::move(mix);
    *outSampleRate = state.projectSampleRate;
    return true;
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
    state.tracks.clear();
    state.trackGainDb.clear();
    state.trackMute.clear();
    state.trackSolo.clear();
    state.trackRecordArm.clear();
    state.trackBusIndex.clear();
    state.trackPan.clear();
    state.trackInsertSlots.clear();
    state.trackInsertEffects.clear();
    state.trackInsertBypass.clear();
    state.trackInsertParams.clear();
    state.audio.clear();
    state.clips.clear();

    state.busGainDb = {0.0f, 0.0f, 0.0f, 0.0f};
    state.busMute = {false, false, false, false};
    state.busPan = {0.0f, 0.0f, 0.0f, 0.0f};
    state.busInsertSlots = {0, 0, 0, 0};
    state.busInsertEffects.assign(kBusCount, DefaultInsertEffects());
    state.busInsertBypass.assign(kBusCount, DefaultInsertBypass());
    state.busInsertParams.assign(kBusCount, DefaultInsertParams());

    state.projectSampleRate = audio.sampleRate;

    state.tracks.push_back(audio.displayName);
    state.trackGainDb.push_back(0.0f);
    state.trackMute.push_back(false);
    state.trackSolo.push_back(false);
    state.trackRecordArm.push_back(false);
    state.trackBusIndex.push_back(3); // mastered stereo should route directly to Master
    state.trackPan.push_back(0.0f);
    state.trackInsertSlots.push_back(0);
    state.trackInsertEffects.push_back(DefaultInsertEffects());
    state.trackInsertBypass.push_back(DefaultInsertBypass());
    state.trackInsertParams.push_back(DefaultInsertParams());
    state.audio.push_back(std::move(audio));

    const float lengthBeats = static_cast<float>(state.audio.back().frames) / SamplesPerBeat(state);
    state.clips.push_back(ClipItem{
        0,
        0,
        0.0f,
        std::max(0.25f, lengthBeats),
        kPalette.clip1,
        state.tracks.back(),
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
        if (!state.clips.empty() && state.selectedClipIndex >= 0 && state.selectedClipIndex < static_cast<int>(state.clips.size())) {
            const ClipItem& c = state.clips[static_cast<size_t>(state.selectedClipIndex)];
            if (c.audioIndex >= 0 && c.audioIndex < static_cast<int>(state.audio.size())) {
                sourceWav = state.audio[static_cast<size_t>(c.audioIndex)].sourcePath;
            }
        }
        if (sourceWav.empty() && state.audio.size() == 1) {
            sourceWav = state.audio[0].sourcePath;
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
    if (state.tracks.empty() || state.clips.empty()) {
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
    if (state.tracks.empty() || state.clips.empty()) {
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

std::wstring ReadSmallUtf8Text(const std::filesystem::path& p) {
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

bool AnalyzeSelectedTrackQuality(HWND hwnd, UiState& state) {
    const int trackIndex = state.selectedTrackIndex;
    if (trackIndex < 0 || trackIndex >= static_cast<int>(state.tracks.size())) {
        MessageBoxW(hwnd, L"Select a track first, then run Vocal Check.", L"Vocal Check", MB_OK | MB_ICONINFORMATION);
        return false;
    }

    std::vector<float> stereo;
    std::vector<float> referenceStereo;
    int sampleRate = 0;
    int referenceSampleRate = 0;
    EnterCriticalSection(&state.audioStateLock);
    const bool hasAudio = RenderTrackToStereoLocked(state, trackIndex, &stereo, &sampleRate);
    const bool hasReference = RenderProjectMixToStereoLocked(state, trackIndex, &referenceStereo, &referenceSampleRate);
    const std::wstring trackName = state.tracks[static_cast<size_t>(trackIndex)];
    LeaveCriticalSection(&state.audioStateLock);

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

    if (!WriteWavPcm16Stereo(wavPath.wstring(), stereo, sampleRate)) {
        MessageBoxW(hwnd, L"Failed to write temporary audio for analysis.", L"Vocal Check", MB_OK | MB_ICONERROR);
        return false;
    }

    bool wroteReference = false;
    if (hasReference && !referenceStereo.empty() && referenceSampleRate == sampleRate) {
        wroteReference = WriteWavPcm16Stereo(referencePath.wstring(), referenceStereo, referenceSampleRate);
    }

    std::wstring cmd =
        QuoteArg(pythonExe.wstring()) +
        L" -m daw_ai.vocal_check --input " + QuoteArg(wavPath.wstring()) +
        L" --output " + QuoteArg(txtPath.wstring()) +
        L" --bpm " + std::to_wstring(state.bpm) +
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

        if (state.projectSampleRate == 0) {
            state.projectSampleRate = audio.sampleRate;
        } else if (audio.sampleRate != state.projectSampleRate) {
            skipped += std::filesystem::path(path).filename().wstring() + L": sample rate mismatch\n";
            continue;
        }

        const int trackIndex = static_cast<int>(state.tracks.size());
        const int audioIndex = static_cast<int>(state.audio.size());

        EnterCriticalSection(&state.audioStateLock);
        state.tracks.push_back(audio.displayName);
        state.trackGainDb.push_back(0.0f);
        state.trackMute.push_back(false);
        state.trackSolo.push_back(false);
        state.trackRecordArm.push_back(false);
        state.trackBusIndex.push_back(1);
        state.trackPan.push_back(0.0f);
        state.trackInsertSlots.push_back(0);
        state.trackInsertEffects.push_back(DefaultInsertEffects());
        state.trackInsertBypass.push_back(DefaultInsertBypass());
        state.trackInsertParams.push_back(DefaultInsertParams());
        state.audio.push_back(std::move(audio));

        const float lengthBeats = static_cast<float>(state.audio.back().frames) / SamplesPerBeat(state);
        state.clips.push_back(ClipItem{
            trackIndex,
            audioIndex,
            0.0f,
            std::max(0.25f, lengthBeats),
            clipColors[trackIndex % 4],
            state.tracks.back(),
        });
        LeaveCriticalSection(&state.audioStateLock);
    }

    if (!state.clips.empty()) {
        float endBeat = 0.0f;
        EnterCriticalSection(&state.audioStateLock);
        for (const ClipItem& clip : state.clips) {
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

DWORD WINAPI AudioThreadProc(LPVOID param) {
    auto* state = reinterpret_cast<UiState*>(param);
    if (state == nullptr) {
        return 0;
    }

    bool draining = false;
    while (!state->audioStopRequested.load()) {
        bool anyInQueue = false;

        for (int i = 0; i < static_cast<int>(state->waveHeaders.size()); ++i) {
            WAVEHDR& hdr = state->waveHeaders[static_cast<size_t>(i)];
            if ((hdr.dwFlags & WHDR_INQUEUE) != 0) {
                anyInQueue = true;
                continue;
            }

            if (draining) {
                continue;
            }

            bool reachedEnd = false;
            EnterCriticalSection(&state->audioStateLock);
            FillRealtimeForDeviceLocked(
                *state,
                state->waveData[static_cast<size_t>(i)].data(),
                kAudioBufferFrames,
                static_cast<int>(state->waveFormat.nSamplesPerSec),
                &reachedEnd);
            LeaveCriticalSection(&state->audioStateLock);

            hdr.lpData = reinterpret_cast<LPSTR>(state->waveData[static_cast<size_t>(i)].data());
            hdr.dwBufferLength = static_cast<DWORD>(state->waveData[static_cast<size_t>(i)].size() * sizeof(std::int16_t));
            hdr.dwFlags &= ~WHDR_DONE;

            if (waveOutWrite(state->waveOut, &hdr, sizeof(WAVEHDR)) == MMSYSERR_NOERROR) {
                anyInQueue = true;
            }

            if (reachedEnd) {
                draining = true;
            }
        }

        if (draining && !anyInQueue) {
            PostMessage(state->hwnd, kMsgPlaybackFinished, 0, 0);
            break;
        }

        Sleep(4);
    }

    state->audioThreadRunning.store(false);
    return 0;
}

bool StartPlayback(HWND hwnd, UiState& state);
std::uint64_t GetRenderedPlaybackFrame(const UiState& state);

static IMMDevice* FindWasapiCaptureEndpoint(const std::wstring& preferredName) {
    IMMDeviceEnumerator* enumerator = nullptr;
    if (CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator)) != S_OK || enumerator == nullptr) {
        return nullptr;
    }

    auto toLower = [](std::wstring s) {
        std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(::towlower(c)); });
        return s;
    };

    IMMDevice* chosen = nullptr;
    if (!preferredName.empty()) {
        IMMDeviceCollection* coll = nullptr;
        if (enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &coll) == S_OK && coll != nullptr) {
            UINT count = 0;
            coll->GetCount(&count);
            const std::wstring target = toLower(preferredName);
            for (UINT i = 0; i < count; ++i) {
                IMMDevice* dev = nullptr;
                if (coll->Item(i, &dev) != S_OK || dev == nullptr) {
                    continue;
                }
                IPropertyStore* props = nullptr;
                if (dev->OpenPropertyStore(STGM_READ, &props) == S_OK && props != nullptr) {
                    PROPVARIANT pv;
                    PropVariantInit(&pv);
                    if (props->GetValue(PKEY_Device_FriendlyName, &pv) == S_OK && pv.vt == VT_LPWSTR && pv.pwszVal != nullptr) {
                        const std::wstring friendly = toLower(pv.pwszVal);
                        if (friendly.find(target) != std::wstring::npos) {
                            chosen = dev;
                            PropVariantClear(&pv);
                            props->Release();
                            break;
                        }
                    }
                    PropVariantClear(&pv);
                    props->Release();
                }
                dev->Release();
            }
            coll->Release();
        }
    }

    if (chosen == nullptr) {
        enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &chosen);
    }

    enumerator->Release();
    return chosen;
}

static IMMDevice* FindWasapiOutputEndpoint(const std::wstring& preferredName) {
    IMMDeviceEnumerator* enumerator = nullptr;
    if (CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator)) != S_OK || enumerator == nullptr) {
        return nullptr;
    }

    auto toLower = [](std::wstring s) {
        std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(::towlower(c)); });
        return s;
    };

    IMMDevice* chosen = nullptr;
    if (!preferredName.empty()) {
        IMMDeviceCollection* coll = nullptr;
        if (enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &coll) == S_OK && coll != nullptr) {
            UINT count = 0;
            coll->GetCount(&count);
            const std::wstring target = toLower(preferredName);
            for (UINT i = 0; i < count; ++i) {
                IMMDevice* dev = nullptr;
                if (coll->Item(i, &dev) != S_OK || dev == nullptr) {
                    continue;
                }
                IPropertyStore* props = nullptr;
                if (dev->OpenPropertyStore(STGM_READ, &props) == S_OK && props != nullptr) {
                    PROPVARIANT pv;
                    PropVariantInit(&pv);
                    if (props->GetValue(PKEY_Device_FriendlyName, &pv) == S_OK && pv.vt == VT_LPWSTR && pv.pwszVal != nullptr) {
                        const std::wstring friendly = toLower(pv.pwszVal);
                        if (friendly.find(target) != std::wstring::npos) {
                            chosen = dev;
                            PropVariantClear(&pv);
                            props->Release();
                            break;
                        }
                    }
                    PropVariantClear(&pv);
                    props->Release();
                }
                if (chosen == nullptr) dev->Release();
            }
            coll->Release();
        }
    }

    if (chosen == nullptr) {
        enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &chosen);
    }

    enumerator->Release();
    return chosen;
}

// WASAPI render thread: creates its own COM objects, drives IAudioRenderClient.
// Converts our int16 mix output to the device's native mix format (float32 or int16).
DWORD WINAPI WasapiRenderThreadProc(LPVOID param) {
    auto* state = reinterpret_cast<UiState*>(param);
    if (state == nullptr) return 0;

    state->lastPlaybackInitError.clear();

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool coInitOk = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
    if (!coInitOk) {
        state->lastPlaybackInitError = L"WASAPI COM init failed";
        state->wasapiOutInitState.store(-1);
        state->audioThreadRunning.store(false);
        return 0;
    }

    IMMDevice*          device       = FindWasapiOutputEndpoint(state->selectedOutputDeviceName);
    IAudioClient*       audioClient  = nullptr;
    IAudioRenderClient* renderClient = nullptr;
    WAVEFORMATEX*       mixFmt       = nullptr;
    WAVEFORMATEX*       exclusiveFmt = nullptr;
    bool                fellBackToShared = false;

    auto fail = [&](const wchar_t* msg) {
        if (renderClient) { renderClient->Release(); renderClient = nullptr; }
        if (audioClient)  { audioClient->Release();  audioClient  = nullptr; }
        if (exclusiveFmt) { CoTaskMemFree(exclusiveFmt); exclusiveFmt = nullptr; }
        if (mixFmt)       { CoTaskMemFree(mixFmt);   mixFmt = nullptr; }
        if (device)       { device->Release();       device = nullptr; }
        if (coInitOk) CoUninitialize();
        state->lastPlaybackInitError = (msg != nullptr) ? msg : L"WASAPI initialization failed";
        state->wasapiOutInitState.store(-1);
        state->audioThreadRunning.store(false);
    };

    do {
        if (device == nullptr) { fail(L"no output endpoint"); return 0; }

        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&audioClient));
        if (FAILED(hr) || audioClient == nullptr) { fail(L"Activate"); return 0; }

        hr = audioClient->GetMixFormat(&mixFmt);
        if (FAILED(hr) || mixFmt == nullptr) { fail(L"GetMixFormat"); return 0; }

        AUDCLNT_SHAREMODE shareMode = AUDCLNT_SHAREMODE_SHARED;
        WAVEFORMATEX* openFmt = mixFmt;
        REFERENCE_TIME hnsBuffer = 0;

        if (state->audioBackend == AudioBackend::WasapiExclusive) {
            const int requestedSR = (state->preferredSampleRate > 0)
                ? state->preferredSampleRate
                : state->projectSampleRate;

            if (requestedSR > 0) {
                const size_t fmtBytes = sizeof(WAVEFORMATEX) + static_cast<size_t>(mixFmt->cbSize);
                exclusiveFmt = reinterpret_cast<WAVEFORMATEX*>(CoTaskMemAlloc(fmtBytes));
                if (exclusiveFmt != nullptr) {
                    std::memcpy(exclusiveFmt, mixFmt, fmtBytes);
                    exclusiveFmt->nSamplesPerSec = static_cast<DWORD>(requestedSR);
                    exclusiveFmt->nAvgBytesPerSec = exclusiveFmt->nSamplesPerSec * exclusiveFmt->nBlockAlign;
                    WAVEFORMATEX* closest = nullptr;
                    hr = audioClient->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, exclusiveFmt, &closest);
                    if (closest != nullptr) {
                        CoTaskMemFree(closest);
                        closest = nullptr;
                    }
                    if (SUCCEEDED(hr)) {
                        shareMode = AUDCLNT_SHAREMODE_EXCLUSIVE;
                        openFmt = exclusiveFmt;
                    } else {
                        fellBackToShared = true;
                    }
                } else {
                    fellBackToShared = true;
                }
            } else {
                fellBackToShared = true;
            }

            if (fellBackToShared) {
                state->lastPlaybackInitError = L"WASAPI exclusive failed; falling back to WASAPI shared mode.";
            }
        }

        if (state->preferredBufferFrames > 0 && openFmt->nSamplesPerSec > 0) {
            hnsBuffer = static_cast<REFERENCE_TIME>((10000000LL * static_cast<long long>(state->preferredBufferFrames)) / static_cast<long long>(openFmt->nSamplesPerSec));
            hnsBuffer = std::max<REFERENCE_TIME>(hnsBuffer, 10000);
        }

        hr = audioClient->Initialize(shareMode, 0, hnsBuffer, (shareMode == AUDCLNT_SHAREMODE_EXCLUSIVE) ? hnsBuffer : 0, openFmt, nullptr);
        if (FAILED(hr)) {
            if (shareMode == AUDCLNT_SHAREMODE_EXCLUSIVE) {
                fellBackToShared = true;
                state->lastPlaybackInitError = L"WASAPI exclusive initialization failed; using WASAPI shared mode.";
                hr = audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, hnsBuffer, 0, mixFmt, nullptr);
            }
            if (FAILED(hr)) {
                fail(L"Initialize");
                return 0;
            }
        }

        hr = audioClient->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&renderClient));
        if (FAILED(hr) || renderClient == nullptr) { fail(L"GetService"); return 0; }
    } while (false);

    // Store format metadata for diagnostics (copy to flat WAVEFORMATEX)
    {
        WAVEFORMATEX fmtCopy = *mixFmt;
        EnterCriticalSection(&state->audioStateLock);
        state->wasapiOutFormat              = fmtCopy;
        state->lastOpenedOutputSampleRate   = static_cast<int>(fmtCopy.nSamplesPerSec);
        state->lastOpenedOutputChannels     = static_cast<int>(fmtCopy.nChannels);
        state->activeDeviceSampleRate       = static_cast<int>(fmtCopy.nSamplesPerSec);
        LeaveCriticalSection(&state->audioStateLock);
    }

    const UINT32 nChannels   = mixFmt->nChannels;
    const bool   isFloat     = (mixFmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) ||
                               (mixFmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                                reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mixFmt)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
    const bool   isPcm16     = (mixFmt->wBitsPerSample == 16) &&
                               ((mixFmt->wFormatTag == WAVE_FORMAT_PCM) ||
                                (mixFmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                                 reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mixFmt)->SubFormat == KSDATAFORMAT_SUBTYPE_PCM));

    if (exclusiveFmt != nullptr) {
        CoTaskMemFree(exclusiveFmt);
        exclusiveFmt = nullptr;
    }
    CoTaskMemFree(mixFmt); mixFmt = nullptr;
    device->Release();     device = nullptr;

    UINT32 bufferFrameCount = 0;
    audioClient->GetBufferSize(&bufferFrameCount);
    if (bufferFrameCount == 0) { fail(L"GetBufferSize=0"); return 0; }

    state->activeDeviceBufferFrames = static_cast<int>(bufferFrameCount);

    if (FAILED(audioClient->Start())) { fail(L"Start"); return 0; }

    if (!fellBackToShared) {
        state->lastPlaybackInitError.clear();
    }

    state->wasapiOutInitState.store(1);  // signal ready

    std::vector<std::int16_t> pcmBuf;
    bool draining = false;

    while (!state->audioStopRequested.load()) {
        UINT32 padding = 0;
        if (FAILED(audioClient->GetCurrentPadding(&padding))) break;

        UINT32 available = bufferFrameCount - padding;
        if (available == 0) { Sleep(2); continue; }
        if (available > static_cast<UINT32>(kAudioBufferFrames * 4)) {
            available = static_cast<UINT32>(kAudioBufferFrames * 4);
        }

        if (draining) {
            Sleep(2);
            if (padding == 0) break;
            continue;
        }

        pcmBuf.assign(static_cast<size_t>(available) * 2, 0);
        bool reachedEnd = false;
        EnterCriticalSection(&state->audioStateLock);
        FillRealtimeForDeviceLocked(*state, pcmBuf.data(), static_cast<int>(available), static_cast<int>(state->wasapiOutFormat.nSamplesPerSec), &reachedEnd);
        LeaveCriticalSection(&state->audioStateLock);

        BYTE* pData = nullptr;
        if (FAILED(renderClient->GetBuffer(available, &pData)) || pData == nullptr) break;

        if (isFloat) {
            auto* fOut = reinterpret_cast<float*>(pData);
            for (UINT32 i = 0; i < available; ++i) {
                fOut[i * nChannels]     = static_cast<float>(pcmBuf[i * 2])     / 32767.0f;
                fOut[i * nChannels + 1] = static_cast<float>(pcmBuf[i * 2 + 1]) / 32767.0f;
                for (UINT32 ch = 2; ch < nChannels; ++ch) fOut[i * nChannels + ch] = 0.0f;
            }
        } else if (isPcm16) {
            auto* sOut = reinterpret_cast<std::int16_t*>(pData);
            for (UINT32 i = 0; i < available; ++i) {
                sOut[i * nChannels]     = pcmBuf[i * 2];
                sOut[i * nChannels + 1] = pcmBuf[i * 2 + 1];
                for (UINT32 ch = 2; ch < nChannels; ++ch) sOut[i * nChannels + ch] = 0;
            }
        } else {
            std::memset(pData, 0, static_cast<size_t>(available) * state->wasapiOutFormat.nBlockAlign);
        }

        renderClient->ReleaseBuffer(available, 0);
        if (reachedEnd) draining = true;
    }

    audioClient->Stop();
    renderClient->Release();
    audioClient->Release();

    if (coInitOk) CoUninitialize();
    state->audioThreadRunning.store(false);

    if (!state->audioStopRequested.load()) {
        PostMessage(state->hwnd, kMsgPlaybackFinished, 0, 0);
    }
    return 0;
}

DWORD WINAPI WasapiRecordThreadProc(LPVOID param) {
    auto* state = reinterpret_cast<UiState*>(param);
    if (state == nullptr) {
        return 0;
    }

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool coInitOk = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
    if (!coInitOk) {
        state->lastRecordInitError = L"WASAPI COM init failed";
        state->recordInitState.store(-1);
        return 0;
    }

    IMMDevice* device = FindWasapiCaptureEndpoint(state->selectedInputDeviceName);
    IAudioClient* audioClient = nullptr;
    IAudioCaptureClient* captureClient = nullptr;
    WAVEFORMATEX* mixFmt = nullptr;

    do {
        if (device == nullptr) {
            state->lastRecordInitError = L"No WASAPI capture endpoint found";
            break;
        }
        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&audioClient));
        if (FAILED(hr) || audioClient == nullptr) {
            state->lastRecordInitError = L"WASAPI Activate(IAudioClient) failed";
            break;
        }

        hr = audioClient->GetMixFormat(&mixFmt);
        if (FAILED(hr) || mixFmt == nullptr) {
            state->lastRecordInitError = L"WASAPI GetMixFormat failed";
            break;
        }

        hr = audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 0, 0, mixFmt, nullptr);
        if (FAILED(hr)) {
            state->lastRecordInitError = L"WASAPI Initialize(shared) failed";
            break;
        }

        hr = audioClient->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&captureClient));
        if (FAILED(hr) || captureClient == nullptr) {
            state->lastRecordInitError = L"WASAPI GetService(IAudioCaptureClient) failed";
            break;
        }

        const int mixCh = std::max<int>(1, static_cast<int>(mixFmt->nChannels));
        const int outCh = (mixCh >= 2) ? 2 : 1;
        const int sr = static_cast<int>(mixFmt->nSamplesPerSec);

        state->waveInFormat = *mixFmt;
        state->recordInputChannels = outCh;
        state->lastOpenedInputSampleRate = sr;
        state->lastOpenedInputChannels = outCh;
        if (state->projectSampleRate <= 0 && sr > 0) {
            state->projectSampleRate = sr;
        }

        if (FAILED(audioClient->Start())) {
            state->lastRecordInitError = L"WASAPI capture start failed";
            break;
        }

        state->recordInitState.store(1);

        while (!state->recordStopRequested.load()) {
            UINT32 packetFrames = 0;
            if (FAILED(captureClient->GetNextPacketSize(&packetFrames))) {
                break;
            }

            if (packetFrames == 0) {
                Sleep(2);
                continue;
            }

            while (packetFrames > 0) {
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                if (FAILED(captureClient->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) {
                    packetFrames = 0;
                    break;
                }

                const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
                if (!silent && data != nullptr && frames > 0) {
                    const bool isFloat = (mixFmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) ||
                        (mixFmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE && reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mixFmt)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
                    const bool isPcm16 = (mixFmt->wBitsPerSample == 16) &&
                        ((mixFmt->wFormatTag == WAVE_FORMAT_PCM) ||
                         (mixFmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE && reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mixFmt)->SubFormat == KSDATAFORMAT_SUBTYPE_PCM));

                    if (isFloat) {
                        const float* f = reinterpret_cast<const float*>(data);
                        for (UINT32 i = 0; i < frames; ++i) {
                            float l = f[static_cast<size_t>(i) * mixCh];
                            float r = (mixCh > 1) ? f[static_cast<size_t>(i) * mixCh + 1] : l;
                            const std::int16_t li = static_cast<std::int16_t>(std::lrint(std::clamp(l, -1.0f, 1.0f) * 32767.0f));
                            const std::int16_t ri = static_cast<std::int16_t>(std::lrint(std::clamp(r, -1.0f, 1.0f) * 32767.0f));
                            if (outCh == 1) {
                                state->recordedInputPcm.push_back(li);
                            } else {
                                state->recordedInputPcm.push_back(li);
                                state->recordedInputPcm.push_back(ri);
                            }
                        }
                    } else if (isPcm16) {
                        const std::int16_t* s = reinterpret_cast<const std::int16_t*>(data);
                        for (UINT32 i = 0; i < frames; ++i) {
                            const std::int16_t li = s[static_cast<size_t>(i) * mixCh];
                            const std::int16_t ri = (mixCh > 1) ? s[static_cast<size_t>(i) * mixCh + 1] : li;
                            if (outCh == 1) {
                                state->recordedInputPcm.push_back(li);
                            } else {
                                state->recordedInputPcm.push_back(li);
                                state->recordedInputPcm.push_back(ri);
                            }
                        }
                    }

                    if (state->inputMonitoring) {
                        EnterCriticalSection(&state->audioStateLock);
                        const size_t n = state->recordedInputPcm.size();
                        const size_t appendCount = static_cast<size_t>(frames) * static_cast<size_t>(outCh);
                        if (appendCount <= n) {
                            state->monitorInputPcm.insert(state->monitorInputPcm.end(),
                                state->recordedInputPcm.end() - static_cast<std::vector<std::int16_t>::difference_type>(appendCount),
                                state->recordedInputPcm.end());
                        }
                        LeaveCriticalSection(&state->audioStateLock);
                    }
                }

                captureClient->ReleaseBuffer(frames);
                if (FAILED(captureClient->GetNextPacketSize(&packetFrames))) {
                    packetFrames = 0;
                }
            }
        }

        audioClient->Stop();
    } while (false);

    if (state->recordInitState.load() == 0) {
        state->recordInitState.store(-1);
    }

    if (mixFmt != nullptr) CoTaskMemFree(mixFmt);
    if (captureClient != nullptr) captureClient->Release();
    if (audioClient != nullptr) audioClient->Release();
    if (device != nullptr) device->Release();
    if (coInitOk) CoUninitialize();
    return 0;
}

static bool TryStartWasapiRecording(HWND hwnd, UiState& state, int armedTrack, bool wasPlaying) {
    state.recordedInputPcm.clear();
    state.monitorInputPcm.clear();
    state.monitorInputReadPos = 0;
    state.recordTrackIndex = armedTrack;
    state.recordCaptureStartTickMs = GetTickCount64();
    const float samplesPerBeat = std::max(1.0f, SamplesPerBeat(state));
    const std::uint64_t timelineStartFrame = state.playing
        ? GetRenderedPlaybackFrame(state)
        : static_cast<std::uint64_t>(std::llround(static_cast<double>(std::max(0.0f, state.playheadBeat)) * static_cast<double>(samplesPerBeat)));

    // Compute preroll duration upfront so count-in click can play immediately.
    state.recordPrerollFrames = 0;
    if (!wasPlaying && state.countInEnabled) {
        state.recordPrerollFrames = static_cast<std::uint64_t>(std::llround(4.0 * static_cast<double>(state.countInBars) * static_cast<double>(samplesPerBeat)));
    }
    // Tentative placement: preroll end is deterministic regardless of init latency.
    state.recordStartFrame = timelineStartFrame + state.recordPrerollFrames;
    state.countingIn = (state.recordPrerollFrames > 0);

    state.recordUsingWasapi = true;
    state.recordStopRequested.store(false);
    state.recordInitState.store(0);
    state.lastRecordInitError.clear();
    state.recordThread = CreateThread(nullptr, 0, WasapiRecordThreadProc, &state, 0, nullptr);
    if (state.recordThread == nullptr) {
        state.countingIn = false;
        state.recordUsingWasapi = false;
        return false;
    }

    for (int i = 0; i < 60 && state.recordInitState.load() == 0; ++i) {
        Sleep(10);
    }

    if (state.recordInitState.load() != 1) {
        state.recordStopRequested.store(true);
        WaitForSingleObject(state.recordThread, INFINITE);
        CloseHandle(state.recordThread);
        state.recordThread = nullptr;
        state.countingIn = false;
        state.recordUsingWasapi = false;
        if (!state.lastRecordInitError.empty()) {
            MessageBoxW(hwnd, (L"WASAPI capture failed: " + state.lastRecordInitError + L"\nFalling back to MME.").c_str(), L"Record", MB_OK | MB_ICONWARNING);
        }
        return false;
    }

    // Capture is now running. Refine skip based on actual capture-start position so
    // clip placement is deterministic (same beat) across all takes.
    {
        const std::uint64_t captureNow = state.playing ? GetRenderedPlaybackFrame(state) : 0;
        const std::uint64_t scheduledStart = state.recordStartFrame;
        const std::uint64_t actualSkip = (captureNow < scheduledStart) ? (scheduledStart - captureNow) : 0;
        state.recordPrerollFrames = actualSkip;          // strip only remaining preroll
        state.recordStartFrame    = captureNow + actualSkip; // = max(captureNow, scheduledStart)
    }

    state.recording = true;
    return true;
}

DWORD WINAPI RecordThreadProc(LPVOID param) {
    auto* state = reinterpret_cast<UiState*>(param);
    if (state == nullptr) {
        return 0;
    }

    while (!state->recordStopRequested.load()) {
        for (size_t i = 0; i < state->waveInHeaders.size(); ++i) {
            WAVEHDR& hdr = state->waveInHeaders[i];
            if ((hdr.dwFlags & WHDR_DONE) == 0) {
                continue;
            }

            if (hdr.dwBytesRecorded > 0) {
                const size_t sampleCount = static_cast<size_t>(hdr.dwBytesRecorded / sizeof(std::int16_t));
                const std::int16_t* samples = reinterpret_cast<const std::int16_t*>(hdr.lpData);
                state->recordedInputPcm.insert(state->recordedInputPcm.end(), samples, samples + sampleCount);
                // Requeue first so capture does not stall while doing monitor bookkeeping.
                hdr.dwBytesRecorded = 0;
                hdr.dwFlags &= ~WHDR_DONE;
                if (!state->recordStopRequested.load() && state->waveIn != nullptr) {
                    waveInAddBuffer(state->waveIn, &hdr, sizeof(WAVEHDR));
                }

                if (state->inputMonitoring) {
                    EnterCriticalSection(&state->audioStateLock);
                    state->monitorInputPcm.insert(state->monitorInputPcm.end(), samples, samples + sampleCount);
                    LeaveCriticalSection(&state->audioStateLock);
                }
            } else if (!state->recordStopRequested.load() && state->waveIn != nullptr) {
                hdr.dwBytesRecorded = 0;
                hdr.dwFlags &= ~WHDR_DONE;
                waveInAddBuffer(state->waveIn, &hdr, sizeof(WAVEHDR));
            }
        }
        Sleep(4);
    }

    return 0;
}

void StopRecording(UiState& state, bool commitTake) {
    if (!state.recording && state.waveIn == nullptr) {
        return;
    }

    state.recordStopRequested.store(true);

    if (state.recordThread != nullptr) {
        WaitForSingleObject(state.recordThread, INFINITE);
        CloseHandle(state.recordThread);
        state.recordThread = nullptr;
    }

    if (state.waveIn != nullptr) {
        waveInStop(state.waveIn);
        waveInReset(state.waveIn);
        for (size_t i = 0; i < state.waveInHeaders.size(); ++i) {
            waveInUnprepareHeader(state.waveIn, &state.waveInHeaders[i], sizeof(WAVEHDR));
        }
        waveInClose(state.waveIn);
        state.waveIn = nullptr;
    }

    if (commitTake && state.recordTrackIndex >= 0 && !state.recordedInputPcm.empty()) {
        const int channels = std::max(1, state.recordInputChannels);
        const std::uint32_t totalFrames = static_cast<std::uint32_t>(state.recordedInputPcm.size() / static_cast<size_t>(channels));
        const ULONGLONG nowTick = GetTickCount64();
        const ULONGLONG elapsedMsUll = (state.recordCaptureStartTickMs > 0 && nowTick > state.recordCaptureStartTickMs)
            ? (nowTick - state.recordCaptureStartTickMs)
            : 0ULL;
        const int elapsedMs = static_cast<int>(std::min<ULONGLONG>(elapsedMsUll, static_cast<ULONGLONG>(std::numeric_limits<int>::max())));
        const double expectedFrames = (elapsedMs > 0 && state.waveInFormat.nSamplesPerSec > 0)
            ? (static_cast<double>(elapsedMs) * static_cast<double>(state.waveInFormat.nSamplesPerSec) / 1000.0)
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
            take.displayName = L"Take " + std::to_wstring(static_cast<int>(state.audio.size()) + 1);
            take.sampleRate = static_cast<int>(state.waveInFormat.nSamplesPerSec);
            take.frames = frames;
            take.stereo = std::move(stereo);

            state.lastCommittedTakeSampleRate = take.sampleRate;
            state.lastCommittedTakeFrames = static_cast<int>(take.frames);
            state.lastCommittedTakeChannels = 2;

            const COLORREF clipColors[4] = {kPalette.clip1, kPalette.clip2, kPalette.clip3, kPalette.clip4};
            EnterCriticalSection(&state.audioStateLock);
            const int audioIndex = static_cast<int>(state.audio.size());
            state.audio.push_back(std::move(take));

            const float samplesPerBeat = SamplesPerBeat(state);
            const float startBeat = static_cast<float>(state.recordStartFrame) / std::max(1.0f, samplesPerBeat);
            const float lengthBeats = static_cast<float>(frames) / std::max(1.0f, samplesPerBeat);

            if (state.recordTrackIndex >= 0 && state.recordTrackIndex < static_cast<int>(state.tracks.size())) {
                state.clips.push_back(ClipItem{
                    state.recordTrackIndex,
                    audioIndex,
                    startBeat,
                    std::max(0.25f, lengthBeats),
                    clipColors[state.recordTrackIndex % 4],
                    state.tracks[static_cast<size_t>(state.recordTrackIndex)] + L" Rec",
                });
                state.projectModified = true;
                if (state.hwnd) UpdateWindowTitle(state.hwnd, state);
            }
            LeaveCriticalSection(&state.audioStateLock);
        }
    }

    state.waveInHeaders.clear();
    state.waveInData.clear();
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
    for (size_t i = 0; i < state.trackRecordArm.size(); ++i) {
        if (state.trackRecordArm[i]) {
            armedTrack = static_cast<int>(i);
            break;
        }
    }
    if (armedTrack < 0) {
        MessageBoxW(hwnd, L"Arm a track first using the R button.", L"Record", MB_OK | MB_ICONINFORMATION);
        return false;
    }

    const bool wasPlaying = state.playing;
    const bool hasTimelineAudio = !state.clips.empty() || !state.audio.empty();

    if (!state.playing && (!state.clips.empty() || state.metronomeRecord || state.countInEnabled)) {
        if (!StartPlayback(hwnd, state)) {
            return false;
        }
    }

    if (IsWasapiBackend(state.audioBackend)) {
        if (TryStartWasapiRecording(hwnd, state, armedTrack, wasPlaying)) {
            return true;
        }
    }

    std::vector<int> sampleRates;
    if (state.preferredSampleRate > 0) {
        sampleRates.push_back(state.preferredSampleRate);
    }
    if (state.projectSampleRate > 0) {
        sampleRates.push_back(state.projectSampleRate);
    }
    if (state.lastOpenedInputSampleRate > 0 &&
        std::find(sampleRates.begin(), sampleRates.end(), state.lastOpenedInputSampleRate) == sampleRates.end()) {
        sampleRates.push_back(state.lastOpenedInputSampleRate);
    }
    if (sampleRates.empty()) {
        MessageBoxW(hwnd,
            L"No input sample rate is configured.\nOpen Audio menu and choose a sample rate first.",
            L"Record",
            MB_OK | MB_ICONERROR);
        return false;
    }

    int chosenSampleRate = 0;

    auto fillFormat = [&](int channels, int sampleRate) {
        state.waveInFormat.wFormatTag = WAVE_FORMAT_PCM;
        state.waveInFormat.nChannels = static_cast<WORD>(channels);
        state.waveInFormat.nSamplesPerSec = static_cast<DWORD>(sampleRate);
        state.waveInFormat.wBitsPerSample = 16;
        state.waveInFormat.nBlockAlign = static_cast<WORD>((state.waveInFormat.nChannels * state.waveInFormat.wBitsPerSample) / 8);
        state.waveInFormat.nAvgBytesPerSec = state.waveInFormat.nSamplesPerSec * state.waveInFormat.nBlockAlign;
        state.waveInFormat.cbSize = 0;
    };

    MMRESULT openResult = MMSYSERR_ERROR;
    const UINT preferredDevice = state.selectedInputDeviceId;
    const UINT fallbackDevice = WAVE_MAPPER;
    const UINT devicesToTry[2] = {preferredDevice, fallbackDevice};
    for (UINT dev : devicesToTry) {
        if (dev == fallbackDevice && preferredDevice == fallbackDevice) {
            continue;
        }
        for (int srTry : sampleRates) {
            // Prefer mono for guitar tracking stability on MME, then stereo fallback.
            for (int chTry = 1; chTry <= 2; ++chTry) {
                fillFormat(chTry, srTry);
                openResult = waveInOpen(&state.waveIn, dev, &state.waveInFormat, 0, 0, CALLBACK_NULL);
                if (openResult == MMSYSERR_NOERROR && state.waveIn != nullptr) {
                    chosenSampleRate = srTry;
                    break;
                }
                state.waveIn = nullptr;
            }
            if (openResult == MMSYSERR_NOERROR && state.waveIn != nullptr) {
                break;
            }
        }
        if (openResult == MMSYSERR_NOERROR && state.waveIn != nullptr) {
            break;
        }
    }
    if (openResult != MMSYSERR_NOERROR || state.waveIn == nullptr) {
        state.waveIn = nullptr;
        MessageBoxW(hwnd, L"Could not open selected input device for recording (tried stereo and mono).", L"Record", MB_OK | MB_ICONERROR);
        return false;
    }

    if (chosenSampleRate > 0) {
        state.lastOpenedInputSampleRate = chosenSampleRate;
        if (state.projectSampleRate <= 0) {
            state.projectSampleRate = chosenSampleRate;
        }
    }

    if (!state.playing && (!state.clips.empty() || state.metronomeRecord)) {
        if (!StartPlayback(hwnd, state)) {
            waveInClose(state.waveIn);
            state.waveIn = nullptr;
            return false;
        }
    }

    state.waveInData.assign(kRecordBufferCount, std::vector<std::int16_t>(kRecordBufferFrames * state.waveInFormat.nChannels, 0));
    state.waveInHeaders.assign(kRecordBufferCount, WAVEHDR{});
    for (int i = 0; i < kRecordBufferCount; ++i) {
        WAVEHDR& hdr = state.waveInHeaders[static_cast<size_t>(i)];
        hdr.lpData = reinterpret_cast<LPSTR>(state.waveInData[static_cast<size_t>(i)].data());
        hdr.dwBufferLength = static_cast<DWORD>(state.waveInData[static_cast<size_t>(i)].size() * sizeof(std::int16_t));
        hdr.dwFlags = 0;
        hdr.dwBytesRecorded = 0;
        if (waveInPrepareHeader(state.waveIn, &hdr, sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
            StopRecording(state, false);
            MessageBoxW(hwnd, L"Could not prepare recording buffers.", L"Record", MB_OK | MB_ICONERROR);
            return false;
        }
        waveInAddBuffer(state.waveIn, &hdr, sizeof(WAVEHDR));
    }

    state.recordedInputPcm.clear();
    state.monitorInputPcm.clear();
    state.monitorInputReadPos = 0;
    state.recordInputChannels = state.waveInFormat.nChannels;
    state.recordTrackIndex = armedTrack;
    state.recordCaptureStartTickMs = GetTickCount64();
    const float samplesPerBeat = std::max(1.0f, SamplesPerBeat(state));
    const std::uint64_t timelineStartFrame = state.playing
        ? GetRenderedPlaybackFrame(state)
        : static_cast<std::uint64_t>(std::llround(static_cast<double>(std::max(0.0f, state.playheadBeat)) * static_cast<double>(samplesPerBeat)));

    // Set preroll + tentative placement before starting capture.
    state.recordPrerollFrames = 0;
    if (!wasPlaying && state.countInEnabled) {
        state.recordPrerollFrames = static_cast<std::uint64_t>(std::llround(4.0 * static_cast<double>(state.countInBars) * static_cast<double>(samplesPerBeat)));
    }
    state.recordStartFrame = timelineStartFrame + state.recordPrerollFrames;
    state.countingIn = (state.recordPrerollFrames > 0);

    state.recordStopRequested.store(false);
    state.recordThread = CreateThread(nullptr, 0, RecordThreadProc, &state, 0, nullptr);
    if (state.recordThread == nullptr) {
        state.countingIn = false;
        StopRecording(state, false);
        MessageBoxW(hwnd, L"Could not start recording thread.", L"Record", MB_OK | MB_ICONERROR);
        return false;
    }

    // MME capture starts synchronously, so latency is very small.
    // Sample playback position right as capture goes live for accurate placement.
    waveInStart(state.waveIn);
    {
        const std::uint64_t captureNow = state.playing ? GetRenderedPlaybackFrame(state) : 0;
        const std::uint64_t scheduledStart = state.recordStartFrame;
        const std::uint64_t actualSkip = (captureNow < scheduledStart) ? (scheduledStart - captureNow) : 0;
        state.recordPrerollFrames = actualSkip;
        state.recordStartFrame    = captureNow + actualSkip;
    }

    state.lastOpenedInputSampleRate = static_cast<int>(state.waveInFormat.nSamplesPerSec);
    state.lastOpenedInputChannels = static_cast<int>(state.waveInFormat.nChannels);
    state.recordUsingWasapi = false;
    state.recordInitState.store(1);
    state.recording = true;
    return true;
}

bool StartPlayback(HWND hwnd, UiState& state) {
    state.lastPlaybackInitError.clear();

    RefreshOutputDevices(state);
    if (state.outputDeviceIds.empty()) {
        MessageBoxW(hwnd, L"No audio output devices detected.", L"Playback error", MB_OK | MB_ICONERROR);
        return false;
    }

    if (state.clips.empty() && !state.metronomePlay && !state.metronomeRecord && !state.countInEnabled) {
        MessageBoxW(hwnd, L"Import at least one supported WAV file first.", L"No audio to play", MB_OK | MB_ICONINFORMATION);
        return false;
    }

    StopPlayback(state, false);

    // ---- Try WASAPI output (shared or exclusive based on settings) ----
    if (IsWasapiBackend(state.audioBackend)) {
        EnterCriticalSection(&state.audioStateLock);
        const float startBeat  = std::max(0.0f, state.playheadBeat);
        const std::uint64_t startFrame = static_cast<std::uint64_t>(
            std::llround(static_cast<double>(startBeat) * static_cast<double>(SamplesPerBeat(state))));
        state.playbackFrameCursor.store(startFrame);
        const std::uint64_t endFrame = ComputeProjectEndFrameLocked(state);
        LeaveCriticalSection(&state.audioStateLock);

        state.playbackStartTick = 0;
        state.playbackStartBeat = startBeat;
        state.playbackEndBeat   = static_cast<float>(endFrame) / SamplesPerBeat(state);
        state.playing           = true;
        state.playingViaWasapi  = true;
        state.audioStopRequested.store(false);
        state.wasapiOutInitState.store(0);
        state.audioThreadRunning.store(true);
        state.audioThread = CreateThread(nullptr, 0, WasapiRenderThreadProc, &state, 0, nullptr);

        if (state.audioThread != nullptr) {
            // Wait up to 600 ms for the thread to open the device
            for (int i = 0; i < 60 && state.wasapiOutInitState.load() == 0; ++i) {
                Sleep(10);
            }
            if (state.wasapiOutInitState.load() == 1) {
                state.playbackStartTick = GetTickCount64();
                const int devSR = state.activeDeviceSampleRate;
                const bool hasTimelineAudio = !state.clips.empty() || !state.audio.empty();
                if (hasTimelineAudio && state.projectSampleRate > 0 && devSR > 0 && state.projectSampleRate != devSR) {
                    std::wstringstream mismatch;
                    mismatch
                        << L"The selected device does not support " << state.projectSampleRate << L" Hz.\n\n"
                        << L"Yes: Switch project to " << devSR << L" Hz\n"
                        << L"No: Use device at " << devSR << L" Hz and resample project audio in real time\n"
                        << L"Cancel: Abort playback (choose a different device from Audio menu)";
                    const int choice = MessageBoxW(hwnd, mismatch.str().c_str(), L"Sample Rate Mismatch", MB_YESNOCANCEL | MB_ICONWARNING);
                    if (choice == IDYES) {
                        state.projectSampleRate = devSR;
                    } else if (choice == IDCANCEL) {
                        StopPlayback(state, false);
                        return false;
                    }
                }
                if (!state.lastPlaybackInitError.empty()) {
                    MessageBoxW(hwnd, state.lastPlaybackInitError.c_str(), L"Playback warning", MB_OK | MB_ICONWARNING);
                }
                return true;  // WASAPI output running
            }
            // Thread failed to open device – fall back to MME
            state.audioStopRequested.store(true);
            WaitForSingleObject(state.audioThread, INFINITE);
            CloseHandle(state.audioThread);
            state.audioThread = nullptr;
        }
        // Reset for MME fallback
        state.playing          = false;
        state.playingViaWasapi = false;
        state.audioStopRequested.store(false);
        state.audioThreadRunning.store(false);
    }
    // ---- Fall back to MME waveOut ----
    int mmeSampleRate = 0;
    if (state.preferredSampleRate > 0) {
        mmeSampleRate = state.preferredSampleRate;
    } else if (state.projectSampleRate > 0) {
        mmeSampleRate = state.projectSampleRate;
    } else if (state.lastOpenedOutputSampleRate > 0) {
        mmeSampleRate = state.lastOpenedOutputSampleRate;
    }
    if (mmeSampleRate <= 0) {
        MessageBoxW(hwnd,
            L"No playback sample rate is configured.\nOpen Audio menu and choose a sample rate first.",
            L"Playback error",
            MB_OK | MB_ICONERROR);
        return false;
    }
    state.waveFormat.nChannels = 2;
    state.waveFormat.nSamplesPerSec = static_cast<DWORD>(mmeSampleRate);
    state.waveFormat.wBitsPerSample = 16;
    state.waveFormat.nBlockAlign = static_cast<WORD>((state.waveFormat.nChannels * state.waveFormat.wBitsPerSample) / 8);
    state.waveFormat.nAvgBytesPerSec = state.waveFormat.nSamplesPerSec * state.waveFormat.nBlockAlign;
    state.waveFormat.cbSize = 0;

    MMRESULT outOpen = waveOutOpen(&state.waveOut, state.selectedOutputDeviceId, &state.waveFormat, 0, 0, CALLBACK_NULL);
    if (outOpen != MMSYSERR_NOERROR) {
        outOpen = waveOutOpen(&state.waveOut, WAVE_MAPPER, &state.waveFormat, 0, 0, CALLBACK_NULL);
    }
    if (outOpen != MMSYSERR_NOERROR) {
        wchar_t mmeErr[256]{};
        if (waveOutGetErrorTextW(outOpen, mmeErr, static_cast<UINT>(std::size(mmeErr))) != MMSYSERR_NOERROR) {
            wcscpy_s(mmeErr, L"Unknown MME error");
        }
        if (state.lastPlaybackInitError.empty()) {
            state.lastPlaybackInitError = std::wstring(L"MME open failed: ") + mmeErr;
        } else {
            state.lastPlaybackInitError += std::wstring(L"; MME open failed: ") + mmeErr;
        }
        state.waveOut = nullptr;
        const std::wstring msg = L"Unable to open selected output device.\n\n" + state.lastPlaybackInitError;
        MessageBoxW(hwnd, msg.c_str(), L"Playback error", MB_OK | MB_ICONERROR);
        return false;
    }
    state.lastOpenedOutputSampleRate = static_cast<int>(state.waveFormat.nSamplesPerSec);
    state.lastOpenedOutputChannels = static_cast<int>(state.waveFormat.nChannels);
    state.activeDeviceSampleRate = state.lastOpenedOutputSampleRate;
    state.activeDeviceBufferFrames = kAudioBufferFrames;

    state.waveData.assign(kAudioBufferCount, std::vector<std::int16_t>(kAudioBufferFrames * 2, 0));
    state.waveHeaders.assign(kAudioBufferCount, WAVEHDR{});

    for (int i = 0; i < kAudioBufferCount; ++i) {
        WAVEHDR& hdr = state.waveHeaders[static_cast<size_t>(i)];
        hdr.lpData = reinterpret_cast<LPSTR>(state.waveData[static_cast<size_t>(i)].data());
        hdr.dwBufferLength = static_cast<DWORD>(state.waveData[static_cast<size_t>(i)].size() * sizeof(std::int16_t));
        hdr.dwFlags = 0;
        if (waveOutPrepareHeader(state.waveOut, &hdr, sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
            StopPlayback(state, false);
            MessageBoxW(hwnd, L"Unable to prepare audio buffers.", L"Playback error", MB_OK | MB_ICONERROR);
            return false;
        }
    }

    EnterCriticalSection(&state.audioStateLock);
    const float startBeat = std::max(0.0f, state.playheadBeat);
    const std::uint64_t startFrame = static_cast<std::uint64_t>(
        std::llround(static_cast<double>(startBeat) * static_cast<double>(SamplesPerBeat(state))));
    state.playbackFrameCursor.store(startFrame);
    const std::uint64_t endFrame = ComputeProjectEndFrameLocked(state);
    LeaveCriticalSection(&state.audioStateLock);

    state.playbackStartTick = GetTickCount64();
    state.playbackStartBeat = startBeat;
    state.playbackEndBeat = static_cast<float>(endFrame) / SamplesPerBeat(state);
    state.playing = true;
    state.audioStopRequested.store(false);
    state.audioThreadRunning.store(true);
    state.audioThread = CreateThread(nullptr, 0, AudioThreadProc, &state, 0, nullptr);

    if (state.audioThread == nullptr) {
        StopPlayback(state, false);
        MessageBoxW(hwnd, L"Unable to start audio thread.", L"Playback error", MB_OK | MB_ICONERROR);
        return false;
    }

    return true;
}

std::uint64_t GetRenderedPlaybackFrame(const UiState& state) {
    auto clockFrame = [&state]() -> std::uint64_t {
        if (state.projectSampleRate <= 0 || state.playbackStartTick == 0) {
            return 0;
        }
        const ULONGLONG elapsedMs = GetTickCount64() - state.playbackStartTick;
        return static_cast<std::uint64_t>((elapsedMs * static_cast<ULONGLONG>(state.projectSampleRate)) / 1000ULL);
    };

    const float samplesPerBeat = std::max(1.0f, SamplesPerBeat(state));
    const std::uint64_t startFrame = static_cast<std::uint64_t>(
        std::llround(static_cast<double>(std::max(0.0f, state.playbackStartBeat)) * static_cast<double>(samplesPerBeat)));

    // Return absolute project frames. Audio generation may queue ahead, so clamp to elapsed transport time.
    if (state.playingViaWasapi || state.waveOut == nullptr || state.projectSampleRate <= 0) {
        const std::uint64_t cursor = state.playbackFrameCursor.load();
        const std::uint64_t clock  = clockFrame();
        return (clock > 0) ? std::min(cursor, startFrame + clock) : cursor;
    }

    const std::uint64_t clock = clockFrame();

    MMTIME mm{};
    mm.wType = TIME_BYTES;
    if (waveOutGetPosition(state.waveOut, &mm, sizeof(mm)) == MMSYSERR_NOERROR) {
        std::uint64_t deviceFrame = 0;
        if (mm.wType == TIME_BYTES && state.waveFormat.nBlockAlign > 0) {
            deviceFrame = static_cast<std::uint64_t>(mm.u.cb / state.waveFormat.nBlockAlign);
        } else if (mm.wType == TIME_SAMPLES) {
            const std::uint64_t rawSamples = static_cast<std::uint64_t>(mm.u.sample);
            const std::uint64_t channels = std::max<std::uint64_t>(1, state.waveFormat.nChannels);
            deviceFrame = rawSamples / channels;
        } else if (mm.wType == TIME_MS) {
            deviceFrame = static_cast<std::uint64_t>((static_cast<ULONGLONG>(mm.u.ms) * static_cast<ULONGLONG>(state.projectSampleRate)) / 1000ULL);
        }

        if (deviceFrame > 0) {
            // Guard against bad driver/unit reporting that can run the playhead far ahead.
            const std::uint64_t maxReasonableAhead = static_cast<std::uint64_t>(state.projectSampleRate);
            if (clock > 0 && deviceFrame > clock + maxReasonableAhead) {
                return startFrame + clock;
            }
            return startFrame + deviceFrame;
        }
    }

    if (clock > 0) {
        return startFrame + clock;
    }
    return state.playbackFrameCursor.load();
}

DWORD WINAPI AutoMixThreadProc(LPVOID param) {
    auto* state = reinterpret_cast<UiState*>(param);
    if (state == nullptr || state->hwnd == nullptr) {
        return 0;
    }

    const bool ok = ApplyAutoMixToFaders(state->hwnd, *state);
    state->automixRunning.store(false);
    PostMessage(state->hwnd, kMsgAutoMixFinished, ok ? 1 : 0, 0);
    return 0;
}

void StartAutoMixAsync(HWND hwnd, UiState& state) {
    if (state.automixRunning.load()) {
        MessageBoxW(hwnd, L"AutoMix is already running.", L"AutoMix", MB_OK | MB_ICONINFORMATION);
        return;
    }

    state.automixRunning.store(true);
    state.automixThread = CreateThread(nullptr, 0, AutoMixThreadProc, &state, 0, nullptr);
    if (state.automixThread == nullptr) {
        state.automixRunning.store(false);
        MessageBoxW(hwnd, L"Could not start AutoMix worker thread.", L"AutoMix", MB_OK | MB_ICONERROR);
        return;
    }

    InvalidateRect(hwnd, nullptr, FALSE);
}


LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<UiState*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));

    switch (msg) {
    case WM_CREATE: {
        auto* initial = new UiState();
        initial->hwnd = hwnd;
        initial->busInsertEffects.assign(kBusCount, DefaultInsertEffects());
        initial->busInsertBypass.assign(kBusCount, DefaultInsertBypass());
        initial->busInsertParams.assign(kBusCount, DefaultInsertParams());
        RefreshInputDevices(*initial);
        RefreshOutputDevices(*initial);
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
            const float samplesPerBeat = SamplesPerBeat(*state);
            const std::uint64_t absoluteFrame = GetRenderedPlaybackFrame(*state);
            state->playheadBeat = static_cast<float>(absoluteFrame) / std::max(1.0f, samplesPerBeat);

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

            if (state->selectedClipIndex >= 0 && state->selectedClipIndex < static_cast<int>(state->clips.size())) {
                DeleteSelectedClip(*state);
                state->projectModified = true;
                UpdateWindowTitle(hwnd, *state);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }

            if (state->selectedTrackIndex >= 0 && state->selectedTrackIndex < static_cast<int>(state->tracks.size())) {
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
                state->bpm = std::max(40, state->bpm - (coarse ? 5 : 1));
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (PtInRect(&state->bpmUpRect, pt)) {
                const bool coarse = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                state->bpm = std::min(260, state->bpm + (coarse ? 5 : 1));
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
            const LayoutRects layout = ComputeLayout(client);

            // ── Insert inspector click handling ──────────────────────────
            if (state->fxInspectorOpen && state->fxInspectorIndex >= 0) {
                const RECT inspPanel = GetInspectorPanelRect(client, *state);
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
                    InsertParamsArray* pParams  = nullptr;
                    if (state->fxInspectorIsTrack) {
                        if (idx < static_cast<int>(state->trackInsertSlots.size()))
                            pSlots = &state->trackInsertSlots[static_cast<size_t>(idx)];
                        if (idx < static_cast<int>(state->trackInsertEffects.size()))
                            pEffects = &state->trackInsertEffects[static_cast<size_t>(idx)];
                        if (idx < static_cast<int>(state->trackInsertBypass.size()))
                            pBypass = &state->trackInsertBypass[static_cast<size_t>(idx)];
                        if (idx < static_cast<int>(state->trackInsertParams.size()))
                            pParams = &state->trackInsertParams[static_cast<size_t>(idx)];
                    } else {
                        if (idx < static_cast<int>(state->busInsertSlots.size()))
                            pSlots = &state->busInsertSlots[static_cast<size_t>(idx)];
                        if (idx < static_cast<int>(state->busInsertEffects.size()))
                            pEffects = &state->busInsertEffects[static_cast<size_t>(idx)];
                        if (idx < static_cast<int>(state->busInsertBypass.size()))
                            pBypass = &state->busInsertBypass[static_cast<size_t>(idx)];
                        if (idx < static_cast<int>(state->busInsertParams.size()))
                            pParams = &state->busInsertParams[static_cast<size_t>(idx)];
                    }

                    const int slotCount = pSlots ? std::clamp(*pSlots, 0, kMaxInsertSlots) : 0;

                    // Close button
                    RECT closeBtn{inspPanel.right - 24, inspPanel.top + 4, inspPanel.right - 4, inspPanel.top + kInspHeaderH - 4};
                    if (PtInRect(&closeBtn, pt)) {
                        state->fxInspectorOpen = false;
                        InvalidateRect(hwnd, nullptr, FALSE);
                        return 0;
                    }

                    // ADD / REM buttons
                    const int ctrlTop = inspPanel.top + kInspHeaderH;
                    RECT addBtn{inspPanel.left + 6,  ctrlTop + 4, inspPanel.left + 66,  ctrlTop + kInspCtrlH - 4};
                    RECT remBtn{inspPanel.left + 72, ctrlTop + 4, inspPanel.left + 132, ctrlTop + kInspCtrlH - 4};
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
                    const int slotsTop = ctrlTop + kInspCtrlH;
                    for (int s = 0; s < slotCount; ++s) {
                        const int rowTop = slotsTop + s * kInspSlotH;
                        const int rowBot = rowTop + kInspSlotH;
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
                        const int paramTop = slotsTop + slotCount * kInspSlotH;
                        const int ky = paramTop + 16;
                        const int kw = (kInspW - 12) / 4;
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
                            const RECT kRect{kx, ky, kx + kw - 2, ky + kInspParamH - 18};
                            if (PtInRect(&kRect, pt)) {
                                // Get current value for this paramId
                                const InsertParams& P = (*pParams)[static_cast<size_t>(selSlot)];
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
                const float beat = std::max(0.0f, XToBeat(layout.ruler, *state, pt.x));
                state->playheadBeat = SnapBeat(beat);
                state->draggingPlayhead = true;
                SetCapture(hwnd);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }

            if (PtInRect(&layout.leftPanel, pt) && pt.y > layout.leftPanel.top + kRulerHeight && !state->tracks.empty()) {
                const int trackIndex = TrackIndexFromY(layout.arrange, *state, pt.y);
                if (trackIndex >= 0 && trackIndex < static_cast<int>(state->trackGainDb.size())) {
                    state->selectedTrackIndex = trackIndex;
                    state->selectedClipIndex = -1;

                    RECT busRect{};
                    RECT panKnobRect{};
                    RECT panValRect{};
                    RECT fxRect{};
                    GetTrackRoutingRects(layout.leftPanel, trackIndex, &busRect, &panKnobRect, &panValRect, &fxRect);
                    if (PtInRect(&busRect, pt)) {
                        EnterCriticalSection(&state->audioStateLock);
                        if (trackIndex < static_cast<int>(state->trackBusIndex.size())) {
                            const int cur = TrackBusIndexAt(*state, trackIndex);
                            state->trackBusIndex[static_cast<size_t>(trackIndex)] = (cur + 1) % kBusCount;
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
                            if (trackIndex < static_cast<int>(state->trackPan.size()))
                                state->trackPan[static_cast<size_t>(trackIndex)] = 0.0f;
                            state->projectModified = true;
                            UpdateWindowTitle(hwnd, *state);
                            LeaveCriticalSection(&state->audioStateLock);
                        } else if (trackIndex < static_cast<int>(state->trackPan.size())) {
                            // Start drag
                            state->draggingPan    = true;
                            state->dragPanIsBus   = false;
                            state->dragPanIndex   = trackIndex;
                            state->dragPanStartY  = pt.y;
                            state->dragPanStartVal = state->trackPan[static_cast<size_t>(trackIndex)];
                            SetCapture(hwnd);
                        }
                        InvalidateRect(hwnd, nullptr, FALSE);
                        return 0;
                    }

                    RECT muteRect{};
                    RECT soloRect{};
                    RECT recRect{};
                    GetTrackButtonRects(layout.leftPanel, trackIndex, &muteRect, &soloRect, &recRect);
                    if (PtInRect(&muteRect, pt)) {
                        EnterCriticalSection(&state->audioStateLock);
                        if (trackIndex < static_cast<int>(state->trackMute.size())) {
                            state->trackMute[static_cast<size_t>(trackIndex)] = !state->trackMute[static_cast<size_t>(trackIndex)];
                        }
                        LeaveCriticalSection(&state->audioStateLock);
                        InvalidateRect(hwnd, nullptr, FALSE);
                        return 0;
                    }
                    if (PtInRect(&recRect, pt)) {
                        EnterCriticalSection(&state->audioStateLock);
                        if (trackIndex < static_cast<int>(state->trackRecordArm.size())) {
                            state->trackRecordArm[static_cast<size_t>(trackIndex)] = !state->trackRecordArm[static_cast<size_t>(trackIndex)];
                        }
                        LeaveCriticalSection(&state->audioStateLock);
                        InvalidateRect(hwnd, nullptr, FALSE);
                        return 0;
                    }
                    if (PtInRect(&soloRect, pt)) {
                        EnterCriticalSection(&state->audioStateLock);
                        if (trackIndex < static_cast<int>(state->trackSolo.size())) {
                            state->trackSolo[static_cast<size_t>(trackIndex)] = !state->trackSolo[static_cast<size_t>(trackIndex)];
                        }
                        LeaveCriticalSection(&state->audioStateLock);
                        InvalidateRect(hwnd, nullptr, FALSE);
                        return 0;
                    }

                    RECT rail{};
                    RECT knob{};
                    GetTrackFaderRects(layout.leftPanel, trackIndex, &rail, &knob);
                    RECT hitRect{rail.left - 12, rail.top, rail.right + 12, rail.bottom};
                    if (PtInRect(&hitRect, pt)) {
                        PushUndo(*state);
                        state->draggingFader = true;
                        state->dragFaderTrack = trackIndex;
                        state->dragFaderStartY = pt.y;
                        EnterCriticalSection(&state->audioStateLock);
                        state->dragFaderStartDb = GainFromFaderY(rail, pt.y);
                        state->trackGainDb[static_cast<size_t>(trackIndex)] = state->dragFaderStartDb;
                        LeaveCriticalSection(&state->audioStateLock);
                        SetCapture(hwnd);
                        InvalidateRect(hwnd, nullptr, FALSE);
                        return 0;
                    }
                }

                const int busTop = BusPanelTop(layout.leftPanel, *state);
                if (pt.y >= busTop + 18) {
                    for (int b = 0; b < kBusCount; ++b) {
                        RECT rowRect{};
                        RECT muteRect{};
                        RECT gainDownRect{};
                        RECT gainUpRect{};
                        RECT panKnobRect{};
                        RECT panValRect{};
                        RECT fxRect{};
                        GetBusControlRects(layout.leftPanel, *state, b, &rowRect, &muteRect, &gainDownRect, &gainUpRect, &panKnobRect, &panValRect, &fxRect);
                        if (!PtInRect(&rowRect, pt)) {
                            continue;
                        }

                        EnterCriticalSection(&state->audioStateLock);
                        if (PtInRect(&muteRect, pt) && b < static_cast<int>(state->busMute.size())) {
                            state->busMute[static_cast<size_t>(b)] = !state->busMute[static_cast<size_t>(b)];
                        } else if (PtInRect(&gainDownRect, pt) && b < static_cast<int>(state->busGainDb.size())) {
                            state->busGainDb[static_cast<size_t>(b)] = std::max(kFaderMinDb, state->busGainDb[static_cast<size_t>(b)] - 1.0f);
                        } else if (PtInRect(&gainUpRect, pt) && b < static_cast<int>(state->busGainDb.size())) {
                            state->busGainDb[static_cast<size_t>(b)] = std::min(kFaderMaxDb, state->busGainDb[static_cast<size_t>(b)] + 1.0f);
                        } else if ((PtInRect(&panKnobRect, pt) || PtInRect(&panValRect, pt)) && b < static_cast<int>(state->busPan.size())) {
                            LeaveCriticalSection(&state->audioStateLock);
                            state->draggingPan    = true;
                            state->dragPanIsBus   = true;
                            state->dragPanIndex   = b;
                            state->dragPanStartY  = pt.y;
                            state->dragPanStartVal = state->busPan[static_cast<size_t>(b)];
                            SetCapture(hwnd);
                            InvalidateRect(hwnd, nullptr, FALSE);
                            return 0;
                        } else if (PtInRect(&fxRect, pt) && b < static_cast<int>(state->busInsertSlots.size())) {
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
                for (int i = static_cast<int>(state->clips.size()) - 1; i >= 0; --i) {
                    RECT r{};
                    if (!ClipRectForDraw(layout.arrange, *state, state->clips[static_cast<size_t>(i)], &r)) {
                        continue;
                    }
                    if (PtInRect(&r, pt)) {
                        state->selectedClipIndex = i;
                        state->selectedTrackIndex = state->clips[static_cast<size_t>(i)].trackIndex;

                        constexpr int kEdgeThresh = 7;
                        const int fullLeft  = BeatToX(layout.arrange, *state, state->clips[static_cast<size_t>(i)].startBeat);
                        const int fullRight = BeatToX(layout.arrange, *state, state->clips[static_cast<size_t>(i)].startBeat + state->clips[static_cast<size_t>(i)].lengthBeats);
                        const bool nearLeft  = (pt.x - fullLeft)  <= kEdgeThresh && (pt.x - fullLeft)  >= 0;
                        const bool nearRight = (fullRight - pt.x)  <= kEdgeThresh && (fullRight - pt.x) >= 0;

                        if (nearLeft || nearRight) {
                            // Trim
                            state->trimmingClip         = true;
                            state->trimClipIndex        = i;
                            state->trimIsLeft           = nearLeft;
                            state->trimOrigStart        = state->clips[static_cast<size_t>(i)].startBeat;
                            state->trimOrigLen          = state->clips[static_cast<size_t>(i)].lengthBeats;
                            state->trimOrigSourceOffset = state->clips[static_cast<size_t>(i)].sourceOffsetFrames;
                            PushUndo(*state);
                            SetCapture(hwnd);
                        } else {
                            // Drag
                            PushUndo(*state);
                            state->draggingClip = true;
                            state->dragClipIndex = i;
                            state->dragOffsetBeats = XToBeat(layout.arrange, *state, pt.x) - state->clips[static_cast<size_t>(i)].startBeat;
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
            const LayoutRects layout = ComputeLayout(client);
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
            if (pt.y > layout.leftPanel.top + kRulerHeight && !state->tracks.empty()) {
                trackIndex = TrackIndexFromY(layout.arrange, *state, pt.y);
            }
            if (trackIndex >= 0 && trackIndex < static_cast<int>(state->trackRecordArm.size())) {
                const bool armed = state->trackRecordArm[static_cast<size_t>(trackIndex)];
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
            } else if (cmd == kCmdTrackNew + 1000 && trackIndex >= 0 && trackIndex < static_cast<int>(state->trackRecordArm.size())) {
                EnterCriticalSection(&state->audioStateLock);
                state->trackRecordArm[static_cast<size_t>(trackIndex)] = !state->trackRecordArm[static_cast<size_t>(trackIndex)];
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
            const LayoutRects layout = ComputeLayout(client);
            const float beat = std::max(0.0f, XToBeat(layout.ruler, *state, GET_X_LPARAM(lParam)));
            state->playheadBeat = SnapBeat(beat);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        if (state->trimmingClip && state->trimClipIndex >= 0 &&
            state->trimClipIndex < static_cast<int>(state->clips.size())) {
            RECT client{};
            GetClientRect(hwnd, &client);
            const LayoutRects layout = ComputeLayout(client);
            const float mouseBeat = SnapBeat(std::max(0.0f, XToBeat(layout.arrange, *state, GET_X_LPARAM(lParam))));
            ClipItem& clip = state->clips[static_cast<size_t>(state->trimClipIndex)];
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
            const LayoutRects layout = ComputeLayout(client);
            RECT rail{};
            RECT knob{};
            GetTrackFaderRects(layout.leftPanel, state->dragFaderTrack, &rail, &knob);
            const int mouseY = GET_Y_LPARAM(lParam);
            const bool shiftFine = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            float newDb;
            if (shiftFine) {
                // Fine mode: 1px = 0.05 dB (drag relative from start point)
                const int dy = mouseY - state->dragFaderStartY;
                newDb = std::clamp(state->dragFaderStartDb - dy * 0.05f, kFaderMinDb, kFaderMaxDb);
            } else {
                newDb = GainFromFaderY(rail, mouseY);
            }
            EnterCriticalSection(&state->audioStateLock);
            state->trackGainDb[static_cast<size_t>(state->dragFaderTrack)] = newDb;
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
                if (state->dragPanIndex < static_cast<int>(state->busPan.size()))
                    state->busPan[static_cast<size_t>(state->dragPanIndex)] = newPan;
            } else {
                if (state->dragPanIndex < static_cast<int>(state->trackPan.size()))
                    state->trackPan[static_cast<size_t>(state->dragPanIndex)] = newPan;
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
            InsertParamsArray* pPA = nullptr;
            if (state->fxInspectorIsTrack) {
                if (inspIdx >= 0 && inspIdx < static_cast<int>(state->trackInsertParams.size()))
                    pPA = &state->trackInsertParams[static_cast<size_t>(inspIdx)];
            } else {
                if (inspIdx >= 0 && inspIdx < static_cast<int>(state->busInsertParams.size()))
                    pPA = &state->busInsertParams[static_cast<size_t>(inspIdx)];
            }
            if (pPA && slotIdx >= 0 && slotIdx < kMaxInsertSlots) {
                InsertParams& P = (*pPA)[static_cast<size_t>(slotIdx)];
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
            const LayoutRects layout = ComputeLayout(client);

            const int mouseX = GET_X_LPARAM(lParam);
            const int mouseY = GET_Y_LPARAM(lParam);
            float newStart = XToBeat(layout.arrange, *state, mouseX) - state->dragOffsetBeats;
            newStart = std::max(0.0f, SnapBeat(newStart));

            EnterCriticalSection(&state->audioStateLock);
            ClipItem& clip = state->clips[static_cast<size_t>(state->dragClipIndex)];
            clip.startBeat = newStart;
            clip.trackIndex = TrackIndexFromY(layout.arrange, *state, mouseY);
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
                const LayoutRects layout = ComputeLayout(client);
                POINT p{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                ScreenToClient(hwnd, &p);
                const int focusX = std::clamp(p.x, layout.arrange.left, layout.arrange.right);
                const float beatAtCursor = XToBeat(layout.arrange, *state, focusX);
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

        Fill(memDc, client, kPalette.windowBg);

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
            DrawTopBar(memDc, client, *state);

            SelectObject(memDc, smallFont);

            const LayoutRects layout = ComputeLayout(client);
            DrawLeftTrackPanel(memDc, layout.leftPanel, *state);
            DrawRuler(memDc, layout.ruler, *state);
            DrawArrangeLanes(memDc, layout.arrange, *state);

            // Inspector panel floats on top of everything
            if (state->fxInspectorOpen)
                DrawInsertInspector(memDc, client, *state);

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
