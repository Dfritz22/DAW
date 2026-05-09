# DAW Architecture Map (Current, Post-Phase 7 Final Cleanup)

This document describes the architecture of the codebase as it exists now, after the Phase 7 state split and final layer-boundary cleanup.
It describes only the current implementation.

## 1) High-Level Architecture Overview

### Major subsystems

- App orchestration and event loop
  - `apps/daw_gui/main.cpp`
  - Responsibility: Win32 window lifecycle, input handling, transport controls, menus, command routing, paint loop, and top-level orchestration.

- AppState API bridge
  - `apps/daw_gui/app_state_api.cpp`
  - Responsibility: AppState-level wrappers that forward to split-state internals (`CoreState`, `AudioRuntimeState`, `UiRuntimeState`).

- Core domain (project model + timeline + automation + edits)
  - `apps/daw_gui/core/CoreState.h`
  - `apps/daw_gui/core/ProjectData.h`
  - `apps/daw_gui/core/timeline.cpp`
  - `apps/daw_gui/core/automation.cpp`
  - `apps/daw_gui/core/timeline_edit.cpp`
  - Responsibility: project data, tempo/timeline math, automation curve evaluation, timeline edit/undo/redo operations.

- Audio engine domain
  - `apps/daw_gui/audio/engine.cpp`
  - `apps/daw_gui/audio/engine_utils.cpp`
  - Responsibility: realtime mix/render, offline render, track/bus processing, metronome/count-in, input monitoring mix, sample-rate adaptation.

- Audio device backend domain
  - Shared device logic: `apps/daw_gui/audio/device_common.cpp`
  - MME backend: `apps/daw_gui/audio/device_mme.cpp`
  - WASAPI backend: `apps/daw_gui/audio/device_wasapi.cpp`
  - Responsibility: device enumeration, diagnostics, backend startup/shutdown, render/capture threads, playback cursor reporting.
  - WASAPI backend now takes split-state inputs (`CoreState`, `AudioRuntimeState`, primitives) — no direct `AppState` or `state.ui.*` access.

- Engine facade (optional stable surface)
  - `apps/daw_gui/engine/EngineFacade.h`
  - `apps/daw_gui/engine/EngineFacade.cpp`
  - Responsibility: thin forwarding wrappers for engine init/shutdown, realtime fill, and full-mix offline render. No added behavior.

- Device facade (optional stable surface)
  - `apps/daw_gui/audio/DeviceFacade.h`
  - `apps/daw_gui/audio/DeviceFacade.cpp`
  - Responsibility: thin forwarding wrappers for playback/recording backend start/stop. No added behavior.

- UI domain
  - Drawing: `apps/daw_gui/ui/draw.cpp`
  - Layout math/hit geometry: `apps/daw_gui/ui/layout.cpp`
  - Responsibility: all UI visuals + geometry conversions + control rectangles used by event handling.

- DSP domain
  - Types/config/state: `apps/daw_gui/dsp/insert_types.h`
  - Processing: `apps/daw_gui/dsp/chain.cpp`
  - Responsibility: insert FX processing for tracks and buses.

- I/O domain
  - Project I/O: `apps/daw_gui/io/project_io.cpp`
  - WAV I/O: `apps/daw_gui/io/wav_io.cpp`
  - Responsibility: save/load `.dawproj`, load/write WAV, materialize recorded takes.

- AI integration domain
  - `apps/daw_gui/ai/automix_bridge.cpp`
  - Responsibility: AutoMix and Vocal Check process invocation + applying AI output to project state.

- Public API surface
  - `apps/daw_gui/include/daw_sdk.h` and `daw_*.h` headers
  - Responsibility: AppState-based public SDK entry points.

### Subsystem communication

- UI/events (`main.cpp`) call AppState-level public functions.
- AppState wrappers forward into split-state internals (`core`, `audio`, `ui`).
- Device render/capture threads call engine functions.
- Engine reads CoreState project/automation and mutates AudioRuntimeState runtime fields.
- IO and AI modules mutate state via AppState orchestration.

## 2) State Model Summary

Canonical split state:

- `AppState` in `apps/daw_gui/AppState.h`
  - `CoreState core;`
  - `AudioRuntimeState audio;`
  - `UiRuntimeState ui;`

### CoreState fields

From `apps/daw_gui/core/CoreState.h`:

