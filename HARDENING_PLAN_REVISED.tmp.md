# DAW Hardening Plan — Revised (TEMP)

> Working document. Updated post-Phase 16 (WndProc split complete).
> The original "Step A is the load-bearing prerequisite" concern is now
> resolved: main.cpp is 281 lines and cleanly delegates to per-message
> handlers. This document tracks what shipped, what remains, and what the
> next phase (Phase 17) should look like.

---

## 1. Current state (measured, post-Phase 16t)

- `apps/daw_gui/main.cpp` is **281 lines** (was 4008 pre-Phase 16). Down
  -3727 lines / -93%. Contains: includes, `BusName`, `WindowProc`
  dispatcher, `wWinMain`. No business logic.
- `WindowProc` is now a pure switch. Every non-trivial message body is one
  call into a `WndProcOn*` handler in `ui/wndproc/`.
- Layer cake at 8 layers, enforced by `tools/layer_lint.ps1`. 184/184 tests
  green throughout the entire 16a–16t arc.
- Largest remaining TUs (apps/daw_gui only):

  | Lines | File | Cohesion |
  |---|---|---|
  | **1281** | `ui/draw.cpp` | LOW — 7 distinct paint surfaces in one TU |
  |  944 | `audio/device_wasapi.cpp` | high (vendor WASAPI client) |
  |  768 | `io/project_io.cpp` | high (versioned (de)serializer) |
  |  712 | `ai/automix_bridge.cpp` | high (Python subprocess + thread) |
  |  586 | `ui/wndproc/lbuttondown.cpp` | medium (already a focused handler) |
  |  572 | `ui/dialogs.cpp` | medium (2 modal dialogs) |
  |  529 | `ui/dock_persist.cpp` | high (JSON schema (de)serializer) |
  |  526 | `audio/engine.cpp` | high (realtime callback) |
  |  486 | `audio/orchestration.cpp` | medium (3 Do* + 3 helpers, 16s) |

  Of these only `ui/draw.cpp` is large because of mixed concerns. The rest
  are large because the *domain* is large; splitting would not improve
  readability.

---

## 2. What shipped (vs original §8 plan)

| Step | Status | Notes |
|---|---|---|
| **A0** Lock dock contract / floating persistence | ✅ Done | 16i (`ui/floating.{h,cpp}`), 16j (lazy restore in WM_PAINT). Schema stable; no version bump needed. |
| **A1** Extract dialogs + menu | ✅ Done | 16k `ui/menu_build.{h,cpp}`, 16l `ui/dialogs.{h,cpp}` (Audio Settings + Project Sample Rate). |
| **A2** Extract floating window subsystem | ✅ Done | 16i. `FloatingWindowProc` lives in `ui/floating.cpp`. The `GWLP_USERDATA`-swap trick described in §6 of the original plan was kept (mouse handlers don't pump messages) — see §6 below for whether to revisit. |
| **A3** Extract dock input | ✅ Done | 16p `ui/dock_drop.{h,cpp}` (drop-target resolve); dock splitter/tab drag in `ui/wndproc/{mousemove,lbutton,lbuttondown}.cpp`. |
| **A4** Extract panel input | ✅ Done | 16h mousemove, 16n+16o lbuttondown (6 helpers keyed by region). |
| **B** `AudioInit()` + `AudioEngineState` | ✅ Done in spirit | `AudioInitializeRuntime(hwnd, core, audio)` in `audio/engine.cpp`; engineState reaches Ready before `WM_CREATE` returns. `wWinMain` no longer touches audio. |
| **E** Splash screen | ⏸ Deferred indefinitely | Startup is sub-frame. |

---

## 3. What remains

Five steps survive from the original plan and are now actually unblocked.

### Step G — UiRuntimeState ownership split  *(architectural)*

`UiRuntimeState` still mixes 4 concerns:
- **Dock state**: `dockRoot`, `dockSplitters`, `dockTabs`, `floatingPanels`,
  `dragSplitterNode`, `dragTabSource`, `dropTargetLeaf`.
- **Edit-tool drag state**: `draggingClip`, `draggingPlayhead`,
  `draggingFader`, `draggingPan`, `draggingKnob`, plus their target
  indices and pixel offsets.
- **View state**: `viewStartBeat`, `viewBeatsVisible`, `tracksScrollY`,
  `selectedTrackIndex`, `selectedClipIndex`, `playheadBeat`.
- **Inspector state**: `inspectorOpen`, `inspectorTrackIndex`,
  `inspectorBusIndex`, `inspectorSlot`.

Now that each consumer lives in one TU (post-16), splitting into
`DockState` + `EditToolState` + `UiViewState` + `InspectorState` becomes
mechanical: each TU's `state.ui.X` accesses can be migrated to the
narrower struct. **Pre-req for Step F.**

### Step F — Repaint policy  *(correctness, currently latent)*

Today every state mutation calls `InvalidateRect(state.ui.hwnd, …)` which
only invalidates the **main** window. A floating Mixer panel with live
meters would not repaint during playback. Currently masked because the
Mixer doesn't animate, but Phase 4b/5 work (live meters, automation lanes
in floating panels) will trip this.

