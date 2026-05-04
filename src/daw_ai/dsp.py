from __future__ import annotations

import numpy as np
from scipy import signal

EPSILON = 1e-12


def db_to_linear(db_value: float) -> float:
    return float(10.0 ** (db_value / 20.0))


def highpass_filter(audio: np.ndarray, sr: int, cutoff_hz: float) -> np.ndarray:
    if cutoff_hz <= 0.0:
        return audio
    nyquist = sr * 0.5
    normalized = min(max(cutoff_hz / nyquist, 1e-5), 0.99)
    sos = signal.butter(2, normalized, btype="highpass", output="sos")
    return signal.sosfilt(sos, audio).astype(np.float32)


def band_adjust(audio: np.ndarray, sr: int, low_hz: float, high_hz: float, gain_db: float) -> np.ndarray:
    if abs(gain_db) < 1e-6:
        return audio
    nyquist = sr * 0.5
    low = min(max(low_hz / nyquist, 1e-5), 0.98)
    high = min(max(high_hz / nyquist, low + 1e-5), 0.999)
    sos = signal.butter(2, [low, high], btype="bandpass", output="sos")
    band = signal.sosfilt(sos, audio)
    factor = db_to_linear(gain_db) - 1.0
    return (audio + band * factor).astype(np.float32)


def high_shelf_adjust(audio: np.ndarray, sr: int, cutoff_hz: float, gain_db: float) -> np.ndarray:
    if abs(gain_db) < 1e-6:
        return audio
    nyquist = sr * 0.5
    normalized = min(max(cutoff_hz / nyquist, 1e-5), 0.99)
    sos = signal.butter(2, normalized, btype="highpass", output="sos")
    band = signal.sosfilt(sos, audio)
    factor = db_to_linear(gain_db) - 1.0
    return (audio + band * factor).astype(np.float32)


def low_shelf_adjust(audio: np.ndarray, sr: int, cutoff_hz: float, gain_db: float) -> np.ndarray:
    if abs(gain_db) < 1e-6:
        return audio
    nyquist = sr * 0.5
    normalized = min(max(cutoff_hz / nyquist, 1e-5), 0.99)
    sos = signal.butter(2, normalized, btype="lowpass", output="sos")
    band = signal.sosfilt(sos, audio)
    factor = db_to_linear(gain_db) - 1.0
    return (audio + band * factor).astype(np.float32)


# ---------------------------------------------------------------------------
# Reverb — multi-tap diffuse echo (fast, pure numpy)
# ---------------------------------------------------------------------------
# Uses a dense grid of echo taps with exponential decay and slight HF rolloff
# per tap to simulate room diffusion. No per-sample loops — fully vectorised.
# Not a true Freeverb, but sounds natural and runs fast on long audio.

# Early-reflection delay offsets in ms (prime-ish spacings for diffusion)
_ER_MS = (17.3, 29.7, 41.1, 53.8, 68.4, 83.6, 102.1, 124.5,
          149.2, 178.0, 211.3, 250.7)


def apply_reverb(
    audio: np.ndarray,
    sr: int,
    decay_s: float,
    pre_delay_ms: float = 0.0,
    mix: float = 0.10,
) -> np.ndarray:
    """Multi-tap diffuse reverb. Fast vectorised implementation."""
    if mix <= 0.0 or decay_s <= 0.0:
        return audio

    n = len(audio)
    wet = np.zeros(n, dtype=np.float32)

    # Low-pass filter to darken the wet signal (reverb tails are darker)
    nyq = sr * 0.5
    sos_lp = signal.butter(2, min(4500.0 / nyq, 0.98), btype="low", output="sos")
    dark = signal.sosfilt(sos_lp, audio).astype(np.float32)

    for er_ms in _ER_MS:
        tap_ms = pre_delay_ms + er_ms
        tap_s = tap_ms / 1000.0
        shift = int(tap_s * sr)
        if shift >= n:
            break
        # Amplitude at this tap decays with RT60-style exponential
        amplitude = float(np.exp(-6.91 * tap_s / max(decay_s, 0.05)))
        wet[shift:] += dark[: n - shift] * amplitude

    # Normalise wet relative to input so mix% is predictable
    wet_peak = float(np.max(np.abs(wet)) + EPSILON)
    src_peak = float(np.max(np.abs(audio)) + EPSILON)
    if wet_peak > EPSILON:
        wet = wet * (src_peak / wet_peak)

    return (audio * (1.0 - mix) + wet * mix).astype(np.float32)


def apply_delay(
    audio: np.ndarray,
    sr: int,
    time_ms: float,
    feedback: float = 0.25,
    mix: float = 0.15,
) -> np.ndarray:
    """Tape-style feedback delay (echo). Vectorised — no Python loop per sample."""
    if mix <= 0.0 or time_ms <= 0.0:
        return audio
    delay_samples = int(sr * time_ms / 1000.0)
    if delay_samples <= 0 or delay_samples >= len(audio):
        return audio
    # Unroll the feedback loop as a geometric series of shifted copies
    wet = np.zeros_like(audio)
    level = feedback
    for k in range(1, 16):
        shift = delay_samples * k
        if shift >= len(audio) or level < 0.005:
            break
        wet[shift:] += audio[: len(audio) - shift] * level
        level *= feedback
    return (audio * (1.0 - mix) + wet * mix).astype(np.float32)


def apply_saturation(
    audio: np.ndarray,
    drive_db: float,
    blend: float = 0.5,
) -> np.ndarray:
    """Soft-clip (tanh) saturation with parallel dry/wet blend."""
    if blend <= 0.0 or drive_db <= 0.0:
        return audio
    drive = db_to_linear(drive_db)
    driven = np.tanh(audio * drive) / drive
    return (audio * (1.0 - blend) + driven * blend).astype(np.float32)


def bus_compress(
    stereo: np.ndarray,
    sr: int,
    threshold_db: float = -12.0,
    ratio: float = 2.5,
    makeup_db: float = 1.5,
) -> np.ndarray:
    """Light mix-bus compression applied to a stereo array (shape: [n_samples, 2])."""
    left = compress_audio(stereo[:, 0], threshold_db=threshold_db, ratio=ratio, makeup_db=makeup_db, sr=sr)
    right = compress_audio(stereo[:, 1], threshold_db=threshold_db, ratio=ratio, makeup_db=makeup_db, sr=sr)
    return np.stack([left, right], axis=1)

def compress_audio(
    audio: np.ndarray,
    threshold_db: float,
    ratio: float,
    makeup_db: float = 0.0,
    window_ms: float = 25.0,
    sr: int = 44100,
) -> np.ndarray:
    if ratio <= 1.0:
        return audio

    window = max(1, int(sr * window_ms / 1000.0))
    kernel = np.ones(window, dtype=np.float32) / window
    envelope = np.sqrt(np.convolve(np.square(audio), kernel, mode="same") + EPSILON)
    envelope_db = 20.0 * np.log10(np.maximum(envelope, EPSILON))
    overshoot = np.maximum(envelope_db - threshold_db, 0.0)
    gain_reduction_db = overshoot * (1.0 - (1.0 / ratio))
    gain = np.power(10.0, (-gain_reduction_db + makeup_db) / 20.0)
    return (audio * gain).astype(np.float32)
