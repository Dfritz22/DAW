from __future__ import annotations

from itertools import combinations

import numpy as np

from .models import Recommendation, TrackFeatures


def _infer_role(name: str) -> str:
    lower = name.lower()
    if "kick" in lower:
        return "kick"
    if "bass" in lower:
        return "bass"
    if "vocal" in lower or "vox" in lower:
        return "vocal"
    if "snare" in lower:
        return "snare"
    if "overhead" in lower:
        return "overheads"
    if "tom" in lower:
        return "toms"
    if "perc" in lower:
        return "percussion"
    if "guitar" in lower or "gtr" in lower:
        return "guitar"
    if "piano" in lower or "keys" in lower:
        return "keys"
    return "other"


def generate_track_recommendations(track: TrackFeatures) -> list[Recommendation]:
    recs: list[Recommendation] = []
    role = _infer_role(track.name)

    if track.clipping_ratio > 0.001:
        recs.append(
            Recommendation(
                scope="track",
                target=track.name,
                category="hardware_gain",
                action="Reduce preamp/interface input gain by 4-8 dB on next take. If source is loud, enable input pad.",
                confidence=0.9,
                rationale="Detected digital clipping ratio above threshold, which suggests front-end gain staging issues.",
            )
        )

    if track.peak_dbfs > -1.0:
        recs.append(
            Recommendation(
                scope="track",
                target=track.name,
                category="gain_staging",
                action="Lower clip gain or trim by 2-4 dB to recover mix headroom.",
                confidence=0.86,
                rationale="Track peak is too close to full scale and leaves limited processing headroom.",
            )
        )

    if track.lufs_integrated < -30.0 and track.peak_dbfs < -6.0:
        recs.append(
            Recommendation(
                scope="track",
                target=track.name,
                category="gain_staging",
                action="Raise clip gain/fader by 4-8 dB before tone-shaping so decisions are made at a workable monitoring level.",
                confidence=0.82,
                rationale="Integrated loudness is very low, which can hide tonal and dynamic decisions during mixing.",
            )
        )

    if (role in {"kick", "snare", "toms", "overheads", "percussion"} and track.crest_db > 26.0) or (
        role in {"vocal", "guitar", "keys", "other"} and track.crest_db > 20.0
    ):
        recs.append(
            Recommendation(
                scope="track",
                target=track.name,
                category="compression",
                action="Use gentle serial compression (for example 2:1 then 3:1, each with 1-3 dB GR) to reduce excessive dynamic range.",
                confidence=0.8,
                rationale="High crest factor indicates wide peak-to-average dynamics that can make a source hard to seat in the mix.",
            )
        )

    if track.presence_band_ratio > 0.34:
        recs.append(
            Recommendation(
                scope="track",
                target=track.name,
                category="eq",
                action="Try a broad EQ cut of 1-3 dB around 2.5-5 kHz to reduce harshness.",
                confidence=0.74,
                rationale="Presence-band energy is elevated relative to full-spectrum energy.",
            )
        )

    if track.low_band_ratio > 0.45 and track.spectral_centroid_hz < 180.0:
        recs.append(
            Recommendation(
                scope="track",
                target=track.name,
                category="eq",
                action="Apply a gentle high-pass filter (start around 40-80 Hz) and re-check low-end buildup.",
                confidence=0.72,
                rationale="Low-frequency dominance may create rumble and mask other instruments.",
            )
        )

    if role == "vocal" and track.air_band_ratio > 0.20:
        recs.append(
            Recommendation(
                scope="track",
                target=track.name,
                category="de_esser",
                action="Check 6-10 kHz for sibilance and apply de-essing only as needed (aim for 1-4 dB reduction on esses).",
                confidence=0.66,
                rationale="Upper-band energy is elevated and may become sharp once level and compression are increased.",
            )
        )

    if role in {"overheads", "percussion"} and track.air_band_ratio > 0.26:
        recs.append(
            Recommendation(
                scope="track",
                target=track.name,
                category="eq",
                action="If cymbals feel brittle in-context, try a gentle high-shelf cut (around 9-12 kHz, 1-2 dB).",
                confidence=0.62,
                rationale="Very high top-end energy can become fatiguing when the full arrangement is dense.",
            )
        )

    if _infer_role(track.name) == "vocal" and track.presence_band_ratio > 0.30:
        recs.append(
            Recommendation(
                scope="track",
                target=track.name,
                category="mic_placement",
                action="For next recording, rotate mic 15-25 degrees off-axis and increase distance by 2-4 inches to tame sibilance/harshness.",
                confidence=0.68,
                rationale="Vocal-labeled track shows elevated presence energy consistent with overly bright capture.",
            )
        )

    return recs


def generate_group_recommendations(
    tracks: list[TrackFeatures],
    groups: dict[str, list[str]],
) -> list[Recommendation]:
    recs: list[Recommendation] = []
    by_name = {t.name.lower(): t for t in tracks}

    for group_name, members in groups.items():
        group_tracks = [by_name[m.lower()] for m in members if m.lower() in by_name]
        if len(group_tracks) < 2:
            continue

        for a, b in combinations(group_tracks, 2):
            low_overlap = 1.0 - abs(a.low_band_ratio - b.low_band_ratio)
            presence_overlap = 1.0 - abs(a.presence_band_ratio - b.presence_band_ratio)

            role_pair = {_infer_role(a.name), _infer_role(b.name)}
            if {"kick", "bass"}.issubset(role_pair) and low_overlap > 0.78:
                recs.append(
                    Recommendation(
                        scope="group",
                        target=group_name,
                        category="masking",
                        action=f"Kick/Bass masking likely between {a.name} and {b.name}. Split low-end roles with EQ and/or sidechain compression.",
                        confidence=float(np.clip(low_overlap, 0.0, 0.95)),
                        rationale="Both tracks have very similar low-band energy distribution.",
                    )
                )

            if "vocal" in role_pair and "guitar" in role_pair and presence_overlap > 0.75:
                recs.append(
                    Recommendation(
                        scope="group",
                        target=group_name,
                        category="masking",
                        action=f"Vocal/Guitar intelligibility conflict likely between {a.name} and {b.name}. Carve 2-4 kHz in guitar or automate dynamic EQ.",
                        confidence=float(np.clip(presence_overlap, 0.0, 0.92)),
                        rationale="Presence-region energy overlap is high, which often reduces vocal clarity.",
                    )
                )

    return recs


def rank_recommendations(recommendations: list[Recommendation]) -> list[Recommendation]:
    return sorted(recommendations, key=lambda r: r.confidence, reverse=True)