Fix: single `daw::ui::RequestRepaintAll(AppState&)` that walks
`floatingPanels` plus the main hwnd. Replace ~60+ ad-hoc `InvalidateRect`
sites by grep.

### Step H — Floating-window lifetime hardening  *(correctness)*

`FloatingWndData` holds a raw `AppState*`. Shutdown order today is "lucky":
`wWinMain`'s `WM_DESTROY` saves the dock layout (which iterates floats),
then deletes `state`. If a floating window's `WM_CLOSE` fires after main
state is deleted (e.g. Windows sends close events out-of-order during a
forced session-end), we crash.

Fix: in `wWinMain` `WM_DESTROY` (before `delete state`), explicitly
`DestroyWindow` every `floatingPanels[i].hwnd`, drain the message queue,
then proceed. Optional: give `FloatingWndData` an opaque token + lookup
table so its `WindowProc` can early-out if the token is dead.

### Step C — Engine-readiness assert  *(small, mechanical)*

One `assert(state.audio.activeDeviceSampleRate > 0)` at the top of
`StartPlayback` and `StartRecording` in `audio/transport_control.cpp`.
Plus one regression test in `libs/services` or `apps/daw_gui` test target
that constructs a default `AudioRuntimeState` and verifies the assert
fires (debug-only).

### Step D — Allocation-free realtime path  *(audit + small fixes)*

Current state is mostly clean — `engineTrackMixScratch` and
`engineBusMixScratch` were pre-allocated in Phase 12d-extra. What remains
is an audit pass:

1. Grep `EngineFillRealtimeBufferLocked` and every callee for `new`,
   `malloc`, `std::vector::push_back`, `std::string` ops, `resize` with
   size > capacity.
2. Verify the WASAPI / MME callbacks don't allocate on the hot path.
3. Add a lightweight `#ifdef DAW_RT_ALLOC_TRACE` hook that asserts in
   debug builds when an allocation happens on the audio thread.

---

## 4. New: Step I — `ui/draw.cpp` decomposition  *(this phase, Phase 17)*

User-flagged. `ui/draw.cpp` is 1281 lines containing 7 unrelated paint
surfaces plus 90 lines of shared helpers. Current shape:

| Lines | Public function | Responsibility |
|---:|---|---|
| 1–92 | (anon helpers) | `FormatTimecode`, `DrawKnob`, `DrawFader`, palette lookups |
| 93–110 | `UiDrawTopBar` | top chrome strip |
| 114–129 | `UiDrawTransport` | Play/Stop/Record/BPM buttons |
| 134–154 | `UiDrawStatusBar` | bottom strip |
| 157–174 | `UiDrawTools` | tool palette |
| 177–195 | `UiDrawGetInspectorPanelRect` | inspector geometry helper |
| 196–445 | `UiDrawInsertInspector` | FX inspector (250 lines) |
| 446–604 | `UiDrawLeftTrackPanel` | track-header column (159) |
| 609–690 | `UiDrawBusesPanel` | mixer buses (82) |
| 691–889 | `UiDrawRuler` | timeline ruler (199) |
| 890–1066 | `UiDrawArrangeLanes` | clip lanes + waveforms (177) |
| 1067–1156 | `UiDrawDockLeavesAndSplitters` | dock chrome (90) |
| 1157–1281 | `UiDrawDockDropOverlay` | tab-drag preview (125) |