- `ProjectData project`
- `std::wstring projectFilePath`
- `bool projectModified`
- `std::vector<UndoEntry> undoStack`
- `std::vector<UndoEntry> redoStack`
- `std::vector<TrackAutomationCurve> trackGainCurves`
- `std::vector<TrackAutomationCurve> trackPanCurves`
- `std::vector<TrackAutomationCurve> trackBusCurves`

`UndoEntry` fields:

- `std::vector<ClipItem> clips`
- `std::vector<float> trackGainDb`

### ProjectData fields

From `apps/daw_gui/core/ProjectData.h`:

- `float bpm`
- `int projectSampleRate`
- `std::vector<TrackData> tracks`
- `std::vector<BusData> buses`
- `std::vector<LoadedAudio> audio`
- `std::vector<ClipItem> clips`

`TrackData` fields:

- `std::wstring name`
- `float gainDb`
- `bool mute`
- `bool solo`
- `bool recordArm`
- `int busIndex`
- `float pan`
- `int insertSlots`
- `ProjectInsertEffectArray insertEffects`
- `ProjectInsertBypassArray insertBypass`
- `ProjectInsertConfigArray insertConfig`

`BusData` fields:

- `std::wstring name`
- `float gainDb`
- `bool mute`
- `float pan`
- `int insertSlots`
- `ProjectInsertEffectArray insertEffects`
- `ProjectInsertBypassArray insertBypass`
- `ProjectInsertConfigArray insertConfig`

### AudioRuntimeState fields

From `apps/daw_gui/audio/AudioRuntimeState.h`:

- Win/audio context
  - `HWND hwnd`
  - `CoreState* coreContext`

- Transport runtime
  - `bool playing`
  - `bool recording`
  - `float playbackEndBeat`
  - `ULONGLONG playbackStartTick`
  - `float playbackStartBeat`

- MME output runtime
  - `HWAVEOUT waveOut`
  - `WAVEFORMATEX waveFormat`
  - `std::vector<WAVEHDR> waveHeaders`
  - `std::vector<std::vector<std::int16_t>> waveData`
  - `HANDLE audioThread`
  - `std::atomic<bool> audioStopRequested`
  - `std::atomic<bool> audioThreadRunning`

- WASAPI output runtime
  - `bool playingViaWasapi`
  - `WAVEFORMATEX wasapiOutFormat`
  - `std::atomic<int> wasapiOutInitState`

- AutoMix worker runtime
  - `HANDLE automixThread`
  - `std::atomic<bool> automixRunning`

- Synchronization/cursor/backend prefs
  - `std::atomic<std::uint64_t> playbackFrameCursor`
  - `CRITICAL_SECTION audioStateLock`
  - `AudioBackend audioBackend`
  - `int preferredSampleRate`
  - `int preferredBufferFrames`
  - `int activeDeviceSampleRate`
  - `int activeDeviceBufferFrames`

- Device lists/selections
  - `std::vector<UINT> inputDeviceIds`
  - `std::vector<std::wstring> inputDeviceNames`
  - `UINT selectedInputDeviceId`
  - `std::wstring selectedInputDeviceName`
  - `std::vector<UINT> outputDeviceIds`
  - `std::vector<std::wstring> outputDeviceNames`
  - `UINT selectedOutputDeviceId`
  - `std::wstring selectedOutputDeviceName`

- MME input/capture runtime
  - `HWAVEIN waveIn`
  - `WAVEFORMATEX waveInFormat`
  - `std::vector<WAVEHDR> waveInHeaders`
  - `std::vector<std::vector<std::int16_t>> waveInData`
  - `std::vector<std::int16_t> recordedInputPcm`
  - `HANDLE recordThread`
  - `std::atomic<bool> recordStopRequested`
  - `int recordInputChannels`
  - `std::uint64_t recordStartFrame`
  - `int recordTrackIndex`
  - `ULONGLONG recordCaptureStartTickMs`

- Tracking workflow state
  - `bool metronomePlay`
  - `bool metronomeRecord`
  - `bool inputMonitoring`
  - `float inputMonitorGain`
  - `bool countInEnabled`
  - `int countInBars`
  - `std::uint64_t recordPrerollFrames`
  - `bool countingIn`
  - `std::vector<std::int16_t> monitorInputPcm`
  - `size_t monitorInputReadPos`

