#include "engine.h"
#include "audio/engine_utils.h"
#include "core/CoreState.h"
#include "audio/AudioRuntimeState.h"
#include "dsp/chain.h"
#include "dsp/mix.h"
#include "dsp/metronome.h"
#include "engine/clip_render.h"
#include "core/automation.h"
#include "core/automation_types.h"
#include "core/timeline.h"

#include <algorithm>
#include <cmath>
#include <cstring>

// ── Engine function definitions ───────────────────────────────────────────────

bool EngineFillRealtimeBufferLocked(CoreState& core, AudioRuntimeState& audio, std::int16_t* outInterleaved, int frames, bool* reachedEnd) {
    if (core.project.projectSampleRate <= 0) {
        std::fill(outInterleaved, outInterleaved + (frames * 2), static_cast<std::int16_t>(0));
        *reachedEnd = true;
        return false;
    }

    // Check if count-in has finished and clear the flag. Count-in runs in
    // its OWN time domain (countInFrameCursor); the playback cursor is held
    // at recordStartFrame for the entire count-in so the playhead does not
    // move and captured audio lands exactly at the press position.
    if (audio.countingIn && audio.countInFrameCursor.load() >= audio.recordPrerollFrames) {
        audio.countingIn = false;
    }

    EnsureInsertDspStateStorage(core, audio);

    const bool runMetPlay = audio.playing && !audio.recording && audio.metronomePlay;
    const bool runMetRec = audio.recording && audio.metronomeRecord;
    // Play count-in clicks while in the preroll window (driven by the
    // count-in cursor, not the playback cursor).
    const bool runCountInClick = audio.countingIn
        && audio.countInFrameCursor.load() < audio.recordPrerollFrames;
    // Keep the render path alive whenever we are recording or counting in,
    // even if there are no clips, no metronome, and no monitoring. Without
    // this, an empty-project record session with both metronomes off would
    // immediately return reachedEnd=true, which the playback thread interprets
    // as "song over" and tears down recording. Recording must own the render
    // loop until the user explicitly stops.
    const bool allowNoClipPlayback = runMetPlay || runMetRec || runCountInClick
        || audio.recording || audio.countingIn;

    if (core.project.clips.empty() && !allowNoClipPlayback) {
        std::fill(outInterleaved, outInterleaved + (frames * 2), static_cast<std::int16_t>(0));
        *reachedEnd = true;
        return false;
    }

    const float samplesPerBeat = TimelineSamplesPerBeat(core);
    const std::uint64_t endFrame = ComputeProjectEndFrameLocked(core);
    const std::uint64_t startCursor = audio.playbackFrameCursor.load();
    const float sampleRate = static_cast<float>(core.project.projectSampleRate);

    int activeFrames = 0;
    // Keep running while recording, during count-in, or metronome-only playback.
    if (audio.recording || audio.countingIn || (core.project.clips.empty() && (runMetPlay || runMetRec))) {
        activeFrames = frames;
        *reachedEnd = false;
    } else {
        activeFrames = static_cast<int>(
            std::min(static_cast<std::uint64_t>(frames), endFrame > startCursor ? endFrame - startCursor : 0u));
        *reachedEnd = (activeFrames < frames);
    }

    // Per-track stereo buffers: collect clips, apply track insert chain
    const int trackCount = static_cast<int>(core.project.tracks.size());

    // Bus stereo accumulation buffers — reuse pre-allocated scratch from
    // AudioRuntimeState. Grow capacity on demand only; never shrink. Steady-
    // state callbacks now perform ZERO heap allocations here. Without this
    // every callback was constructing kBusCount + (1 per track) + 1 master
    // std::vector<float>; over minutes of playback the heap fragmented and
    // small-buffer (64-frame) deadlines started getting missed.
    const size_t frameSamples = static_cast<size_t>(frames) * 2;
    for (int b = 0; b < kBusCount; ++b) {
        if (audio.engineBusScratch[b].capacity() < frameSamples) {
            audio.engineBusScratch[b].reserve(frameSamples);
        }
        // Resize to EXACTLY frameSamples each callback. DspApplyInsertChain
        // computes its frame count from buf.size(); if we left the vector at
        // the largest-ever-seen size, smaller callbacks would feed stale
        // tail samples into delay/reverb/compressor state, producing the
        // "horrible sound" reported after AutoMix added effects.
        audio.engineBusScratch[b].resize(frameSamples);
        std::fill(audio.engineBusScratch[b].begin(), audio.engineBusScratch[b].end(), 0.0f);
    }
    auto& busBuf = audio.engineBusScratch;

    if (static_cast<int>(audio.engineTrackScratch.size()) < trackCount) {
        audio.engineTrackScratch.resize(static_cast<size_t>(trackCount));
    }

    for (int ti = 0; ti < trackCount; ++ti) {
        if (!IsTrackAudible(core, ti)) continue;

        const int busIdx = std::clamp(AutomationTrackBusIndexAt(core, ti), 0, kBusCount - 1);

        // Reusable per-track buffer. Resize to exactly activeSamples each
        // call (vector::resize down doesn't free) so DspApplyInsertChain's
        // buf.size()-derived frame count matches activeFrames.
        std::vector<float>& trackBuf = audio.engineTrackScratch[static_cast<size_t>(ti)];
        const size_t activeSamples = static_cast<size_t>(activeFrames) * 2;
        if (trackBuf.capacity() < activeSamples) {
            trackBuf.reserve(activeSamples);
        }
        trackBuf.resize(activeSamples);
        std::fill(trackBuf.begin(), trackBuf.end(), 0.0f);

        if (!runCountInClick) {
            daw::engine::RenderClipsForTrack(
                core.project.clips,
                core.project.audio,
                ti,
                core.project.projectSampleRate,
                samplesPerBeat,
                /*bufferStartFrame*/ startCursor,
                trackBuf.data(),
                static_cast<std::uint64_t>(activeFrames));
        }

        // Apply track insert chain
        ApplyTrackInsertChain(core, audio, ti, trackBuf, sampleRate);

        // Apply track gain + pan then mix into bus
        const float trackGain = DbToLinear(AutomationTrackGainDbAt(core, ti));
        const float pan = AutomationTrackPanAt(core, ti);
        float panL = 0.0f, panR = 0.0f;
        daw::dsp::EqualPowerPan(pan, &panL, &panR);
        const float gainL = trackGain * panL;
        const float gainR = trackGain * panR;
        daw::dsp::MixAddStereoWithGain(trackBuf.data(), busBuf[busIdx].data(), activeFrames, gainL, gainR);
    }

    // Apply bus insert chains, then mix into master
    if (audio.engineMasterScratch.capacity() < frameSamples) {
        audio.engineMasterScratch.reserve(frameSamples);
    }
    audio.engineMasterScratch.resize(frameSamples);
    std::fill(audio.engineMasterScratch.begin(), audio.engineMasterScratch.end(), 0.0f);
    std::vector<float>& masterBuf = audio.engineMasterScratch;
    for (int b = 0; b < kBusCount; ++b) {
        if (BusMuteAt(core, b)) continue;

        // Apply bus insert chain (bus 3 = master)
        ApplyBusInsertChain(core, audio, b, busBuf[b], sampleRate);

        const float busGain = DbToLinear(BusGainDbAt(core, b));
        const float busPan  = BusPanAt(core, b);
        float bPanL = 0.0f, bPanR = 0.0f;
        daw::dsp::EqualPowerPan(busPan, &bPanL, &bPanR);
        const float bGainL  = (b == 3) ? busGain : busGain * bPanL;
        const float bGainR  = (b == 3) ? busGain : busGain * bPanR;
        daw::dsp::MixAddStereoWithGain(busBuf[b].data(), masterBuf.data(), activeFrames, bGainL, bGainR);
    }

    // Input monitor path for low-latency tracking.
    if (audio.recording && audio.inputMonitoring && audio.recordInputChannels > 0) {
        const int inCh = std::max(1, audio.recordInputChannels);
        const size_t availableSamples = (audio.monitorInputPcm.size() > audio.monitorInputReadPos)
            ? (audio.monitorInputPcm.size() - audio.monitorInputReadPos)
            : 0;
        const int availableFrames = static_cast<int>(availableSamples / static_cast<size_t>(inCh));
        const int requestFrames = std::min(activeFrames, availableFrames);
        const float monGain = std::clamp(audio.inputMonitorGain, 0.0f, 2.0f);
        const int mixFrames = daw::dsp::MixPcm16InputToFloatStereo(
            audio.monitorInputPcm.data() + audio.monitorInputReadPos,
            requestFrames, inCh,
            masterBuf.data(), requestFrames,
            monGain);
        audio.monitorInputReadPos += static_cast<size_t>(mixFrames * inCh);
        if (audio.monitorInputReadPos > 16384 && audio.monitorInputReadPos * 2 > audio.monitorInputPcm.size()) {
            audio.monitorInputPcm.erase(
                audio.monitorInputPcm.begin(),
                audio.monitorInputPcm.begin() + static_cast<std::vector<std::int16_t>::difference_type>(audio.monitorInputReadPos));
            audio.monitorInputReadPos = 0;
        }
    }

    // Metronome click (4/4, accented downbeat).
    if (runMetPlay || runMetRec || runCountInClick) {
        // During count-in, drive clicks from the count-in cursor (starts at 0)
        // so we always get N evenly-spaced clicks regardless of where the
        // playhead is parked. During normal play/record, drive from the
        // playback cursor so clicks align with the bar grid.
        const std::uint64_t clickBase = runCountInClick
            ? audio.countInFrameCursor.load()
            : startCursor;
        daw::dsp::RenderMetronomeClicks(
            masterBuf.data(), activeFrames, sampleRate, samplesPerBeat, clickBase, 3.0f);
    }

    // Convert to int16
    daw::dsp::FloatToPcm16Clamped(masterBuf.data(), outInterleaved, frames * 2);

    // Cursor advance:
    //   - During count-in: hold the playback cursor steady at recordStartFrame
    //     and advance the count-in cursor instead. This keeps the playhead
    //     parked while the clicks play and ensures captured audio (which the
    //     record thread starts writing the moment countingIn flips false)
    //     lands at the press position.
    //   - Otherwise: advance the playback cursor by the rendered frame count.
    if (audio.countingIn) {
        audio.countInFrameCursor.fetch_add(static_cast<std::uint64_t>(frames));
    } else {
        audio.playbackFrameCursor.store(startCursor + static_cast<std::uint64_t>(frames));
    }
    return true;
}

