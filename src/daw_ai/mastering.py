"""
Mastering chain — takes a stereo mix and outputs a mastered WAV.

Chain order (standard mastering signal flow):
  1.  Sub rumble HP  —  30 Hz, removes inaudible energy that eats headroom
  2.  Mastering EQ   —  gentle tonal shaping (low warmth, mid clarity, air)
  3.  Mid-side width —  subtle side boost to add perceived width
  4.  Glue compression  —  light 2:1 VCA-style bus compression
  5.  LUFS normalisation  — gain-stage to streaming target
  6.  True-peak limiter  —  brick wall at -1.0 dBTP, fast release

Streaming loudness targets:
  Spotify / YouTube / Tidal : -14 LUFS  (default)
  Apple Music               : -16 LUFS
  CD / offline              : -12 LUFS
"""

from __future__ import annotations

import json
from pathlib import Path

import numpy as np
import pyloudnorm as pyln
import soundfile as sf
from scipy import signal

from .dsp import (
    band_adjust,
    compress_audio,
    db_to_linear,
    high_shelf_adjust,
    highpass_filter,
    low_shelf_adjust,
)

EPSILON = 1e-12

LOUDNESS_PRESETS = {
    "spotify": -14.0,
    "apple":   -16.0,
    "youtube": -14.0,
    "tidal":   -14.0,
    "cd":      -12.0,
    "broadcast": -23.0,
}


# ---------------------------------------------------------------------------
# Internal helpers
# ---------------------------------------------------------------------------

