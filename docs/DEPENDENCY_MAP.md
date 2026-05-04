# daw_gui Dependency Map

Generated from static analysis of `apps/daw_gui/` source files.

---

## 1. Include Chain

```
state.h
  ├── windows.h, windowsx.h, commdlg.h
  ├── mmsystem.h        (WinMM: waveOut/waveIn MME API)
  ├── mmdeviceapi.h     (WASAPI device enumeration)
  ├── audioclient.h     (WASAPI IAudioClient render/capture)
  ├── Functiondiscoverykeys_devpkey.h  (WASAPI device friendly-name lookup)
  ├── ks.h / ksmedia.h  (WASAPI format negotiation)
  ├── <algorithm> <array> <atomic> <map> <cmath> <cstdint>
  │   <filesystem> <fstream> <sstream> <string> <vector>
  └── daw_core/Engine.hpp   ← included but never instantiated (dead dep)

draw.h
  └── state.h           ← inherits all of state.h's includes

draw.cpp
  └── draw.h

main.cpp
  └── draw.h            ← single include, pulls in state.h transitively
```

**Note:** `main.cpp` only has one `#include`. All types, Windows APIs, and STL
are available to it entirely through `draw.h → state.h`.

---

## 2. Function Definitions vs. Declarations

A key structural issue: **layout/coordinate functions are declared in `draw.h`
but defined in `main.cpp`**. This means `draw.h` is not purely a draw-module
header — it is a shared interface header for both compilation units.

| Function | Declared in | Defined in | Used by |
|---|---|---|---|
| `SamplesPerBeat` | `draw.h` | `main.cpp` | `draw.cpp`, `main.cpp` |
| `ComputeLayout` | *(implicit)* | `main.cpp` | `main.cpp` (WndProc) |
| `XToBeat` | `draw.h` | `main.cpp` | `draw.cpp`, `main.cpp` |
| `BeatToX` | `draw.h` | `main.cpp` | `draw.cpp`, `main.cpp` |
| `TrackIndexFromY` | `draw.h` | `main.cpp` | `main.cpp` (WndProc) |
| `ClipRectForDraw` | `draw.h` | `main.cpp` | `draw.cpp`, `main.cpp` |
| `GetInspectorPanelRect` | `draw.h` | `draw.cpp` | `draw.cpp`, `main.cpp` |
| `GetTrackFaderRects` | `draw.h` | `main.cpp` | `draw.cpp`, `main.cpp` |
| `GetTrackButtonRects` | `draw.h` | `main.cpp` | `draw.cpp`, `main.cpp` |
| `GetTrackRoutingRects` | `draw.h` | `main.cpp` | `draw.cpp`, `main.cpp` |
| `GetBusControlRects` | `draw.h` | `main.cpp` | `draw.cpp`, `main.cpp` |
| `TrackGainDbAt` | `draw.h` | `main.cpp` | `draw.cpp`, `main.cpp` |
| `TrackPanAt` | `draw.h` | `main.cpp` | `draw.cpp`, `main.cpp` |
| `TrackBusIndexAt` | `draw.h` | `main.cpp` | `draw.cpp`, `main.cpp` |
| `IsTrackAudible` | `draw.h` | `main.cpp` | `draw.cpp`, `main.cpp` |
| `FaderKnobTopFromGain` | `draw.h` | `main.cpp` | `draw.cpp`, `main.cpp` |
| `GainFromFaderY` | `draw.h` | `main.cpp` | `main.cpp` (WndProc) |
| `BusPanelTop` | `draw.h` | `main.cpp` | `draw.cpp`, `main.cpp` |
| `SnapBeat` | `draw.h` | `main.cpp` | `main.cpp` |
| `Fill` | `draw.h` | `draw.cpp` | `draw.cpp` only |
| `StrokeRect` | `draw.h` | `draw.cpp` | `draw.cpp` only |
| `DrawButton` | `draw.h` | `draw.cpp` | `draw.cpp` only |
| `DrawPanKnob` | `draw.h` | `draw.cpp` | `draw.cpp` only |
| `DrawMenuTab` | `draw.h` | `draw.cpp` | `draw.cpp` only |
| `DrawTopBar` | `draw.h` | `draw.cpp` | `main.cpp` (WndProc WM_PAINT) |
| `DrawInsertInspector` | `draw.h` | `draw.cpp` | `main.cpp` (WndProc WM_PAINT) |
| `DrawLeftTrackPanel` | `draw.h` | `draw.cpp` | `main.cpp` (WndProc WM_PAINT) |
| `DrawRuler` | `draw.h` | `draw.cpp` | `main.cpp` (WndProc WM_PAINT) |
| `DrawClipWaveform` | `draw.h` | `draw.cpp` | `draw.cpp` (internal) |
| `DrawArrangeLanes` | `draw.h` | `draw.cpp` | `main.cpp` (WndProc WM_PAINT) |

