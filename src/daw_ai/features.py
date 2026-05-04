from __future__ import annotations

from pathlib import Path

import librosa
import numpy as np
import pyloudnorm as pyln

from .models import TrackFeatures

EPSILON = 1e-12


def _dbfs(value: float) -> float:
    return float(20.0 * np.log10(max(value, EPSILON)))


def _safe_ratio(numerator: float, denominator: float) -> float:
    return float(numerator / denominator) if denominator > EPSILON else 0.0


def extract_track_features(path: Path) -> TrackFeatures:
    audio, sr = librosa.load(path.as_posix(), sr=None, mono=True)

    if audio.size == 0:
        raise ValueError(f"Audio file contains no samples: {path}")

    peak = float(np.max(np.abs(audio)))
    rms = float(np.sqrt(np.mean(np.square(audio))))
    crest_db = _dbfs(peak) - _dbfs(rms)
    clipping_ratio = float(np.mean(np.abs(audio) >= 0.999))

    meter = pyln.Meter(sr)
    loudness = float(meter.integrated_loudness(audio))

    centroid = librosa.feature.spectral_centroid(y=audio, sr=sr)
    spectral_centroid = float(np.mean(centroid))

    stft = librosa.stft(audio, n_fft=4096, hop_length=1024)
    mag = np.abs(stft)
    freqs = librosa.fft_frequencies(sr=sr, n_fft=4096)

    power_per_bin = np.mean(mag, axis=1)
    total_power = float(np.sum(power_per_bin) + EPSILON)

    low_power = float(np.sum(power_per_bin[(freqs >= 20.0) & (freqs < 120.0)]))
    presence_power = float(np.sum(power_per_bin[(freqs >= 2000.0) & (freqs < 5000.0)]))
    air_power = float(np.sum(power_per_bin[freqs >= 8000.0]))

    return TrackFeatures(
        name=path.name,
        path=str(path),
        sample_rate=int(sr),
        duration_sec=float(len(audio) / sr),
        peak_dbfs=_dbfs(peak),
        rms_dbfs=_dbfs(rms),
        lufs_integrated=loudness,
        crest_db=crest_db,
        clipping_ratio=clipping_ratio,
        spectral_centroid_hz=spectral_centroid,
        low_band_ratio=_safe_ratio(low_power, total_power),
        presence_band_ratio=_safe_ratio(presence_power, total_power),
        air_band_ratio=_safe_ratio(air_power, total_power),
    )