bool EngineFillRealtimeForDeviceLocked(
    CoreState& core,
    AudioRuntimeState& audio,
    std::int16_t* outInterleaved,
    int deviceFrames,
    int deviceSampleRate,
    bool* reachedEnd)
{
    if (outInterleaved == nullptr || reachedEnd == nullptr || deviceFrames <= 0 || deviceSampleRate <= 0) {
        if (reachedEnd != nullptr) *reachedEnd = true;
        return false;
    }

    // The realtime audio thread MUST NOT mutate project state. If the project
    // sample rate is unset for some reason, just bail with silence rather than
    // silently retuning the user's project to whatever the device happens to be.
    const int projectSampleRate = core.project.projectSampleRate;
    if (projectSampleRate <= 0) {
        std::memset(outInterleaved, 0, static_cast<size_t>(deviceFrames) * 2 * sizeof(std::int16_t));
        *reachedEnd = false;
        return false;
    }
    if (projectSampleRate == deviceSampleRate) {
        return EngineFillRealtimeBufferLocked(core, audio, outInterleaved, deviceFrames, reachedEnd);
    }

    // Stateful linear resampler: maintains a fractional phase and a one-frame
    // carry across callbacks so the output stream is continuous (no per-buffer
    // boundary clicks). We over-fetch source frames by a small margin, then
    // ask the resampler exactly how many it consumed and rewind the engine's
    // playback cursor by the unused remainder so the next callback resumes
    // sample-accurately.
    const double step = static_cast<double>(projectSampleRate) / static_cast<double>(deviceSampleRate);
    const int projectFramesNeeded = std::max(2, static_cast<int>(std::ceil(static_cast<double>(deviceFrames) * step)) + 2);
    const size_t neededSamples = static_cast<size_t>(projectFramesNeeded) * 2;
    if (audio.engineSrcScratchPcm.size() < neededSamples) {
        audio.engineSrcScratchPcm.resize(neededSamples);
    }
    std::int16_t* projectPcm = audio.engineSrcScratchPcm.data();
    std::memset(projectPcm, 0, neededSamples * sizeof(std::int16_t));

    // Snapshot the cursor that EngineFillRealtimeBufferLocked will advance so
    // we can rewind the unused portion after we know how many frames were
    // actually consumed by the resampler.
    const bool wasCountingIn = audio.countingIn;
    const std::uint64_t cursorBefore = wasCountingIn
        ? audio.countInFrameCursor.load()
        : audio.playbackFrameCursor.load();

    bool localReachedEnd = false;
    EngineFillRealtimeBufferLocked(core, audio, projectPcm, projectFramesNeeded, &localReachedEnd);

    const int consumed = ResampleStereoPcm16LinearStateful(
        projectPcm, projectFramesNeeded,
        outInterleaved, deviceFrames,
        step,
        &audio.engineSrcPhase,
        &audio.engineSrcLastL, &audio.engineSrcLastR,
        &audio.engineSrcPrimed);

    // Rewind unused source frames so the next callback continues exactly where
    // the resampler left off. Without this, every callback silently discards
    // (projectFramesNeeded - consumed) project frames, producing constant
    // sample-skipping artifacts.
    if (consumed > 0 && consumed < projectFramesNeeded) {
        const std::uint64_t newCursor = cursorBefore + static_cast<std::uint64_t>(consumed);
        if (wasCountingIn) {
            audio.countInFrameCursor.store(newCursor);
        } else {
            audio.playbackFrameCursor.store(newCursor);
        }
    }

    *reachedEnd = localReachedEnd;
    return true;
}

