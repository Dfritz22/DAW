# DAW Hardening Plan — Revised (TEMP)

> Temporary working document. Safe to delete once the plan is folded into the
> permanent design notes. Generated after the docking / tear-off / multi-window
> subsystem landed.

---

## 1. Current state (measured)

- `apps/daw_gui/main.cpp` is **3955 lines** — the largest file in the project
  by 3.7×. Next-largest is `ui/draw.cpp` at 1065.
- `WindowProc` alone now spans roughly lines 2225–3877 (~1650 lines) and acts
  as the de-facto god-object: dock splitter drag, tab drag/drop, tear-off,
  redock, panel hit-tests for Transport / Tools / Tracks / Mixer / Browser /
  Arrange, clip drag/trim, fader/pan/knob drag, playhead scrub, popup menus,
  painting, cursor selection.
- `FloatingWindowProc` (~lines 2048–2183) intercepts mouse messages and
  **re-enters `WindowProc` with a swapped `GWLP_USERDATA`** to share the
  hit-test code. It works but is a fragile coupling.
- New dock-only state lives in `UiRuntimeState` mixed with legacy clip/fader
  drag state — no separation between "dock-system state" and "edit-tool
  state."

---

## 2. What still makes sense

- **Step A (split main.cpp)** — *more urgent than before*. main.cpp grew from
  ~3.3k → ~4.0k purely from docking. WindowProc has crossed the threshold
  where any further feature lands in a file no human will fully reason about.
- **Step B (`AudioInit` + `AudioEngineState`)** — still valid and unaffected
  by docking. Audio init currently happens scattered across `wWinMain` and
  dialog Apply paths.
- **Step C (verify SR init / engine readiness)** — still valid; reduce to a
  regression test plus an assert in the engine entry. The bug class is fixed
  but the invariant isn't enforced.
- **Step D (allocation-free realtime path)** — unchanged in scope. Docking
  added zero realtime-thread code.
- **Step E (splash screen)** — keep deferred. Docking made startup do *more*
  (load `layout.json`, lazily build dock tree) but it's still sub-frame.

---

## 3. What needs modification

- **Step A must be re-scoped and re-ordered to run first.** The original plan
  assumed the split was a tidy one-pass extraction. With dock + tear-off
  intertwined with edit-tool code in `WindowProc`, the split is now a
  *prerequisite* for safely doing B and D — otherwise refactors keep
  colliding inside the same 1650-line switch.
- **Step C** should be downgraded from "implementation" to a checklist:
  1. audit `DeviceOpen*` paths,
  2. add one debug assert that `state.audio.activeDeviceSampleRate > 0`
     before the first `StartPlayback`,
  3. keep the unit fix as a regression note.
  No new code module.
- **Step D** should explicitly exclude the dock subsystem. Dock code runs on
  the UI thread only; conflating "no allocations" with dock cleanup will
  muddy both efforts.

---

## 4. What should be added (new steps)

- **Step A0 — Stabilize the dock contract before splitting.** Lock down the
  public surface of dock (`DockNode`, `PanelKind`, `DockLayoutLeaf`,
  drop-target resolution) so the extraction in Step A doesn't have to chase a
  moving target. Concretely:
  - freeze the JSON schema version,
  - document which `UiRuntimeState` fields belong to dock vs edit tools,
  - finish the still-pending floating-panel persistence so the schema
    doesn't change again right after the split.

- **Step A1 — Extract a `UiEventRouter`** that owns the per-message dispatch
  table currently inlined into `WindowProc`. This is what makes
  `FloatingWindowProc`'s USERDATA-swap hack obsolete: the router takes
  `(HWND, AppState&, msg, w, l)` and decides dock vs panel-input routing
  explicitly instead of depending on `hwnd == state.ui.hwnd`.

- **Step F — Multi-window invalidation policy.** Right now any state change
  calls `InvalidateRect(mainHwnd, …)` and the floating-mouse path adds an
  extra invalidate of the main window. There is no rule for "which windows
  must repaint when track N's gain changes." Define one (e.g.
  `state.ui.RequestRepaintAll()` walks `floatingPanels`).

- **Step G — Dock state ownership audit.** `UiRuntimeState` currently owns
  dock tree, drag state, drop preview, floating panel list, *and* every
  clip/fader/knob drag flag. Split into `DockState` + `EditToolState` so
  future panel-input refactors don't have to grep through a 219-line struct.

- **Step H — Floating window lifetime hardening.** Today `WM_DESTROY` deletes
  `FloatingWndData` and re-docks; if `DockReturnPanelToMain` throws, or app
  shutdown destroys the main HWND first, the floating windows leak / crash.
  Need an explicit shutdown sequence (close all floats first, then save
  layout, then destroy main).

---

## 5. What should be removed / deferred further

- **Step E (splash)** — defer indefinitely; revisit only if startup exceeds
  ~250 ms.
- The original Step A's implicit assumption that the split is "zero behavior
  change" — keep the goal, but accept that extracting the dock subsystem
  will surface latent bugs (e.g. the `isMainHwnd` gate currently leans on
  coincidence). Plan for a small bug-fix tail after each extraction.

---

## 6. New architectural risks introduced by docking

