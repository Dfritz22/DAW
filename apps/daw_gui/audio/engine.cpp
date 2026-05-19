#include "engine.h"
#include "audio/engine_utils.h"
#include "core/CoreState.h"
#include "audio/AudioRuntimeState.h"
#include "dsp/chain.h"
#include "dsp/mix.h"
#include "dsp/metronome.h"
#include "dsp/util.h"
#include "engine/clip_render.h"
#include "engine/mix_pipeline.h"
#include "core/automation.h"
#include "core/automation_types.h"
#include "core/timeline.h"

#include <algorithm>
#include <cmath>
#include <cstring>

// Mirrors UiRuntimeState.h (UI headers are app-private; duplicating the
// constant here matches the existing pattern in device_mme.cpp /
// device_wasapi.cpp for kMsgPlaybackFinished).
constexpr UINT kMsgCountInComplete = WM_APP + 3;

// ── Engine function definitions ───────────────────────────────────────────────

bool EngineFillRealtimeBufferLocked(CoreState& core, AudioRuntimeState& audio, std::int16_t* outInterleaved, int frames, bool* reachedEnd) {
    if (core.project.projectSampleRate <= 0) {
        std::fill(outInterleaved, outInterleaved + (frames * 2), static_cast<std::int16_t>(0));
        *reachedEnd = true;
        return false;
    }

    // Phase 24 / Step K2 — Load the published MixSnapshot once per block.
    // The shared_ptr keeps the snapshot alive for the duration of this
    // callback even if the UI thread publishes a newer one mid-block. K2
    // only records the observed generation for diagnostics; K3+ migrate
    // the resolve calls below to read from `mixSnapshot->trackMixes` /
    // `busMixes` instead of CoreState so the lock can be dropped in K5.
    const auto mixSnapshot = audio.mixSnapshotPublisher.Load();
    if (mixSnapshot) {
        audio.lastObservedMixSnapshotGen.store(
            mixSnapshot->generation, std::memory_order_relaxed);
    }
    (void)mixSnapshot;  // keeps the snapshot alive; readers in K3+.

    // Check if count-in has finished and clear the flag. Count-in runs in
    // its OWN time domain (countInFrameCursor); the playback cursor is held
    // at recordStartFrame for the entire count-in so the playhead does not
    // move and captured audio lands exactly at the press position.
    //
    // The flag flip MUST happen on the audio thread for tight timing
    // (next callback must immediately stop emitting count-in clicks). We
    // also post a kMsgCountInComplete to the UI thread so the FSM observes
    // the CountingIn → Recording transition through DispatchTransportEvent
    // rather than only via state derivation. This keeps the FSM the single
    // chokepoint for transport transitions even for engine-driven events.
    // The dispatched StartRecording action is idempotent (StartRecording
    // early-returns when audio.recording is already true, which it is —
    // recording was armed at the start of count-in).
    if (audio.countingIn && audio.countInFrameCursor.load() >= audio.recordPrerollFrames) {
        audio.countingIn = false;
        if (audio.hwnd) {
            PostMessage(audio.hwnd, kMsgCountInComplete, 0, 0);
        }
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

    // Track stage via shared helper. trackScratch is a single reused buffer
    // (engineTrackScratch[0]); the helper zeros it once per track. Sized to
    // activeFrames*2 so DspApplyInsertChain (called from the apply lambda)
    // sees the correct frame count via buf.size().
    if (audio.engineTrackScratch.empty()) {
        audio.engineTrackScratch.resize(1);
    }
    std::vector<float>& trackScratch = audio.engineTrackScratch[0];
    const size_t activeSamples = static_cast<size_t>(activeFrames) * 2;
    if (trackScratch.capacity() < activeSamples) {
        trackScratch.reserve(activeSamples);
    }
    trackScratch.resize(activeSamples);

    // Pre-resolve per-track mix parameters (solo + automation aware).
    if (audio.engineTrackMixScratch.size() < static_cast<size_t>(trackCount)) {
        audio.engineTrackMixScratch.resize(static_cast<size_t>(trackCount));
    }
    // Phase 24 / Step K5c.3 — prefer snapshot trackMixes (K5c.2 builds them
    // via ResolveTrackRealtimeMix, so solo/mute audibility matches the per-
    // block path). Fall back to per-block core read when the snapshot is
    // not sized to match the current track count (eg. mid-publish during a
    // track add/remove, or pre-init transient).
    const bool snapHasTrackMixes = mixSnapshot
        && mixSnapshot->trackMixes.size() == static_cast<size_t>(trackCount);
    if (snapHasTrackMixes) {
        for (int ti = 0; ti < trackCount; ++ti) {
            audio.engineTrackMixScratch[static_cast<size_t>(ti)] =
                mixSnapshot->trackMixes[static_cast<size_t>(ti)];
        }
    } else {
        for (int ti = 0; ti < trackCount; ++ti) {
            audio.engineTrackMixScratch[static_cast<size_t>(ti)] = ResolveTrackRealtimeMix(core, ti);
        }
    }

    daw::engine::MixTracksToBuses(
        audio.engineTrackMixScratch.data(),
        trackCount,
        /*renderTrack*/ [&](int ti, float* dst, int n) {
            if (runCountInClick) return;  // count-in: no clip audio
            // Phase 24 / Step K5b \u2014 prefer snapshot when populated. Snapshot
            // audioSources are shared_ptr<const LoadedAudio> so the realtime
            // thread reads PCM without touching core.project. Snapshot clips
            // are an immutable POD copy from the same publish, so timeline
            // placements and source references agree. Fallback to core for
            // empty/uninitialized snapshots (eg. pre-K2 transient states).
            if (mixSnapshot && !mixSnapshot->audioSources.empty()) {
                daw::engine::RenderClipsForTrack(
                    mixSnapshot->clips,
                    mixSnapshot->audioSources,
                    ti,
                    core.project.projectSampleRate,
                    samplesPerBeat,
                    /*bufferStartFrame*/ startCursor,
                    dst,
                    static_cast<std::uint64_t>(n));
            } else {
                daw::engine::RenderClipsForTrack(
                    core.project.clips,
                    core.project.audio,
                    ti,
                    core.project.projectSampleRate,
                    samplesPerBeat,
                    /*bufferStartFrame*/ startCursor,
                    dst,
                    static_cast<std::uint64_t>(n));
            }
        },
        /*applyTrackInserts*/ [&](int ti, float* buf, int n) {
            // ApplyTrackInsertChain takes a vector& because DspApplyInsertChain
            // derives frame count from buf.size(); trackScratch is already
            // sized to activeFrames*2 so this is correct.
            (void)buf; (void)n;
            // Phase 24 / Step K5b \u2014 read insert configs from snapshot when
            // available. Snapshot trackInserts is sized to match
            // core.project.tracks at publish time; DSP state ownership stays
            // in AudioRuntimeState.trackInsertDspState (keyed by trackIndex).
            if (mixSnapshot && ti < static_cast<int>(mixSnapshot->trackInserts.size())) {
                ApplyTrackInsertChain(
                    mixSnapshot->trackInserts[static_cast<size_t>(ti)],
                    audio, ti, trackScratch, sampleRate);
            } else {
                ApplyTrackInsertChain(core, audio, ti, trackScratch, sampleRate);
            }
        },
        trackScratch.data(),
        busBuf.data(),
        kBusCount,
        activeFrames,
        /*busFilter*/ -1);

    // Bus stage via shared helper. masterScratch pre-zeroed; helper applies
    // bus inserts (caller-provided lambda) on the summed bus, then mixes
    // into master with bus gain+pan from ResolveBusRealtimeMix.
    if (audio.engineMasterScratch.capacity() < frameSamples) {
        audio.engineMasterScratch.reserve(frameSamples);
    }
    audio.engineMasterScratch.resize(frameSamples);
    std::fill(audio.engineMasterScratch.begin(), audio.engineMasterScratch.end(), 0.0f);
    std::vector<float>& masterBuf = audio.engineMasterScratch;

    if (audio.engineBusMixScratch.size() < static_cast<size_t>(kBusCount)) {
        audio.engineBusMixScratch.resize(static_cast<size_t>(kBusCount));
    }
    // Phase 24 / Step K5c.3 — prefer snapshot busMixes. Snapshot builder
    // sizes busMixes to exactly kBusCount, so the bounds check is just
    // defensive against an uninitialized snapshot.
    const bool snapHasBusMixes = mixSnapshot
        && mixSnapshot->busMixes.size() >= static_cast<size_t>(kBusCount);
    if (snapHasBusMixes) {
        for (int b = 0; b < kBusCount; ++b) {
            audio.engineBusMixScratch[static_cast<size_t>(b)] =
                mixSnapshot->busMixes[static_cast<size_t>(b)];
        }
    } else {
        for (int b = 0; b < kBusCount; ++b) {
            audio.engineBusMixScratch[static_cast<size_t>(b)] = ResolveBusRealtimeMix(core, b);
        }
    }

    daw::engine::MixBusesToMaster(
        audio.engineBusMixScratch.data(),
        kBusCount,
        /*applyBusInserts*/ [&](int bi, float* buf, int n) {
            (void)buf; (void)n;
            // Phase 24 / Step K5b \u2014 prefer snapshot bus inserts.
            if (mixSnapshot && bi < static_cast<int>(mixSnapshot->busInserts.size())) {
                ApplyBusInsertChain(
                    mixSnapshot->busInserts[static_cast<size_t>(bi)],
                    audio, bi, busBuf[static_cast<size_t>(bi)], sampleRate);
            } else {
                ApplyBusInsertChain(core, audio, bi, busBuf[static_cast<size_t>(bi)], sampleRate);
            }
        },
        busBuf.data(),
        masterBuf.data(),
        activeFrames);

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

// Renders the full mix to stereo. Mirrors the realtime callback structure
// so offline output matches what the user hears in realtime: per-bus
// accumulator -> bus inserts on summed bus -> mix to master with bus
// gain+pan (master bus skips pan, see ResolveBusRealtimeMix).
bool RenderFullMixToStereoLocked(const CoreState& core, AudioRuntimeState& audio, std::vector<float>* outStereo, int* outSampleRate) {
    if (core.project.projectSampleRate <= 0 || core.project.clips.empty() || core.project.tracks.empty()) {
        return false;
    }

    EnsureInsertDspStateStorage(core, audio);

    const float spb = TimelineSamplesPerBeat(core);
    const std::uint64_t endFrame = ComputeProjectEndFrameLocked(core);
    if (endFrame == 0) return false;

    const size_t stereoSamples = static_cast<size_t>(endFrame) * 2;
    const float sampleRate = static_cast<float>(core.project.projectSampleRate);
    const int trackCount = static_cast<int>(core.project.tracks.size());

    // Per-bus accumulators. Tracks mix in here with track-only gain+pan.
    std::vector<std::vector<float>> busBuf(static_cast<size_t>(kBusCount));
    for (auto& b : busBuf) b.assign(stereoSamples, 0.0f);

    // Pre-resolve per-track and per-bus mix parameters.
    std::vector<daw::engine::TrackMix> trackMixes(static_cast<size_t>(trackCount));
    for (int ti = 0; ti < trackCount; ++ti) {
        trackMixes[static_cast<size_t>(ti)] = ResolveTrackBusMix(core, ti);
    }
    std::vector<daw::engine::BusMix> busMixes(static_cast<size_t>(kBusCount));
    for (int b = 0; b < kBusCount; ++b) {
        busMixes[static_cast<size_t>(b)] = ResolveBusRealtimeMix(core, b);
    }

    // Track stage via shared helper (same code path as realtime).
    std::vector<float> trackScratch(stereoSamples, 0.0f);
    daw::engine::MixTracksToBuses(
        trackMixes.data(),
        trackCount,
        /*renderTrack*/ [&](int ti, float* dst, int n) {
            (void)n;
            daw::engine::RenderClipsForTrack(
                core.project.clips,
                core.project.audio,
                ti,
                core.project.projectSampleRate,
                spb,
                /*bufferStartFrame*/ 0,
                dst,
                endFrame);
        },
        /*applyTrackInserts*/ [&](int ti, float* buf, int n) {
            (void)buf; (void)n;
            ApplyTrackInsertChain(core, audio, ti, trackScratch, sampleRate);
        },
        trackScratch.data(),
        busBuf.data(),
        kBusCount,
        static_cast<int>(endFrame),
        /*busFilter*/ -1);

    // Bus stage via shared helper (same code path as realtime). Bus inserts
    // run on the post-track-insert summed bus signal; bus gain+pan applied
    // ONCE on the summed bus (master skips pan via ResolveBusRealtimeMix).
    std::vector<float> mix(stereoSamples, 0.0f);
    daw::engine::MixBusesToMaster(
        busMixes.data(),
        kBusCount,
        /*applyBusInserts*/ [&](int bi, float* buf, int n) {
            (void)buf; (void)n;
            ApplyBusInsertChain(core, audio, bi, busBuf[static_cast<size_t>(bi)], sampleRate);
        },
        busBuf.data(),
        mix.data(),
        static_cast<int>(endFrame));

    daw::dsp::ClampStereoBuffer(mix.data(), static_cast<int>(mix.size()));

    *outStereo     = std::move(mix);
    *outSampleRate = core.project.projectSampleRate;
    return true;
}

// Renders a single bus's contribution to master as a stereo stem.
// Mirrors the realtime per-bus stage: sum tracks routed to busIndex into
// busBuf (track inserts + track gain+pan), apply bus inserts on the
// summed bus, then output with bus gain+pan via ResolveBusRealtimeMix.
// Must NOT be called with audioStateLock held.
bool RenderBusStemToStereoLocked(const CoreState& core, AudioRuntimeState& audio, int busIndex, std::vector<float>* outStereo, int* outSampleRate) {
    EnsureInsertDspStateStorage(core, audio);

    if (core.project.projectSampleRate <= 0 || core.project.clips.empty() || core.project.tracks.empty()) return false;
    if (busIndex < 0 || busIndex >= kBusCount) return false;

    const float spb = TimelineSamplesPerBeat(core);
    const std::uint64_t endFrame = ComputeProjectEndFrameLocked(core);
    if (endFrame == 0) return false;

    const size_t stereoSamples = static_cast<size_t>(endFrame) * 2;
    const float sampleRate = static_cast<float>(core.project.projectSampleRate);
    const int trackCount = static_cast<int>(core.project.tracks.size());

    std::vector<std::vector<float>> busBuf(static_cast<size_t>(kBusCount));
    for (auto& b : busBuf) b.assign(stereoSamples, 0.0f);
    std::vector<float> trackScratch(stereoSamples, 0.0f);

    std::vector<daw::engine::TrackMix> trackMixes(static_cast<size_t>(trackCount));
    for (int ti = 0; ti < trackCount; ++ti) {
        trackMixes[static_cast<size_t>(ti)] = ResolveTrackBusMix(core, ti);
    }

    // Stem path: filter to one bus via busFilter. Track stage same as full mix.
    daw::engine::MixTracksToBuses(
        trackMixes.data(),
        trackCount,
        /*renderTrack*/ [&](int ti, float* dst, int n) {
            (void)n;
            daw::engine::RenderClipsForTrack(
                core.project.clips,
                core.project.audio,
                ti,
                core.project.projectSampleRate,
                spb,
                /*bufferStartFrame*/ 0,
                dst,
                endFrame);
        },
        /*applyTrackInserts*/ [&](int ti, float* buf, int n) {
            (void)buf; (void)n;
            ApplyTrackInsertChain(core, audio, ti, trackScratch, sampleRate);
        },
        trackScratch.data(),
        busBuf.data(),
        kBusCount,
        static_cast<int>(endFrame),
        /*busFilter*/ busIndex);

    // Apply bus inserts on the summed bus (matches realtime), then output
    // with bus gain+pan. Only the filtered bus has any contribution.
    ApplyBusInsertChain(core, audio, busIndex, busBuf[static_cast<size_t>(busIndex)], sampleRate);

    std::vector<float> mix(stereoSamples, 0.0f);
    const daw::engine::BusMix bm = ResolveBusRealtimeMix(core, busIndex);
    if (bm.active) {
        daw::dsp::MixAddStereoWithGain(busBuf[static_cast<size_t>(busIndex)].data(), mix.data(),
                                       static_cast<int>(endFrame), bm.gainL, bm.gainR);
    }

    daw::dsp::ClampStereoBuffer(mix.data(), static_cast<int>(mix.size()));
    *outStereo     = std::move(mix);
    *outSampleRate = core.project.projectSampleRate;
    return true;
}