### Phase 17 plan (mirrors the 16a–16t cadence)

Order chosen smallest → largest so each step builds muscle memory:

1. **17a** — `ui/draw/draw_internal.h` — promote anon-namespace helpers
   (`FormatTimecode`, `DrawKnob`, `DrawFader`, `RoundedFillRect`,
   palette accessors) to a shared internal header. No behavior change.
2. **17b** — `ui/draw/chrome.cpp` — TopBar + Transport + StatusBar +
   Tools (~80 lines). All small, similar style.
3. **17c** — `ui/draw/buses.cpp` — BusesPanel (~82).
4. **17d** — `ui/draw/dock_chrome.cpp` — DockLeavesAndSplitters +
   DockDropOverlay (~215). Cohesive (both render dock UI on top of
   panel content).
5. **17e** — `ui/draw/tracks.cpp` — LeftTrackPanel (~159).
6. **17f** — `ui/draw/inspector.cpp` — InsertInspector + GetInspectorPanelRect (~270).
7. **17g** — `ui/draw/ruler.cpp` — Ruler (~199).
8. **17h** — `ui/draw/arrange.cpp` — ArrangeLanes (~177).
9. **17i** — delete the now-empty `ui/draw.cpp` (or reduce to a 1-line
   "umbrella" if any caller depends on the file existing).

After Phase 17: largest UI TU is `ui/dialogs.cpp` (572) or
`wndproc/lbuttondown.cpp` (586), both already cohesive single-purpose
handlers. The "no file > 600 lines" goal is met for all `apps/daw_gui/`
code that isn't an inherently monolithic vendor/codec/IPC module.

### Public API guarantee

All `UiDraw*` functions keep their declarations in `ui/draw.h`. WM_PAINT
in `ui/wndproc/paint.cpp` and any caller of `UiDrawGetInspectorPanelRect`
in `ui/wndproc/lbuttondown.cpp` see zero churn.

### CMakeLists strategy

Add `ui/draw/*.cpp` files alphabetically next to `ui/draw.cpp`. Final
17i pass removes `ui/draw.cpp` from the source list.

### Risk

LOW. Each function is a self-contained `void UiDrawX(HDC, …, AppState&)`
with no inter-function state. The only non-local sharing is the anon
helpers, which 17a hoists upfront.

---

## 5. Revised hardening plan (resequenced, post-Phase 16)

| # | Step | Status | Effort |
|---|---|---|---|
| 1 | A0/A1/A2/A3/A4 (the WndProc split) | ✅ Done (16a–16t) | — |
| 2 | B `AudioInit` | ✅ Done | — |
| 3 | **I — `ui/draw.cpp` split** | 🔄 Phase 17 (next) | medium |
| 4 | **G — UiRuntimeState split** | ⏳ Phase 18 | medium-large |
| 5 | **F — Repaint policy** | ⏳ Phase 19 | small (mechanical grep) |
| 6 | **H — Floating-window lifetime** | ⏳ Phase 20 | small |
| 7 | **C — Engine-readiness assert + test** | ⏳ Phase 21 | small |
| 8 | **D — Realtime allocation audit** | ⏳ Phase 22 | medium (mostly auditing) |
| 9 | E — Splash | ⏸ Deferred | — |

### Sequencing rationale

- **I before G**: each `UiDraw*` ends up in its own TU, so the eventual
  `UiRuntimeState → {DockState, EditToolState, UiViewState, InspectorState}`
  rewrite can grep one file per consumer.
