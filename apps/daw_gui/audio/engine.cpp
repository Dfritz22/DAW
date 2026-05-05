#include "engine.h"
#include "../dsp/chain.h"

// ── File-private audio-domain copies of layout helpers ───────────────────────
// engine.cpp must not include ui/layout.h (no dependency on the UI layer).
// These anonymous-namespace functions are exact duplicates of the corresponding
// functions in ui/layout.cpp; they have internal linkage so there is no ODR
// conflict or linker collision.
namespace {

float SamplesPerBeat(const UiState& state) {
    int sr = state.project.projectSampleRate;
    if (sr <= 0) sr = state.activeDeviceSampleRate;
    if (sr <= 0) sr = state.preferredSampleRate;
    if (sr <= 0) sr = 1;
    return static_cast<float>(sr) * 60.0f / state.project.bpm;
}

float TrackGainDbAt(const UiState& state, int trackIndex) {
    if (trackIndex < 0 || trackIndex >= static_cast<int>(state.project.tracks.size())) {
        return 0.0f;
    }
    return state.project.tracks[static_cast<size_t>(trackIndex)].gainDb;
}

int TrackBusIndexAt(const UiState& state, int trackIndex) {
    if (trackIndex < 0 || trackIndex >= static_cast<int>(state.project.tracks.size())) {
        return 1;
    }
    return std::clamp(state.project.tracks[static_cast<size_t>(trackIndex)].busIndex, 0, kBusCount - 1);
}

float TrackPanAt(const UiState& state, int trackIndex) {
    if (trackIndex < 0 || trackIndex >= static_cast<int>(state.project.tracks.size())) {
        return 0.0f;
    }
    return std::clamp(state.project.tracks[static_cast<size_t>(trackIndex)].pan, -1.0f, 1.0f);
}

} // namespace

// ── Engine function definitions ───────────────────────────────────────────────

