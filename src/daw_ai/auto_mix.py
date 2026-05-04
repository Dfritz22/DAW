from __future__ import annotations

import json
import math
from copy import deepcopy
from dataclasses import dataclass
from pathlib import Path

import librosa
import numpy as np
import pyloudnorm as pyln
import soundfile as sf

from .dsp import (
    apply_delay,
    apply_reverb,
    apply_saturation,
    band_adjust,
    bus_compress,
    compress_audio,
    high_shelf_adjust,
    highpass_filter,
    low_shelf_adjust,
)
from .features import extract_track_features
from .models import TrackFeatures

EPSILON = 1e-12


@dataclass
class TrackState:
    name: str
    role: str
    sample_rate: int
    audio: np.ndarray
    feature: TrackFeatures
    gain_db: float
    pan: float
    highpass_hz: float = 0.0
    low_shelf_db: float = 0.0
    presence_gain_db: float = 0.0
    air_gain_db: float = 0.0
    deesser_db: float = 0.0
    comp_threshold_db: float = 0.0
    comp_ratio: float = 1.0
    comp_makeup_db: float = 0.0
    low_mid_db: float = 0.0       # 200-500 Hz  (mud / body zone)
    # Time-based / harmonic effects — applied only on final render, not during scoring
    reverb_decay_s: float = 0.0
    reverb_predelay_ms: float = 0.0
    reverb_mix: float = 0.0
    delay_time_ms: float = 0.0
    delay_feedback: float = 0.0
    delay_mix: float = 0.0
    saturation_drive_db: float = 0.0
    saturation_blend: float = 0.0


def _infer_role(name: str) -> str:
    lower = name.lower()
    if "kick" in lower:
        return "kick"
    if "bass" in lower:
        return "bass"
    if "snare" in lower:
        return "snare"
    if "overhead" in lower:
        return "overheads"
    if "tom" in lower:
        return "toms"
    if "perc" in lower:
        return "percussion"
    if "vocal" in lower or "vox" in lower:
        return "vocal"
    if "piano" in lower or "keys" in lower:
        return "keys"
    if "guitar" in lower or "gtr" in lower:
        return "guitar"
    return "other"


def _dbfs(value: float) -> float:
    return float(20.0 * np.log10(max(value, EPSILON)))


def _lin(db_value: float) -> float:
    return float(10.0 ** (db_value / 20.0))


def _safe_ratio(num: float, den: float) -> float:
    return float(num / den) if den > EPSILON else 0.0


def _pan_gains(pan: float) -> tuple[float, float]:
    p = float(np.clip(pan, -1.0, 1.0))
    left = math.cos((p + 1.0) * math.pi / 4.0)
    right = math.sin((p + 1.0) * math.pi / 4.0)
    return left, right


def _load_mono(path: Path, target_sr: int | None = None) -> tuple[np.ndarray, int]:
    try:
        audio, sr = sf.read(path)
        if audio.ndim == 2:
            audio = np.mean(audio, axis=1)
        audio = audio.astype(np.float32)
    except Exception:
        audio, sr = librosa.load(path.as_posix(), sr=target_sr, mono=True)
        audio = audio.astype(np.float32)
        return audio, sr
    if target_sr is not None and sr != target_sr:
        audio = librosa.resample(audio, orig_sr=sr, target_sr=target_sr)
        sr = target_sr
    return audio, sr


def _load_reference_metrics(path: Path, target_sr: int, preview_samples: int) -> dict[str, float]:
    audio, sr = _load_mono(path, target_sr=target_sr)
    if len(audio) < preview_samples:
        padded = np.zeros(preview_samples, dtype=np.float32)
        padded[: len(audio)] = audio
        audio = padded
    else:
        audio = audio[:preview_samples]
    stereo = np.stack([audio, audio], axis=1)
    return _mix_metrics(stereo, sr)


def _mix_metrics(stereo_audio: np.ndarray, sr: int) -> dict[str, float]:
    mono = np.mean(stereo_audio, axis=1)
    peak = float(np.max(np.abs(mono)))
    rms = float(np.sqrt(np.mean(np.square(mono))))
    clipping_ratio = float(np.mean(np.abs(mono) >= 0.999))

    meter = pyln.Meter(sr)
    lufs = float(meter.integrated_loudness(mono))

    stft = librosa.stft(mono, n_fft=4096, hop_length=1024)
    mag = np.abs(stft)
    freqs = librosa.fft_frequencies(sr=sr, n_fft=4096)
    power_per_bin = np.mean(mag, axis=1)
    total_power = float(np.sum(power_per_bin) + EPSILON)

    low_power = float(np.sum(power_per_bin[(freqs >= 20.0) & (freqs < 120.0)]))
    presence_power = float(np.sum(power_per_bin[(freqs >= 2000.0) & (freqs < 5000.0)]))
    air_power = float(np.sum(power_per_bin[freqs >= 8000.0]))

    return {
        "peak_dbfs": _dbfs(peak),
        "rms_dbfs": _dbfs(rms),
        "lufs_integrated": lufs,
        "crest_db": _dbfs(peak) - _dbfs(rms),
        "clipping_ratio": clipping_ratio,
        "low_band_ratio": _safe_ratio(low_power, total_power),
        "presence_band_ratio": _safe_ratio(presence_power, total_power),
        "air_band_ratio": _safe_ratio(air_power, total_power),
    }


