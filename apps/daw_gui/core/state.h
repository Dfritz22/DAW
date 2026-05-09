#pragma once
#ifdef DAW_PUBLIC_API
#error "This header is internal-only."
#endif

#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <mmsystem.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <Functiondiscoverykeys_devpkey.h>
#include <ks.h>
#include <ksmedia.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <map>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include "daw_core/Engine.hpp"
#include "core/automation_types.h"
#include "dsp/insert_types.h"

#pragma comment(lib, "Comdlg32.lib")
#pragma comment(lib, "Winmm.lib")
#pragma comment(lib, "Ole32.lib")

constexpr wchar_t kWindowClassName[] = L"DawGuiWindowClass";
constexpr UINT_PTR kPlaybackTimerId = 1;
constexpr int kPlaybackTimerMs = 33;
constexpr UINT kMsgPlaybackFinished = WM_APP + 1;
constexpr UINT kMsgAutoMixFinished = WM_APP + 2;
constexpr UINT kCmdFileImportWav  = 40001;
constexpr UINT kCmdFileExit       = 40002;
constexpr UINT kCmdFileSave       = 40003;
constexpr UINT kCmdFileSaveAs     = 40004;
constexpr UINT kCmdFileOpen       = 40005;
constexpr UINT kCmdFileExportWav  = 40006;
constexpr UINT kCmdMixReadiness   = 40007;
constexpr UINT kCmdAutoMaster     = 40008;
constexpr UINT kCmdViewZoomIn = 40101;
constexpr UINT kCmdViewZoomOut = 40102;
constexpr UINT kCmdViewReset = 40103;
constexpr UINT kCmdAudioRefreshInputs = 40201;
constexpr UINT kCmdAudioDiagnostics = 40202;
constexpr UINT kCmdAudioBackendMME = 40203;
constexpr UINT kCmdAudioBackendWasapiShared = 40204;
constexpr UINT kCmdAudioBackendWasapiExclusive = 40205;
constexpr UINT kCmdAudioBackendAsio = 40206;
constexpr UINT kCmdAudioSampleRateBase = 43000;
constexpr UINT kCmdAudioBufferSizeBase = 43100;
constexpr UINT kCmdTrackNew = 40301;
constexpr UINT kCmdAudioInputBase = 41000;
constexpr UINT kCmdAudioOutputBase = 42000;
constexpr UINT kCmdAudioSettings = 44000;
constexpr int kAudioBufferFrames = 256;
constexpr int kAudioBufferCount = 4;
constexpr int kRecordBufferFrames = 256;
constexpr int kRecordBufferCount = 4;

struct Palette {
    COLORREF windowBg {RGB(18, 20, 23)};
    COLORREF topBar {RGB(29, 32, 36)};
    COLORREF topBarEdge {RGB(42, 46, 52)};
    COLORREF transportBtn {RGB(52, 57, 64)};
    COLORREF transportBtnActive {RGB(66, 145, 108)};
    COLORREF textPrimary {RGB(224, 226, 230)};
    COLORREF textMuted {RGB(150, 156, 164)};
    COLORREF leftPanel {RGB(35, 38, 43)};
    COLORREF leftPanelHeader {RGB(45, 49, 55)};
    COLORREF arrangeBg {RGB(22, 24, 28)};
    COLORREF rulerBg {RGB(26, 29, 33)};
    COLORREF laneDark {RGB(30, 33, 38)};
    COLORREF laneLight {RGB(33, 37, 43)};
    COLORREF barLine {RGB(70, 74, 81)};
    COLORREF beatLine {RGB(47, 50, 56)};
    COLORREF clip1 {RGB(88, 131, 199)};
    COLORREF clip2 {RGB(97, 163, 122)};
    COLORREF clip3 {RGB(193, 125, 91)};
    COLORREF clip4 {RGB(169, 118, 188)};
    COLORREF playhead {RGB(232, 91, 91)};
};

inline const Palette kPalette{};

constexpr int kTopBarHeight = 64;
constexpr int kLeftPanelWidth = 300;
constexpr int kRulerHeight = 30;
constexpr int kTrackRowHeight = 66;
constexpr int kClipInsetY = 12;
constexpr int kFaderRailWidth = 6;
constexpr int kFaderKnobWidth = 20;
constexpr int kFaderKnobHeight = 8;
constexpr float kFaderMinDb = -60.0f;
constexpr float kFaderMaxDb = 6.0f;
constexpr int kBusCount = 4;

struct LayoutRects {
    RECT topBar;
    RECT leftPanel;
    RECT ruler;
    RECT arrange;
};

struct LoadedAudio {
    std::wstring sourcePath;
    std::wstring displayName;
    int sampleRate {0};
    std::uint32_t frames {0};
    std::vector<float> stereo;
};

struct ClipItem {
    int trackIndex {0};
    int audioIndex {-1};
    float startBeat {0.0f};
    float lengthBeats {4.0f};
    COLORREF color {RGB(88, 131, 199)};
    std::wstring name;
    std::uint64_t sourceOffsetFrames {0};  // first audio frame this clip plays
};

enum class AudioBackend {
    MME,
    WasapiShared,
    WasapiExclusive,
    Asio,
};

inline bool IsWasapiBackend(AudioBackend backend) {
    return backend == AudioBackend::WasapiShared || backend == AudioBackend::WasapiExclusive;
}

#include "core/ProjectData.h"