---

## 3. Subsystem Map (main.cpp)

`main.cpp` contains 12 distinct logical subsystems in a single file:

| # | Subsystem | Key Functions | Approx. Lines |
|---|---|---|---|
| 1 | **Project Serialization** | `SaveProject`, `LoadProject`, `DoSave`, `DoSaveAs`, `DoOpen` | 264–755 |
| 2 | **Clip / Timeline Editing** | `PushUndo`, `ApplyUndo/Redo`, `SplitSelectedClip`, `DuplicateSelectedClip`, `NudgeSelectedClip`, `DeleteSelectedClip`, `AddNewTrack`, `DeleteTrackAt` | 766–1060 |
| 3 | **Layout / Coordinate Math** | `SamplesPerBeat`, `XToBeat`, `BeatToX`, `TrackIndexFromY`, `ClipRectForDraw`, `GetTrackFaderRects`, `GetBusControlRects` | 901–1160 |
| 4 | **Audio Device Management** | `RefreshInputDevices`, `RefreshOutputDevices`, `BuildAudioDiagnosticsReport`, `IsWasapiBackend`, `AudioBackendToString/FromString`, `ShowAudioSettingsDialog` | 1157–1690 |
| 5 | **Menu System** | `ShowTopMenu` | 1691–1899 |
| 6 | **Realtime Render Engine** | `FillRealtimeBufferLocked`, `FillRealtimeForDeviceLocked`, `ResampleStereoPcm16Linear`, `ReadClipSampleAtProjectFrame`, `ApplyInsertChain` | 1900–2230 |
| 7 | **AutoMix / AI Bridge** | `ParseAutoMixGains`, `ParseAutoMixSettings`, `ApplyAutoMixToFaders`, `StartAutoMixAsync`, `AutoMixThreadProc`, `AnalyzeSelectedTrackQuality` | 2224–2806 |
| 8 | **DSP Chain** | `DspApplyBiquadBand`, `DspApplyEQ`, `DspApplyCompressor`, `DspApplySaturation`, `DspApplyDelay`, `DspApplyReverb`, `DspApplyGate`, `DspApplyDeEsser`, `DspApplyLimiter` | 3032–3410 |
| 9 | **Offline Render / Export** | `RenderTrackToStereoLocked`, `RenderProjectMixToStereoLocked`, `RenderFullMixToStereoLocked`, `RenderBusStemToStereoLocked`, `DoExportMix`, `DoAutoMaster`, `DoMixReadiness` | 3411–4260 |
| 10 | **WAV File I/O** | `LoadWavStereo`, `WriteWavPcm16Stereo` | 2837–3031 |
| 11 | **Audio Threads** | `AudioThreadProc` (MME), `WasapiRenderThreadProc`, `WasapiRecordThreadProc`, `RecordThreadProc`, `TryStartWasapiRecording` | 4476–5110 |
| 12 | **Transport Control** | `StartPlayback`, `StopPlayback`, `StartRecording`, `StopRecording` | 2807 + 5113–5640 |

---

## 4. Which Functions Call Which Subsystems