def _score_mix(
    metrics: dict[str, float],
    role_levels: dict[str, float],
    reference_metrics: dict[str, float] | None = None,
) -> tuple[float, list[str]]:
    score = 100.0
    notes: list[str] = []

    # Targets calibrated for a professional pre-mastering mix.
    # Derived from analysis of reference mixes and industry standards.
    peak_target = -5.0       # modern mix typically peaks at -6 to -3 dBFS
    lufs_target = -18.0      # modern pop/rock mix pre-master (was -21, too conservative)
    crest_target = 17.0      # target some density; raw stems sit around 20-24 dB
    low_target = 0.14        # tight bass end (reference: 0.127)
    presence_target = 0.19   # clear, present mids (reference: 0.221)
    air_target = 0.15        # good extension without brittleness (reference: 0.200)

    score -= abs(metrics["peak_dbfs"] - peak_target) * 1.4
    score -= abs(metrics["lufs_integrated"] - lufs_target) * 1.15
    score -= abs(metrics["crest_db"] - crest_target) * 1.0  # penalise being too sparse or too squashed
    score -= abs(metrics["low_band_ratio"] - low_target) * 130.0
    score -= abs(metrics["presence_band_ratio"] - presence_target) * 115.0  # presence matters most
    score -= abs(metrics["air_band_ratio"] - air_target) * 75.0
    score -= metrics["clipping_ratio"] * 4500.0

    if metrics["peak_dbfs"] > -1.0:
        notes.append("Mix is too hot and needs more headroom")
    if metrics["crest_db"] > 21.0:
        notes.append("Mix is too sparse/dynamic — needs more compression or density")
    if metrics["crest_db"] < 12.0:
        notes.append("Mix is over-compressed — losing transients and punch")
    if metrics["low_band_ratio"] > 0.22:
        notes.append("Low-end dominance is high")
    if metrics["presence_band_ratio"] < 0.14:
        notes.append("Mids are recessed — mix may sound thin or distant")
    if metrics["presence_band_ratio"] > 0.25:
        notes.append("Presence energy may be fatiguing")
    if metrics["air_band_ratio"] > 0.22:
        notes.append("Top-end is likely brittle")
    if metrics["clipping_ratio"] > 0.0005:
        notes.append("Potential clipping artifacts")

    target_roles = {
        "vocal": -24.0,
        "kick": -24.0,
        "bass": -24.5,
        "snare": -27.0,
        "overheads": -30.5,
        "toms": -29.0,
        "percussion": -30.5,
        "guitar": -30.0,
        "keys": -30.0,
    }
    for role, target in target_roles.items():
        if role in role_levels:
            score -= abs(role_levels[role] - target) * 0.4

    if reference_metrics is not None:
        low_distance = abs(metrics["low_band_ratio"] - reference_metrics["low_band_ratio"])
        presence_distance = abs(metrics["presence_band_ratio"] - reference_metrics["presence_band_ratio"])
        air_distance = abs(metrics["air_band_ratio"] - reference_metrics["air_band_ratio"])
        crest_distance = abs(metrics["crest_db"] - reference_metrics["crest_db"])

        score -= low_distance * 140.0
        score -= presence_distance * 140.0
        score -= air_distance * 90.0
        score -= crest_distance * 1.2

        if low_distance > 0.05:
            notes.append("Low-end balance is still far from reference")
        if presence_distance > 0.04:
            notes.append("Upper-mid balance is still far from reference")
        if air_distance > 0.05:
            notes.append("Top-end balance is still far from reference")

    return float(np.clip(score, 0.0, 100.0)), notes


def _init_pan(name: str, role: str) -> float:
    lower = name.lower()
    if role in {"kick", "snare", "bass", "vocal"}:
        return 0.0
    if "acgtr1" in lower:
        return -0.55
    if "acgtr2" in lower:
        return 0.55
    if role == "overheads":
        return 0.75
    if role == "toms":
        return -0.25
    if role == "percussion":
        return 0.25
    if role == "guitar":
        return -0.35
    if role == "keys":
        return 0.15
    return 0.0


def _role_bus_index(role: str) -> int:
    # 0=Drums, 1=Music, 2=Vocals, 3=Master
    if role in {"kick", "snare", "overheads", "toms", "percussion"}:
        return 0
    if role == "vocal":
        return 2
    return 1


