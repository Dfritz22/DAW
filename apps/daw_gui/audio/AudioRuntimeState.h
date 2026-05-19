#pragma once

#ifdef DAW_PUBLIC_API
#error "This header is internal-only."
#endif

#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <Functiondiscoverykeys_devpkey.h>
#include <ks.h>
#include <ksmedia.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "core/CoreState.h"
#include "dsp/insert_types.h"
#include "engine/mix_pipeline.h"
#include "engine/mix_snapshot.h"

constexpr int kAudioBufferFrames = 256;
constexpr int kAudioBufferCount = 4;
constexpr int kRecordBufferFrames = 256;
constexpr int kRecordBufferCount = 4;

enum class AudioBackend {
    MME,
    WasapiShared,
    WasapiExclusive,
    Asio,
};

inline bool IsWasapiBackend(AudioBackend backend) {
    return backend == AudioBackend::WasapiShared || backend == AudioBackend::WasapiExclusive;
}

// High-level audio engine lifecycle state. Used as a single source of truth
// for "is the engine ready to accept transport commands?". Set by
// AudioInitializeRuntime(); read by transport entry points.
enum class AudioEngineState {
    Uninitialized = 0,  // Pre-init. Devices not enumerated, SR unknown.
    Ready,              // Devices enumerated, SR known. Safe to Play/Record.
    Running,            // A backend is currently rendering or capturing.
    Error,              // Init or runtime error. Transport should refuse to start.
};

struct AudioRuntimeState {
    HWND hwnd {nullptr};
    CoreState* coreContext {nullptr};

    std::atomic<AudioEngineState> engineState {AudioEngineState::Uninitialized};
    std::wstring engineInitError;

    bool playing {false};
    bool recording {false};
    float playbackEndBeat {0.0f};
    ULONGLONG playbackStartTick {0};
    float playbackStartBeat {0.0f};

    HWAVEOUT waveOut {nullptr};
    WAVEFORMATEX waveFormat {};
    std::vector<WAVEHDR> waveHeaders;
    std::vector<std::vector<std::int16_t>> waveData;
    HANDLE audioThread {nullptr};
    std::atomic<bool> audioStopRequested {false};
    std::atomic<bool> audioThreadRunning {false};

    // WASAPI output state
    bool playingViaWasapi {false};
    WAVEFORMATEX wasapiOutFormat {};
    std::atomic<int> wasapiOutInitState {0};

    HANDLE automixThread {nullptr};
    std::atomic<bool> automixRunning {false};

    std::atomic<std::uint64_t> playbackFrameCursor {0};
    CRITICAL_SECTION audioStateLock {};

    // Phase 23 / Step J — audio-thread non-blocking guarantee.
    // Counts the number of times an audio render callback failed to acquire
    // `audioStateLock` via TryEnterCriticalSection and had to emit silence
    // instead of blocking. Steady-state value should be 0; a non-zero value
    // indicates UI or worker-thread contention on the audio path and is the
    // canonical signal that a hot-path callee (project save, AutoMix, future
    // plugin GUI) is holding the lock too long.
    // Read by diagnostic UI; written only by audio threads.
    std::atomic<std::uint64_t> audioCallbackLockMisses {0};

    // Phase 24 / Step K1 — lock-free mix-parameter snapshot publisher.
    // UI / control thread publishes a new immutable snapshot on every
    // mix-affecting mutation; the audio callback will Load() it at the top
    // of each block (wiring lands in K2). Today the publisher is allocated
    // and reachable but unused; this lets later K phases migrate data into
    // the snapshot one call site at a time without further AudioRuntimeState
    // surface changes. The publisher is non-copyable / non-movable, so it
    // forces AudioRuntimeState to be likewise (already the case via
    // CRITICAL_SECTION).
    daw::engine::MixSnapshotPublisher mixSnapshotPublisher;

    // Realtime-thread scratch buffer reused by the engine when the device
    // sample rate differs from the project sample rate. Owning it here keeps
    // the realtime callback heap-allocation-free.
    std::vector<std::int16_t> engineSrcScratchPcm;

    // Stateful linear resampler state for the engine SRC path. `engineSrcPhase`
    // is the fractional source-sample position in [0, 1). `engineSrcLastL/R`
    // hold the last input frame from the previous callback so we can
    // interpolate continuously across callback boundaries (otherwise every
    // buffer edge produces an audible click). `engineSrcPrimed` indicates the
    // last-frame snapshot is valid; reset on transport start/seek/SR change.
    double engineSrcPhase {0.0};
    std::int16_t engineSrcLastL {0};
    std::int16_t engineSrcLastR {0};
    bool engineSrcPrimed {false};