```
WndProc (WM_LBUTTONDOWN / WM_KEYDOWN / WM_PAINT)
  ├── [SS3] Layout helpers (ComputeLayout, GetInspectorPanelRect, BeatToX, etc.)
  ├── [SS5] ShowTopMenu
  ├── [SS12] StartPlayback, StopPlayback, StartRecording, StopRecording
  ├── [SS2]  PushUndo, ApplyUndo/Redo, SplitSelectedClip, DuplicateSelectedClip
  │          NudgeSelectedClip, DeleteSelectedClip, DeleteTrackAt
  ├── [SS7]  StartAutoMixAsync, AnalyzeSelectedTrackQuality
  ├── [SS1]  DoOpen, DoSave, DoSaveAs
  ├── draw.cpp  DrawTopBar, DrawInsertInspector, DrawLeftTrackPanel,
  │             DrawRuler, DrawArrangeLanes
  └── UpdateWindowTitle

ShowTopMenu [SS5]
  ├── [SS4]  RefreshInputDevices, RefreshOutputDevices
  │          ShowAudioSettingsDialog, BuildAudioDiagnosticsReport
  ├── [SS1]  DoOpen, DoSave, DoSaveAs, ImportWavFiles, DoExportMix
  ├── [SS9]  DoAutoMaster, DoMixReadiness
  ├── [SS7]  ApplyAutoMixToFaders
  └── [SS2]  AddNewTrack

StartPlayback [SS12]
  ├── [SS6]  FillRealtimeBufferLocked (passed to audio thread callback)
  ├── [SS4]  RefreshOutputDevices
  └── [SS11] launches AudioThreadProc or WasapiRenderThreadProc

StartRecording [SS12]
  ├── [SS12] StartPlayback (recording runs over playback)
  └── [SS11] launches RecordThreadProc or WasapiRecordThreadProc

WasapiRenderThreadProc / AudioThreadProc [SS11]
  └── [SS6]  FillRealtimeForDeviceLocked → FillRealtimeBufferLocked
                → [SS8] ApplyInsertChain (per-track DSP)
                → [SS3] SamplesPerBeat, IsTrackAudible, BusGainDbAt, etc.

RenderTrackToStereoLocked [SS9]
  ├── [SS3]  SamplesPerBeat, IsTrackAudible, TrackGainDbAt, etc.
  └── [SS8]  ApplyInsertChain

DoAutoMaster [SS9]
  ├── [SS9]  RenderFullMixToStereoLocked
  ├── [SS10] WriteWavPcm16Stereo
  └── shells out to Python mastering.py via CreateProcessW

DoMixReadiness [SS9]
  ├── [SS9]  RenderBusStemToStereoLocked (×4 buses)
  ├── [SS10] WriteWavPcm16Stereo
  └── shells out to Python mix_readiness.py via CreateProcessW

ApplyAutoMixToFaders [SS7]
  ├── [SS9]  RenderTrackToStereoLocked (renders each track for analysis)
  ├── [SS10] WriteWavPcm16Stereo
  └── shells out to Python auto_mix.py, then ParseAutoMixSettings

DSP functions [SS8]
  └── No outward calls — operate only on float buffers + InsertParams
```

---

## 5. UiState Field Access by System

### draw.cpp reads these UiState fields:

**Top bar / transport state:**
`playing`, `recording`, `automixRunning`, `metronomePlay`, `metronomeRecord`,
`inputMonitoring`, `countInEnabled`, `bpm`, `projectSampleRate`, `viewBeatsVisible`,
`selectedInputDeviceName`, `selectedOutputDeviceName`

**Button hit-rect writes** (draw.cpp *writes* these during DrawTopBar):
`playRect`, `stopRect`, `recordRect`, `importRect`, `automixRect`, `vocalCheckRect`,
`autoMasterRect`, `metPlayRect`, `metRecRect`, `monitorRect`, `bpmDownRect`,
`bpmUpRect`, `countInRect`, `fileMenuRect`, `viewMenuRect`, `audioMenuRect`, `trackMenuRect`

**Inspector panel:**
`fxInspectorOpen`, `fxInspectorIsTrack`, `fxInspectorIndex`, `fxInspectorSelectedSlot`,
`trackInsertSlots`, `trackInsertEffects`, `trackInsertBypass`, `trackInsertParams`,
`busInsertSlots`, `busInsertEffects`, `busInsertBypass`, `busInsertParams`, `tracks`

**Track panel:**
`tracks`, `selectedTrackIndex`, `trackMute`, `trackSolo`, `trackRecordArm`,
`trackInsertSlots`, `trackInsertEffects`, `trackInsertBypass`

**Ruler / arrange lanes:**
`viewStartBeat`, `viewBeatsVisible`, `playheadBeat`, `clips`, `audio`,
`selectedClipIndex`, `tracks`

### Realtime Render Engine reads:
`clips`, `audio`, `tracks`, `trackGainDb`, `trackMute`, `trackSolo`,
`trackBusIndex`, `trackPan`, `trackInsertSlots`, `trackInsertEffects`,
`trackInsertBypass`, `trackInsertParams`, `busGainDb`, `busMute`, `busPan`,
`busInsertSlots`, `busInsertEffects`, `busInsertBypass`, `busInsertParams`,
`projectSampleRate`, `playbackFrameCursor`, `playing`, `recording`,
`metronomePlay`, `metronomeRecord`, `countingIn`, `recordStartFrame`,
`inputMonitoring`, `inputMonitorGain`, `monitorInputPcm`, `monitorInputReadPos`

### Audio Threads read/write:
`waveOut`, `waveHeaders`, `waveData`, `audioThread`, `audioStopRequested`,
`audioThreadRunning`, `audioStateLock`, `playingViaWasapi`, `wasapiOutFormat`,
`wasapiOutInitState`, `waveIn`, `waveInHeaders`, `waveInData`, `recordThread`,
`recordStopRequested`, `recordedInputPcm`, `recordInitState`, `recordUsingWasapi`,
`recordTrackIndex`, `recordStartFrame`, `activeDeviceSampleRate`, `activeDeviceBufferFrames`,
`preferredSampleRate`, `preferredBufferFrames`, `audioBackend`