def _target_track_lufs(role: str) -> float:
    targets = {
        "kick": -24.0,
        "bass": -24.5,
        "snare": -27.0,
        "overheads": -30.5,
        "toms": -29.0,
        "percussion": -30.5,
        "vocal": -24.0,
        "guitar": -30.0,
        "keys": -30.0,
        "other": -30.0,
    }
    return targets.get(role, -30.0)


def _init_gain_db(feature: TrackFeatures, role: str, mix_mode: str) -> float:
    decent = float(np.clip(_target_track_lufs(role) - feature.lufs_integrated, -18.0, 12.0))
    if mix_mode == "decent":
        offsets = {
            "vocal": -1.2,
            "bass": 1.0,
            "kick": 0.8,
            "overheads": 0.8,
            "guitar": 0.5,
            "keys": 0.4,
        }
        return float(np.clip(decent + offsets.get(role, 0.2), -18.0, 12.0))

    if mix_mode == "bad":
        if role == "vocal":
            return decent - 7.0
        if role in {"kick", "bass"}:
            return decent + 6.0
        if role in {"overheads", "percussion"}:
            return decent + 4.0
        if role in {"guitar", "keys"}:
            return decent + 3.0
        return decent + 2.0

    return decent


def _init_processing(state: TrackState, mix_mode: str) -> None:
    role = state.role
    if mix_mode == "bad":
        if role == "bass":
            state.low_shelf_db = 2.5
        if role == "guitar":
            state.presence_gain_db = 1.5
        if role == "vocal":
            state.deesser_db = 0.5
        return

    # --- High-pass filters (role-aware) ---
    if role in {"vocal", "guitar", "keys"}:
        state.highpass_hz = 90.0
    elif role in {"overheads", "percussion"}:
        state.highpass_hz = 140.0
    elif role == "snare":
        state.highpass_hz = 80.0
    elif role == "toms":
        state.highpass_hz = 50.0
    elif role == "bass":
        state.highpass_hz = 35.0

    # --- Per-instrument EQ curves (applied in both decent and best) ---
    if role == "kick":
        state.low_shelf_db = 1.5         # +1.5 dB sub/punch shelf at 120 Hz
        state.low_mid_db = -1.5          # -1.5 dB cut 200-500 Hz (boxiness)
        state.presence_gain_db = 1.5     # +1.5 dB 2-5 kHz (beater click / attack)

    elif role == "snare":
        state.low_mid_db = 1.5           # +1.5 dB 200-500 Hz (body/crack)
        state.presence_gain_db = 2.0     # +2.0 dB 2-5 kHz (crack/snap)

    elif role == "bass":
        state.low_mid_db = -1.5          # -1.5 dB 200-500 Hz (mud reduction)
        state.presence_gain_db = 1.0     # +1.0 dB 2-5 kHz (definition on small speakers)

    elif role == "overheads":
        state.low_mid_db = -2.0          # -2.0 dB 200-500 Hz (remove drum bleed mud)
        state.air_gain_db = 1.5          # +1.5 dB 8 kHz+ (cymbal shimmer/air)

    elif role == "toms":
        state.low_mid_db = 1.0           # +1.0 dB body
        state.presence_gain_db = 1.0     # +1.0 dB 2-5 kHz (attack definition)

    elif role == "vocal":
        is_backing = "backing" in state.name.lower() or "bvox" in state.name.lower()
        state.low_mid_db = -1.5          # -1.5 dB 200-500 Hz (mud / boxiness)
        state.presence_gain_db = 1.5 if not is_backing else 0.5   # intelligibility; less on BVs
        state.air_gain_db = 1.0 if not is_backing else 0.5        # sheen / air
        state.deesser_db = -0.8 if mix_mode == "best" else -0.4

    elif role == "guitar":
        state.low_mid_db = -1.5          # -1.5 dB 200-500 Hz (mud)
        # Cut upper-mids slightly to leave space for the vocal
        state.presence_gain_db = -1.0 if mix_mode == "best" else -0.5

    elif role == "keys":
        state.low_mid_db = -1.0          # -1.0 dB 200-500 Hz (muddiness)
        state.presence_gain_db = 0.5     # slight presence lift for clarity

    # --- Compression (role-aware) ---
    if role == "vocal":
        state.comp_threshold_db = -24.0
        state.comp_ratio = 1.8 if mix_mode == "decent" else 2.2
        state.comp_makeup_db = 0.4
    elif role in {"snare", "toms"}:
        state.comp_threshold_db = -26.0
        state.comp_ratio = 1.7 if mix_mode == "decent" else 2.2
    elif role == "bass":
        state.comp_threshold_db = -24.0
        state.comp_ratio = 1.9 if mix_mode == "best" else 1.5
        state.comp_makeup_db = 0.3

    # Effects applied on final render only (not during iterative scoring)
    if mix_mode != "best":
        return

    if role == "vocal" and "backing" not in state.name.lower() and "bvox" not in state.name.lower():
        state.reverb_decay_s = 1.2
        state.reverb_predelay_ms = 22.0
        state.reverb_mix = 0.10          # 10% wet — present but not washy
        state.delay_time_ms = 125.0
        state.delay_feedback = 0.20
        state.delay_mix = 0.08
        state.saturation_drive_db = 1.5
        state.saturation_blend = 0.30
    elif role == "vocal":
        # Backing / harmony vocals: slightly more reverb to sit behind lead
        state.reverb_decay_s = 1.4
        state.reverb_predelay_ms = 28.0
        state.reverb_mix = 0.14
        state.saturation_drive_db = 1.2
        state.saturation_blend = 0.25
    elif role == "snare":
        state.reverb_decay_s = 0.35
        state.reverb_predelay_ms = 5.0
        state.reverb_mix = 0.12
    elif role == "guitar":
        state.reverb_decay_s = 1.0
        state.reverb_predelay_ms = 10.0
        state.reverb_mix = 0.09
        state.saturation_drive_db = 1.5
        state.saturation_blend = 0.22
    elif role == "keys":
        state.reverb_decay_s = 1.3
        state.reverb_predelay_ms = 12.0
        state.reverb_mix = 0.10
    elif role == "bass":
        # No reverb — keeps the low end tight
        state.saturation_drive_db = 2.5
        state.saturation_blend = 0.25
    elif role == "overheads":
        state.reverb_decay_s = 0.8
        state.reverb_predelay_ms = 8.0
        state.reverb_mix = 0.07


