#include "engine_utils.h"
#include "core/automation.h"
#include "core/timeline.h"
#include "core/track_audibility.h"
#include "dsp/chain.h"
#include "dsp/mix.h"
#include "dsp/resample.h"
#include "dsp/util.h"
#include "engine/clip_reader.h"
#include "engine/timeline_layout.h"

#include <algorithm>

// CoreState/AudioRuntimeState-aware helpers stay here. Pure DSP (DbToLinear,
// resamplers) lives in libs/dsp; this file's same-named free functions are
// now thin shims that delegate, preserving the public surface used by
// engine.cpp, device_*.cpp, and main.cpp.

float DbToLinear(float db) {
    return daw::dsp::DbToLinear(db);
}

float BusGainDbAt(const CoreState& core, int busIndex) {
    if (busIndex < 0 || busIndex >= static_cast<int>(core.project.buses.size())) {
        return 0.0f;
    }
    return core.project.buses[static_cast<size_t>(busIndex)].gainDb;
}

float BusPanAt(const CoreState& core, int busIndex) {
    if (busIndex < 0 || busIndex >= static_cast<int>(core.project.buses.size())) {
        return 0.0f;
    }
    return std::clamp(core.project.buses[static_cast<size_t>(busIndex)].pan, -1.0f, 1.0f);
}

bool BusMuteAt(const CoreState& core, int busIndex) {
    if (busIndex < 0 || busIndex >= static_cast<int>(core.project.buses.size())) {
        return false;
    }
    return core.project.buses[static_cast<size_t>(busIndex)].mute;
}

void EnsureInsertDspStateStorage(const CoreState& core, AudioRuntimeState& audio) {
    if (audio.trackInsertDspState.size() != core.project.tracks.size()) {
        audio.trackInsertDspState.resize(core.project.tracks.size());
    }
}

bool IsTrackAudible(const CoreState& core, int trackIndex) {
    return daw::core::IsTrackAudible(core.project.tracks, trackIndex);
}

TrackBusMix ResolveTrackBusMix(const CoreState& core, int trackIndex) {
    TrackBusMix r{ /*audible*/ false, /*busIndex*/ 0, /*gainL*/ 0.0f, /*gainR*/ 0.0f };
    const int trackCount = static_cast<int>(core.project.tracks.size());
    if (trackIndex < 0 || trackIndex >= trackCount) {
        return r;
    }
    const auto& t = core.project.tracks[static_cast<size_t>(trackIndex)];
    r.busIndex = std::clamp(t.busIndex, 0, kBusCount - 1);

    if (t.mute) return r;
    if (r.busIndex >= static_cast<int>(core.project.buses.size())) return r;
    if (core.project.buses[static_cast<size_t>(r.busIndex)].mute) return r;

    // Track-only gain/pan. Bus dB/pan are applied later at the bus stage so
    // offline output matches realtime (equal-power pan is non-linear, so
    // folding bus pan per-track and after-sum produce different results).
    const float gain = daw::dsp::DbToLinear(t.gainDb);
    const float pan  = std::clamp(t.pan, -1.0f, 1.0f);
    daw::dsp::ApplyGainAndPan(gain, pan, &r.gainL, &r.gainR);
    r.audible = true;
    return r;
}

TrackBusMix ResolveTrackRealtimeMix(const CoreState& core, int trackIndex) {
    TrackBusMix r{ /*audible*/ false, /*busIndex*/ 0, /*gainL*/ 0.0f, /*gainR*/ 0.0f };
    if (trackIndex < 0 || trackIndex >= static_cast<int>(core.project.tracks.size())) {
        return r;
    }
    if (!IsTrackAudible(core, trackIndex)) {
        return r;
    }
    r.busIndex = std::clamp(AutomationTrackBusIndexAt(core, trackIndex), 0, kBusCount - 1);
    const float gain = daw::dsp::DbToLinear(AutomationTrackGainDbAt(core, trackIndex));
    const float pan  = AutomationTrackPanAt(core, trackIndex);
    daw::dsp::ApplyGainAndPan(gain, pan, &r.gainL, &r.gainR);
    r.audible = true;
    return r;
}

