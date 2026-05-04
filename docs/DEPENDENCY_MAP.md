# daw_gui Dependency Map

Generated from static analysis of `apps/daw_gui/` source files.
Last updated: Phase 2 Step 3 complete (ProjectData migration, legacy UiState fields removed).

---

## 1. File Structure

```
apps/daw_gui/
  state.h                  ← master types: UiState, InsertParams, ClipItem, LoadedAudio, etc.
  draw.h                   ← draw + layout declarations; includes state.h + ui/layout.h
  draw.cpp                 ← all Draw* functions + primitives
  main.cpp                 ← WndProc + all subsystems not yet extracted
  core/
    ProjectData.h          ← TrackData, BusData, ProjectData structs (included by state.h)
    timeline_edit.h        ← PushUndo, ApplyUndo/Redo, clip edits, AddNewTrack, DeleteTrackAt
    timeline_edit.cpp      ← implementation of timeline_edit.h
  dsp/
    chain.h                ← ApplyInsertChain declaration
    chain.cpp              ← DspApply* + ApplyInsertChain implementation
  io/
    wav_io.h               ← LoadWavStereo, WriteWavPcm16Stereo declarations
    wav_io.cpp             ← WAV I/O implementation
  ui/
    layout.h               ← all layout/coordinate math declarations
    layout.cpp             ← SamplesPerBeat, BeatToX, GetTrack*Rects, etc. implementation
```

## 2. Include Chain

```
core/ProjectData.h
  └── <vector> <string> <cstdint> <array>
      (forward-declares InsertParams, LoadedAudio, ClipItem to avoid circular include)

state.h
  ├── windows.h, windowsx.h, commdlg.h
  ├── mmsystem.h        (WinMM: waveOut/waveIn MME API)
  ├── mmdeviceapi.h     (WASAPI device enumeration)
  ├── audioclient.h     (WASAPI IAudioClient render/capture)
  ├── Functiondiscoverykeys_devpkey.h  (WASAPI device friendly-name lookup)
  ├── ks.h / ksmedia.h  (WASAPI format negotiation)
  ├── <algorithm> <array> <atomic> <map> <cmath> <cstdint>
  │   <filesystem> <fstream> <sstream> <string> <vector>
  ├── daw_core/Engine.hpp   ← included but never instantiated (dead dep — see §6.2)
  └── core/ProjectData.h   ← TrackData, BusData, ProjectData

ui/layout.h
  └── state.h

dsp/chain.h
  └── state.h

io/wav_io.h
  └── state.h

core/timeline_edit.h
  └── windows.h (forward-declares UiState only)

draw.h
  ├── state.h
  └── ui/layout.h       ← layout declarations now in own header (no longer in draw.h)

draw.cpp
  └── draw.h

main.cpp
  └── draw.h            ← single include, pulls in state.h + ui/layout.h transitively
```

**Note:** `main.cpp` only has one `#include`. All types, Windows APIs, and STL
are available to it entirely through `draw.h → state.h`. Subsystem headers
(`core/timeline_edit.h`, `dsp/chain.h`, `io/wav_io.h`) are included via
`draw.h → state.h` transitively or included directly where needed.

---

## 3. Function Definitions vs. Declarations

Layout/coordinate functions previously split across `draw.h` / `main.cpp` are now fully
extracted to `ui/layout.h` + `ui/layout.cpp`.

