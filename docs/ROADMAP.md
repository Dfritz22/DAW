# DAW Development Roadmap

Reference GOALS.md before starting any milestone. Each item here must map to at
least one stage gate improvement (Capture / Performance / Mix Readiness / Mix
Assist / Master Readiness).

---

## Current State (May 2026)

### Working
- Win32 GUI: Import WAV, arrange clips, play/stop, recording via WaveIn
- Track controls: gain fader, mute, solo, record-arm
- Vocal Check: per-track analysis pipeline (key, tempo, pitch, timing, scoring)
- AutoMix: iterative gain staging applied back to faders
- Python AI layer: analysis, auto-mix, mastering, effects recommendations

### Missing (blocking daily-driver use)
- Project save / load (session lost on close)
- Undo / redo
- Clip editing (split, trim, fade, nudge)
- Bus / routing / send structure
- Bounce / export mix
- AI stage-gate UI (capture coach, phrase re-record map)

---

## Milestone 1 — Session Persistence (target: 2 weeks)

Goal: A user can save their session and re-open it exactly as they left it.

**DAW tasks**
- [ ] JSON project serialization in C++ (tracks, clips, gains, mutes, solos, BPM)
- [ ] Save Project (File > Save / Ctrl+S) — write `*.dawproj` file
- [ ] Save As (File > Save As) — open Save dialog
- [ ] Open Project (File > Open / Ctrl+O) — load `*.dawproj` and reconstruct session
- [ ] Recent files list (optional but fast win)
- [ ] Title bar shows project name and modified-flag (`*`)

**AI task aligned to this milestone**
- [ ] Capture Readiness: at load-time, scan all tracks for noise floor, clipping
  risk, and warn if any track fails the Capture gate before mixing begins.

Gate check: Does this help a user not lose their work and identify capture
problems before mixing? Yes.

---

## Milestone 2 — Non-Destructive Editing (target: 2 weeks)

Goal: A user can fix mistakes without re-recording the whole take.

**DAW tasks**
- [ ] Undo / redo command stack (10+ levels)
- [ ] Clip split at playhead (S key)
- [ ] Clip trim (drag left/right clip edges)
- [ ] Clip nudge (arrow keys, snap-to-beat option)
- [ ] Clip duplicate (Ctrl+D)
- [ ] Linear fades on clip edges (draw on clip corners)
- [ ] Delete selected clip (Delete key)

**AI task aligned to this milestone**
- [ ] Phrase re-record map: after Vocal Check, highlight specific clip regions
  in the timeline that need re-recording (pitch/timing fail zones, coloured
  overlay on clip).

Gate check: Does this give the user the tools to act on AI advice without
re-recording the full take? Yes.

---

## Milestone 3 — Mix Routing Foundation (target: 2 weeks)

Goal: Enough bus structure to apply mix moves meaningfully.

**DAW tasks**
- [ ] Fixed buses: Drums, Music, Vocals, Master
- [ ] Track-to-bus assignment (click in track panel)
- [ ] Bus gain and mute
- [ ] Per-track and per-bus pan control
- [ ] Insert chain placeholder (empty slot list per track / bus)
- [ ] Bounce / Export Mix (File > Export WAV) — renders final mix to file

**AI task aligned to this milestone**
- [ ] Mix Readiness gate: detect masking between Vocals and Music bus, flag
  low-end conflicts on Drums vs Bass, output plain-language corrective moves
  before the user starts creative mixing.

Gate check: Does this give AI a real bus structure to reason about and the user
real controls to execute the suggestions? Yes.

---

## Milestone 4 — Mix Assist AI (target: 2 weeks)

Goal: AI proposes a complete starting mix the user can accept or adjust.

**DAW tasks**
- [ ] Basic compressor DSP (threshold, ratio, attack, release) per insert slot
- [ ] Basic parametric EQ DSP (3-4 bands) per insert slot
- [ ] De-esser per insert slot
- [ ] Apply AutoMix pushes EQ + compression settings, not just gain
- [ ] In-app progress panel for AI processing (replace MessageBox popups)

**AI task aligned to this milestone**
- [ ] Mix Assist: extend AutoMix to produce EQ/compression moves and map them
  into the real insert slots, not just gain.
- [ ] Master Readiness gate: loudness, true peak, tonal balance check with
  specific corrective actions before bouncing final mix.

Gate check: Does this take the AI from "advisor" to "hands-on engineer"? Yes.

---

## Milestone 5 — Hardware-Aware Capture Coach (target: 2 weeks)

Goal: The AI coaches recording setup based on what it hears, not what the user
tells it.

**DAW tasks**
- [ ] Input monitoring (zero-latency or low-latency pass-through via WaveIn)
- [ ] Pre-record level meter and clip warning in UI
- [ ] Record countdown + metronome click output

**AI task aligned to this milestone**
- [ ] Capture Coach: on every recorded take, immediately analyse noise floor,
  room reverb tail, mic distance consistency, plosive/sibilance risk.
  Output: "Move mic 3-4 in closer" or "Add absorber behind mic" before the
  user commits the take.