def _build_track_states(paths: list[Path], mix_mode: str) -> tuple[list[TrackState], int, int]:
    features = [extract_track_features(p) for p in paths]
    base_sr = features[0].sample_rate

    track_states: list[TrackState] = []
    max_len = 0
    for path, feature in zip(paths, features):
        role = _infer_role(path.name)
        mono, sr = _load_mono(path, target_sr=base_sr)
        if sr != base_sr:
            raise ValueError("Resampling failure while building track states")
        max_len = max(max_len, len(mono))
        gain_db = _init_gain_db(feature, role, mix_mode)
        pan = _init_pan(path.name, role)
        if mix_mode == "bad":
            if role == "vocal":
                pan = 1.0
            elif role == "bass":
                pan = -1.0
        state = TrackState(
            name=path.name,
            role=role,
            sample_rate=sr,
            audio=mono,
            feature=feature,
            gain_db=gain_db,
            pan=pan,
        )
        _init_processing(state, mix_mode)
        track_states.append(state)

    return track_states, base_sr, max_len


def _prepare_track_audio(state: TrackState, n_samples: int, apply_effects: bool = False) -> np.ndarray:
    src = state.audio
    if len(src) < n_samples:
        padded = np.zeros(n_samples, dtype=np.float32)
        padded[: len(src)] = src
        src = padded
    else:
        src = src[:n_samples].copy()

    if state.highpass_hz > 0.0:
        src = highpass_filter(src, state.sample_rate, state.highpass_hz)
    if abs(state.low_shelf_db) > 1e-6:
        src = low_shelf_adjust(src, state.sample_rate, 120.0, state.low_shelf_db)
    if abs(state.low_mid_db) > 1e-6:
        src = band_adjust(src, state.sample_rate, 200.0, 500.0, state.low_mid_db)
    if abs(state.presence_gain_db) > 1e-6:
        src = band_adjust(src, state.sample_rate, 2000.0, 5000.0, state.presence_gain_db)
    if abs(state.air_gain_db) > 1e-6:
        src = high_shelf_adjust(src, state.sample_rate, 8000.0, state.air_gain_db)
    if abs(state.deesser_db) > 1e-6:
        src = band_adjust(src, state.sample_rate, 6000.0, 10000.0, state.deesser_db)
    if state.comp_ratio > 1.0:
        src = compress_audio(
            src,
            threshold_db=state.comp_threshold_db,
            ratio=state.comp_ratio,
            makeup_db=state.comp_makeup_db,
            sr=state.sample_rate,
        )
    # Time-based and harmonic effects — only applied on final render to keep
    # the scoring loop fast (they don't affect relative balance decisions).
    if apply_effects:
        if state.saturation_blend > 0.0 and state.saturation_drive_db > 0.0:
            src = apply_saturation(src, state.saturation_drive_db, state.saturation_blend)
        if state.reverb_mix > 0.0 and state.reverb_decay_s > 0.0:
            src = apply_reverb(
                src,
                state.sample_rate,
                decay_s=state.reverb_decay_s,
                pre_delay_ms=state.reverb_predelay_ms,
                mix=state.reverb_mix,
            )
        if state.delay_mix > 0.0 and state.delay_time_ms > 0.0:
            src = apply_delay(
                src,
                state.sample_rate,
                time_ms=state.delay_time_ms,
                feedback=state.delay_feedback,
                mix=state.delay_mix,
            )
    return src.astype(np.float32)


