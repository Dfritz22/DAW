#pragma once

#ifdef DAW_PUBLIC_API
#error "This header is internal-only."
#endif

#define NOMINMAX
#include <windows.h>

#include <cstdint>

#include "ui/dock.h"

constexpr wchar_t kWindowClassName[] = L"DawGuiWindowClass";
constexpr wchar_t kFloatingClassName[] = L"DawGuiFloatingClass";
constexpr UINT_PTR kPlaybackTimerId = 1;
constexpr int kPlaybackTimerMs = 33;
constexpr UINT kMsgPlaybackFinished = WM_APP + 1;
constexpr UINT kMsgAutoMixFinished = WM_APP + 2;
constexpr UINT kMsgCountInComplete = WM_APP + 3;
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
constexpr UINT kCmdProjectSampleRate = 44100;
constexpr UINT kCmdAudioConvertImported = 44200;

// Window menu (panel show/hide toggles + layout reset).
constexpr UINT kCmdWindowResetLayout = 45000;
constexpr UINT kCmdWindowPanelBase   = 45100; // +PanelKind ordinal

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
constexpr int kStatusBarHeight = 22;
constexpr int kLeftPanelWidth = 300;
constexpr int kRulerHeight = 30;
constexpr int kTrackRowHeight = 66;
constexpr int kClipInsetY = 12;
constexpr int kFaderRailWidth = 6;
constexpr int kFaderKnobWidth = 20;
constexpr int kFaderKnobHeight = 8;
constexpr float kFaderMinDb = -60.0f;
constexpr float kFaderMaxDb = 6.0f;

// Bus panel pinned to the bottom of the left panel.
// Layout inside: 16px header + kBusCount(4) rows of 28px + 6px top margin + 4px bottom padding.
constexpr int kBusRowHeight = 28;
constexpr int kBusPanelHeaderHeight = 16;
constexpr int kBusPanelTopMargin = 6;
constexpr int kBusPanelBottomPad = 4;
// 6 + 16 + 4*28 + 4 = 138
constexpr int kBusPanelHeight =
    kBusPanelTopMargin + kBusPanelHeaderHeight + 4 * kBusRowHeight + kBusPanelBottomPad;

struct LayoutRects {
    RECT topBar;
    RECT leftPanel;
    RECT ruler;
    RECT arrange;
};

// Phase 18 / Step G: UiRuntimeState ownership split. Reference aliases at
// UiRuntimeState scope keep pre-split callsites compiling during 18c-18f
// migration; aliases removed in 18g.

struct UiViewState {
    float playheadBeat    {0.0f};
    float viewStartBeat   {0.0f};
    float viewBeatsVisible{32.0f};
    int   tracksScrollY   {0};
    int   selectedClipIndex  {-1};
    int   selectedTrackIndex {-1};
};

struct TopBarHitRects {
    RECT playRect       {0, 0, 0, 0};
    RECT stopRect       {0, 0, 0, 0};
    RECT recordRect     {0, 0, 0, 0};
    RECT importRect     {0, 0, 0, 0};
    RECT automixRect    {0, 0, 0, 0};
    RECT vocalCheckRect {0, 0, 0, 0};
    RECT autoMasterRect {0, 0, 0, 0};
    RECT metPlayRect    {0, 0, 0, 0};
    RECT metRecRect     {0, 0, 0, 0};
    RECT monitorRect    {0, 0, 0, 0};
    RECT bpmDownRect    {0, 0, 0, 0};
    RECT bpmUpRect      {0, 0, 0, 0};
    RECT countInRect    {0, 0, 0, 0};
    RECT fileMenuRect   {0, 0, 0, 0};
    RECT viewMenuRect   {0, 0, 0, 0};
    RECT audioMenuRect  {0, 0, 0, 0};
    RECT trackMenuRect  {0, 0, 0, 0};
};