| Function | Declared in | Defined in | Used by |
|---|---|---|---|
| `SamplesPerBeat` | `ui/layout.h` | `ui/layout.cpp` | `draw.cpp`, `main.cpp`, `core/timeline_edit.cpp` |
| `ComputeLayout` | `ui/layout.h` | `ui/layout.cpp` | `main.cpp` (WndProc) |
| `XToBeat` | `ui/layout.h` | `ui/layout.cpp` | `draw.cpp`, `main.cpp` |
| `BeatToX` | `ui/layout.h` | `ui/layout.cpp` | `draw.cpp`, `main.cpp` |
| `TrackIndexFromY` | `ui/layout.h` | `ui/layout.cpp` | `main.cpp` (WndProc) |
| `ClipRectForDraw` | `ui/layout.h` | `ui/layout.cpp` | `draw.cpp`, `main.cpp` |
| `GetInspectorPanelRect` | `draw.h` | `draw.cpp` | `draw.cpp`, `main.cpp` |
| `GetTrackFaderRects` | `ui/layout.h` | `ui/layout.cpp` | `draw.cpp`, `main.cpp` |
| `GetTrackButtonRects` | `ui/layout.h` | `ui/layout.cpp` | `draw.cpp`, `main.cpp` |
| `GetTrackRoutingRects` | `ui/layout.h` | `ui/layout.cpp` | `draw.cpp`, `main.cpp` |
| `GetBusControlRects` | `ui/layout.h` | `ui/layout.cpp` | `draw.cpp`, `main.cpp` |
| `TrackGainDbAt` | `ui/layout.h` | `ui/layout.cpp` | `draw.cpp`, `main.cpp` |
| `TrackPanAt` | `ui/layout.h` | `ui/layout.cpp` | `draw.cpp`, `main.cpp` |
| `TrackBusIndexAt` | `ui/layout.h` | `ui/layout.cpp` | `draw.cpp`, `main.cpp` |
| `IsTrackAudible` | `ui/layout.h` | `ui/layout.cpp` | `draw.cpp`, `main.cpp` |
| `FaderKnobTopFromGain` | `ui/layout.h` | `ui/layout.cpp` | `draw.cpp`, `main.cpp` |
| `GainFromFaderY` | `ui/layout.h` | `ui/layout.cpp` | `main.cpp` (WndProc) |
| `BusPanelTop` | `ui/layout.h` | `ui/layout.cpp` | `draw.cpp`, `main.cpp` |
| `SnapBeat` | `ui/layout.h` | `ui/layout.cpp` | `main.cpp` |
| `ApplyInsertChain` | `dsp/chain.h` | `dsp/chain.cpp` | `main.cpp` (render engine) |
| `LoadWavStereo` | `io/wav_io.h` | `io/wav_io.cpp` | `main.cpp` |
| `WriteWavPcm16Stereo` | `io/wav_io.h` | `io/wav_io.cpp` | `main.cpp` |
| `PushUndo` | `core/timeline_edit.h` | `core/timeline_edit.cpp` | `main.cpp` (WndProc) |
| `ApplyUndo/Redo` | `core/timeline_edit.h` | `core/timeline_edit.cpp` | `main.cpp` (WndProc) |
| `SplitSelectedClip` | `core/timeline_edit.h` | `core/timeline_edit.cpp` | `main.cpp` (WndProc) |
| `DuplicateSelectedClip` | `core/timeline_edit.h` | `core/timeline_edit.cpp` | `main.cpp` (WndProc) |
| `NudgeSelectedClip` | `core/timeline_edit.h` | `core/timeline_edit.cpp` | `main.cpp` (WndProc) |
| `DeleteSelectedClip` | `core/timeline_edit.h` | `core/timeline_edit.cpp` | `main.cpp` (WndProc) |
| `AddNewTrack` | `core/timeline_edit.h` | `core/timeline_edit.cpp` | `main.cpp` (menu) |
| `DeleteTrackAt` | `core/timeline_edit.h` | `core/timeline_edit.cpp` | `main.cpp` (WndProc) |
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

## 4. Subsystem Map

`main.cpp` still contains most subsystems. Extracted subsystems are noted with their new file.

| # | Subsystem | Key Functions | Location | Approx. Lines (main.cpp) |
|---|---|---|---|---|
| 1 | **Project Serialization** | `SaveProject`, `LoadProject`, `DoSave`, `DoSaveAs`, `DoOpen` | `main.cpp` | 264–755 |
| 2 | **Clip / Timeline Editing** | `PushUndo`, `ApplyUndo/Redo`, `SplitSelectedClip`, `DuplicateSelectedClip`, `NudgeSelectedClip`, `DeleteSelectedClip`, `AddNewTrack`, `DeleteTrackAt` | **`core/timeline_edit.cpp`** ✅ | extracted |
| 3 | **Layout / Coordinate Math** | `SamplesPerBeat`, `XToBeat`, `BeatToX`, `TrackIndexFromY`, `ClipRectForDraw`, `GetTrackFaderRects`, `GetBusControlRects` | **`ui/layout.cpp`** ✅ | extracted |
| 4 | **Audio Device Management** | `RefreshInputDevices`, `RefreshOutputDevices`, `BuildAudioDiagnosticsReport`, `IsWasapiBackend`, `AudioBackendToString/FromString`, `ShowAudioSettingsDialog` | `main.cpp` | 1157–1690 |
| 5 | **Menu System** | `ShowTopMenu` | `main.cpp` | 1691–1899 |
| 6 | **Realtime Render Engine** | `FillRealtimeBufferLocked`, `FillRealtimeForDeviceLocked`, `ResampleStereoPcm16Linear`, `ReadClipSampleAtProjectFrame`, `ApplyInsertChain` | `main.cpp` + **`dsp/chain.cpp`** ✅ | 1900–2230 |
| 7 | **AutoMix / AI Bridge** | `ParseAutoMixGains`, `ParseAutoMixSettings`, `ApplyAutoMixToFaders`, `StartAutoMixAsync`, `AutoMixThreadProc`, `AnalyzeSelectedTrackQuality` | `main.cpp` | 2224–2806 |
| 8 | **DSP Chain** | `DspApplyBiquadBand`, `DspApplyEQ`, `DspApplyCompressor`, `DspApplySaturation`, `DspApplyDelay`, `DspApplyReverb`, `DspApplyGate`, `DspApplyDeEsser`, `DspApplyLimiter` | **`dsp/chain.cpp`** ✅ | extracted |
| 9 | **Offline Render / Export** | `RenderTrackToStereoLocked`, `RenderProjectMixToStereoLocked`, `RenderFullMixToStereoLocked`, `RenderBusStemToStereoLocked`, `DoExportMix`, `DoAutoMaster`, `DoMixReadiness` | `main.cpp` | 3411–4260 |
| 10 | **WAV File I/O** | `LoadWavStereo`, `WriteWavPcm16Stereo` | **`io/wav_io.cpp`** ✅ | extracted |
| 11 | **Audio Threads** | `AudioThreadProc` (MME), `WasapiRenderThreadProc`, `WasapiRecordThreadProc`, `RecordThreadProc`, `TryStartWasapiRecording` | `main.cpp` | 4476–5110 |
| 12 | **Transport Control** | `StartPlayback`, `StopPlayback`, `StartRecording`, `StopRecording` | `main.cpp` | 2807 + 5113–5640 |

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

