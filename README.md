# DAW AI Assistant (MVP)

This project is a first-step offline AI mix assistant. It analyzes individual tracks or selected track groups and produces:

- objective track metrics (peak, RMS, LUFS, crest factor, spectral features)
- mix recommendations (EQ, gain staging, masking fixes)
- tracking suggestions (mic placement and hardware gain guidance)
- machine-readable output (`analysis.json`)
- human-readable report (`report.md`)

## Why this first

This gives you immediate value without building a full real-time DAW engine yet. Once this analysis and recommendation layer is solid, you can attach it to a JUCE app/plugin for real-time workflows.

## Goal Guardrail (Read Before Implementing)

Before adding any new AI feature or tuning logic, check the project goal document:

- `docs/GOALS.md`

This is the source of truth for avoiding side tracks and keeping implementation aligned with the core product promise: an AI engineer that helps users maximize results with the hardware and room they actually have.

Quick pre-implementation check:

1. Identify the stage gate this change improves.
2. Define how this helps a concrete user decision.
3. Define measurable success and likely failure modes.
4. Confirm no regression on known good takes.
5. Ensure output includes actionable user guidance.

## Quick Start

1. Create a virtual environment and install dependencies.

```powershell
python -m venv .venv
.\.venv\Scripts\Activate.ps1
pip install -r requirements.txt
pip install -e .
```

2. Put stems in a folder (wav/flac/aiff).

3. Run analysis:

```powershell
daw-ai-analyze --input-dir .\examples\stems --output-dir .\analysis_out
```

4. Optional: analyze only selected tracks.

```powershell
daw-ai-analyze --input-dir .\examples\stems --select vocal.wav kick.wav bass.wav
```

5. Optional: add explicit groups.

```powershell
daw-ai-analyze --input-dir .\examples\stems --group rhythm:kick.wav,bass.wav --group vocals:lead_vocal.wav,backing_vocal.wav
```

## Output

- `analysis_out/analysis.json`
- `analysis_out/report.md`

## Iterative Auto-Mix Demo

You can now run an iterative mix loop that:

- renders a preview mix each iteration
- scores the result
- tests candidate EQ, compression, de-essing, filtering, and gain actions
- keeps only score-improving moves for the next pass

Render a true unmixed baseline:

```powershell
daw-ai-analyze --input-dir .\examples --output-dir .\analysis_out --render-unmixed
```

Run best (iterative) mode:

```powershell
daw-ai-analyze --input-dir .\examples --output-dir .\analysis_out --auto-mix --mix-mode best --iterations 8 --preview-sec 20
```

Run best mode against a mixed/mastered reference target:

```powershell
daw-ai-analyze --input-dir .\examples --output-dir .\analysis_out --auto-mix --mix-mode best --iterations 8 --preview-sec 20 --reference-file .\full_preview\ImAlright_Full_Preview.mp3
```

Run deliberate quality test modes:

```powershell
daw-ai-analyze --input-dir .\examples --output-dir .\analysis_out --auto-mix --mix-mode bad --preview-sec 20
daw-ai-analyze --input-dir .\examples --output-dir .\analysis_out --auto-mix --mix-mode decent --preview-sec 20
```

Auto-mix output files:

- `analysis_out/auto_mix_bad.json`
- `analysis_out/auto_mix_bad.md`
- `analysis_out/auto_mix_decent.json`
- `analysis_out/auto_mix_decent.md`
- `analysis_out/auto_mix_best.json`
- `analysis_out/auto_mix_best.md`
- `analysis_out/mix_bad_final.wav`
- `analysis_out/mix_decent_final.wav`
- `analysis_out/mix_best_final.wav`
- `analysis_out/unmixed_sum.wav`
- `analysis_out/unmixed_sum.json`
- `analysis_out/unmixed_sum.md`
- `analysis_out/mix_previews/*.wav`

## Next Milestones

- Add plugin parameter suggestion mapping for popular channel strip plugins
- Add section-aware analysis (verse/chorus)
- Add iterative auto-mix pass generation
- Add mastering profile and quality gates

## Bare-Bones DAW Workspace Layout

The DAW runtime direction is C++.
Python remains the AI/offline assistant layer.

- `packages/daw_core`: C++ DAW core library (engine, project, timeline, transport)
- `apps/daw_host`: Minimal C++ host executable for local engine tests
- `src/daw_ai`: Python AI/offline pipeline (analysis, auto-mix, mastering)
- `apps/cli`: Optional Python app wrappers/orchestration
- `projects`: Session/project files and templates
- `tests`: Unit and integration tests
- `docs`: Architecture and design docs

Open `Daw.code-workspace` in VS Code if you want a multi-folder workspace view.

## Build C++ Skeleton

```powershell
cmake -S . -B .\build
cmake --build .\build --config Release
.\build\apps\daw_host\Release\daw_host.exe
```

## Build DAW GUI (Windows)

Install CMake once:

```powershell
winget install --id Kitware.CMake -e --accept-source-agreements --accept-package-agreements
```

Build and run the GUI app:

```powershell
cmake -S . -B .\\build
cmake --build .\\build --config Release --target daw_gui
.\\build\\apps\\daw_gui\\Release\\daw_gui.exe
```

Current GUI scope (bare bones):
- Native Win32 window
- Transport buttons (Play/Pause, Stop)
- Basic timeline grid
- C++ engine instantiated at startup