struct EditToolState {
    bool  draggingClip     {false};
    int   dragClipIndex    {-1};
    float dragOffsetBeats  {0.0f};

    bool  draggingFader    {false};
    int   dragFaderTrack   {-1};
    int   dragFaderStartY  {0};
    float dragFaderStartDb {0.0f};

    bool  draggingPan      {false};
    bool  dragPanIsBus     {false};
    int   dragPanIndex     {-1};
    int   dragPanStartY    {0};
    float dragPanStartVal  {0.0f};

    bool  draggingParamKnob     {false};
    int   paramKnobParamId      {-1};
    int   paramKnobDragStartY   {0};
    float paramKnobDragStartVal {0.0f};

    bool  draggingPlayhead {false};

    bool          trimmingClip         {false};
    int           trimClipIndex        {-1};
    bool          trimIsLeft           {false};
    float         trimOrigStart        {0.0f};
    float         trimOrigLen          {0.0f};
    std::uint64_t trimOrigSourceOffset {0};
};

struct InspectorState {
    bool fxInspectorOpen         {false};
    bool fxInspectorIsTrack      {true};
    int  fxInspectorIndex        {-1};
    int  fxInspectorSelectedSlot {-1};
};

struct DockState {
    std::unique_ptr<daw::ui::DockNode>        dockRoot;
    std::vector<daw::ui::DockLeafLayout>      dockLayout;
    std::vector<daw::ui::DockSplitterLayout>  dockSplitters;
    std::vector<daw::ui::DockTabHit>          dockTabs;

    bool                                      draggingSplitter {false};
    daw::ui::DockNode*                        dragSplitterNode {nullptr};
    bool                                      dragSplitterHorizontal {false};

    bool                                      dragTabArmed   {false};
    bool                                      dragTabActive  {false};
    daw::ui::DockNode*                        dragTabSource  {nullptr};
    int                                       dragTabIndex   {-1};
    daw::ui::PanelKind                        dragTabPanel   {};
    POINT                                     dragTabStartPt {0, 0};
    POINT                                     dragTabCurPt   {0, 0};

    daw::ui::DockNode*                        dropTargetLeaf  {nullptr};
    daw::ui::DockDropSide                     dropTargetSide  {daw::ui::DockDropSide::Center};
    int                                       dropTargetTabAt {-1};
    RECT                                      dropPreviewRect {0,0,0,0};

    struct FloatingPanel {
        HWND               hwnd  {nullptr};
        daw::ui::PanelKind panel {};
    };
    std::vector<FloatingPanel>                floatingPanels;
};

struct UiRuntimeState {
    UiViewState     view;
    TopBarHitRects  topBar;
    EditToolState   tools;
    InspectorState  inspector;
    DockState       dock;

    // ── Deprecated reference aliases (removed in Phase 18g) ──────────────
    float& playheadBeat       {view.playheadBeat};
    float& viewStartBeat      {view.viewStartBeat};
    float& viewBeatsVisible   {view.viewBeatsVisible};
    int&   tracksScrollY      {view.tracksScrollY};

    RECT& playRect       {topBar.playRect};
    RECT& stopRect       {topBar.stopRect};
    RECT& recordRect     {topBar.recordRect};
    RECT& importRect     {topBar.importRect};
    RECT& automixRect    {topBar.automixRect};
    RECT& vocalCheckRect {topBar.vocalCheckRect};
    RECT& autoMasterRect {topBar.autoMasterRect};
    RECT& metPlayRect    {topBar.metPlayRect};
    RECT& metRecRect     {topBar.metRecRect};
    RECT& monitorRect    {topBar.monitorRect};
    RECT& bpmDownRect    {topBar.bpmDownRect};
    RECT& bpmUpRect      {topBar.bpmUpRect};
    RECT& countInRect    {topBar.countInRect};
    RECT& fileMenuRect   {topBar.fileMenuRect};
    RECT& viewMenuRect   {topBar.viewMenuRect};
    RECT& audioMenuRect  {topBar.audioMenuRect};
    RECT& trackMenuRect  {topBar.trackMenuRect};