def _render_mix(track_states: list[TrackState], n_samples: int, apply_effects: bool = False) -> np.ndarray:
    left = np.zeros(n_samples, dtype=np.float32)
    right = np.zeros(n_samples, dtype=np.float32)

    for state in track_states:
        src = _prepare_track_audio(state, n_samples, apply_effects=apply_effects)
        gain = _lin(state.gain_db)
        pan_l, pan_r = _pan_gains(state.pan)
        left += src * gain * pan_l
        right += src * gain * pan_r

    return np.stack([left, right], axis=1)


def _render_unmixed(track_states: list[TrackState], n_samples: int) -> np.ndarray:
    mono = np.zeros(n_samples, dtype=np.float32)
    for state in track_states:
        src = state.audio
        if len(src) < n_samples:
            padded = np.zeros(n_samples, dtype=np.float32)
            padded[: len(src)] = src
            src = padded
        else:
            src = src[:n_samples]
        mono += src
    return np.stack([mono, mono], axis=1)


def write_unmixed_render(paths: list[Path], output_dir: Path) -> dict:
    track_states, sr, max_len = _build_track_states(paths, mix_mode="best")
    stereo = _render_unmixed(track_states, max_len)
    output_dir.mkdir(parents=True, exist_ok=True)
    out_path = output_dir / "unmixed_sum.wav"
    sf.write(out_path, stereo, sr, subtype="FLOAT")
    metrics = _mix_metrics(stereo, sr)
    summary = {
        "sample_rate": sr,
        "path": str(out_path),
        "metrics": metrics,
        "description": "Simple raw sum of all source files with no gain staging, panning, EQ, or compression.",
    }
    json_path = output_dir / "unmixed_sum.json"
    md_path = output_dir / "unmixed_sum.md"
    json_path.write_text(json.dumps(summary, indent=2), encoding="utf-8")
    md_path.write_text(
        "\n".join(
            [
                "# Unmixed Sum",
                "",
                f"Path: {out_path}",
                "",
                "This is the raw sum of all source files with no mixing moves applied.",
                "",
                f"- Peak: {metrics['peak_dbfs']:.2f} dBFS",
                f"- LUFS: {metrics['lufs_integrated']:.2f}",
                f"- Low band ratio: {metrics['low_band_ratio']:.3f}",
                f"- Presence ratio: {metrics['presence_band_ratio']:.3f}",
                f"- Air ratio: {metrics['air_band_ratio']:.3f}",
            ]
        )
        + "\n",
        encoding="utf-8",
    )
    return {"path": str(out_path), "json_path": str(json_path), "markdown_path": str(md_path), "metrics": metrics}


def _role_levels(track_states: list[TrackState]) -> dict[str, float]:
    by_role: dict[str, list[float]] = {}
    for state in track_states:
        adjusted_lufs = state.feature.lufs_integrated + state.gain_db
        by_role.setdefault(state.role, []).append(adjusted_lufs)
    return {role: float(np.mean(vals)) for role, vals in by_role.items()}


def _apply_action(track_states: list[TrackState], action: dict) -> None:
    if action["type"] == "global_gain":
        for state in track_states:
            state.gain_db = float(np.clip(state.gain_db + action["delta_db"], -24.0, 12.0))
        return

    targets = action.get("targets", [])
    for state in track_states:
        if state.name not in targets and state.role not in targets:
            continue
        if action["type"] == "gain":
            state.gain_db = float(np.clip(state.gain_db + action["delta_db"], -24.0, 12.0))
        elif action["type"] == "presence":
            state.presence_gain_db = float(np.clip(state.presence_gain_db + action["delta_db"], -6.0, 6.0))
        elif action["type"] == "air":
            state.air_gain_db = float(np.clip(state.air_gain_db + action["delta_db"], -6.0, 6.0))
        elif action["type"] == "deesser":
            state.deesser_db = float(np.clip(state.deesser_db + action["delta_db"], -8.0, 2.0))
        elif action["type"] == "low_shelf":
            state.low_shelf_db = float(np.clip(state.low_shelf_db + action["delta_db"], -6.0, 6.0))
        elif action["type"] == "highpass":
            state.highpass_hz = float(np.clip(max(state.highpass_hz, action["hz"]), 0.0, 220.0))
        elif action["type"] == "compress":
            state.comp_ratio = float(np.clip(max(state.comp_ratio, action["ratio"]), 1.0, 6.0))
            state.comp_threshold_db = float(action["threshold_db"])
            state.comp_makeup_db = float(np.clip(state.comp_makeup_db + action.get("makeup_db", 0.0), 0.0, 6.0))