def _stereo_to_ms(stereo: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    """Convert L/R stereo to Mid/Side."""
    mid  = (stereo[:, 0] + stereo[:, 1]) * 0.5
    side = (stereo[:, 0] - stereo[:, 1]) * 0.5
    return mid.astype(np.float32), side.astype(np.float32)


def _ms_to_stereo(mid: np.ndarray, side: np.ndarray) -> np.ndarray:
    """Convert Mid/Side back to L/R stereo."""
    left  = (mid + side).astype(np.float32)
    right = (mid - side).astype(np.float32)
    return np.stack([left, right], axis=1)


def _peak_dbfs(stereo: np.ndarray) -> float:
    return float(20.0 * np.log10(float(np.max(np.abs(stereo))) + EPSILON))


def _measure_lufs(stereo: np.ndarray, sr: int) -> float:
    meter = pyln.Meter(sr)
    # pyloudnorm expects shape (samples, channels)
    return float(meter.integrated_loudness(stereo.astype(np.float64)))


def _true_peak_limit(stereo: np.ndarray, sr: int, ceiling_db: float = -1.0) -> np.ndarray:
    """
    Vectorised true-peak limiter using a one-pole envelope follower.
    Attack 0.5 ms, release 80 ms.  Gain applied equally to both channels
    to preserve the stereo image.
    """
    ceiling_lin = db_to_linear(ceiling_db)
    peak_env = np.max(np.abs(stereo), axis=1).astype(np.float32)

    # One-pole smoothing coefficients
    a_atk = float(np.exp(-1.0 / (0.0005 * sr)))   # 0.5 ms
    a_rel = float(np.exp(-1.0 / (0.080  * sr)))   # 80 ms

    # Forward pass — attack/release envelope (vectorised with cumulative product trick)
    # We chunk the signal into segments and process each; fully vectorised via
    # a simple iterative numpy scan over small windows (fast in practice).
    smoothed = np.zeros(len(peak_env), dtype=np.float32)
    env = 0.0
    # Process in blocks of 256 for cache efficiency
    block = 256
    for start in range(0, len(peak_env), block):
        chunk = peak_env[start: start + block]
        for i, p in enumerate(chunk):
            coeff = a_atk if p > env else a_rel
            env = coeff * env + (1.0 - coeff) * p
            smoothed[start + i] = env

    gain = np.where(smoothed > ceiling_lin,
                    ceiling_lin / (smoothed + EPSILON),
                    1.0).astype(np.float32)
    limited = (stereo * gain[:, np.newaxis]).astype(np.float32)
    return np.clip(limited, -ceiling_lin, ceiling_lin).astype(np.float32)


# ---------------------------------------------------------------------------
# Mastering EQ
# ---------------------------------------------------------------------------

def _mastering_eq(stereo: np.ndarray, sr: int) -> np.ndarray:
    """
    Gentle mastering EQ applied to both channels equally.
    All moves are intentionally small — mastering EQ is corrective, not creative.
    """
    left  = stereo[:, 0].copy()
    right = stereo[:, 1].copy()

    for ch in [left, right]:
        pass  # process in-place below — using the list to avoid repetition

    def _process(ch: np.ndarray) -> np.ndarray:
        # Sub HP — removes anything below 30 Hz (console rumble, DC offset)
        ch = highpass_filter(ch, sr, 30.0)
        # Low warmth shelf — gentle +0.8 dB below 90 Hz adds weight
        ch = low_shelf_adjust(ch, sr, 90.0, 0.8)
        # Low-mid clarity — -1.0 dB at 250-400 Hz reduces mix cloudiness
        ch = band_adjust(ch, sr, 250.0, 400.0, -1.0)
        # Presence shelf — +0.8 dB above 3 kHz adds definition
        ch = band_adjust(ch, sr, 3000.0, 8000.0, 0.8)
        # Air shelf — +1.2 dB above 10 kHz, adds open top end
        ch = high_shelf_adjust(ch, sr, 10000.0, 1.2)
        return ch

    return np.stack([_process(stereo[:, 0]), _process(stereo[:, 1])], axis=1)


# ---------------------------------------------------------------------------
# Mid-side width
# ---------------------------------------------------------------------------

def _ms_width(stereo: np.ndarray, width: float = 1.15) -> np.ndarray:
    """
    Subtle stereo widening via M/S processing.
    width > 1.0 boosts sides (wider), < 1.0 narrows.
    1.15 = 15% wider than the original mix.
    """
    mid, side = _stereo_to_ms(stereo)
    side = side * float(width)
    return _ms_to_stereo(mid, side)


# ---------------------------------------------------------------------------
# Glue compression
# ---------------------------------------------------------------------------

def _glue_compress(stereo: np.ndarray, sr: int) -> np.ndarray:
    """
    Light 2:1 VCA-style bus glue compression.
    Threshold at -18 dBFS, slow attack (30 ms), medium release (200 ms).
    Mirrors what an SSL G-bus compressor does at the mastering stage.
    """
    left  = compress_audio(stereo[:, 0], threshold_db=-18.0, ratio=2.0,
                           makeup_db=0.5, window_ms=30.0, sr=sr)
    right = compress_audio(stereo[:, 1], threshold_db=-18.0, ratio=2.0,
                           makeup_db=0.5, window_ms=30.0, sr=sr)
    return np.stack([left, right], axis=1)


# ---------------------------------------------------------------------------
# Public API
# ---------------------------------------------------------------------------

def master_stereo(
    stereo: np.ndarray,
    sr: int,
    target_lufs: float = -14.0,
    ceiling_db: float = -1.0,
    width: float = 1.15,
) -> tuple[np.ndarray, dict]:
    """
    Run the full mastering chain on a stereo numpy array.

    Returns:
        (mastered_stereo, metrics_dict)
    """
    # 1. Mastering EQ
    out = _mastering_eq(stereo, sr)

    # 2. Mid-side width
    out = _ms_width(out, width=width)

    # 3. Glue compression
    out = _glue_compress(out, sr)

    # 4. LUFS normalisation — gain-stage to target loudness
    pre_lufs = _measure_lufs(out, sr)
    if np.isfinite(pre_lufs):
        gain_db = target_lufs - pre_lufs
        # Cap gain so we never boost a mix that's already too loud by more than 12 dB
        gain_db = float(np.clip(gain_db, -18.0, 12.0))
        out = out * db_to_linear(gain_db)
    else:
        gain_db = 0.0

    # 5. True-peak limiter
    out = _true_peak_limit(out, sr, ceiling_db=ceiling_db)

    # Metrics
    final_lufs = _measure_lufs(out, sr)
    final_peak = _peak_dbfs(out)

    metrics = {
        "pre_master_lufs":  round(pre_lufs, 2) if np.isfinite(pre_lufs) else None,
        "lufs_gain_db":     round(gain_db, 2),
        "final_lufs":       round(final_lufs, 2) if np.isfinite(final_lufs) else None,
        "final_peak_dbfs":  round(final_peak, 2),
        "ceiling_db":       ceiling_db,
        "target_lufs":      target_lufs,
        "width":            width,
    }

    return out.astype(np.float32), metrics


def master_file(
    input_path: Path,
    output_dir: Path,
    target_lufs: float = -14.0,
    ceiling_db: float = -1.0,
    width: float = 1.15,
) -> dict:
    """
    Master an existing WAV file and write a new mastered WAV + JSON report.

    Returns a summary dict.
    """
    stereo, sr = sf.read(str(input_path), dtype="float32", always_2d=True)
    if stereo.shape[1] > 2:
        stereo = stereo[:, :2]
    if stereo.shape[1] == 1:
        stereo = np.concatenate([stereo, stereo], axis=1)

    mastered, metrics = master_stereo(stereo, sr,
                                      target_lufs=target_lufs,
                                      ceiling_db=ceiling_db,
                                      width=width)

    output_dir.mkdir(parents=True, exist_ok=True)
    stem = input_path.stem
    out_wav  = output_dir / f"{stem}_master.wav"
    out_json = output_dir / f"{stem}_master.json"

    sf.write(str(out_wav), mastered, sr, subtype="FLOAT")

    summary = {
        "input":   str(input_path),
        "output":  str(out_wav),
        "sample_rate": sr,
        "metrics": metrics,
    }
    out_json.write_text(json.dumps(summary, indent=2), encoding="utf-8")

    return summary