- Diagnostics / last-known formats
  - `int lastOpenedInputSampleRate`
  - `int lastOpenedInputChannels`
  - `int lastOpenedOutputSampleRate`
  - `int lastOpenedOutputChannels`
  - `int lastCommittedTakeSampleRate`
  - `int lastCommittedTakeFrames`
  - `int lastCommittedTakeChannels`
  - `int lastCaptureElapsedMs`
  - `double lastCaptureObservedRateRatio`
  - `int lastCaptureFrameStride`
  - `std::atomic<int> recordInitState`
  - `std::wstring lastRecordInitError`
  - `std::wstring lastPlaybackInitError`
  - `bool recordUsingWasapi`

- Runtime DSP storage (not serialized)
  - `mutable std::vector<InsertDspStateArray> trackInsertDspState`
  - `mutable std::array<InsertDspStateArray, kBusCount> busInsertDspState`

### UiRuntimeState fields

From `apps/daw_gui/ui/UiRuntimeState.h`:

- View/transport position
  - `float playheadBeat`
  - `float viewStartBeat`
  - `float viewBeatsVisible`

- Window handle
  - `HWND hwnd`

- Top-bar and menu hit rectangles
  - `playRect, stopRect, recordRect, importRect, automixRect, vocalCheckRect, autoMasterRect, metPlayRect, metRecRect, monitorRect, bpmDownRect, bpmUpRect, countInRect, fileMenuRect, viewMenuRect, audioMenuRect, trackMenuRect`

- Drag/selection state
  - Clip drag: `draggingClip, dragClipIndex, dragOffsetBeats`
  - Fader drag: `draggingFader, dragFaderTrack, dragFaderStartY, dragFaderStartDb`
  - Pan drag: `draggingPan, dragPanIsBus, dragPanIndex, dragPanStartY, dragPanStartVal`
  - Selection: `selectedClipIndex, selectedTrackIndex`

- FX inspector state
  - `fxInspectorOpen, fxInspectorIsTrack, fxInspectorIndex, fxInspectorSelectedSlot`

- Param-knob drag state
  - `draggingParamKnob, paramKnobParamId, paramKnobDragStartY, paramKnobDragStartVal`

- Playhead drag
  - `draggingPlayhead`

- Clip trim state
  - `trimmingClip, trimClipIndex, trimIsLeft, trimOrigStart, trimOrigLen, trimOrigSourceOffset`

### State-type dependency mapping

- Core-only users of `CoreState`
  - `core/timeline.cpp`, `core/automation.cpp`, `core/timeline_edit.cpp`

- Split-state audio users (`CoreState` + `AudioRuntimeState`)
  - `audio/engine.cpp`, `audio/engine_utils.cpp`, `audio/device_mme.cpp`, `audio/device_common.cpp`, `audio/device_wasapi.cpp`

- App-layer users of `AppState`
  - `main.cpp`, `ui/draw.cpp`, `ui/layout.cpp`, `io/project_io.cpp`, `ai/automix_bridge.cpp`, `app_state_api.cpp`
  - `engine/EngineFacade.cpp`, `audio/DeviceFacade.cpp` (facade wrappers only)

### Layer-boundary confirmation (post-cleanup)

- No legacy `UiState` shim usage remains.
- No `core/state.h` include remains.
- `core/timeline_edit.cpp` is now core-pure: operates on `CoreState` only; AppState/UI/audio-lock concerns moved to `app_state_api.cpp` wrappers.
- `audio/device_wasapi.cpp` no longer depends on `AppState` or reads `state.ui.*`; backend takes split-state inputs (`CoreState`, `AudioRuntimeState`) and UI primitives (hwnd, playhead beat) as parameters.
- No remaining cross-layer violations in core or audio backend modules.

## 3) Public API Surface

### Public umbrella header

- `apps/daw_gui/include/daw_sdk.h`
  - Includes: `daw_audio.h`, `daw_timeline.h`, `daw_automation.h`, `daw_project.h`

### `daw_audio.h` functions and behavior

From `apps/daw_gui/include/daw_audio.h`:

- `bool EngineInit(AppState& state)`
  - Current behavior: stub returning true (`app_state_api.cpp`).

- `void EngineShutdown(AppState& state)`
  - Current behavior: no-op stub (`app_state_api.cpp`).

- `bool StartPlayback(HWND hwnd, AppState& state)`
  - In `main.cpp`: validates output devices and conditions, stops existing playback, starts selected backend.