## 5. UiState / ProjectData Field Access by System

### Data model split (Phase 2 Step 3)

All project-level data now lives in `state.project` (type `ProjectData`), **not** directly
on `UiState`. The old flat parallel vectors (`state.tracks`, `state.trackGainDb`, etc.) are removed.

| Old field (removed) | New location |
|---|---|
| `state.tracks` | `state.project.tracks[i].name` |
| `state.trackGainDb[i]` | `state.project.tracks[i].gainDb` |
| `state.trackMute[i]` | `state.project.tracks[i].mute` |
| `state.trackSolo[i]` | `state.project.tracks[i].solo` |
| `state.trackRecordArm[i]` | `state.project.tracks[i].recordArm` |
| `state.trackBusIndex[i]` | `state.project.tracks[i].busIndex` |
| `state.trackPan[i]` | `state.project.tracks[i].pan` |
| `state.trackInsertSlots[i]` | `state.project.tracks[i].insertSlots` |
| `state.trackInsertEffects[i]` | `state.project.tracks[i].insertEffects` |
| `state.trackInsertBypass[i]` | `state.project.tracks[i].insertBypass` |
| `state.trackInsertParams[i]` | `state.project.tracks[i].insertParams` |
| `state.busGainDb[b]` | `state.project.buses[b].gainDb` |
| `state.busMute[b]` | `state.project.buses[b].mute` |
| `state.busPan[b]` | `state.project.buses[b].pan` |
| `state.busInsertSlots[b]` | `state.project.buses[b].insertSlots` |
| `state.busInsertEffects[b]` | `state.project.buses[b].insertEffects` |
| `state.busInsertBypass[b]` | `state.project.buses[b].insertBypass` |
| `state.busInsertParams[b]` | `state.project.buses[b].insertParams` |
| `state.audio` | `state.project.audio` |
| `state.clips` | `state.project.clips` |
| `state.bpm` | `state.project.bpm` (now `float`) |
| `state.projectSampleRate` | `state.project.projectSampleRate` |

### draw.cpp reads these UiState fields:

**Top bar / transport state:**
`playing`, `recording`, `automixRunning`, `metronomePlay`, `metronomeRecord`,
`inputMonitoring`, `countInEnabled`, `viewBeatsVisible`,
`selectedInputDeviceName`, `selectedOutputDeviceName`

**From `state.project`:**
`project.bpm`, `project.projectSampleRate`, `project.tracks[i].*`,
`project.buses[b].*`, `project.clips`, `project.audio`

**Button hit-rect writes** (draw.cpp *writes* these during DrawTopBar):
`playRect`, `stopRect`, `recordRect`, `importRect`, `automixRect`, `vocalCheckRect`,
`autoMasterRect`, `metPlayRect`, `metRecRect`, `monitorRect`, `bpmDownRect`,
`bpmUpRect`, `countInRect`, `fileMenuRect`, `viewMenuRect`, `audioMenuRect`, `trackMenuRect`