bool RenderTrackToStereoLocked(const CoreState& core, AudioRuntimeState& audio, int trackIndex, std::vector<float>* outStereo, int* outSampleRate) {
        EnsureInsertDspStateStorage(core, audio);

    if (trackIndex < 0 || trackIndex >= static_cast<int>(core.project.tracks.size()) || core.project.projectSampleRate <= 0) {
        return false;
    }

    std::uint64_t startFrame = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t endFrame = 0;
    const float spb = TimelineSamplesPerBeat(core);

    for (const ClipItem& clip : core.project.clips) {
        if (clip.trackIndex != trackIndex || clip.audioIndex < 0 || clip.audioIndex >= static_cast<int>(core.project.audio.size())) {
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

    daw::engine::RenderClipsForTrack(
        core.project.clips,
        core.project.audio,
        trackIndex,
        core.project.projectSampleRate,
        spb,
        /*bufferStartFrame*/ startFrame,
        stereo.data(),
        totalFrames);

    daw::dsp::ClampStereoBuffer(stereo.data(), static_cast<int>(stereo.size()));

    *outStereo = std::move(stereo);
    *outSampleRate = core.project.projectSampleRate;
    return true;
}

// Renders the full mix to stereo: all un-muted tracks summed with track gain,
// track pan, bus gain, and bus mute applied. Bus pan is also applied.
bool RenderFullMixToStereoLocked(const CoreState& core, AudioRuntimeState& audio, std::vector<float>* outStereo, int* outSampleRate) {
    if (core.project.projectSampleRate <= 0 || core.project.clips.empty() || core.project.tracks.empty()) {
        return false;
    }

    EnsureInsertDspStateStorage(core, audio);

    // Determine total length across all clips
    const float spb = TimelineSamplesPerBeat(core);
    const std::uint64_t endFrame = ComputeProjectEndFrameLocked(core);
    if (endFrame == 0) return false;

    std::vector<float> mix(static_cast<size_t>(endFrame) * 2, 0.0f);

    const int trackCount = static_cast<int>(core.project.tracks.size());
    for (int ti = 0; ti < trackCount; ++ti) {
        // Track mute check
        const bool trackMuted = (ti < static_cast<int>(core.project.tracks.size())) && core.project.tracks[static_cast<size_t>(ti)].mute;
        if (trackMuted) continue;

        // Bus mute check
        const int busIdx = (ti < static_cast<int>(core.project.tracks.size()))
            ? std::clamp(core.project.tracks[static_cast<size_t>(ti)].busIndex, 0, kBusCount - 1) : 0;
        const bool busMuted = (busIdx < static_cast<int>(core.project.buses.size())) && core.project.buses[static_cast<size_t>(busIdx)].mute;
        if (busMuted) continue;

        // Gain: track dB + bus dB
        const float trackDb = (ti < static_cast<int>(core.project.tracks.size()))
            ? core.project.tracks[static_cast<size_t>(ti)].gainDb : 0.0f;
        const float busDb = BusGainDbAt(core, busIdx);
        const float gain = std::pow(10.0f, (trackDb + busDb) / 20.0f);

        // Pan: track pan + bus pan combined (simple additive, clamped)
        const float trackPan = (ti < static_cast<int>(core.project.tracks.size()))
            ? core.project.tracks[static_cast<size_t>(ti)].pan : 0.0f;
        const float busPan = BusPanAt(core, busIdx);
        const float pan = std::clamp(trackPan + busPan, -1.0f, 1.0f);
        // Constant-power panning
        float panL = 0.0f, panR = 0.0f;
        daw::dsp::EqualPowerPan(pan, &panL, &panR);
        const float gainL = gain * panL;
        const float gainR = gain * panR;

        // Build per-track buffer for DSP
        std::vector<float> trackBuf(static_cast<size_t>(endFrame) * 2, 0.0f);
        daw::engine::RenderClipsForTrack(
            core.project.clips,
            core.project.audio,
            ti,
            core.project.projectSampleRate,
            spb,
            /*bufferStartFrame*/ 0,
            trackBuf.data(),
            endFrame);

        // Apply track insert chain
        ApplyTrackInsertChain(core, audio, ti, trackBuf, static_cast<float>(core.project.projectSampleRate));

        // Mix into master with gain+pan
        daw::dsp::MixAddStereoWithGain(trackBuf.data(), mix.data(), static_cast<int>(endFrame), gainL, gainR);
    }

    // Apply bus insert chains per bus
    for (int b = 0; b < kBusCount; ++b) {
        if (b >= static_cast<int>(core.project.buses.size())) continue;
        if (b >= static_cast<int>(core.project.buses.size()))  continue;
        if (b >= static_cast<int>(core.project.buses.size()))  continue;
        if (b >= static_cast<int>(core.project.buses.size()))   continue;
        if (core.project.buses[static_cast<size_t>(b)].insertSlots <= 0)    continue;
        // Collect all tracks on this bus into a sub-mix
        std::vector<float> busBuf(static_cast<size_t>(endFrame) * 2, 0.0f);
        bool hasContent = false;
        for (int ti2 = 0; ti2 < trackCount; ++ti2) {
            const int tbus = (ti2 < static_cast<int>(core.project.tracks.size()))
                ? std::clamp(core.project.tracks[static_cast<size_t>(ti2)].busIndex, 0, kBusCount - 1) : 0;
            if (tbus != b) continue;
            const bool muted = (ti2 < static_cast<int>(core.project.tracks.size())) && core.project.tracks[static_cast<size_t>(ti2)].mute;
            if (muted) continue;
            hasContent = true;
            daw::engine::RenderClipsForTrack(
                core.project.clips,
                core.project.audio,
                ti2,
                core.project.projectSampleRate,
                spb,
                /*bufferStartFrame*/ 0,
                busBuf.data(),
                endFrame);
        }
        if (!hasContent) continue;
        // Apply bus inserts — result is a differential that we add back to mix
        // (subtract unprocessed, add processed)
        std::vector<float> busBufPre = busBuf;
        ApplyBusInsertChain(core, audio, b, busBuf, static_cast<float>(core.project.projectSampleRate));
        const float busGainLin = std::pow(10.0f, BusGainDbAt(core, b) / 20.0f);
        daw::dsp::MixDifferentialAddWithGain(
            busBufPre.data(), busBuf.data(), mix.data(),
            static_cast<int>(endFrame) * 2, busGainLin);
    }

    // Soft clip / clamp
    daw::dsp::ClampStereoBuffer(mix.data(), static_cast<int>(mix.size()));

    *outStereo     = std::move(mix);
    *outSampleRate = core.project.projectSampleRate;
    return true;
}

// Renders all tracks routed to busIndex (with track+bus gain/pan applied) into a stereo buffer.
// Must NOT be called with audioStateLock held.
bool RenderBusStemToStereoLocked(const CoreState& core, AudioRuntimeState& audio, int busIndex, std::vector<float>* outStereo, int* outSampleRate) {
        EnsureInsertDspStateStorage(core, audio);

    if (core.project.projectSampleRate <= 0 || core.project.clips.empty() || core.project.tracks.empty()) return false;

    const float spb = TimelineSamplesPerBeat(core);
    const std::uint64_t endFrame = ComputeProjectEndFrameLocked(core);
    if (endFrame == 0) return false;

    std::vector<float> mix(static_cast<size_t>(endFrame) * 2, 0.0f);
    const int trackCount = static_cast<int>(core.project.tracks.size());

    for (int ti = 0; ti < trackCount; ++ti) {
        const int tbus = (ti < static_cast<int>(core.project.tracks.size()))
            ? std::clamp(core.project.tracks[static_cast<size_t>(ti)].busIndex, 0, kBusCount - 1) : 0;
        if (tbus != busIndex) continue;

        const bool trackMuted = (ti < static_cast<int>(core.project.tracks.size())) && core.project.tracks[static_cast<size_t>(ti)].mute;
        const bool busMuted   = (busIndex < static_cast<int>(core.project.buses.size())) && core.project.buses[static_cast<size_t>(busIndex)].mute;
        if (trackMuted || busMuted) continue;

        const float trackDb = (ti < static_cast<int>(core.project.tracks.size())) ? core.project.tracks[static_cast<size_t>(ti)].gainDb : 0.0f;
        const float busDb   = BusGainDbAt(core, busIndex);
        const float gain    = std::pow(10.0f, (trackDb + busDb) / 20.0f);

        const float trackPan = (ti < static_cast<int>(core.project.tracks.size())) ? core.project.tracks[static_cast<size_t>(ti)].pan : 0.0f;
        const float busPan   = BusPanAt(core, busIndex);
        const float pan      = std::clamp(trackPan + busPan, -1.0f, 1.0f);
        float panL = 0.0f, panR = 0.0f;
        daw::dsp::EqualPowerPan(pan, &panL, &panR);
        const float gainL    = gain * panL;
        const float gainR    = gain * panR;

        // Build per-track buffer for DSP
        std::vector<float> trackBuf(static_cast<size_t>(endFrame) * 2, 0.0f);
        daw::engine::RenderClipsForTrack(
            core.project.clips,
            core.project.audio,
            ti,
            core.project.projectSampleRate,
            spb,
            /*bufferStartFrame*/ 0,
            trackBuf.data(),
            endFrame);

        // Apply track insert chain
        ApplyTrackInsertChain(core, audio, ti, trackBuf, static_cast<float>(core.project.projectSampleRate));

        // Apply bus insert chain as post-fader processing
        ApplyBusInsertChain(core, audio, busIndex, trackBuf, static_cast<float>(core.project.projectSampleRate));

        daw::dsp::MixAddStereoWithGain(trackBuf.data(), mix.data(), static_cast<int>(endFrame), gainL, gainR);
    }
    daw::dsp::ClampStereoBuffer(mix.data(), static_cast<int>(mix.size()));
    *outStereo     = std::move(mix);
    *outSampleRate = core.project.projectSampleRate;
    return true;
}