| Risk area | Description |
|---|---|
| **Window lifetime** | Floating windows hold `AppState*` raw. Nothing prevents `wWinMain` shutdown from destroying `state` while a floating window's queued messages still target it. WM_DESTROY ordering is currently lucky, not guaranteed. |
| **Event routing** | `FloatingWindowProc` re-enters `WindowProc` after swapping `GWLP_USERDATA` for the duration of one call. Any code path inside `WindowProc` that re-enters the message pump (`TrackPopupMenu`, `MessageBox`, modal dialogs) during that window will see the wrong USERDATA semantics for its hwnd. Today this is masked because mouse handlers don't pump, but a right-click context menu inside a floating panel would break it. |
| **State ownership** | Panel hit-rects (`playRect`, `faderRect`, etc.) are *single-valued* in `UiRuntimeState`. If the same `PanelKind` ever exists in both a docked tab and a floating window (impossible today, but the dock tree doesn't enforce it), the rects ping-pong each paint and clicks land on whichever painted last. |
| **Redraw scheduling** | No central invalidate. WM_TIMER invalidates main only — a floating Mixer with live meters wouldn't repaint during playback. Currently masked because the Mixer doesn't animate. |
| **Multi-window sync** | No notification when a tab is dragged from main to floating. Cached layout pointers (`dragSplitterNode`, `dragTabSource`, `dropTargetLeaf`) are raw `DockNode*` into `state.ui.dockRoot`. A re-dock that collapses a split frees those nodes — handled today only by ad-hoc "if dst was root, re-resolve" patches. |
| **main.cpp growth** | Adding any per-floating-window feature (titlebar buttons, nested dock, panel persistence) lands in main.cpp by default. There is no obvious place else to put it. |

---

## 7. New modularization opportunities

| Proposed module | Source today | Approx. lines |
|---|---|---|
| `ui/dock_input.cpp` | splitter drag + tab drag/drop + drop-target resolve in WindowProc | ~350 |
| `ui/floating_window.cpp` | `FloatingWindowProc`, `SpawnFloatingPanel`, `DockReturnPanelToMain`, class registration | ~200 |
| `ui/panel_input.cpp` | per-panel hit-tests for Play/Stop/Record/BPM/Tools/Mixer fader/Browser etc. | ~600 |
| `ui/event_router.cpp` | top-level WindowProc switch becomes a dispatch table | ~150 |
| `ui/menu.cpp` | `BuildMainMenuBar`, `Populate*Menu`, `RefreshTopLevelPopup`, `HandleMenuCommand` | ~300 |
| `ui/dialogs/audio_settings.cpp` | `AudioSettingsDlgProc` + `ShowAudioSettingsDialog` | ~280 |
| `ui/dialogs/project_sample_rate.cpp` | `ProjectSampleRateDlgProc` + `ShowProjectSampleRateDialog` | ~140 |
| `app/commands.cpp` | `HandleMenuCommand`, transport command implementations | ~250 |
| `app/lifecycle.cpp` | `wWinMain` body, class registration, shutdown ordering | ~150 |

That carves main.cpp down to roughly **<400 lines** (entry point, glue, the
irreducible top-level switch).

---

## 8. Revised hardening plan (sequenced)

1. **A0 — Lock the dock contract.** Finish floating-panel persistence
   (`floating: [...]` in `layout.json`), version the schema
   (`"version": 2`), document the dock public API. *Small, days.*
2. **A1 — Extract dialog modules first** (`audio_settings`,
   `project_sample_rate`, `menu`). Lowest-risk, ~700 lines off main.cpp,
   gives muscle memory for the bigger split. *Mechanical.*
3. **A2 — Extract floating window subsystem** (`ui/floating_window.cpp`).
   At the same time, replace the `GWLP_USERDATA` swap hack with an explicit
   `UiEventRouter::DispatchPanelInput(host, state, panel, msg, w, l)`.
   Removes a sharp edge.
4. **A3 — Extract dock input** (splitter/tab drag/drop, tear-off, redock)
   into `ui/dock_input.cpp`. WindowProc shrinks by ~350 lines; dock state
   stays inside the dock module.
5. **A4 — Extract panel input** into `ui/panel_input.cpp` keyed by
   `PanelKind`. WindowProc no longer knows what a fader is.
6. **G — State ownership split.** `UiRuntimeState` → `DockState` +
   `EditToolState` + `UiViewState`. Touched only after A3/A4 so each
   consumer is now in one file.
7. **F — Repaint policy.** Single `RequestRepaintAll(state)` walking main +
   `floatingPanels`; replace ad-hoc `InvalidateRect(mainHwnd, …)` calls.
8. **H — Lifetime hardening.** Shutdown order: close floats → save layout
   (incl. floats) → destroy main → free state. `FloatingWndData` gains a
   weak-ish "state alive" check (or floats are owned-popups whose
   destruction is forced by main's WM_DESTROY).
9. **B — `AudioInit()` + `AudioEngineState`.** Now safe because main.cpp is
   small enough to read and the dock churn is over.
10. **C — Engine-readiness asserts** (one debug assert + regression test).
    No new module.
11. **D — Allocation-free realtime path.** Unchanged scope, deliberately
    last so dock refactors can't introduce realtime-thread regressions.
12. **E — Splash screen.** Indefinitely deferred.

### Sequencing rationale

- A0–A4 must come before B/D because the realtime / audio-init refactors
  will touch `AppState` and `wWinMain`, both of which are currently buried
  under dock code.
- G/F/H slot in mid-stream because they only become tractable after the
  dock subsystem has its own files.
- The plan now has **9 active steps + 1 deferred**, vs the original 5.
  That reflects the added surface area of docking, not scope creep — each
  new step maps to a concrete risk in §6.

---

## 9. Headline change vs original plan

> The original plan treated Step A as a tidy housekeeping pass.
> **It is now the load-bearing step.** Until main.cpp is split, every other
> hardening item compounds risk because it has to be performed inside a
> 4k-line file whose ownership model is implicit.