- `void StopPlayback(AppState& state, bool rewind)`
  - In `main.cpp`: stops backend, clears running flags, optional rewind.

- `bool StartRecording(HWND hwnd, AppState& state)`
  - In `main.cpp`: requires armed track, ensures playback/count-in path, starts recording backend.

- `void StopRecording(AppState& state, bool commitTake)`
  - In `main.cpp`: stops backend and optionally commits captured take as clip/audio.

- `bool RenderFullMixToStereoLocked(const AppState& state, std::vector<float>* outStereo, int* outSampleRate)`
  - AppState inline forwarder in `AppState.h` to split-state engine render.

- Device orchestration/diagnostics
  - `void DeviceRefreshInputDevices(AppState& state)`
  - `void DeviceRefreshOutputDevices(AppState& state)`
  - `std::wstring DeviceBuildAudioDiagnosticsReport(const AppState& state)`
  - `const wchar_t* DeviceAudioBackendLabel(AudioBackend backend)`
  - `std::uint64_t DeviceGetRenderedPlaybackFrame(const AppState& state)`
  - `bool DeviceStartPlaybackBackend(HWND hwnd, AppState& state)`
  - `void DeviceStopPlaybackBackend(AppState& state)`
  - `bool DeviceStartRecordingBackend(HWND hwnd, AppState& state, int armedTrack, bool wasPlaying)`
  - `void DeviceStopRecordingBackend(AppState& state)`
  - Forwarding implemented in `app_state_api.cpp`.

- WAV API
  - `bool LoadWavStereo(const std::wstring& path, LoadedAudio* out, std::wstring* error)`
  - `bool WriteWavPcm16Stereo(const std::wstring& path, const std::vector<float>& stereo, int sampleRate)`
  - Implemented in `io/wav_io.cpp`.

### `daw_timeline.h` functions and behavior

From `apps/daw_gui/include/daw_timeline.h`:

- `float SamplesPerBeat(const AppState& app)`
- `std::uint64_t FramesFromBeats(const AppState& app, float beat)`
- `float BeatsFromFrames(const AppState& app, std::uint64_t frame)`

Behavior: wrappers in `app_state_api.cpp` forward to CoreState timeline math.

### `daw_automation.h` functions and behavior

From `apps/daw_gui/include/daw_automation.h`:

- `float TrackGainDbAt(const AppState& app, int trackIndex, float beat)`
- `float TrackPanAt(const AppState& app, int trackIndex, float beat)`
- `int TrackBusIndexAt(const AppState& app, int trackIndex, float beat)`
- `float TrackGainDbAt(const AppState& app, int trackIndex)`
- `float TrackPanAt(const AppState& app, int trackIndex)`
- `int TrackBusIndexAt(const AppState& app, int trackIndex)`

Behavior: wrappers in `app_state_api.cpp` forward to CoreState automation evaluators.

### `daw_project.h` functions and behavior

From `apps/daw_gui/include/daw_project.h`:

- Project file API
  - `bool SaveProject(const std::wstring& path, AppState& state)`
  - `bool LoadProject(const std::wstring& path, AppState& state)`
  - `bool DoSaveAs(HWND hwnd, AppState& state)`
  - `bool DoSave(HWND hwnd, AppState& state)`
  - `bool DoOpen(HWND hwnd, AppState& state)`
  - Implemented via `io/project_io.cpp`.

- Timeline edit API
  - `void PushUndo(AppState& state)`
  - `void ApplyUndo(HWND hwnd, AppState& state)`
  - `void ApplyRedo(HWND hwnd, AppState& state)`
  - `void SplitSelectedClip(AppState& state)`
  - `void DuplicateSelectedClip(AppState& state)`
  - `void NudgeSelectedClip(AppState& state, float deltaBeats)`
  - `void DeleteSelectedClip(AppState& state)`
  - `int AddNewTrack(AppState& state)`
  - `void DeleteTrackAt(AppState& state, int trackIndex)`
  - Public AppState wrappers implemented in `app_state_api.cpp`; core edit logic in `core/timeline_edit.cpp` operates on `CoreState` only.

## 4) Engine Architecture

Core realtime entry points in `apps/daw_gui/audio/engine.cpp`:

