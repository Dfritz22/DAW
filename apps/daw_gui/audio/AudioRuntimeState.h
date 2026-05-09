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

struct AudioRuntimeState {
    HWND hwnd {nullptr};
    CoreState* coreContext {nullptr};

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

    AudioBackend audioBackend {AudioBackend::WasapiShared};
    int preferredSampleRate {0};
    int preferredBufferFrames {kAudioBufferFrames};
    int activeDeviceSampleRate {0};
    int activeDeviceBufferFrames {0};

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
    bool countingIn {false};
    std::vector<std::int16_t> monitorInputPcm;
    size_t monitorInputReadPos {0};

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
};