def _build_candidate_actions(track_states: list[TrackState], metrics: dict[str, float]) -> list[dict]:
    actions: list[dict] = []
    role_levels = _role_levels(track_states)

    if metrics["peak_dbfs"] > -3.0:
        actions.append({"type": "global_gain", "delta_db": -0.75, "label": "Global trim for headroom"})

    if metrics["lufs_integrated"] < -22.8:
        actions.append({"type": "gain", "targets": ["vocal"], "delta_db": 0.6, "label": "Lift vocal presence in balance"})
        actions.append({"type": "gain", "targets": ["snare", "kick"], "delta_db": 0.4, "label": "Lift core rhythm elements"})

    if metrics["low_band_ratio"] > 0.17:
        actions.append({"type": "gain", "targets": ["bass"], "delta_db": -0.6, "label": "Reduce bass level for low-end clarity"})
        actions.append({"type": "low_shelf", "targets": ["bass"], "delta_db": -0.8, "label": "Trim bass low shelf"})
        actions.append({"type": "low_shelf", "targets": ["kick"], "delta_db": -0.5, "label": "Tighten kick low shelf"})

    # Lift presence when mids are recessed (common in raw stem mixes)
    if metrics["presence_band_ratio"] < 0.17:
        actions.append({"type": "presence", "targets": ["vocal"], "delta_db": 0.7, "label": "Lift vocal presence for clarity"})
        actions.append({"type": "presence", "targets": ["guitar"], "delta_db": 0.5, "label": "Boost guitar mids for body"})
        actions.append({"type": "presence", "targets": ["keys"], "delta_db": 0.4, "label": "Lift keys presence"})
        actions.append({"type": "air", "targets": ["vocal"], "delta_db": 0.5, "label": "Add air to vocals"})
    elif metrics["presence_band_ratio"] > 0.23:
        actions.append({"type": "presence", "targets": ["guitar"], "delta_db": -0.75, "label": "Reduce guitar presence buildup"})
        actions.append({"type": "air", "targets": ["overheads", "percussion"], "delta_db": -0.5, "label": "Soften bright cymbal content"})

    # Push density when mix is too sparse (high crest = not enough compression/sustain)
    if metrics["crest_db"] > 18.5:
        actions.append(
            {
                "type": "compress",
                "targets": ["vocal"],
                "ratio": 2.5,
                "threshold_db": -22.0,
                "makeup_db": 0.5,
                "label": "Compress vocal for density",
            }
        )
        actions.append(
            {
                "type": "compress",
                "targets": ["bass"],
                "ratio": 3.0,
                "threshold_db": -20.0,
                "makeup_db": 0.4,
                "label": "Compress bass for glue",
            }
        )
        actions.append(
            {
                "type": "compress",
                "targets": ["guitar"],
                "ratio": 2.2,
                "threshold_db": -24.0,
                "makeup_db": 0.3,
                "label": "Compress guitars for sustain",
            }
        )

    for state in track_states:
        current = state.feature.lufs_integrated + state.gain_db
        target = _target_track_lufs(state.role)
        delta = target - current
        if abs(delta) > 1.5:
            actions.append(
                {
                    "type": "gain",
                    "targets": [state.name],
                    "delta_db": float(np.clip(delta * 0.35, -0.8, 0.8)),
                    "label": f"Nudge {state.name} toward role target",
                }
            )

        if state.role == "vocal" and state.feature.air_band_ratio > 0.18:
            actions.append({"type": "deesser", "targets": [state.name], "delta_db": -0.6, "label": f"De-ess {state.name}"})

        if state.role in {"vocal", "snare", "toms", "bass"} and state.feature.crest_db > 20.0:
            actions.append(
                {
                    "type": "compress",
                    "targets": [state.name],
                    "ratio": 2.4 if state.role == "vocal" else 2.8,
                    "threshold_db": -24.0 if state.role == "vocal" else -26.0,
                    "makeup_db": 0.25,
                    "label": f"Add compression to {state.name}",
                }
            )

        if state.role in {"vocal", "guitar", "keys"} and state.highpass_hz < 80.0:
            actions.append({"type": "highpass", "targets": [state.name], "hz": 90.0, "label": f"High-pass {state.name}"})

    if role_levels.get("vocal", -30.0) < -25.0:
        actions.append({"type": "gain", "targets": ["vocal"], "delta_db": 0.5, "label": "Bring vocals forward"})

    return actions


def _evaluate_states(
    track_states: list[TrackState],
    sr: int,
    preview_samples: int,
    reference_metrics: dict[str, float] | None = None,
) -> tuple[float, dict[str, float], dict[str, float], list[str]]:
    stereo = _render_mix(track_states, preview_samples)
    metrics = _mix_metrics(stereo, sr)
    role_levels = _role_levels(track_states)
    score, notes = _score_mix(metrics, role_levels, reference_metrics)
    return score, metrics, role_levels, notes