- `EngineFillRealtimeBufferLocked(CoreState&, AudioRuntimeState&, int16_t* out, int frames, bool* reachedEnd)`
- `EngineFillRealtimeForDeviceLocked(CoreState&, AudioRuntimeState&, int16_t* out, int deviceFrames, int deviceSampleRate, bool* reachedEnd)`

### Realtime mix pipeline

- Validates sample rate/state.
- Ensures runtime insert DSP storage sizes.
- Computes metronome/count-in/input-monitor mode flags.
- Determines active frame count and end condition.
- For each audible track:
  - Collects clip samples into track buffer (with interpolation and source offset support).
  - Applies track insert chain.
  - Applies track automation gain/pan.
  - Routes to bus buffer via automated bus index.
- For each bus:
  - Skips muted buses.
  - Applies bus insert chain.
  - Applies bus gain/pan and sums to master.
- Adds input monitor mix (if recording+monitoring).
- Adds metronome/count-in click synthesis.
- Clamps and converts to interleaved PCM16.
- Advances playback frame cursor.

### Device-rate path

`EngineFillRealtimeForDeviceLocked` calls project-rate renderer and linearly resamples when project sample rate != device sample rate.

### Offline render paths

In `engine.cpp`:

- `RenderTrackToStereoLocked(const CoreState&, AudioRuntimeState&, int trackIndex, std::vector<float>*, int*)`
- `RenderFullMixToStereoLocked(const CoreState&, AudioRuntimeState&, std::vector<float>*, int*)`
- `RenderBusStemToStereoLocked(const CoreState&, AudioRuntimeState&, int busIndex, std::vector<float>*, int*)`

These are used by export, mix readiness, and AI analysis paths.

### Multi-track playback behavior

- Iterates tracks and clips to accumulate timeline-aligned audio.
- Mute/solo gating via `IsTrackAudible` (`engine_utils.cpp`).
- Per-track bus routing and panning, then bus processing to master.

### Automation and timeline integration

- Timeline math: `core/timeline.cpp`.
- Automation eval: `core/automation.cpp`.
- Engine calls automation evaluators for gain/pan/bus and timeline helpers for beat/frame conversion.

## 5) Device Backend Architecture

### MME backend

Files:

- `apps/daw_gui/audio/device_mme.h`
- `apps/daw_gui/audio/device_mme.cpp`

Behavior:

- Playback:
  - Opens `waveOut` with preferred/selected output.
  - Prepares ring buffers.
  - Audio thread repeatedly renders via engine and submits to waveOut.
  - Drains and posts playback-finished message.

- Recording:
  - Opens `waveIn` (tries selected device and fallback mapper; tries mono/stereo with candidate sample rates).
  - Collects PCM blocks into `recordedInputPcm`.
  - Mirrors input into monitor buffer when monitoring is enabled.

### WASAPI Shared/Exclusive backend

Files:

- `apps/daw_gui/audio/device_wasapi.h`
- `apps/daw_gui/audio/device_wasapi.cpp`

Behavior:

- API now takes split-state inputs (`CoreState`, `AudioRuntimeState`) and UI primitives (hwnd, playhead beat as `float`) as explicit parameters. No `AppState` or `state.ui.*` access inside the backend.

- Playback:
  - Endpoint resolve by preferred device name or default endpoint.
  - If backend is exclusive, attempts exclusive format init; if it fails, falls back to shared mode.
  - Captures negotiated format for diagnostics.
  - Render thread pulls padding/available frames and fills via engine.

- Recording:
  - Capture endpoint open and shared-mode client setup.
  - Packet loop via `IAudioCaptureClient`.
  - Converts float/PCM packets to int16 capture buffer.

### Backend to engine linkage

- Both MME and WASAPI render threads call `EngineFillRealtimeForDeviceLocked`.
- Recording paths feed capture PCM into `AudioRuntimeState` buffers used by commit logic in `StopRecording`.

### Fallback logic

- App wrapper (`app_state_api.cpp`) tries WASAPI first when selected.
- If WASAPI startup fails, falls back to split-state backend start path.
- Current split-state backend start in `device_common.cpp` routes to MME path.
- MME output/input startup also has selected-device to mapper fallback attempts.

## 6) UI Architecture

### `draw.cpp` responsibilities

- Draws transport/menu/top bar.
- Draws track rows, meters, faders, mute/solo/arm buttons.
- Draws bus section controls.
- Draws insert inspector and param UI.
- Draws ruler and arrange view (grid, clips, waveforms, playhead).

