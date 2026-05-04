# DAW AI North Star and Implementation Guardrails

This document is the decision filter for all new work.

If a task does not improve recording, mix, or master outcomes for real users with real hardware constraints, it is not a priority.

## North Star

Build a DAW with an AI engineer that helps users get the best possible result with the hardware and room they actually have.

- A home setup and a high-end studio will not produce identical results.
- Both should be measurably better after following AI guidance.
- Guidance must be practical, specific, and prioritized by impact.

## Product Promise

For every stage (recording, editing, mixing, mastering), the AI should:

1. Detect the biggest bottlenecks first.
2. Explain the issue in plain language.
3. Recommend the highest-impact fix the user can do now.
4. Point to exact time ranges or tracks when relevant.
5. Re-evaluate after changes and show improvement.

## Stage Gates

Do not optimize later stages until earlier gates are passing.

1. Capture Readiness
- Noise floor, clipping, room/reflection issues, vocal intelligibility, gain staging.
- Output: pass/fail plus actionable setup fixes.

2. Performance Quality
- Phrase-level pitch and timing reliability, sustained-note stability.
- Output: phrase re-record map with timestamps and severity.

3. Mix Readiness
- Track cleanup, masking risks, level consistency, arrangement conflicts.
- Output: corrective moves before creative mixing.

4. Mix Assist
- Levels, panning, EQ/compression/de-essing/reverb suggestions.
- Output: starting moves and confidence level.

5. Master Readiness
- Loudness, true peak, dynamics, tonal balance, translation risk.
- Output: blockers and finalization guidance.

## Hard Requirements for New Features

A change is only complete if it satisfies all of the following:

1. Improves one stage gate metric or recommendation quality.
2. Does not regress known good scenarios.
3. Produces user-facing guidance, not just internal numbers.
4. Includes confidence or uncertainty handling for weak signals.
5. Has at least one realistic test case (good take and bad take).

## Anti-Side-Track Rules

- Do not add features that only look impressive but do not change user outcomes.
- Do not optimize synthetic-only benchmarks at the cost of real recordings.
- Do not rely on a single global quality score without diagnostic breakdown.
- Do not hide uncertainty when confidence is low.

## Implementation Review Checklist (Run Before Coding)

1. Which stage gate does this change improve?
2. What user decision becomes easier because of it?
3. What is the measurable success metric?
4. What is the failure mode on poor mic/poor room recordings?
5. What existing behavior could regress?

If any answer is unclear, refine scope before implementation.

## Definition of Done for AI Features

1. Detects issue reliably on at least one real-world bad recording.
2. Keeps a known good recording from being falsely flagged.
3. Produces specific, actionable recommendations.
4. Reports confidence and avoids over-claiming.
5. Integrates into the existing report/UI flow.