- **G before F**: the repaint walk needs a stable `DockState::floatingPanels`
  list; doing F first means rewriting it after G.
- **H slot in after F**: the centralized repaint helper makes the
  shutdown sequence easier to reason about (drain floats → save → close).
- **C before D**: assert proves the invariant; D's audit can then trust it.
- **D last**: realtime audits are easiest when the rest of the codebase
  isn't moving.

---

## 6. Risks remaining (post-Phase 16)

The original §6 risk table is now mostly resolved. Surviving items:

| Risk | Status | Addressed by |
|---|---|---|
| Window lifetime (raw `AppState*` in floats) | OPEN | Step H |
| FloatingWindowProc USERDATA-swap re-entry | OPEN, latent | Step H + optional UiEventRouter in a future phase if right-click context menus land in floating panels |
| Single-valued panel hit-rects | OPEN, masked | None (impossible today; revisit if floating Mixer is allowed) |
| Multi-window repaint scheduling | OPEN | Step F |
| Stale `DockNode*` after collapse | OPEN, ad-hoc patches in place | Long-term: hand out opaque `DockNodeId` instead of raw pointers. Out of scope for current hardening pass. |
| main.cpp growth | RESOLVED | 281 lines + each new feature has an obvious destination TU |

---

## 7. Headline change vs previous revision

> The previous revision opened with: *"The original plan treated Step A as
> a tidy housekeeping pass. It is now the load-bearing step."*
>
> **That step is complete.** main.cpp shrank from 4008 → 281 across 20
> sub-phases (16a–16t) with zero test regressions. Every other hardening
> step in this document is now individually small. The remaining work is a
> series of small passes, not a months-long structural overhaul.

The next phase (17) is the natural successor: `ui/draw.cpp` is the only
remaining "kitchen-sink TU" in `apps/daw_gui/`, and its decomposition is
the same mechanical pattern that worked for WndProc — split by surface,
keep public headers stable, ship one TU per micro-phase, verify
build/tests/lint after each.

---

## 8. Phase 16 summary (for the record)

| Sub-phase | Commit | Extracted to | Lines off main.cpp |
|---|---|---|---|
| 16a | — | `ui/draw.cpp` UiDrawDockDropOverlay | -130 |
| 16b | — | `ui/draw.cpp` UiDrawDockLeavesAndSplitters | -90 |
| 16c | — | `ui/wndproc/wheel.{h,cpp}` + `ui/layout` hoists | — |
| 16d | — | `ui/wndproc/timer.cpp` | -58 |
| 16e | — | `ui/wndproc/keys.{h,cpp}` | -140 |
| 16f | — | `ui/wndproc/rbutton.{h,cpp}` | -50 |
| 16g | — | `ui/wndproc/lbutton.{h,cpp}` | -120 |
| 16h | — | `ui/wndproc/mousemove.{h,cpp}` | -240 |
| 16i | — | `ui/floating.{h,cpp}` | — |
| 16j | — | `ui/wndproc/paint.{h,cpp}` | -100 |
| 16k | — | `ui/menu_build.{h,cpp}` | -190 |
| 16l | — | `ui/dialogs.{h,cpp}` | -556 |
| 16m | — | `ui/wndproc/command.{h,cpp}` | -145 |
| 16n | — | (internal split of LBUTTONDOWN) | +26 |
| 16o | a3884fb | `ui/wndproc/lbuttondown.{h,cpp}` | -560 |
| 16p | 0bc5b68 | `ui/dock_drop.{h,cpp}` | -116 |
| 16q | a0b8b4d | `audio/transport_control.cpp` | -241 |
| 16r | 0471fb3 | `audio/import.cpp` | -218 |
| 16s | bcf7f7d | `audio/orchestration.cpp` | -460 |
| 16t | 4f22c02 | (audit: dead code + comment cleanup) | -63 |
| **Total** | | | **-3727** (4008 → 281) |

184/184 tests + layer_lint clean throughout.
