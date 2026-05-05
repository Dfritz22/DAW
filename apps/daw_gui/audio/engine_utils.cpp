#include "engine_utils.h"

// ── File-private helpers (audio-domain copies of layout math) ────────────────
// These are anonymous-namespace duplicates of the same functions that live in
// ui/layout.cpp.  They exist so that engine_utils.cpp (and transitively
// engine.cpp) never need to include ui/layout.h, keeping the dependency
// direction main.cpp → audio/engine → engine_utils, never the reverse.
namespace {

float SamplesPerBeat(const UiState& state) {
    int sr = state.project.projectSampleRate;
    if (sr <= 0) sr = state.activeDeviceSampleRate;
    if (sr <= 0) sr = state.preferredSampleRate;
    if (sr <= 0) sr = 1;
    return static_cast<float>(sr) * 60.0f / state.project.bpm;
}

} // namespace

// ── Public definitions ────────────────────────────────────────────────────────

float DbToLinear(float db) {
    return std::pow(10.0f, db / 20.0f);
}

float BusGainDbAt(const UiState& state, int busIndex) {
    if (busIndex < 0 || busIndex >= static_cast<int>(state.project.buses.size())) {
        return 0.0f;
    }
    return state.project.buses[static_cast<size_t>(busIndex)].gainDb;
}

float BusPanAt(const UiState& state, int busIndex) {
    if (busIndex < 0 || busIndex >= static_cast<int>(state.project.buses.size())) {
        return 0.0f;
    }
    return std::clamp(state.project.buses[static_cast<size_t>(busIndex)].pan, -1.0f, 1.0f);
}

bool BusMuteAt(const UiState& state, int busIndex) {
    if (busIndex < 0 || busIndex >= static_cast<int>(state.project.buses.size())) {
        return false;
    }
    return state.project.buses[static_cast<size_t>(busIndex)].mute;
}

void EnsureInsertDspStateStorage(const UiState& state) {
    if (state.trackInsertDspState.size() != state.project.tracks.size()) {
        state.trackInsertDspState.resize(state.project.tracks.size());
    }
}

bool IsTrackAudible(const UiState& state, int trackIndex) {
    if (trackIndex < 0 || trackIndex >= static_cast<int>(state.project.tracks.size())) {
        return false;
    }

    const bool muted =
        trackIndex < static_cast<int>(state.project.tracks.size()) &&
        state.project.tracks[static_cast<size_t>(trackIndex)].mute;
    const bool soloed =
        trackIndex < static_cast<int>(state.project.tracks.size()) &&
        state.project.tracks[static_cast<size_t>(trackIndex)].solo;

    bool anySolo = false;
    for (size_t i = 0; i < state.project.tracks.size(); ++i) {
        if (state.project.tracks[i].solo) {
            anySolo = true;
            break;
        }
    }

    if (muted) {
        return false;
    }
    if (anySolo && !soloed) {
        return false;
    }
    return true;
}

std::uint64_t ComputeProjectEndFrameLocked(const UiState& state) {
    const float samplesPerBeat = SamplesPerBeat(state);
    std::uint64_t maxFrame = 0;
    for (const ClipItem& clip : state.project.clips) {
        if (clip.audioIndex < 0 || clip.audioIndex >= static_cast<int>(state.project.audio.size())) {
            continue;
        }
        const std::uint64_t clipStartTL   = static_cast<std::uint64_t>(std::llround(std::max(0.0f, clip.startBeat) * samplesPerBeat));
        const std::uint64_t clipLenFrames = static_cast<std::uint64_t>(std::llround(clip.lengthBeats * samplesPerBeat));
        maxFrame = std::max(maxFrame, clipStartTL + clipLenFrames);
    }
    return maxFrame;
}

bool ReadClipSampleAtProjectFrame(
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

void ResampleStereoPcm16Linear(const std::int16_t* src, int srcFrames, std::int16_t* dst, int dstFrames) {
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