### Transport Control reads/writes:
`playing`, `recording`, `playbackFrameCursor`, `playbackStartBeat`, `playbackEndBeat`,
`playbackStartTick`, `projectSampleRate`, `bpm`, `selectedOutputDeviceId`,
`selectedInputDeviceId`, `audioBackend`, `countInEnabled`, `countInBars`,
`countingIn`, `recordPrerollFrames`, + most audio thread fields above

### Project I/O reads/writes (save/load):
`tracks`, `trackGainDb`, `trackMute`, `trackSolo`, `trackRecordArm`, `trackBusIndex`,
`trackPan`, `trackInsertSlots`, `trackInsertEffects`, `trackInsertBypass`,
`trackInsertParams`, `busGainDb`, `busMute`, `busPan`, `busInsertSlots`,
`busInsertEffects`, `busInsertBypass`, `busInsertParams`, `clips`, `audio`,
`projectSampleRate`, `bpm`, `audioBackend`, `preferredSampleRate`,
`preferredBufferFrames`, `selectedOutputDeviceName`, `selectedInputDeviceName`,
`projectFilePath`, `projectModified`

---

## 6. Circular Dependencies

### 6.1 — Layout functions split across `draw.h` / `main.cpp`

```
draw.h  declares  SamplesPerBeat, BeatToX, XToBeat, ClipRectForDraw,
                  GetTrackFaderRects, GetBusControlRects, ...

main.cpp  defines  all of the above

draw.cpp  calls    all of the above (via draw.h declaration)
main.cpp  also calls them (WndProc hit-testing)
```

**Effect:** `draw.cpp` and `main.cpp` are logically a single compilation unit
held apart only by the `draw.h` declaration boundary. Moving any layout
function to a proper `layout.cpp` requires touching both files and the header.

### 6.2 — Dead include: `state.h` → `daw_core/Engine.hpp`

`state.h` includes `daw_core/Engine.hpp`, but no object of type
`daw_core::Engine` is ever constructed or referenced in `main.cpp`, `draw.cpp`,
or anywhere in `daw_gui`. The include pulls in `Project.hpp`, `Timeline.hpp`,
and `Transport.hpp` as transitive baggage with no payoff. Any change to
`daw_core` headers triggers a full recompile of all `daw_gui` translation units.

### 6.3 — `InsertParams` contains mutable DSP processor state

`InsertParams` (defined in `state.h`) holds both user-visible parameters
(threshold, ratio, etc.) **and** internal DSP processor state (biquad filter
delay lines, envelope followers, reverb comb buffer pointers). This means:

- Any code that touches `UiState` can accidentally stomp on live DSP state.
- Saving/loading the project serializes user params but must carefully skip the
  mutable DSP fields — currently managed by the serializer manually.
- The audio thread and the main thread share the same `InsertParams` objects
  protected only by `audioStateLock`.

### 6.4 — `FillRealtimeBufferLocked` reads mutable UI state under lock

`FillRealtimeBufferLocked` is called from audio threads but reads
`state.clips`, `state.audio`, `state.tracks`, `state.trackGainDb`, etc. —
fields that the main thread modifies without entering `audioStateLock` (e.g.
drag operations in WndProc). Only transport start/stop paths consistently
acquire the lock. A clip drag mid-playback is a potential race.

---

## 7. Tight Coupling Analysis

| System pair | Coupling type | Why it's tight |
|---|---|---|
| WndProc ↔ All subsystems | **Call coupling** | WndProc directly invokes every subsystem by name; any refactor of a function signature requires editing WndProc |
| Transport ↔ Audio Threads ↔ Render Engine | **Data + lock coupling** | All three share `audioStateLock`, `playbackFrameCursor`, and `waveOut/wasapiClient` handles inside UiState |
| draw.cpp ↔ main.cpp | **Declaration coupling** | draw.h bridges both; layout function definitions live in main.cpp but draw.cpp depends on them |
| Render Engine ↔ DSP Chain | **Interface coupling** | `FillRealtimeBufferLocked` calls `ApplyInsertChain` inline; audio thread, offline render, and export all share the same function — changing its signature cascades everywhere |
| Project I/O ↔ UiState | **Full-graph coupling** | `SaveProject`/`LoadProject` serialize almost every field of UiState; any field addition requires updating both functions |
| InsertParams ↔ DSP Chain | **State ownership coupling** | DSP processor state (biquad delay lines, envelopes) lives inside InsertParams inside UiState; the DSP and the data model are the same object |