**Inspector panel:**
`fxInspectorOpen`, `fxInspectorIsTrack`, `fxInspectorIndex`, `fxInspectorSelectedSlot`
→ insert data via `state.project.tracks[i].insertSlots/Effects/Bypass/Params`
   and `state.project.buses[b].insertSlots/Effects/Bypass/Params`

**Track panel:**
`selectedTrackIndex`
→ track data via `state.project.tracks[i].name/mute/solo/recordArm/insertSlots/Effects/Bypass`

**Ruler / arrange lanes:**
`viewStartBeat`, `viewBeatsVisible`, `playheadBeat`, `selectedClipIndex`
→ clip/audio data via `state.project.clips`, `state.project.audio`

### Realtime Render Engine reads:

**From `state.project`:**
`project.clips`, `project.audio`, `project.tracks[i].*`, `project.buses[b].*`,
`project.projectSampleRate`

**From UiState directly:**
`playbackFrameCursor`, `playing`, `recording`, `metronomePlay`, `metronomeRecord`,
`countingIn`, `recordStartFrame`, `inputMonitoring`, `inputMonitorGain`,
`monitorInputPcm`, `monitorInputReadPos`

### Audio Threads read/write (UiState directly — unchanged):
`waveOut`, `waveHeaders`, `waveData`, `audioThread`, `audioStopRequested`,
`audioThreadRunning`, `audioStateLock`, `playingViaWasapi`, `wasapiOutFormat`,
`wasapiOutInitState`, `waveIn`, `waveInHeaders`, `waveInData`, `recordThread`,
`recordStopRequested`, `recordedInputPcm`, `recordInitState`, `recordTrackIndex`,
`recordStartFrame`, `activeDeviceSampleRate`, `activeDeviceBufferFrames`,
`preferredSampleRate`, `preferredBufferFrames`, `audioBackend`

### Transport Control reads/writes (UiState directly — unchanged):
`playing`, `recording`, `playbackFrameCursor`, `playbackStartBeat`, `playbackEndBeat`,
`playbackStartTick`, `selectedOutputDeviceId`, `selectedInputDeviceId`, `audioBackend`,
`countInEnabled`, `countInBars`, `countingIn`, `recordPrerollFrames`
→ `project.projectSampleRate`, `project.bpm` (read via `state.project`)

### Project I/O reads/writes:

Serializes `state.project` (the `ProjectData` struct) plus:
`audioBackend`, `preferredSampleRate`, `preferredBufferFrames`,
`selectedOutputDeviceName`, `selectedInputDeviceName`,
`projectFilePath`, `projectModified`

---

## 6. Circular Dependencies

### 6.1 — ~~Layout functions split across `draw.h` / `main.cpp`~~ ✅ RESOLVED

Layout/coordinate functions are now declared in `ui/layout.h` and defined in `ui/layout.cpp`.
`draw.h` includes `ui/layout.h` rather than re-declaring them. `draw.cpp` and `main.cpp`
are now proper separate compilation units with a clean interface boundary.

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
`state.project.clips`, `state.project.audio`, `state.project.tracks`, etc. —
fields that the main thread modifies without entering `audioStateLock` (e.g.
drag operations in WndProc). Only transport start/stop paths consistently
acquire the lock. A clip drag mid-playback is a potential race.

### 6.5 — `core/ProjectData.h` forward-declares types from `state.h`

`ProjectData.h` cannot include `state.h` (circular: `state.h` includes
`core/ProjectData.h`), so it forward-declares `InsertParams`, `LoadedAudio`,
and `ClipItem`. This means `ProjectData.h` can only use those types by pointer
or reference in isolation — in practice they are used by value inside
`TrackData`/`BusData`, which works because `state.h` is always included before
any TU uses `ProjectData`. Fragile but currently correct.

---

## 7. Tight Coupling Analysis

| System pair | Coupling type | Why it's tight |
|---|---|---|
| WndProc ↔ All subsystems | **Call coupling** | WndProc directly invokes every subsystem by name; any refactor of a function signature requires editing WndProc |
| Transport ↔ Audio Threads ↔ Render Engine | **Data + lock coupling** | All three share `audioStateLock`, `playbackFrameCursor`, and `waveOut/wasapiClient` handles inside UiState |
| ~~draw.cpp ↔ main.cpp~~ | ~~Declaration coupling~~ | **RESOLVED** — layout functions now in `ui/layout.cpp` with clean `ui/layout.h` interface |
| Render Engine ↔ DSP Chain | **Interface coupling** | `FillRealtimeBufferLocked` calls `ApplyInsertChain` inline; audio thread, offline render, and export all share the same function — changing its signature cascades everywhere |
| Project I/O ↔ UiState | **Partial coupling** | `SaveProject`/`LoadProject` now serialize `state.project` (ProjectData) for the bulk of data; still serializes a few UiState device/preference fields directly |
| InsertParams ↔ DSP Chain | **State ownership coupling** | DSP processor state (biquad delay lines, envelopes) lives inside InsertParams inside TrackData/BusData inside ProjectData; the DSP and the data model are the same object |
| ProjectData.h ↔ state.h | **Forward-declaration coupling** | Circular include avoided via forward declarations; `ProjectData.h` forward-declares `InsertParams`, `LoadedAudio`, `ClipItem` from `state.h` |