bool FillRealtimeBufferLocked(UiState& state, std::int16_t* outInterleaved, int frames, bool* reachedEnd) {
    if (state.project.projectSampleRate <= 0) {
        std::fill(outInterleaved, outInterleaved + (frames * 2), 0);
        *reachedEnd = true;
        return false;
    }

    EnsureInsertDspStateStorage(state);

    const bool runMetPlay = state.playing && !state.recording && state.metronomePlay;
    const bool runMetRec = state.recording && state.metronomeRecord;
    // Play count-in click from Record-press until preroll ends (recordStartFrame).
    const bool runCountInClick = state.countingIn
        && state.playbackFrameCursor.load() < state.recordStartFrame;
    const bool allowNoClipPlayback = runMetPlay || runMetRec || runCountInClick || (state.recording && state.inputMonitoring);

    if (state.project.clips.empty() && !allowNoClipPlayback) {
        std::fill(outInterleaved, outInterleaved + (frames * 2), 0);
        *reachedEnd = true;
        return false;
    }

    const float samplesPerBeat = SamplesPerBeat(state);
    const std::uint64_t endFrame = ComputeProjectEndFrameLocked(state);
    const std::uint64_t startCursor = state.playbackFrameCursor.load();
    const float sampleRate = static_cast<float>(state.project.projectSampleRate);

    int activeFrames = 0;
    // Keep running while recording, during count-in, or metronome-only playback.
    if (state.recording || state.countingIn || (state.project.clips.empty() && (runMetPlay || runMetRec))) {
        activeFrames = frames;
        *reachedEnd = false;
    } else {
        activeFrames = static_cast<int>(
            std::min(static_cast<std::uint64_t>(frames), endFrame > startCursor ? endFrame - startCursor : 0u));
        *reachedEnd = (activeFrames < frames);
    }

    // Per-track stereo buffers: collect clips, apply track insert chain
    const int trackCount = static_cast<int>(state.project.tracks.size());

    // Bus stereo accumulation buffers
    std::vector<float> busBuf[kBusCount];
    for (int b = 0; b < kBusCount; ++b)
        busBuf[b].assign(static_cast<size_t>(frames) * 2, 0.0f);

    for (int ti = 0; ti < trackCount; ++ti) {
        if (!IsTrackAudible(state, ti)) continue;

        const int busIdx = std::clamp(TrackBusIndexAt(state, ti), 0, kBusCount - 1);

        // Fill track buffer from clips
        std::vector<float> trackBuf(static_cast<size_t>(activeFrames) * 2, 0.0f);
        for (const ClipItem& clip : state.project.clips) {
            if (clip.trackIndex != ti) continue;
            if (clip.audioIndex < 0 || clip.audioIndex >= static_cast<int>(state.project.audio.size())) continue;
            const LoadedAudio& a = state.project.audio[static_cast<size_t>(clip.audioIndex)];
            const std::uint64_t clipStart = static_cast<std::uint64_t>(
                std::llround(std::max(0.0f, clip.startBeat) * samplesPerBeat));
            const std::uint64_t clipEnd = clipStart + static_cast<std::uint64_t>(
                std::llround(clip.lengthBeats * samplesPerBeat));

            for (int i = 0; i < activeFrames; ++i) {
                const std::uint64_t gf = startCursor + static_cast<std::uint64_t>(i);
                if (gf < clipStart || gf >= clipEnd) continue;
                float l = 0.0f;
                float r = 0.0f;
                if (!ReadClipSampleAtProjectFrame(a, gf - clipStart, state.project.projectSampleRate, clip.sourceOffsetFrames, &l, &r)) {
                    continue;
                }
                trackBuf[static_cast<size_t>(i)*2]   += l;
                trackBuf[static_cast<size_t>(i)*2+1] += r;
            }
        }

        // Apply track insert chain
        if (ti < static_cast<int>(state.project.tracks.size()) &&
            ti < static_cast<int>(state.project.tracks.size()) &&
            ti < static_cast<int>(state.project.tracks.size()) &&
            ti < static_cast<int>(state.project.tracks.size())) {
            ApplyInsertChain(trackBuf, sampleRate,
                state.project.tracks[static_cast<size_t>(ti)].insertEffects,
                state.project.tracks[static_cast<size_t>(ti)].insertBypass,
                state.project.tracks[static_cast<size_t>(ti)].insertConfig,
                state.trackInsertDspState[static_cast<size_t>(ti)],
                state.project.tracks[static_cast<size_t>(ti)].insertSlots);
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
        if (b < static_cast<int>(state.project.buses.size()) &&
            b < static_cast<int>(state.project.buses.size()) &&
            b < static_cast<int>(state.project.buses.size()) &&
            b < static_cast<int>(state.project.buses.size())) {
            ApplyInsertChain(busBuf[b], sampleRate,
                state.project.buses[static_cast<size_t>(b)].insertEffects,
                state.project.buses[static_cast<size_t>(b)].insertBypass,
                state.project.buses[static_cast<size_t>(b)].insertConfig,
                state.busInsertDspState[static_cast<size_t>(b)],
                state.project.buses[static_cast<size_t>(b)].insertSlots);
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

bool FillRealtimeForDeviceLocked(
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

    if (state.project.projectSampleRate <= 0) {
        state.project.projectSampleRate = deviceSampleRate;
    }

    const int projectSampleRate = state.project.projectSampleRate;
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

bool RenderTrackToStereoLocked(const UiState& state, int trackIndex, std::vector<float>* outStereo, int* outSampleRate) {
        EnsureInsertDspStateStorage(state);

    if (trackIndex < 0 || trackIndex >= static_cast<int>(state.project.tracks.size()) || state.project.projectSampleRate <= 0) {
        return false;
    }

    std::uint64_t startFrame = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t endFrame = 0;
    const float spb = SamplesPerBeat(state);

    for (const ClipItem& clip : state.project.clips) {
        if (clip.trackIndex != trackIndex || clip.audioIndex < 0 || clip.audioIndex >= static_cast<int>(state.project.audio.size())) {
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

    for (const ClipItem& clip : state.project.clips) {
        if (clip.trackIndex != trackIndex || clip.audioIndex < 0 || clip.audioIndex >= static_cast<int>(state.project.audio.size())) {
            continue;
        }
        const LoadedAudio& a = state.project.audio[static_cast<size_t>(clip.audioIndex)];
        const std::uint64_t clipStartAbs = static_cast<std::uint64_t>(std::llround(std::max(0.0f, clip.startBeat) * spb));
        if (clipStartAbs < startFrame) {
            continue;
        }

        const std::uint64_t writeStart = clipStartAbs - startFrame;
        const std::uint64_t clipFrames = static_cast<std::uint64_t>(std::llround(clip.lengthBeats * spb));
        for (std::uint64_t f = 0; f < clipFrames && (writeStart + f) < totalFrames; ++f) {
            float l = 0.0f;
            float r = 0.0f;
            if (!ReadClipSampleAtProjectFrame(a, f, state.project.projectSampleRate, clip.sourceOffsetFrames, &l, &r)) {
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
    *outSampleRate = state.project.projectSampleRate;
    return true;
}

// Renders the full mix to stereo: all un-muted tracks summed with track gain,
// track pan, bus gain, and bus mute applied. Bus pan is also applied.
bool RenderFullMixToStereoLocked(const UiState& state, std::vector<float>* outStereo, int* outSampleRate) {
    if (state.project.projectSampleRate <= 0 || state.project.clips.empty() || state.project.tracks.empty()) {
        return false;
    }

    EnsureInsertDspStateStorage(state);

    // Determine total length across all clips
    std::uint64_t endFrame = 0;
    const float spb = SamplesPerBeat(state);
    for (const ClipItem& clip : state.project.clips) {
        if (clip.audioIndex < 0 || clip.audioIndex >= static_cast<int>(state.project.audio.size())) continue;
        const std::uint64_t clipStart = static_cast<std::uint64_t>(std::llround(std::max(0.0f, clip.startBeat) * spb));
        endFrame = std::max(endFrame, clipStart + static_cast<std::uint64_t>(std::llround(clip.lengthBeats * spb)));
    }
    if (endFrame == 0) return false;

    std::vector<float> mix(static_cast<size_t>(endFrame) * 2, 0.0f);

    const int trackCount = static_cast<int>(state.project.tracks.size());
    for (int ti = 0; ti < trackCount; ++ti) {
        // Track mute check
        const bool trackMuted = (ti < static_cast<int>(state.project.tracks.size())) && state.project.tracks[static_cast<size_t>(ti)].mute;
        if (trackMuted) continue;

        // Bus mute check
        const int busIdx = (ti < static_cast<int>(state.project.tracks.size()))
            ? std::clamp(state.project.tracks[static_cast<size_t>(ti)].busIndex, 0, kBusCount - 1) : 0;
        const bool busMuted = (busIdx < static_cast<int>(state.project.buses.size())) && state.project.buses[static_cast<size_t>(busIdx)].mute;
        if (busMuted) continue;

        // Gain: track dB + bus dB
        const float trackDb = (ti < static_cast<int>(state.project.tracks.size()))
            ? state.project.tracks[static_cast<size_t>(ti)].gainDb : 0.0f;
        const float busDb = BusGainDbAt(state, busIdx);
        const float gain = std::pow(10.0f, (trackDb + busDb) / 20.0f);

        // Pan: track pan + bus pan combined (simple additive, clamped)
        const float trackPan = (ti < static_cast<int>(state.project.tracks.size()))
            ? state.project.tracks[static_cast<size_t>(ti)].pan : 0.0f;
        const float busPan = BusPanAt(state, busIdx);
        const float pan = std::clamp(trackPan + busPan, -1.0f, 1.0f);
        // Constant-power panning
        const float panRad = (pan + 1.0f) * 0.5f * 3.14159265f * 0.5f;
        const float gainL = gain * std::cos(panRad);
        const float gainR = gain * std::sin(panRad);

        // Build per-track buffer for DSP
        std::vector<float> trackBuf(static_cast<size_t>(endFrame) * 2, 0.0f);
        for (const ClipItem& clip : state.project.clips) {
            if (clip.trackIndex != ti || clip.audioIndex < 0 || clip.audioIndex >= static_cast<int>(state.project.audio.size())) continue;
            const LoadedAudio& a = state.project.audio[static_cast<size_t>(clip.audioIndex)];
            const std::uint64_t clipStart = static_cast<std::uint64_t>(std::llround(std::max(0.0f, clip.startBeat) * spb));
            const std::uint64_t clipFrames = static_cast<std::uint64_t>(std::llround(clip.lengthBeats * spb));
            for (std::uint64_t f = 0; f < clipFrames && (clipStart + f) < endFrame; ++f) {
                float l = 0.0f;
                float r = 0.0f;
                if (!ReadClipSampleAtProjectFrame(a, f, state.project.projectSampleRate, clip.sourceOffsetFrames, &l, &r)) {
                    continue;
                }
                const size_t dst = static_cast<size_t>(clipStart + f) * 2;
                trackBuf[dst]     += l;
                trackBuf[dst + 1] += r;
            }
        }

        // Apply track insert chain
        if (ti < static_cast<int>(state.project.tracks.size()) &&
            ti < static_cast<int>(state.project.tracks.size()) &&
            ti < static_cast<int>(state.project.tracks.size()) &&
            ti < static_cast<int>(state.project.tracks.size())) {
            ApplyInsertChain(trackBuf, static_cast<float>(state.project.projectSampleRate),
                state.project.tracks[static_cast<size_t>(ti)].insertEffects,
                state.project.tracks[static_cast<size_t>(ti)].insertBypass,
                state.project.tracks[static_cast<size_t>(ti)].insertConfig,
                state.trackInsertDspState[static_cast<size_t>(ti)],
                state.project.tracks[static_cast<size_t>(ti)].insertSlots);
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
        if (b >= static_cast<int>(state.project.buses.size())) continue;
        if (b >= static_cast<int>(state.project.buses.size()))  continue;
        if (b >= static_cast<int>(state.project.buses.size()))  continue;
        if (b >= static_cast<int>(state.project.buses.size()))   continue;
        if (state.project.buses[static_cast<size_t>(b)].insertSlots <= 0)    continue;
        // Collect all tracks on this bus into a sub-mix
        std::vector<float> busBuf(static_cast<size_t>(endFrame) * 2, 0.0f);
        bool hasContent = false;
        for (int ti2 = 0; ti2 < trackCount; ++ti2) {
            const int tbus = (ti2 < static_cast<int>(state.project.tracks.size()))
                ? std::clamp(state.project.tracks[static_cast<size_t>(ti2)].busIndex, 0, kBusCount - 1) : 0;
            if (tbus != b) continue;
            const bool muted = (ti2 < static_cast<int>(state.project.tracks.size())) && state.project.tracks[static_cast<size_t>(ti2)].mute;
            if (muted) continue;
            hasContent = true;
            for (const ClipItem& clip : state.project.clips) {
                if (clip.trackIndex != ti2 || clip.audioIndex < 0 || clip.audioIndex >= static_cast<int>(state.project.audio.size())) continue;
                const LoadedAudio& a2 = state.project.audio[static_cast<size_t>(clip.audioIndex)];
                const std::uint64_t cs = static_cast<std::uint64_t>(std::llround(std::max(0.0f, clip.startBeat) * spb));
                const std::uint64_t cf = static_cast<std::uint64_t>(std::llround(clip.lengthBeats * spb));
                for (std::uint64_t f = 0; f < cf && (cs + f) < endFrame; ++f) {
                    float l = 0.0f;
                    float r = 0.0f;
                    if (!ReadClipSampleAtProjectFrame(a2, f, state.project.projectSampleRate, clip.sourceOffsetFrames, &l, &r)) {
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
        ApplyInsertChain(busBuf, static_cast<float>(state.project.projectSampleRate),
            state.project.buses[static_cast<size_t>(b)].insertEffects,
            state.project.buses[static_cast<size_t>(b)].insertBypass,
            state.project.buses[static_cast<size_t>(b)].insertConfig,
            state.busInsertDspState[static_cast<size_t>(b)],
            state.project.buses[static_cast<size_t>(b)].insertSlots);
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
    *outSampleRate = state.project.projectSampleRate;
    return true;
}

// Renders all tracks routed to busIndex (with track+bus gain/pan applied) into a stereo buffer.
// Must NOT be called with audioStateLock held.
bool RenderBusStemToStereoLocked(const UiState& state, int busIndex, std::vector<float>* outStereo, int* outSampleRate) {
        EnsureInsertDspStateStorage(state);

    if (state.project.projectSampleRate <= 0 || state.project.clips.empty() || state.project.tracks.empty()) return false;

    std::uint64_t endFrame = 0;
    const float spb = SamplesPerBeat(state);
    for (const ClipItem& clip : state.project.clips) {
        if (clip.audioIndex < 0 || clip.audioIndex >= static_cast<int>(state.project.audio.size())) continue;
        const std::uint64_t clipStart = static_cast<std::uint64_t>(std::llround(std::max(0.0f, clip.startBeat) * spb));
        endFrame = std::max(endFrame, clipStart + static_cast<std::uint64_t>(std::llround(clip.lengthBeats * spb)));
    }
    if (endFrame == 0) return false;

    std::vector<float> mix(static_cast<size_t>(endFrame) * 2, 0.0f);
    const int trackCount = static_cast<int>(state.project.tracks.size());

    for (int ti = 0; ti < trackCount; ++ti) {
        const int tbus = (ti < static_cast<int>(state.project.tracks.size()))
            ? std::clamp(state.project.tracks[static_cast<size_t>(ti)].busIndex, 0, kBusCount - 1) : 0;
        if (tbus != busIndex) continue;

        const bool trackMuted = (ti < static_cast<int>(state.project.tracks.size())) && state.project.tracks[static_cast<size_t>(ti)].mute;
        const bool busMuted   = (busIndex < static_cast<int>(state.project.buses.size())) && state.project.buses[static_cast<size_t>(busIndex)].mute;
        if (trackMuted || busMuted) continue;

        const float trackDb = (ti < static_cast<int>(state.project.tracks.size())) ? state.project.tracks[static_cast<size_t>(ti)].gainDb : 0.0f;
        const float busDb   = BusGainDbAt(state, busIndex);
        const float gain    = std::pow(10.0f, (trackDb + busDb) / 20.0f);

        const float trackPan = (ti < static_cast<int>(state.project.tracks.size())) ? state.project.tracks[static_cast<size_t>(ti)].pan : 0.0f;
        const float busPan   = BusPanAt(state, busIndex);
        const float pan      = std::clamp(trackPan + busPan, -1.0f, 1.0f);
        const float panRad   = (pan + 1.0f) * 0.5f * 3.14159265f * 0.5f;
        const float gainL    = gain * std::cos(panRad);
        const float gainR    = gain * std::sin(panRad);

        // Build per-track buffer for DSP
        std::vector<float> trackBuf(static_cast<size_t>(endFrame) * 2, 0.0f);
        for (const ClipItem& clip : state.project.clips) {
            if (clip.trackIndex != ti || clip.audioIndex < 0 || clip.audioIndex >= static_cast<int>(state.project.audio.size())) continue;
            const LoadedAudio& a = state.project.audio[static_cast<size_t>(clip.audioIndex)];
            const std::uint64_t clipStart = static_cast<std::uint64_t>(std::llround(std::max(0.0f, clip.startBeat) * spb));
            const std::uint64_t clipFrames = static_cast<std::uint64_t>(std::llround(clip.lengthBeats * spb));
            for (std::uint64_t f = 0; f < clipFrames && (clipStart + f) < endFrame; ++f) {
                float l = 0.0f;
                float r = 0.0f;
                if (!ReadClipSampleAtProjectFrame(a, f, state.project.projectSampleRate, clip.sourceOffsetFrames, &l, &r)) {
                    continue;
                }
                const size_t dst = static_cast<size_t>(clipStart + f) * 2;
                trackBuf[dst]   += l;
                trackBuf[dst+1] += r;
            }
        }

        // Apply track insert chain
        if (ti < static_cast<int>(state.project.tracks.size()) &&
            ti < static_cast<int>(state.project.tracks.size()) &&
            ti < static_cast<int>(state.project.tracks.size()) &&
            ti < static_cast<int>(state.project.tracks.size())) {
            ApplyInsertChain(trackBuf, static_cast<float>(state.project.projectSampleRate),
                state.project.tracks[static_cast<size_t>(ti)].insertEffects,
                state.project.tracks[static_cast<size_t>(ti)].insertBypass,
                state.project.tracks[static_cast<size_t>(ti)].insertConfig,
                state.trackInsertDspState[static_cast<size_t>(ti)],
                state.project.tracks[static_cast<size_t>(ti)].insertSlots);
        }

        // Apply bus insert chain as post-fader processing
        if (busIndex < static_cast<int>(state.project.buses.size()) &&
            busIndex < static_cast<int>(state.project.buses.size()) &&
            busIndex < static_cast<int>(state.project.buses.size()) &&
            busIndex < static_cast<int>(state.project.buses.size()) &&
            state.project.buses[static_cast<size_t>(busIndex)].insertSlots > 0) {
            ApplyInsertChain(trackBuf, static_cast<float>(state.project.projectSampleRate),
                state.project.buses[static_cast<size_t>(busIndex)].insertEffects,
                state.project.buses[static_cast<size_t>(busIndex)].insertBypass,
                state.project.buses[static_cast<size_t>(busIndex)].insertConfig,
                state.busInsertDspState[static_cast<size_t>(busIndex)],
                state.project.buses[static_cast<size_t>(busIndex)].insertSlots);
        }

        for (std::uint64_t f = 0; f < endFrame; ++f) {
            const size_t i = f*2;
            mix[i]   += trackBuf[i]   * gainL;
            mix[i+1] += trackBuf[i+1] * gainR;
        }
    }
    for (float& s : mix) s = std::clamp(s, -1.0f, 1.0f);
    *outStereo     = std::move(mix);
    *outSampleRate = state.project.projectSampleRate;
    return true;
}