- [ ] Hardware profile inference: infer probable mic/interface tier from SNR
  and noise character; tailor advice to what is physically achievable with
  that setup.

Gate check: Is this the highest-impact improvement for a $50 mic in a bad room?
Yes. This is the core product promise.

---

## Milestone 6 — Phrase-Level Performance AI (target: 2 weeks)

Goal: The AI acts like a producer/engineer who listens to every take and marks
exactly what needs fixing.

**AI tasks**
- [ ] Segment vocal take into musical phrases (silence-gated)
- [ ] Per-phrase pitch stability score (IQR, sustained drift)
- [ ] Per-phrase timing placement relative to reference groove
- [ ] Output phrase re-record map: list of `{start_sec, end_sec, reason, severity}`
- [ ] Highlight fail-zones on clip in timeline (Milestone 2 overlay)
- [ ] CREPE or Basic Pitch integration for reliable pitch extraction on weak mics

**DAW tasks aligned**
- [ ] Click a highlighted phrase zone to solo-play that region only
- [ ] One-click punch-in record from a highlighted zone

Gate check: Does a user leave this milestone knowing exactly which 4 bars to
re-record and why? Yes.

---

## Milestone 7 — Clip Pitch Editor (Melodyne-style) (target: 3 weeks)

Goal: For monophonic clips (vocals, bass, solo instruments), the user can see
individual pitched notes, drag them to correct pitch, and export the corrected
audio — offline, non-destructive.

**Scope (in)**
- Monophonic audio only (one pitch at a time per clip)
- Offline workflow: analyze → edit → apply on export/bounce
- Note display overlay on clip in the timeline or a dedicated editor panel

**Scope (out — future)**
- Real-time pitch correction during playback
- Polyphonic pitch detection (chords, full mix)
- Formant-preserving pitch shifting (stretch beyond ±6 semitones)

**DAW tasks**
- [ ] YIN pitch detection: analyze selected clip, output list of
  `{start_frame, end_frame, detected_hz, corrected_hz}`
- [ ] Note segmentation: group stable pitch regions into discrete note objects
  (onset detection via amplitude + pitch continuity)
- [ ] Clip pitch editor panel: piano-roll-style overlay showing note blobs;
  open on double-click of a clip
- [ ] Note drag: drag a note blob vertically to set `corrected_hz`;
  snap to nearest semitone (with toggle for free/snap)
- [ ] PSOLA pitch shifter: Pitch Synchronous Overlap Add to render corrected
  audio per note segment; apply on bounce/export
- [ ] Per-note bypass toggle: right-click note → bypass correction for that note
- [ ] Save/load pitch edits: store note list with correction offsets in `.dawproj`

**AI task aligned to this milestone**
- [ ] Auto-pitch correction: using phrase pitch data from M6 Vocal Check,
  propose `corrected_hz` values automatically (nearest scale degree given
  key detection); user can accept, adjust, or reject per note.

Gate check: Can a user fix an out-of-tune vocal phrase without re-recording,
using AI as a starting point and manual drag as the fallback? Yes.

**Quality target — matching Melodyne naturalness**

Basic PSOLA alone reaches ~70-80% of Melodyne quality. The remaining gap is
closed by three specific techniques — plan for all three from the start:

1. **Formant preservation** (non-negotiable for naturalness)
   When pitch shifts up, the vocal tract resonances (formants) must stay
   anchored or the result sounds chipmunk/monster. Requires spectral envelope
   estimation (LPC or cepstrum) before the PSOLA step. ~300-400 extra lines
   of DSP. This is the single biggest quality differentiator.

2. **Note transition smoothing**
   Model pitch glide between notes (vibrato, portamento) rather than hard-
   quantizing. Prevents the "auto-tune robot" artifact on small corrections.

3. **Phase-coherent overlap-add windowing**
   A naive PSOLA window produces flutter on consonants (T, P, S). Careful
   window tuning eliminates this.

**Research to read before implementing M7:**
- Moulines & Charpentier (1990) — original PSOLA paper
- de Cheveigné & Kawahara (2002) — YIN pitch detector (best royalty-free
  monophonic detector; used by virtually all open-source tools)
- Morise (2016) — WORLD vocoder (open source, formant-preserving, production
  TTS quality; can be embedded directly as the pitch-shift engine)

WORLD vocoder is the recommended engine: it handles formant preservation and
produces natural-sounding output at large pitch shifts. Integrating it as a
C++ static library is the lowest-risk path to Melodyne-level quality.

---

## Standing Rules

1. Every item must satisfy the Definition of Done in GOALS.md before closing.
2. AI features are only added when there is a DAW control to execute the advice.
3. Scoring and recommendations must include confidence level and uncertainty
   handling — never over-claim on a weak signal.
4. After each milestone, run both the good take and bad take through Vocal Check
   to confirm no regressions before moving on.
