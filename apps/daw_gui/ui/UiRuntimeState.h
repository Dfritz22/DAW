#pragma once

#ifdef DAW_PUBLIC_API
#error "This header is internal-only."
#endif

#define NOMINMAX
#include <windows.h>

#include <cstdint>

constexpr wchar_t kWindowClassName[] = L"DawGuiWindowClass";
constexpr UINT_PTR kPlaybackTimerId = 1;
constexpr int kPlaybackTimerMs = 33;
constexpr UINT kMsgPlaybackFinished = WM_APP + 1;
constexpr UINT kMsgAutoMixFinished = WM_APP + 2;
constexpr UINT kCmdFileImportWav = 40001;
constexpr UINT kCmdFileExit = 40002;
constexpr UINT kCmdFileSave = 40003;
constexpr UINT kCmdFileSaveAs = 40004;
constexpr UINT kCmdFileOpen = 40005;
constexpr UINT kCmdFileExportWav = 40006;
constexpr UINT kCmdMixReadiness = 40007;
constexpr UINT kCmdAutoMaster = 40008;
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

inline const Palette kPalette {};

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

struct LayoutRects {
    RECT topBar;
    RECT leftPanel;
    RECT ruler;
    RECT arrange;
};

struct UiRuntimeState {
    float playheadBeat {0.0f};
    float viewStartBeat {0.0f};
    float viewBeatsVisible {32.0f};

    HWND hwnd {nullptr};

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

    bool draggingClip {false};
    int dragClipIndex {-1};
    float dragOffsetBeats {0.0f};

    bool draggingFader {false};
    int dragFaderTrack {-1};
    int dragFaderStartY {0};
    float dragFaderStartDb {0.0f};

    bool draggingPan {false};
    bool dragPanIsBus {false};
    int dragPanIndex {-1};
    int dragPanStartY {0};
    float dragPanStartVal {0.0f};

    int selectedClipIndex {-1};
    int selectedTrackIndex {-1};

    // Insert chain inspector panel
    bool fxInspectorOpen {false};
    bool fxInspectorIsTrack {true};
    int fxInspectorIndex {-1};
    int fxInspectorSelectedSlot {-1};

    // Param knob drag state
    bool draggingParamKnob {false};
    int paramKnobParamId {-1};
    int paramKnobDragStartY {0};
    float paramKnobDragStartVal {0.0f};

    // Playhead drag (ruler click)
    bool draggingPlayhead {false};

    // Clip trim
    bool trimmingClip {false};
    int trimClipIndex {-1};
    bool trimIsLeft {false};
    float trimOrigStart {0.0f};
    float trimOrigLen {0.0f};
    std::uint64_t trimOrigSourceOffset {0};
};