BusRealtimeMix ResolveBusRealtimeMix(const CoreState& core, int busIndex) {
    BusRealtimeMix r{ /*active*/ false, /*gainL*/ 0.0f, /*gainR*/ 0.0f };
    if (busIndex < 0 || busIndex >= kBusCount) {
        return r;
    }
    if (BusMuteAt(core, busIndex)) {
        return r;
    }
    const float busGain = daw::dsp::DbToLinear(BusGainDbAt(core, busIndex));
    // Bus index kBusCount-1 is the master bus and skips the pan stage.
    if (busIndex == kBusCount - 1) {
        r.gainL = busGain;
        r.gainR = busGain;
    } else {
        daw::dsp::ApplyGainAndPan(busGain, BusPanAt(core, busIndex), &r.gainL, &r.gainR);
    }
    r.active = true;
    return r;
}

std::uint64_t ComputeProjectEndFrameLocked(const CoreState& core) {
    return daw::engine::ComputeProjectEndFrame(
        core.project.clips,
        static_cast<int>(core.project.audio.size()),
        TimelineSamplesPerBeat(core));
}

void ApplyTrackInsertChain(
    const CoreState& core,
    AudioRuntimeState& audio,
    int trackIndex,
    std::vector<float>& buf,
    float sampleRate)
{
    if (trackIndex < 0 || trackIndex >= static_cast<int>(core.project.tracks.size())) {
        return;
    }
    const auto& t = core.project.tracks[static_cast<size_t>(trackIndex)];
    DspApplyInsertChain(buf, sampleRate,
        t.insertEffects, t.insertBypass, t.insertConfig,
        audio.trackInsertDspState[static_cast<size_t>(trackIndex)],
        t.insertSlots);
}

void ApplyBusInsertChain(
    const CoreState& core,
    AudioRuntimeState& audio,
    int busIndex,
    std::vector<float>& buf,
    float sampleRate)
{
    if (busIndex < 0 || busIndex >= static_cast<int>(core.project.buses.size())) {
        return;
    }
    const auto& b = core.project.buses[static_cast<size_t>(busIndex)];
    if (b.insertSlots <= 0) {
        return;
    }
    DspApplyInsertChain(buf, sampleRate,
        b.insertEffects, b.insertBypass, b.insertConfig,
        audio.busInsertDspState[static_cast<size_t>(busIndex)],
        b.insertSlots);
}

bool ReadClipSampleAtProjectFrame(
    const LoadedAudio& audio,
    std::uint64_t clipFrameInProjectRate,
    int projectSampleRate,
    std::uint64_t sourceOffsetFrames,
    float* outL,
    float* outR) {
    return daw::engine::ReadClipSample(audio, clipFrameInProjectRate, projectSampleRate, sourceOffsetFrames, outL, outR);
}

void ResampleStereoPcm16Linear(const std::int16_t* src, int srcFrames, std::int16_t* dst, int dstFrames) {
    daw::dsp::ResampleStereoPcm16Linear(src, srcFrames, dst, dstFrames);
}

void ResampleStereoFloatLinear(const float* src, int srcFrames, float* dst, int dstFrames) {
    daw::dsp::ResampleStereoFloatLinear(src, srcFrames, dst, dstFrames);
}

void ResampleStereoFloatSincHQ(
    const float* src, int srcFrames, int srcSampleRate,
    float* dst,       int dstFrames, int dstSampleRate) {
    daw::dsp::ResampleStereoFloatSincHQ(src, srcFrames, srcSampleRate, dst, dstFrames, dstSampleRate);
}

int ResampleStereoPcm16LinearStateful(
    const std::int16_t* src, int srcFrames,
    std::int16_t* dst,       int dstFrames,
    double step,
    double* phase,
    std::int16_t* lastL, std::int16_t* lastR,
    bool* primed) {
    return daw::dsp::ResampleStereoPcm16LinearStateful(
        src, srcFrames, dst, dstFrames, step, phase, lastL, lastR, primed);
}