    AudioBackend audioBackend {AudioBackend::WasapiShared};
    int preferredSampleRate {0};
    int preferredBufferFrames {kAudioBufferFrames};
    int activeDeviceSampleRate {0};
    int activeDeviceBufferFrames {0};

    // Last (projectSR, deviceSR) pair the user explicitly acknowledged in the
    // SR-mismatch dialog. Used to suppress the dialog on every Play once the
    // user has accepted that real-time SRC will run. Reset whenever either SR
    // changes so a new mismatch always re-prompts.
    int acknowledgedMismatchProjectSR {0};
    int acknowledgedMismatchDeviceSR {0};

    std::vector<UINT> inputDeviceIds;
    std::vector<std::wstring> inputDeviceNames;
    UINT selectedInputDeviceId {WAVE_MAPPER};
    std::wstring selectedInputDeviceName {L"Default Input"};
    std::vector<UINT> outputDeviceIds;
    std::vector<std::wstring> outputDeviceNames;
    UINT selectedOutputDeviceId {WAVE_MAPPER};
    std::wstring selectedOutputDeviceName {L"Default Output"};

    HWAVEIN waveIn {nullptr};
    WAVEFORMATEX waveInFormat {};
    std::vector<WAVEHDR> waveInHeaders;
    std::vector<std::vector<std::int16_t>> waveInData;
    std::vector<std::int16_t> recordedInputPcm;
    HANDLE recordThread {nullptr};
    std::atomic<bool> recordStopRequested {false};
    int recordInputChannels {0};
    std::uint64_t recordStartFrame {0};
    int recordTrackIndex {-1};
    ULONGLONG recordCaptureStartTickMs {0};

    // Tracking workflow toggles/state
    bool metronomePlay {false};
    bool metronomeRecord {true};
    bool inputMonitoring {true};
    float inputMonitorGain {0.85f};
    bool countInEnabled {true};
    int countInBars {1};
    std::uint64_t recordPrerollFrames {0};
    std::uint64_t countInEndFrame {0};  // When count-in clicks stop (absolute frame position)
    bool countingIn {false};
    // Free-time cursor for count-in. Starts at 0, advances by `frames` each
    // engine callback while countingIn is true. When it reaches
    // recordPrerollFrames the count-in is done. The playback cursor does NOT
    // advance during count-in; the playhead stays at recordStartFrame.
    std::atomic<std::uint64_t> countInFrameCursor {0};
    std::vector<std::int16_t> monitorInputPcm;
    size_t monitorInputReadPos {0};

    // Live recording clip displayed while recording (UI-safe copy).
    // Updated by UI timer from recordedInputPcm. Discarded when recording stops.
    ClipItem liveRecordingClip;
    std::vector<float> liveRecordingWaveform;  // Pre-computed stereo waveform for fast drawing
    std::uint64_t liveRecordingFramesProcessed {0};  // Frames already in waveform

    // Last known runtime audio format diagnostics.
    int lastOpenedInputSampleRate {0};
    int lastOpenedInputChannels {0};
    int lastOpenedOutputSampleRate {0};
    int lastOpenedOutputChannels {0};
    int lastCommittedTakeSampleRate {0};
    int lastCommittedTakeFrames {0};
    int lastCommittedTakeChannels {0};
    int lastCaptureElapsedMs {0};
    double lastCaptureObservedRateRatio {0.0};
    int lastCaptureFrameStride {1};
    std::atomic<int> recordInitState {0};
    std::wstring lastRecordInitError;
    std::wstring lastPlaybackInitError;
    bool recordUsingWasapi {false};

    // Runtime-only DSP state for insert chains (not serialized)
    mutable std::vector<InsertDspStateArray> trackInsertDspState;
    mutable std::array<InsertDspStateArray, kBusCount> busInsertDspState {};

    // Pre-allocated mix-buffer scratch reused across realtime callbacks. The
    // engine grows these on demand (when frames or trackCount goes up) but
    // never shrinks them, so the steady-state callback path performs ZERO
    // heap allocations. Without this every callback was constructing several
    // std::vector<float> instances, fragmenting the heap; over minutes of
    // playback the allocator slowed down enough to miss small-buffer
    // deadlines.
    std::array<std::vector<float>, kBusCount> engineBusScratch;
    std::vector<std::vector<float>>           engineTrackScratch;
    std::vector<float>                        engineMasterScratch;
    // Pre-resolved per-track / per-bus mix parameters reused across
    // realtime callbacks. Sized lazily in EngineFillRealtimeBufferLocked.
    std::vector<daw::engine::TrackMix>        engineTrackMixScratch;
    std::vector<daw::engine::BusMix>          engineBusMixScratch;
};
