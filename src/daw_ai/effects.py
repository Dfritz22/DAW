"""
Effect inference: analyse track audio features and suggest a processing chain.

This module does NOT apply effects at runtime.  It reads TrackFeatures and
returns structured EffectSuggestion objects that can be written to the mix
report, displayed in a UI, or fed into an LLM for natural-language explanation.

Design rationale
----------------
Professional mixes always contain effects that cannot be inferred from a simple
level/frequency analysis — but several strong correlations do exist:

 - High crest factor (> 20 dB) on transient instruments → parallel compression
   or heavy limiting.
 - High air-band energy (> 0.20) on a vocal → pre-delay reverb or tape
   saturation (tape rolls off harshness while adding warmth).
 - Very low low-band ratio on bass (< 0.60 of its own spectrum) → harmonic
   saturation/drive to add sub presence on small speakers.
 - Narrow spectral centroid on guitars → stereo widening.

All numeric thresholds here are per-track, not mix-bus.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any

from .models import TrackFeatures

EPSILON = 1e-12


@dataclass
class EffectSuggestion:
    effect_type: str        # "reverb" | "delay" | "saturation" | "compression" | "eq" | "stereo_width" | "bus_compression"
    preset_name: str        # human-readable preset/style label
    parameters: dict[str, Any] = field(default_factory=dict)
    reason: str = ""
    priority: str = "recommended"   # "recommended" | "optional" | "consider"


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _crest_db(feature: TrackFeatures) -> float:
    return feature.peak_dbfs - feature.rms_dbfs


def _is_transient_rich(feature: TrackFeatures) -> bool:
    return _crest_db(feature) > 18.0


def _is_highly_dynamic(feature: TrackFeatures) -> bool:
    return _crest_db(feature) > 22.0


# ---------------------------------------------------------------------------
# Per-effect inference
# ---------------------------------------------------------------------------

def _reverb_suggestions(feature: TrackFeatures, role: str) -> list[EffectSuggestion]:
    suggestions: list[EffectSuggestion] = []

    if role == "vocal":
        # Plate reverb is the default vocal reverb.  Add more pre-delay when the
        # vocal is punchy (high crest) to keep the attack dry.
        pre_delay_ms = 25 if _is_transient_rich(feature) else 15
        decay_s = 1.2 if feature.air_band_ratio < 0.12 else 1.8
        suggestions.append(
            EffectSuggestion(
                effect_type="reverb",
                preset_name="Plate Reverb",
                parameters={
                    "pre_delay_ms": pre_delay_ms,
                    "decay_s": decay_s,
                    "mix_percent": 20,
                    "high_cut_hz": 8000,
                },
                reason=(
                    f"Vocal has {'high' if feature.air_band_ratio > 0.16 else 'moderate'} "
                    f"air-band energy ({feature.air_band_ratio:.3f}); plate with pre-delay "
                    f"{pre_delay_ms} ms keeps attack upfront."
                ),
                priority="recommended",
            )
        )

    elif role == "snare":
        if _is_highly_dynamic(feature):
            # Gated reverb enhances very punchy snares
            suggestions.append(
                EffectSuggestion(
                    effect_type="reverb",
                    preset_name="Gated Room",
                    parameters={
                        "pre_delay_ms": 5,
                        "decay_s": 0.4,
                        "gate_threshold_db": -30,
                        "mix_percent": 30,
                    },
                    reason=(
                        f"Snare crest factor is very high ({_crest_db(feature):.1f} dB); "
                        "gated room adds impact without wash."
                    ),
                    priority="recommended",
                )
            )
        else:
            suggestions.append(
                EffectSuggestion(
                    effect_type="reverb",
                    preset_name="Small Room",
                    parameters={"pre_delay_ms": 8, "decay_s": 0.6, "mix_percent": 25},
                    reason="Room reverb adds snare body and acoustic glue.",
                    priority="recommended",
                )
            )

    elif role in {"guitar", "keys"}:
        decay_s = 1.5 if feature.air_band_ratio < 0.10 else 1.0
        suggestions.append(
            EffectSuggestion(
                effect_type="reverb",
                preset_name="Room Reverb",
                parameters={
                    "pre_delay_ms": 12,
                    "decay_s": decay_s,
                    "mix_percent": 18,
                    "low_cut_hz": 200,
                },
                reason=f"Room reverb places {role} in an acoustic space; low-cut keeps bottom clean.",
                priority="recommended",
            )
        )

    elif role == "overheads":
        # Overheads already capture room naturally; only a gentle hall tail for depth
        if feature.air_band_ratio < 0.15:
            suggestions.append(
                EffectSuggestion(
                    effect_type="reverb",
                    preset_name="Large Hall (subtle)",
                    parameters={"pre_delay_ms": 20, "decay_s": 2.0, "mix_percent": 10},
                    reason="Overheads have low air energy — subtle hall adds cymbal shimmer.",
                    priority="optional",
                )
            )

    elif role in {"kick", "bass"}:
        # Minimal to no reverb; very short ambience only if needed for glue
        suggestions.append(
            EffectSuggestion(
                effect_type="reverb",
                preset_name="Tight Room (2–5 ms)",
                parameters={"pre_delay_ms": 0, "decay_s": 0.05, "mix_percent": 8},
                reason=f"Very short ambience provides glue for {role} without muddying the low end.",
                priority="consider",
            )
        )

    return suggestions


def _delay_suggestions(feature: TrackFeatures, role: str) -> list[EffectSuggestion]:
    suggestions: list[EffectSuggestion] = []

    if role == "vocal":
        # Eighth-note delay for rhythmic width; long pre-delay pushes it out of the way
        suggestions.append(
            EffectSuggestion(
                effect_type="delay",
                preset_name="1/8 Stereo Delay",
                parameters={
                    "time_note": "1/8",
                    "feedback_percent": 25,
                    "high_cut_hz": 5000,
                    "mix_percent": 18,
                    "offset_hint": "sync to project BPM",
                },
                reason="Slap/echo delay adds vocal width and depth without masking the dry signal.",
                priority="recommended",
            )
        )
        if feature.presence_band_ratio > 0.20:
            # Bright vocal — a second long delay tucks under presence
            suggestions.append(
                EffectSuggestion(
                    effect_type="delay",
                    preset_name="1/4 Tail Delay",
                    parameters={
                        "time_note": "1/4",
                        "feedback_percent": 15,
                        "high_cut_hz": 3500,
                        "mix_percent": 10,
                    },
                    reason=(
                        f"Vocal presence ratio is high ({feature.presence_band_ratio:.3f}); "
                        "dark delay tail smooths the overall brightness."
                    ),
                    priority="optional",
                )
            )

    elif role == "guitar":
        suggestions.append(
            EffectSuggestion(
                effect_type="delay",
                preset_name="Dotted 1/8 Slapback",
                parameters={
                    "time_note": "dotted 1/8",
                    "feedback_percent": 20,
                    "mix_percent": 14,
                    "high_cut_hz": 6000,
                    "offset_hint": "sync to project BPM",
                },
                reason="Classic guitar delay adds rhythmic space and perceived stereo width.",
                priority="optional",
            )
        )

    elif role == "keys":
        suggestions.append(
            EffectSuggestion(
                effect_type="delay",
                preset_name="Ping-Pong 1/4",
                parameters={
                    "time_note": "1/4",
                    "feedback_percent": 30,
                    "mix_percent": 12,
                    "high_cut_hz": 8000,
                },
                reason="Ping-pong delay widens keys without extra reverb wash.",
                priority="optional",
            )
        )

    return suggestions


def _saturation_suggestions(feature: TrackFeatures, role: str) -> list[EffectSuggestion]:
    suggestions: list[EffectSuggestion] = []
    crest = _crest_db(feature)

    if role == "bass":
        if crest > 22.0:
            # Very peaky bass: fundamentals are present but upper harmonics are sparse
            suggestions.append(
                EffectSuggestion(
                    effect_type="saturation",
                    preset_name="Tube Drive (light)",
                    parameters={"drive_db": 3, "blend_percent": 40, "style": "tube"},
                    reason=(
                        f"Bass crest factor is {crest:.1f} dB — high transients, low sustain. "
                        "Tube drive adds 2nd/3rd harmonics so the bass translates on small speakers."
                    ),
                    priority="recommended",
                )
            )
        elif feature.low_band_ratio > 0.80:
            suggestions.append(
                EffectSuggestion(
                    effect_type="saturation",
                    preset_name="Harmonic Enhancer",
                    parameters={"harmonics": "2nd+3rd", "blend_percent": 30},
                    reason=(
                        f"Bass low-band ratio is very high ({feature.low_band_ratio:.3f}); "
                        "adding harmonics improves presence on systems without subwoofers."
                    ),
                    priority="recommended",
                )
            )

    elif role == "vocal":
        if crest > 20.0:
            suggestions.append(
                EffectSuggestion(
                    effect_type="saturation",
                    preset_name="Tape Saturation",
                    parameters={"drive_db": 1.5, "blend_percent": 50, "style": "tape"},
                    reason=(
                        f"Vocal crest factor is {crest:.1f} dB; tape saturation adds warmth, "
                        "glues transients, and reduces digital harshness."
                    ),
                    priority="recommended",
                )
            )
        if feature.air_band_ratio > 0.20:
            suggestions.append(
                EffectSuggestion(
                    effect_type="saturation",
                    preset_name="Warm Transformer",
                    parameters={"drive_db": 1.0, "style": "transformer"},
                    reason=(
                        f"Vocal air-band ratio is high ({feature.air_band_ratio:.3f}); "
                        "transformer saturation rounds off harsh high-frequency content."
                    ),
                    priority="optional",
                )
            )

    elif role == "guitar":
        if crest > 22.0:
            suggestions.append(
                EffectSuggestion(
                    effect_type="saturation",
                    preset_name="Tape / Console Saturation",
                    parameters={"drive_db": 2, "blend_percent": 35, "style": "tape"},
                    reason=(
                        f"Guitar crest is {crest:.1f} dB; adding tape saturation helps "
                        "sustain notes and glues pick attack into the mix."
                    ),
                    priority="optional",
                )
            )

    elif role == "overheads" and feature.air_band_ratio > 0.25:
        suggestions.append(
            EffectSuggestion(
                effect_type="saturation",
                preset_name="Vintage Tube (subtle)",
                parameters={"drive_db": 0.5, "blend_percent": 20, "style": "tube"},
                reason=(
                    f"Overheads have high air energy ({feature.air_band_ratio:.3f}); "
                    "subtle tube saturation smooths harsh cymbal transients."
                ),
                priority="optional",
            )
        )

    return suggestions


def _compression_suggestions(feature: TrackFeatures, role: str) -> list[EffectSuggestion]:
    suggestions: list[EffectSuggestion] = []
    crest = _crest_db(feature)

    if role == "vocal":
        ratio = 4 if crest > 22 else 3
        suggestions.append(
            EffectSuggestion(
                effect_type="compression",
                preset_name="Optical Vocal Compressor",
                parameters={
                    "ratio": f"{ratio}:1",
                    "threshold_db": -24,
                    "attack_ms": 10,
                    "release_ms": 60,
                    "makeup_db": round(crest * 0.12, 1),
                },
                reason=(
                    f"Vocal crest is {crest:.1f} dB; optical-style compression smooths "
                    "dynamics while preserving breath and consonant detail."
                ),
                priority="recommended",
            )
        )
        if crest > 20.0:
            suggestions.append(
                EffectSuggestion(
                    effect_type="compression",
                    preset_name="Peak Limiter (safety)",
                    parameters={"ceiling_db": -3, "release_ms": 40},
                    reason="Safety brick-wall limit prevents harsh vocal transient peaks.",
                    priority="optional",
                )
            )

    elif role == "kick":
        suggestions.append(
            EffectSuggestion(
                effect_type="compression",
                preset_name="FET Punch (fast)",
                parameters={
                    "ratio": "4:1",
                    "threshold_db": -18,
                    "attack_ms": 2,
                    "release_ms": 80,
                    "makeup_db": round(crest * 0.08, 1),
                },
                reason=(
                    f"Kick crest is {crest:.1f} dB; fast-attack FET controls peak "
                    "while short release restores punch on each hit."
                ),
                priority="recommended",
            )
        )

    elif role == "bass":
        suggestions.append(
            EffectSuggestion(
                effect_type="compression",
                preset_name="VCA Bass Compressor",
                parameters={
                    "ratio": "4:1",
                    "threshold_db": -20,
                    "attack_ms": 15,
                    "release_ms": 100,
                    "makeup_db": round(crest * 0.10, 1),
                },
                reason=(
                    f"Bass crest is {crest:.1f} dB; medium attack lets the note speak "
                    "then clamps sustain for a consistent low-end floor."
                ),
                priority="recommended",
            )
        )

    elif role in {"guitar", "keys"}:
        if crest > 20.0:
            suggestions.append(
                EffectSuggestion(
                    effect_type="compression",
                    preset_name="Soft-Knee Studio Compressor",
                    parameters={
                        "ratio": "3:1",
                        "threshold_db": -22,
                        "attack_ms": 20,
                        "release_ms": 120,
                        "knee_db": 6,
                    },
                    reason=(
                        f"{role.title()} crest is {crest:.1f} dB; soft-knee compression "
                        "adds sustain and fits the instrument into the mix."
                    ),
                    priority="optional",
                )
            )

    elif role == "snare" and crest > 22.0:
        suggestions.append(
            EffectSuggestion(
                effect_type="compression",
                preset_name="Parallel Snare Compression",
                parameters={
                    "ratio": "6:1",
                    "threshold_db": -26,
                    "attack_ms": 4,
                    "release_ms": 60,
                    "blend_percent": 40,
                    "style": "parallel (New York)",
                },
                reason=(
                    f"Snare crest is {crest:.1f} dB; parallel compression blends the "
                    "crushed signal with the dry to add body without killing the crack."
                ),
                priority="recommended",
            )
        )

    return suggestions


def _stereo_width_suggestions(feature: TrackFeatures, role: str) -> list[EffectSuggestion]:
    suggestions: list[EffectSuggestion] = []

    if role == "guitar":
        if feature.low_band_ratio < 0.06:
            # Guitar with very little bass — safe to widen without low-end smear
            suggestions.append(
                EffectSuggestion(
                    effect_type="stereo_width",
                    preset_name="Mid-Side Width (wide)",
                    parameters={"width_percent": 130, "low_mono_below_hz": 200},
                    reason=(
                        f"Guitar low-band ratio is low ({feature.low_band_ratio:.3f}); "
                        "wide stereo spread fills space in the mix panorama."
                    ),
                    priority="recommended",
                )
            )
        else:
            suggestions.append(
                EffectSuggestion(
                    effect_type="stereo_width",
                    preset_name="Mid-Side Width (moderate)",
                    parameters={"width_percent": 115, "low_mono_below_hz": 250},
                    reason="Moderate widening adds dimension while keeping low-mids controlled.",
                    priority="optional",
                )
            )

    elif role == "keys":
        suggestions.append(
            EffectSuggestion(
                effect_type="stereo_width",
                preset_name="Chorus / Ensemble Width",
                parameters={"rate_hz": 0.4, "depth_percent": 20, "mix_percent": 30},
                reason="Subtle chorus or ensemble effect widens keys without obvious modulation.",
                priority="optional",
            )
        )

    elif role == "overheads":
        suggestions.append(
            EffectSuggestion(
                effect_type="stereo_width",
                preset_name="Mid-Side Width (very wide)",
                parameters={"width_percent": 140, "low_mono_below_hz": 300},
                reason="Wide overhead image creates an expansive drum room.",
                priority="recommended",
            )
        )

    return suggestions


# ---------------------------------------------------------------------------
# Public API
# ---------------------------------------------------------------------------

def infer_effects(feature: TrackFeatures, role: str) -> list[EffectSuggestion]:
    """Return a prioritised list of effect suggestions for one track."""
    suggestions: list[EffectSuggestion] = []
    suggestions.extend(_reverb_suggestions(feature, role))
    suggestions.extend(_delay_suggestions(feature, role))
    suggestions.extend(_saturation_suggestions(feature, role))
    suggestions.extend(_compression_suggestions(feature, role))
    suggestions.extend(_stereo_width_suggestions(feature, role))
    return suggestions


def infer_mix_bus_effects(mix_metrics: dict) -> list[EffectSuggestion]:
    """Return mix-bus effect suggestions based on the current mix metrics."""
    suggestions: list[EffectSuggestion] = []
    crest = mix_metrics.get("crest_db", 18.0)
    lufs = mix_metrics.get("lufs_integrated", -20.0)

    if crest > 16.0:
        suggestions.append(
            EffectSuggestion(
                effect_type="bus_compression",
                preset_name="Glue Bus Compressor",
                parameters={
                    "ratio": "2:1",
                    "threshold_db": -12,
                    "attack_ms": 30,
                    "release_ms": 200,
                    "makeup_db": round((crest - 16.0) * 0.12, 1),
                    "knee_db": 6,
                    "style": "VCA",
                },
                reason=(
                    f"Mix crest factor is {crest:.1f} dB; a glue compressor on the bus "
                    "brings the mix together and increases perceived density."
                ),
                priority="recommended",
            )
        )

    if lufs < -20.0:
        suggestions.append(
            EffectSuggestion(
                effect_type="bus_compression",
                preset_name="Makeup Gain / Transparent Limiter",
                parameters={"ceiling_db": -3, "target_lufs": -16},
                reason=(
                    f"Mix LUFS is {lufs:.1f}; a final gain stage and limiter brings the "
                    "pre-master level up to a competitive loudness before mastering."
                ),
                priority="recommended",
            )
        )

    return suggestions


def generate_effects_report(
    track_names: list[str],
    track_features: list[TrackFeatures],
    roles: list[str],
    mix_metrics: dict | None = None,
) -> dict:
    """Build a full effects report dict suitable for JSON serialisation."""
    tracks: list[dict] = []
    for name, feature, role in zip(track_names, track_features, roles):
        suggestions = infer_effects(feature, role)
        tracks.append(
            {
                "name": name,
                "role": role,
                "crest_db": round(_crest_db(feature), 2),
                "effects": [
                    {
                        "effect_type": s.effect_type,
                        "preset_name": s.preset_name,
                        "parameters": s.parameters,
                        "reason": s.reason,
                        "priority": s.priority,
                    }
                    for s in suggestions
                ],
            }
        )

    report: dict = {"per_track": tracks}
    if mix_metrics is not None:
        bus_suggestions = infer_mix_bus_effects(mix_metrics)
        report["mix_bus"] = [
            {
                "effect_type": s.effect_type,
                "preset_name": s.preset_name,
                "parameters": s.parameters,
                "reason": s.reason,
                "priority": s.priority,
            }
            for s in bus_suggestions
        ]

    return report