### `layout.cpp` responsibilities

- Computes top/left/ruler/arrange rectangles.
- Computes per-track/per-bus control rects.
- Provides beat/pixel conversions and snap logic.
- Provides clip draw rectangle and y->track hit mapping.
- Provides fader gain<->y conversion.

### UI interaction with AppState

- Event handling in `main.cpp` mutates `state.ui` interaction fields and `state.core`/`state.audio` model/runtime fields.
- Draw reads all three substates through `AppState`.

### UI triggers for operations

- Playback/recording via top-bar buttons and keyboard shortcuts.
- Editing actions: split, duplicate, nudge, delete, trim, drag.
- Project actions: open/save/save-as/import/export.
- Audio actions: backend/device/sample-rate/buffer settings.
- AI actions: AutoMix, Vocal Check, Auto Master, Mix Readiness.

## 7) DSP Architecture

### Insert chains

- Slot model in `dsp/insert_types.h`.
- Up to 8 insert slots (`kMaxInsertSlots`) per track and per bus.
- Each slot has effect type + bypass + per-slot config.

### DSP runtime state storage

- Stored in `AudioRuntimeState`:
  - `trackInsertDspState` for tracks
  - `busInsertDspState` for buses
- These are runtime-only and reset on project load (`io/project_io.cpp`).

### DSP integration with engine and buses

- Track chain applied in engine after clip accumulation.
- Bus chain applied at bus stage before master summing.
- Full mix and bus stem offline renders also use insert chains.

### Current effect inventory

From `dsp/chain.cpp`:

- EQ (`kFxEQ`)
- Compressor (`kFxCMP`)
- Saturation (`kFxSAT`)
- Delay (`kFxDLY`)
- Reverb (`kFxREV`)
- Gate (`kFxGATE`)
- De-esser (`kFxDEE`)
- Limiter (`kFxLIM`)

## 8) Project Model

### Model contents

- `ProjectData` includes bpm, sample rate, tracks, buses, audio pool, clips.
- `TrackData` includes routing/mix + insert chain config.
- `BusData` includes mix + insert chain config.
- `ClipItem` stores timeline placement and source offset.
- Automation curves are in `CoreState` (`trackGainCurves`, `trackPanCurves`, `trackBusCurves`).

### Undo/redo

In `core/timeline_edit.cpp`:

- `PushUndo`, `ApplyUndo`, `ApplyRedo`.
- Snapshot payload currently includes clip list + track gain snapshot.

### Project I/O

In `io/project_io.cpp`:

- Saves hand-written JSON with project, view, audio prefs, buses, tracks, audio file list, clips, insert settings.
- Materializes in-memory recorded takes to on-disk WAV files during save.
- Loads JSON back into project state; missing audio files become placeholder audio entries to preserve index references.

## 9) Runtime Flow Diagrams (Textual)

### Playback start -> device backend -> engine -> UI updates

1. User presses Play (button or space) in `main.cpp`.
2. `StartPlayback` validates prerequisites and refreshes outputs.
3. `DeviceStartPlaybackBackend` (AppState wrapper) chooses WASAPI or MME path.
4. Backend render thread runs and repeatedly calls `EngineFillRealtimeForDeviceLocked`.
5. Engine renders audio and advances playback frame cursor.
6. `WM_TIMER` polls rendered playback frame, updates `ui.playheadBeat`, scrolls view as needed, invalidates window.

### Recording start -> preroll -> capture -> commit take

1. User presses Record in `main.cpp`.
2. `StartRecording` finds armed track; starts playback if needed.
3. Recording backend starts (WASAPI first when selected, else/fallback MME).
4. `recordPrerollFrames` and `recordStartFrame` are set (count-in aware).
5. Capture thread fills `recordedInputPcm`; monitor buffer optionally updated.
6. On stop with commit, capture is converted to stereo float take, appended to project audio pool, clip added at timeline start beat.

### UI event -> state update -> redraw

1. Mouse/keyboard/menu event in `WindowProc`.
2. Mutates `AppState` (ui/core/audio as appropriate).
3. Calls `InvalidateRect`.
4. `WM_PAINT` draws full frame via `UiDrawTopBar`, `UiDrawLeftTrackPanel`, `UiDrawRuler`, `UiDrawArrangeLanes`, and optional inspector.