    bool&  draggingClip          {tools.draggingClip};
    int&   dragClipIndex         {tools.dragClipIndex};
    float& dragOffsetBeats       {tools.dragOffsetBeats};
    bool&  draggingFader         {tools.draggingFader};
    int&   dragFaderTrack        {tools.dragFaderTrack};
    int&   dragFaderStartY       {tools.dragFaderStartY};
    float& dragFaderStartDb      {tools.dragFaderStartDb};
    bool&  draggingPan           {tools.draggingPan};
    bool&  dragPanIsBus          {tools.dragPanIsBus};
    int&   dragPanIndex          {tools.dragPanIndex};
    int&   dragPanStartY         {tools.dragPanStartY};
    float& dragPanStartVal       {tools.dragPanStartVal};

    int&   selectedClipIndex     {view.selectedClipIndex};
    int&   selectedTrackIndex    {view.selectedTrackIndex};

    bool& fxInspectorOpen         {inspector.fxInspectorOpen};
    bool& fxInspectorIsTrack      {inspector.fxInspectorIsTrack};
    int&  fxInspectorIndex        {inspector.fxInspectorIndex};
    int&  fxInspectorSelectedSlot {inspector.fxInspectorSelectedSlot};

    bool&  draggingParamKnob     {tools.draggingParamKnob};
    int&   paramKnobParamId      {tools.paramKnobParamId};
    int&   paramKnobDragStartY   {tools.paramKnobDragStartY};
    float& paramKnobDragStartVal {tools.paramKnobDragStartVal};

    bool&  draggingPlayhead      {tools.draggingPlayhead};

    bool&          trimmingClip         {tools.trimmingClip};
    int&           trimClipIndex        {tools.trimClipIndex};
    bool&          trimIsLeft           {tools.trimIsLeft};
    float&         trimOrigStart        {tools.trimOrigStart};
    float&         trimOrigLen          {tools.trimOrigLen};
    std::uint64_t& trimOrigSourceOffset {tools.trimOrigSourceOffset};

    std::unique_ptr<daw::ui::DockNode>&        dockRoot       {dock.dockRoot};
    std::vector<daw::ui::DockLeafLayout>&      dockLayout     {dock.dockLayout};
    std::vector<daw::ui::DockSplitterLayout>&  dockSplitters  {dock.dockSplitters};
    std::vector<daw::ui::DockTabHit>&          dockTabs       {dock.dockTabs};

    bool&                                      draggingSplitter       {dock.draggingSplitter};
    daw::ui::DockNode*&                        dragSplitterNode       {dock.dragSplitterNode};
    bool&                                      dragSplitterHorizontal {dock.dragSplitterHorizontal};

    bool&                                      dragTabArmed   {dock.dragTabArmed};
    bool&                                      dragTabActive  {dock.dragTabActive};
    daw::ui::DockNode*&                        dragTabSource  {dock.dragTabSource};
    int&                                       dragTabIndex   {dock.dragTabIndex};
    daw::ui::PanelKind&                        dragTabPanel   {dock.dragTabPanel};
    POINT&                                     dragTabStartPt {dock.dragTabStartPt};
    POINT&                                     dragTabCurPt   {dock.dragTabCurPt};

    daw::ui::DockNode*&                        dropTargetLeaf  {dock.dropTargetLeaf};
    daw::ui::DockDropSide&                     dropTargetSide  {dock.dropTargetSide};
    int&                                       dropTargetTabAt {dock.dropTargetTabAt};
    RECT&                                      dropPreviewRect {dock.dropPreviewRect};

    using FloatingPanel = DockState::FloatingPanel;
    std::vector<FloatingPanel>&                floatingPanels {dock.floatingPanels};
};
