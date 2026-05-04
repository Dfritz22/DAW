# Project Architecture

This DAW is a dual-layer system: a **native Win32 C++ application** (the actual DAW GUI and audio engine) and a **Python offline AI pipeline** (mix analysis, auto-mix, mastering, and vocal checks). Both layers share common conceptual models (project, timeline, transport) but are implemented independently and communicate through file I/O (WAV stems, JSON reports, `.dawproj` files).

---

## Directory Tree

```
Daw/
├── apps/
│   ├── daw_gui/                  # ← The real DAW — Win32 GUI + audio engine
│   │   ├── CMakeLists.txt        #   Build target definition for daw_gui.exe
│   │   ├── state.h               #   All shared types, constants, enums, and UiState struct
│   │   ├── draw.h                #   Declarations for all draw functions + layout constants
│   │   ├── draw.cpp              #   Every rendering function (panels, waveforms, knobs, etc.)
│   │   └── main.cpp              #   Audio engine, transport, WndProc, project I/O, menus
│   │
│   └── daw_host/                 # Minimal C++ console host for engine smoke-tests
│       ├── CMakeLists.txt        #   Build target for daw_host.exe
│       └── main.cpp              #   Creates a daw_core::Engine, calls renderStub(), prints sample count
│
├── packages/
│   └── daw_core/                 # Shared C++ library (engine, project, timeline, transport)
│       ├── CMakeLists.txt        #   Builds daw_core.lib; exposes include path to dependents
│       ├── include/
│       │   └── daw_core/
│       │       ├── Engine.hpp    #   Top-level engine class; owns Project, Timeline, Transport
│       │       ├── Project.hpp   #   Project + TrackClip structs (name, SR, tempo, time-sig, clips)
│       │       ├── Timeline.hpp  #   Tempo/SR math — seconds-per-beat, samples-per-beat, bar→sample
│       │       └── Transport.hpp #   Playback state machine (play/stop/record/rewind, playhead pos)
│       └── src/
│           └── Engine.cpp        #   Engine constructor + renderStub() (returns silent stereo buffer)
│
├── src/
│   └── daw_ai/                   # Python offline AI/analysis pipeline (installable package)
│       ├── __init__.py           #   Package marker; exports: models, io, features, rules, report
│       ├── __main__.py           #   Entry point for `python -m daw_ai` — delegates to cli.main()
│       ├── cli.py                #   `daw-ai-analyze` CLI: arg parsing, orchestrates all pipeline stages
│       ├── models.py             #   Pure dataclasses: TrackFeatures, Recommendation, AnalysisResult, EffectSuggestion
│       ├── io.py                 #   Filesystem helpers: find_audio_files(), filter_selected(), parse_groups()
│       ├── features.py           #   Audio feature extraction: peak, RMS, LUFS, crest factor, spectral centroid/rolloff
│       ├── rules.py              #   Rule-based mix recommendations (gain staging, masking, role inference)
│       ├── dsp.py                #   DSP primitives: highpass/lowpass/shelf/peaking EQ, compression, limiting, normalization
│       ├── effects.py            #   Effect-chain inference: reads TrackFeatures → EffectSuggestion objects (no real-time processing)
│       ├── auto_mix.py           #   Iterative auto-mix engine: scores candidate EQ/comp/gain moves, keeps only improvements
│       ├── mastering.py          #   Offline mastering chain: HP → EQ → M/S width → glue comp → LUFS normalize → true-peak limiter
│       ├── mix_readiness.py      #   Mix readiness gate: 11 checks (masking, headroom, mono compat, sibilance, etc.)
│       ├── vocal_check.py        #   Vocal pitch/key/chord analysis using Krumhansl–Schmuckler key profiles
│       ├── report.py             #   Writes analysis.json and report.md from AnalysisResult
│       └── daw/                  # Python mirror of daw_core concepts (for scripting / AI bridge)
│           ├── __init__.py       #   Package marker for daw sub-module
│           ├── engine.py         #   DawEngine dataclass: holds DawProject + Timeline + TransportState
│           ├── project.py        #   DawProject + TrackClip dataclasses (Python equivalent of Project.hpp)
│           ├── timeline.py       #   Timeline dataclass: tempo/SR math (seconds_per_beat, samples_per_beat, bar→sample)
│           └── transport.py      #   TransportState dataclass: play/stop/record/rewind, playhead_sample
│
├── docs/
│   ├── ARCHITECTURE.md           #   This file
│   ├── GOALS.md                  #   Product goal guardrails — read before adding any AI feature
│   └── ROADMAP.md                #   Milestone and feature backlog
│
├── analysis_out/                 # Generated output from the Python pipeline (not committed)
│   ├── analysis.json             #   Per-track feature data from the last analysis run
│   ├── report.md                 #   Human-readable mix recommendations from the last run
│   ├── auto_mix_best.json        #   Best auto-mix pass results (gains, EQ moves, scores)
│   ├── auto_mix_best.md          #   Human-readable summary of best auto-mix pass
│   ├── auto_mix_decent.json/md   #   Deliberate "decent" quality test mode output
│   ├── auto_mix_bad.json/md      #   Deliberate "bad" quality test mode output
│   ├── unmixed_sum.json/md       #   Baseline unmixed render metrics
│   ├── mix_best_final_master.json#   Post-mastering metrics for the best mix
│   ├── *_vocal_check.json/txt    #   Per-stem pitch/key/chord analysis output
│   └── mastered/                 #   Mastered WAV output and its analysis
│       ├── analysis.json
│       ├── report.md
│       └── test_automix_master.json
│
├── examples/
│   └── Readme.txt                #   Instructions for putting stems in this folder for analysis
│
├── CMakeLists.txt                #   Root CMake: sets C++17, adds packages/ and apps/ subdirs
├── pyproject.toml                #   Python package config: name=daw-ai-assistant, entry point daw-ai-analyze
├── requirements.txt              #   Python runtime deps: numpy, scipy, librosa, soundfile, pyloudnorm
├── README.md                     #   Project overview, quick-start, build steps, CLI usage examples
└── Daw.code-workspace            #   VS Code multi-root workspace file
```