---

## 8. Safe to Extract First

These systems have the fewest inbound dependencies and can be moved to their
own files with minimal ripple:

### ✅ Tier 1 — Extract immediately (almost no dependencies)

| System | Why safe | Target |
|---|---|---|
| **WAV File I/O** (`LoadWavStereo`, `WriteWavPcm16Stereo`) | Depends only on `<fstream>` + `LoadedAudio` struct. No UiState. | `io/wav.h` + `io/wav.cpp` |
| **DSP primitives** (`DspApply*`, `ApplyInsertChain`) | Operate on `float` buffers + `InsertParams` only. Zero Win32 or UiState dependency beyond the data types in state.h. | `dsp/chain.h` + `dsp/chain.cpp` |
| **Draw primitives** (`Fill`, `StrokeRect`, `DrawButton`, `DrawPanKnob`, `DrawMenuTab`) | Pure GDI calls. No UiState reads. | Already in draw.cpp — just separate into a `draw_primitives.cpp` |
| **Layout / coordinate math** (`SamplesPerBeat`, `BeatToX`, `XToBeat`, `ComputeLayout`, `GetTrack*Rects`, `ClipRectForDraw`) | Only reads `viewStartBeat`, `viewBeatsVisible`, `bpm`, `projectSampleRate`, `tracks.size()`. No write access to UiState. | `ui/layout.cpp` — fixes the draw.h split |

### ✅ Tier 2 — Extract with minor interface work

| System | What needs doing first | Target |
|---|---|---|
| **Project I/O** (`SaveProject`, `LoadProject`, `DoSave`, `DoSaveAs`, `DoOpen`) | Needs a plain `ProjectData` struct separate from `UiState`; then serialization doesn't need all of UiState | `io/project.h` + `io/project.cpp` |
| **Clip / Timeline Editing** (`PushUndo`, `SplitSelectedClip`, etc.) | Depends only on `clips`, `tracks`, `undoStack/redoStack`. Needs `ProjectData` split first. | `core/timeline_edit.cpp` |
| **AutoMix / AI Bridge** (`ParseAutoMixSettings`, `ApplyAutoMixToFaders`, `AnalyzeSelectedTrackQuality`) | Calls `RenderTrackToStereoLocked` and `WriteWavPcm16Stereo` — needs wav I/O extracted first | `ai/automix_bridge.cpp` |

### ⚠️ Tier 3 — Do not extract until Tier 1 & 2 are done

| System | Blocker |
|---|---|
| **Audio Device Management** | Depends on WASAPI COM interfaces; tightly tied to `AudioBackend` enum and `audioStateLock` in UiState |
| **Transport Control** (`StartPlayback`, `StartRecording`) | Manages thread handles inside UiState; depends on audio thread, render engine, and device management all at once |
| **Audio Threads** (`AudioThreadProc`, `WasapiRenderThreadProc`, etc.) | Lives inside a DWORD thread proc; accesses nearly all audio fields of UiState under lock |
| **WndProc** | Depends on all other systems; extract last as the integration layer |

---

## 9. Recommended Extraction Order

```
Phase 1 (zero-risk, no refactoring):
  io/wav_io.cpp          ← LoadWavStereo, WriteWavPcm16Stereo
  dsp/insert_chain.cpp   ← DspApply*, ApplyInsertChain
  ui/layout.cpp          ← SamplesPerBeat, BeatToX, GetTrack*Rects, etc.
                            (fixes draw.h split)

Phase 2 (minor data model refactor):
  core/project_data.h    ← ProjectData struct (tracks, clips, buses, SR, BPM)
  io/project_io.cpp      ← SaveProject, LoadProject (operate on ProjectData)
  core/timeline_edit.cpp ← clip edits, undo/redo

Phase 3 (medium refactor):
  audio/device_mgr.cpp   ← RefreshInput/OutputDevices, AudioSettingsDialog
  ai/automix_bridge.cpp  ← ParseAutoMixSettings, ApplyAutoMixToFaders
  audio/dsp_state.h      ← Split InsertParams into InsertConfig (user params)
                            + InsertDspState (runtime processor state)

Phase 4 (major refactor, last):
  audio/render_engine.cpp ← FillRealtimeBufferLocked, FillRealtimeForDeviceLocked
  audio/audio_threads.cpp ← AudioThreadProc, WasapiRenderThreadProc, ...
  audio/transport.cpp     ← StartPlayback, StopPlayback, StartRecording, ...
  ui/wndproc.cpp          ← WndProc (calls into all above via clean interfaces)
```