## 10) Dependency Graph

### Build graph (daw_gui target)

From `apps/daw_gui/CMakeLists.txt`, daw_gui includes:

- App/orchestration: `main.cpp`, `app_state_api.cpp`
- UI: `ui/draw.cpp`, `ui/layout.cpp`
- IO: `io/wav_io.cpp`, `io/project_io.cpp`
- AI: `ai/automix_bridge.cpp`
- DSP: `dsp/chain.cpp`
- Core: `core/automation.cpp`, `core/timeline.cpp`, `core/timeline_edit.cpp`, `core/internal_app_services.cpp`
- Audio: `audio/engine_utils.cpp`, `audio/engine.cpp`, `audio/device_mme.cpp`, `audio/device_wasapi.cpp`, `audio/device_common.cpp`
- Facades: `engine/EngineFacade.cpp`, `audio/DeviceFacade.cpp`

Link libs:

- `daw_core`, `Winmm`, `Comdlg32`, `Ole32`

### Include/dependency highlights by module

- `main.cpp` depends on public SDK + UI + AI + internal app services.
- `app_state_api.cpp` depends on AppState and internal core/audio APIs; hosts AppState-level timeline edit wrappers.
- `core/timeline_edit.cpp` depends only on `CoreState` — no AppState, no UI fields, no audio locks.
- `audio/engine.cpp` depends on split-state core/audio types + automation/timeline + dsp chain.
- `audio/device_mme.cpp` depends on split-state types + engine + timeline/automation.
- `audio/device_wasapi.cpp` depends on split-state types (`CoreState`, `AudioRuntimeState`) + engine/device common — no AppState dependency.
- `engine/EngineFacade.cpp` depends on AppState + internal engine APIs (thin forwarder).
- `audio/DeviceFacade.cpp` depends on AppState + internal device APIs (thin forwarder).
- `ui/draw.cpp` depends on AppState + layout + public timeline/automation helpers.
- `io/project_io.cpp` depends on AppState + internal services + wav io.
- `ai/automix_bridge.cpp` depends on AppState + wav io + internal services.

### Remaining cross-layer notes

- `audio/device_common.cpp` split-state backend start/stop entry points currently route to MME; WASAPI dispatch is done in `app_state_api.cpp`. This is intentional pending a future `device_common` WASAPI split-state migration.
- No other cross-layer violations remain.

## 11) Feature Inventory

Current implemented features include:

- Multi-track playback
- Multi-bus routing (`kBusCount = 4`)
- Track mute/solo/record-arm
- Track and bus gain/pan controls
- Track and bus insert FX chains
- Realtime DSP effects: EQ, comp, sat, delay, reverb, gate, de-esser, limiter
- Timeline clip drag/trim/split/duplicate/nudge/delete
- Track add/delete
- Undo/redo for clip + track-gain snapshots
- Metronome (play and record modes)
- Count-in/preroll recording behavior
- Input monitoring while recording
- MME playback/record backend
- WASAPI playback/record backend (shared with exclusive attempt + fallback behavior)
- Playback cursor tracking and auto-scroll
- WAV import (multi-file)
- WAV mix export
- Bus stem rendering
- Mix Readiness external analysis bridge
- Auto Master external processing bridge
- AutoMix async bridge with full-settings and legacy gain-only parsing modes
- Project save/open/save-as with persistent JSON
- Audio settings dialog and diagnostics report
- Startup project open via command-line path

## 12) Missing or Incomplete Features

Current known gaps/incomplete points reflected in code:

- ASIO backend is not implemented (UI labels it future/planned).
- Split-state backend start/stop in `device_common.cpp` still routes to MME; WASAPI split-state migration for that entry point is pending.
- Undo/redo scope is partial (clips + track gains), not full project snapshot.
- Automation curves are evaluated but there is no dedicated automation-edit UI layer.
- Project JSON parse is minimal handwritten parsing (non-schema parser).
- AutoMix parsing is line-oriented and tolerant, not strict schema validation.

---

## Appendix: Public API Header Index

- `apps/daw_gui/include/daw_sdk.h`
- `apps/daw_gui/include/daw_audio.h`
- `apps/daw_gui/include/daw_timeline.h`
- `apps/daw_gui/include/daw_automation.h`
- `apps/daw_gui/include/daw_project.h`