---

## Layer Summary

### C++ DAW (`apps/daw_gui`)

The production DAW. A single Win32 native application with no external GUI framework dependencies. All audio is handled via Windows WinMM (MME) or WASAPI. The codebase is intentionally split into four files:

| File | Responsibility |
|---|---|
| `state.h` | Single source of truth for all types, constants, command IDs, and the `UiState` struct that every subsystem reads from |
| `draw.h` | Forward declarations for all draw functions; layout constants (`kInsp*`, `kLeftPanelWidth`, etc.) shared between draw and hit-test code |
| `draw.cpp` | All painting: top bar, transport, left track panel, ruler, arrange lanes, clip waveforms, insert inspector, knobs, buttons |
| `main.cpp` | Everything runtime: `WinMain`, `WndProc`, audio threads (MME/WASAPI), transport logic, project load/save, menus, device management, resampling |

### C++ Core Library (`packages/daw_core`)

A clean-room shared library stub. Its headers define the canonical data model (`Project`, `TrackClip`, `Timeline`, `Transport`) that both the C++ host and, by parallel design, the Python `daw/` sub-module mirror. The `Engine::renderStub()` method currently returns silence; this is the integration point for future real-time rendering.

### Python AI Pipeline (`src/daw_ai`)

An offline batch processor installed as the `daw-ai-assistant` package. Invoked via `daw-ai-analyze` CLI. Stages run in order:

```
find_audio_files → extract_track_features → apply_rules → generate_effects_report
                                                        → run_iterative_auto_mix
                                                        → master_file
                                                        → vocal_check
                                                        → write_json / write_markdown
```

The `daw/` sub-package is a Python mirror of `daw_core` — pure dataclasses with no audio I/O — used by scripting and future AI-to-DAW bridge work.