struct UiState {
    bool playing {false};
    bool recording {false};
    float playheadBeat {0.0f};
    float viewStartBeat {0.0f};
    float viewBeatsVisible {32.0f};
    bool draggingClip {false};
    int dragClipIndex {-1};
    float dragOffsetBeats {0.0f};
    bool draggingFader {false};
    int dragFaderTrack {-1};
    int dragFaderStartY {0};
    float dragFaderStartDb {0.0f};
    bool draggingPan {false};
    bool dragPanIsBus {false};
    int dragPanIndex {-1};    // track index or bus index
    int dragPanStartY {0};
    float dragPanStartVal {0.0f};
    int selectedClipIndex {-1};
    int selectedTrackIndex {-1};
    float playbackEndBeat {0.0f};
    ULONGLONG playbackStartTick {0};
    float playbackStartBeat {0.0f};

    HWND hwnd {nullptr};
    HWAVEOUT waveOut {nullptr};
    WAVEFORMATEX waveFormat {};
    std::vector<WAVEHDR> waveHeaders;
    std::vector<std::vector<std::int16_t>> waveData;
    HANDLE audioThread {nullptr};
    std::atomic<bool> audioStopRequested {false};
    std::atomic<bool> audioThreadRunning {false};

    // WASAPI output state
    bool playingViaWasapi {false};
    WAVEFORMATEX        wasapiOutFormat        {};
    std::atomic<int>    wasapiOutInitState     {0}; // 0=pending, 1=ready, -1=fail
    HANDLE automixThread {nullptr};
    std::atomic<bool> automixRunning {false};
    std::atomic<std::uint64_t> playbackFrameCursor {0};
    CRITICAL_SECTION audioStateLock {};

    RECT playRect {0, 0, 0, 0};
    RECT stopRect {0, 0, 0, 0};
    RECT recordRect {0, 0, 0, 0};
    RECT importRect {0, 0, 0, 0};
    RECT automixRect {0, 0, 0, 0};
    RECT vocalCheckRect {0, 0, 0, 0};
    RECT autoMasterRect {0, 0, 0, 0};
    RECT metPlayRect {0, 0, 0, 0};
    RECT metRecRect {0, 0, 0, 0};
    RECT monitorRect {0, 0, 0, 0};
    RECT bpmDownRect {0, 0, 0, 0};
    RECT bpmUpRect {0, 0, 0, 0};
    RECT countInRect {0, 0, 0, 0};
    RECT fileMenuRect {0, 0, 0, 0};
    RECT viewMenuRect {0, 0, 0, 0};
    RECT audioMenuRect {0, 0, 0, 0};
    RECT trackMenuRect {0, 0, 0, 0};

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

    // Insert chain inspector panel
    bool fxInspectorOpen   {false};
    bool fxInspectorIsTrack{true};
    int  fxInspectorIndex  {-1};
    int  fxInspectorSelectedSlot{-1};  // which slot is expanded for param editing

    // Param knob drag state
    bool  draggingParamKnob {false};
    int   paramKnobParamId  {-1};  // encoded: slotIndex*100 + paramIndex
    int   paramKnobDragStartY{0};
    float paramKnobDragStartVal{0.0f};

    // Project persistence
    std::wstring projectFilePath;  // empty = unsaved
    bool projectModified {false};

    // Undo / redo
    struct UndoEntry {
        std::vector<ClipItem> clips;
        std::vector<float>    trackGainDb;
    };
    std::vector<UndoEntry> undoStack;
    std::vector<UndoEntry> redoStack;

    // Playhead drag (ruler click)
    bool draggingPlayhead {false};

    // Clip trim
    bool trimmingClip {false};
    int  trimClipIndex {-1};
    bool trimIsLeft {false};   // true = dragging left edge, false = right edge
    float trimOrigStart {0.0f};
    float trimOrigLen   {0.0f};
    std::uint64_t trimOrigSourceOffset {0};

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
    int  countInBars {1};
    std::uint64_t recordPrerollFrames {0};
    bool countingIn {false};    // true between Record-pressed and end of preroll
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
    std::atomic<int> recordInitState {0}; // 0=pending, 1=ok, -1=failed
    std::wstring lastRecordInitError;
    std::wstring lastPlaybackInitError;
    bool recordUsingWasapi {false};

    // Persistent project data model
    ProjectData project;

    // Runtime-only DSP state for insert chains (not serialized)
    mutable std::vector<InsertDspStateArray> trackInsertDspState;
    mutable std::array<InsertDspStateArray, kBusCount> busInsertDspState{};

    // Track automation curves (single source of truth for automation data)
    std::vector<TrackAutomationCurve> trackGainCurves;
    std::vector<TrackAutomationCurve> trackPanCurves;
    std::vector<TrackAutomationCurve> trackBusCurves;
};

// ── Forward declarations ────────────────────────────────────────────────────
bool RenderTrackToStereoLocked(const UiState& state, int trackIndex, std::vector<float>* outStereo, int* outSampleRate);
bool RenderFullMixToStereoLocked(const UiState& state, std::vector<float>* outStereo, int* outSampleRate);
bool RenderBusStemToStereoLocked(const UiState& state, int busIndex, std::vector<float>* outStereo, int* outSampleRate);
bool DoExportMix(HWND hwnd, UiState& state);
bool DoMixReadiness(HWND hwnd, UiState& state);
bool DoAutoMaster(HWND hwnd, UiState& state);
const wchar_t* BusName(int busIndex);
float BusGainDbAt(const UiState& state, int busIndex);
float BusPanAt(const UiState& state, int busIndex);
bool BusMuteAt(const UiState& state, int busIndex);