def _optimize_iteration(
    working_states: list[TrackState],
    sr: int,
    preview_samples: int,
    reference_metrics: dict[str, float] | None = None,
    max_actions: int = 3,
    min_improvement: float = 0.08,
) -> tuple[list[dict], float, dict[str, float], dict[str, float], list[str]]:
    accepted_actions: list[dict] = []
    current_score, current_metrics, current_levels, current_notes = _evaluate_states(
        working_states,
        sr,
        preview_samples,
        reference_metrics,
    )

    for _ in range(max_actions):
        candidates = _build_candidate_actions(working_states, current_metrics)
        best_candidate: dict | None = None
        best_score = current_score
        best_metrics = current_metrics
        best_levels = current_levels
        best_notes = current_notes

        for candidate in candidates:
            trial_states = deepcopy(working_states)
            _apply_action(trial_states, candidate)
            trial_score, trial_metrics, trial_levels, trial_notes = _evaluate_states(
                trial_states,
                sr,
                preview_samples,
                reference_metrics,
            )
            improvement = trial_score - current_score
            if improvement > min_improvement and trial_score > best_score:
                best_candidate = deepcopy(candidate)
                best_candidate["improvement"] = round(improvement, 4)
                best_score = trial_score
                best_metrics = trial_metrics
                best_levels = trial_levels
                best_notes = trial_notes

        if best_candidate is None:
            break

        _apply_action(working_states, best_candidate)
        accepted_actions.append(best_candidate)
        current_score = best_score
        current_metrics = best_metrics
        current_levels = best_levels
        current_notes = best_notes

    return accepted_actions, current_score, current_metrics, current_levels, current_notes