---

## 8. Safe to Extract Next

### ✅ Tier 1 — Already extracted

| System | File | Status |
|---|---|---|
| **WAV File I/O** | `io/wav_io.cpp` | ✅ Done (Phase 1 Step 1) |
| **DSP primitives** (`DspApply*`, `ApplyInsertChain`) | `dsp/chain.cpp` | ✅ Done (Phase 1 Step 2) |
| **Layout / coordinate math** | `ui/layout.cpp` | ✅ Done (Phase 1 Step 3) |
| **ProjectData model** | `core/ProjectData.h` | ✅ Done (Phase 2 Step 1) |
| **Timeline editing / undo** | `core/timeline_edit.cpp` | ✅ Done (Phase 2 Step 2) |
| **ProjectData migration** (remove legacy UiState fields) | `state.h`, `main.cpp`, `draw.cpp` | ✅ Done (Phase 2 Step 3) |

### ✅ Tier 2 — Extract next (minor interface work)

| System | What needs doing | Target |
|---|---|---|
| **Project I/O** (`SaveProject`, `LoadProject`, `DoSave`, `DoSaveAs`, `DoOpen`) | Operates on `state.project` (ProjectData) already — straightforward to move to own file. Needs a header with forward decl of `UiState`. | `io/project_io.cpp` |
| **AutoMix / AI Bridge** (`ParseAutoMixSettings`, `ApplyAutoMixToFaders`, `AnalyzeSelectedTrackQuality`) | Calls `RenderTrackToStereoLocked` and `WriteWavPcm16Stereo` — wav I/O already extracted; render engine still in main.cpp | `ai/automix_bridge.cpp` |

### ⚠️ Tier 3 — Do not extract until Tier 2 is done

| System | Blocker |
|---|---|
| **Audio Device Management** | Depends on WASAPI COM interfaces; tightly tied to `AudioBackend` enum and `audioStateLock` in UiState |
| **Transport Control** (`StartPlayback`, `StartRecording`) | Manages thread handles inside UiState; depends on audio thread, render engine, and device management all at once |
| **Audio Threads** | Lives inside a DWORD thread proc; accesses nearly all audio fields of UiState under lock |
| **Render Engine** (`FillRealtimeBufferLocked`) | Calls `ApplyInsertChain`, layout helpers, reads all of project data; extract after device mgr and transport are isolated |
| **WndProc** | Depends on all other systems; extract last as the integration layer |

---

## 9. Recommended Extraction Order (updated)

```
Phase 1 (complete ✅):
  io/wav_io.cpp          ← LoadWavStereo, WriteWavPcm16Stereo
  dsp/chain.cpp          ← DspApply*, ApplyInsertChain
  ui/layout.cpp          ← SamplesPerBeat, BeatToX, GetTrack*Rects, etc.

Phase 2 (complete ✅):
  core/ProjectData.h     ← TrackData, BusData, ProjectData structs
  core/timeline_edit.cpp ← PushUndo, ApplyUndo/Redo, clip edits, AddNewTrack, DeleteTrackAt
  state.h migration      ← removed all legacy parallel vectors from UiState

Phase 3 (next):
  io/project_io.cpp      ← SaveProject, LoadProject, DoSave/SaveAs/Open
  ai/automix_bridge.cpp  ← ParseAutoMixSettings, ApplyAutoMixToFaders, AnalyzeSelectedTrackQuality
  audio/dsp_state.h      ← Split InsertParams into InsertConfig (user params)
                            + InsertDspState (runtime processor state)

Phase 4 (major refactor, last):
  audio/device_mgr.cpp   ← RefreshInput/OutputDevices, AudioSettingsDialog
  audio/render_engine.cpp ← FillRealtimeBufferLocked, FillRealtimeForDeviceLocked
  audio/audio_threads.cpp ← AudioThreadProc, WasapiRenderThreadProc, ...
  audio/transport.cpp     ← StartPlayback, StopPlayback, StartRecording, ...
  ui/wndproc.cpp          ← WndProc (calls into all above via clean interfaces)
```