def run_iterative_auto_mix(
    paths: list[Path],
    output_dir: Path,
    mix_mode: str,
    iterations: int,
    preview_sec: float,
    reference_file: Path | None = None,
) -> dict:
    if mix_mode not in {"bad", "decent", "best"}:
        raise ValueError("mix_mode must be one of: bad, decent, best")

    track_states, sr, max_len = _build_track_states(paths, mix_mode)
    preview_samples = min(max_len, int(preview_sec * sr)) if preview_sec > 0 else max_len
    reference_metrics = None
    if reference_file is not None:
        reference_metrics = _load_reference_metrics(reference_file, sr, preview_samples)

    output_dir.mkdir(parents=True, exist_ok=True)
    preview_dir = output_dir / "mix_previews"
    preview_dir.mkdir(parents=True, exist_ok=True)

    working_states = deepcopy(track_states)
    logs: list[dict] = []

    rounds = iterations if mix_mode == "best" else 1
    best_score = -1.0
    best_states = deepcopy(working_states)
    stagnant_rounds = 0

    for idx in range(rounds):
        stereo = _render_mix(working_states, preview_samples)
        metrics = _mix_metrics(stereo, sr)
        role_levels = _role_levels(working_states)
        score, notes = _score_mix(metrics, role_levels, reference_metrics)

        preview_path = preview_dir / f"{mix_mode}_iter_{idx:02d}.wav"
        sf.write(preview_path, stereo, sr, subtype="FLOAT")

        log_entry = {
            "iteration": idx,
            "score": round(score, 3),
            "metrics": metrics,
            "role_levels_lufs": role_levels,
            "notes": notes,
            "preview_path": str(preview_path),
            "track_settings": [
                {
                    "name": s.name,
                    "role": s.role,
                    "gain_db": round(s.gain_db, 3),
                    "pan": round(s.pan, 3),
                    "highpass_hz": round(s.highpass_hz, 2),
                    "low_shelf_db": round(s.low_shelf_db, 2),
                    "presence_gain_db": round(s.presence_gain_db, 2),
                    "air_gain_db": round(s.air_gain_db, 2),
                    "deesser_db": round(s.deesser_db, 2),
                    "comp_ratio": round(s.comp_ratio, 2),
                }
                for s in working_states
            ],
        }
        if reference_metrics is not None:
            log_entry["reference_metrics"] = reference_metrics
        logs.append(log_entry)

        if score > best_score:
            best_score = score
            best_states = deepcopy(working_states)
            stagnant_rounds = 0
        else:
            stagnant_rounds += 1

        if mix_mode == "best" and idx < rounds - 1:
            accepted_actions, opt_score, opt_metrics, _, opt_notes = _optimize_iteration(
                working_states,
                sr,
                preview_samples,
                reference_metrics,
            )
            log_entry["accepted_actions"] = accepted_actions
            if accepted_actions:
                log_entry["post_action_score"] = round(opt_score, 3)
                log_entry["post_action_metrics"] = opt_metrics
                log_entry["post_action_notes"] = opt_notes
                if opt_score > best_score:
                    best_score = opt_score
                    best_states = deepcopy(working_states)
                    stagnant_rounds = 0
            else:
                stagnant_rounds += 1

            if stagnant_rounds >= 2:
                break

    final_states = best_states if mix_mode == "best" else working_states
    final_stereo = _render_mix(final_states, max_len, apply_effects=(mix_mode == "best"))
    # Apply gentle mix-bus compression for "best" mode — adds density and glue
    # without touching individual track dynamics during scoring iterations.
    if mix_mode == "best":
        final_stereo = bus_compress(final_stereo, sr, threshold_db=-12.0, ratio=2.5, makeup_db=1.5)
    final_path = output_dir / f"mix_{mix_mode}_final.wav"
    sf.write(final_path, final_stereo, sr, subtype="FLOAT")

    final_track_settings = [
        {
            "name": s.name,
            "role": s.role,
            "gain_db": round(s.gain_db, 4),
            "pan": round(s.pan, 4),
            "bus_index": _role_bus_index(s.role),
            "highpass_hz": round(s.highpass_hz, 4),
            "low_shelf_db": round(s.low_shelf_db, 4),
            "low_mid_db": round(s.low_mid_db, 4),
            "presence_gain_db": round(s.presence_gain_db, 4),
            "air_gain_db": round(s.air_gain_db, 4),
            "deesser_db": round(s.deesser_db, 4),
            "comp_threshold_db": round(s.comp_threshold_db, 4),
            "comp_ratio": round(s.comp_ratio, 4),
            "comp_makeup_db": round(s.comp_makeup_db, 4),
            "reverb_decay_s": round(s.reverb_decay_s, 4),
            "reverb_predelay_ms": round(s.reverb_predelay_ms, 4),
            "reverb_mix": round(s.reverb_mix, 4),
            "delay_time_ms": round(s.delay_time_ms, 4),
            "delay_feedback": round(s.delay_feedback, 4),
            "delay_mix": round(s.delay_mix, 4),
            "saturation_drive_db": round(s.saturation_drive_db, 4),
            "saturation_blend": round(s.saturation_blend, 4),
        }
        for s in final_states
    ]

    # Match the final Python render's gentle bus compression for host-side application.
    master_bus = {
        "comp_threshold_db": -12.0,
        "comp_ratio": 2.5,
        "comp_makeup_db": 1.5,
    }

    summary = {
        "mix_mode": mix_mode,
        "iterations_requested": iterations,
        "iterations_ran": len(logs),
        "sample_rate": sr,
        "preview_seconds": preview_sec,
        "reference_file": str(reference_file) if reference_file is not None else None,
        "reference_metrics": reference_metrics,
        "best_score": round(best_score, 3),
        "final_mix_path": str(final_path),
        "final_track_settings": final_track_settings,
        "master_bus": master_bus,
        "iteration_logs": logs,
    }

    json_path = output_dir / f"auto_mix_{mix_mode}.json"
    json_path.write_text(json.dumps(summary, indent=2), encoding="utf-8")

    md_lines: list[str] = []
    md_lines.append(f"# Auto-Mix Iteration Report ({mix_mode})")
    md_lines.append("")
    md_lines.append(f"Final mix: {final_path}")
    md_lines.append(f"Best score: {summary['best_score']}")
    if reference_file is not None:
        md_lines.append(f"Reference: {reference_file}")
    md_lines.append("")
    md_lines.append("## Iterations")
    md_lines.append("")
    for log in logs:
        m = log["metrics"]
        md_lines.append(
            f"- Iter {log['iteration']:02d} | score {log['score']:.3f} | peak {m['peak_dbfs']:.2f} dBFS | LUFS {m['lufs_integrated']:.2f} | low {m['low_band_ratio']:.3f} | presence {m['presence_band_ratio']:.3f} | air {m['air_band_ratio']:.3f}"
        )
        for note in log.get("notes", []):
            md_lines.append(f"  - Note: {note}")
        for action in log.get("accepted_actions", []):
            md_lines.append(f"  - Accepted: {action['label']} (+{action['improvement']:.3f})")
        if "post_action_score" in log:
            md_lines.append(f"  - Post-action score: {log['post_action_score']:.3f}")

    md_path = output_dir / f"auto_mix_{mix_mode}.md"
    md_path.write_text("\n".join(md_lines) + "\n", encoding="utf-8")

    # Compact machine-readable payload for host automation apply path.
    apply_json = output_dir / "auto_mix_apply.json"
    apply_json.write_text(
        json.dumps({"tracks": final_track_settings, "master_bus": master_bus}, indent=2),
        encoding="utf-8",
    )

    summary["json_path"] = str(json_path)
    summary["markdown_path"] = str(md_path)
    summary["apply_json_path"] = str(apply_json)
    return summary
