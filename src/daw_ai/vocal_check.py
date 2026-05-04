from __future__ import annotations

import argparse
import json
from pathlib import Path

import librosa
import numpy as np

EPS = 1e-12
MAJOR_PROFILE = np.array([6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88], dtype=float)
MINOR_PROFILE = np.array([6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17], dtype=float)
NOTE_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]

# Triad templates: [root, M3/m3, P5] in pitch-class space
def _make_chord_templates() -> np.ndarray:
    """Return (24, 12) array: rows 0-11 = major triads, 12-23 = minor triads."""
    templates = np.zeros((24, 12), dtype=float)
    for root in range(12):
        maj = np.zeros(12); maj[root] = 1.0; maj[(root+4)%12] = 0.8; maj[(root+7)%12] = 0.9
        templates[root] = maj
        minr = np.zeros(12); minr[root] = 1.0; minr[(root+3)%12] = 0.8; minr[(root+7)%12] = 0.9
        templates[12+root] = minr
    return templates

CHORD_TEMPLATES = _make_chord_templates()

# Diatonic chords per key (root, mode) → list of (chord_root, chord_mode) pairs
# Natural minor: i ii° III iv v VI VII  (we use maj/min, skip dim)
# Natural major: I ii iii IV V vi vii°  (we use maj/min, skip dim)
_NAT_MINOR_CHORD_OFFSETS = [(0,"minor"),(2,"minor"),(3,"major"),(5,"minor"),(7,"minor"),(8,"major"),(10,"major")]
_NAT_MAJOR_CHORD_OFFSETS = [(0,"major"),(2,"minor"),(4,"minor"),(5,"major"),(7,"major"),(9,"minor"),(11,"minor")]

def _diatonic_chords(root: int, mode: str) -> list[tuple[int,str]]:
    offsets = _NAT_MINOR_CHORD_OFFSETS if mode == "minor" else _NAT_MAJOR_CHORD_OFFSETS
    return [((root + off) % 12, cmode) for off, cmode in offsets]


def _db(v: float) -> float:
    return float(20.0 * np.log10(max(v, EPS)))


def _safe_load_mono(path: Path) -> tuple[np.ndarray, int]:
    y, sr = librosa.load(path.as_posix(), sr=None, mono=True)
    if y.size == 0:
        raise ValueError(f"Audio file contains no samples: {path}")
    return y, sr


def _infer_tempo(y: np.ndarray, sr: int, fallback_bpm: float) -> float:
    tempo, _ = librosa.beat.beat_track(y=y, sr=sr)
    if isinstance(tempo, np.ndarray):
        tempo = float(np.mean(tempo)) if tempo.size else float(fallback_bpm)
    tempo = float(tempo)
    if not np.isfinite(tempo) or tempo < 50.0 or tempo > 220.0:
        return float(fallback_bpm)
    return tempo


def _pearson_key_score(chroma_vec: np.ndarray, profiles: list[tuple[str, np.ndarray]]) -> tuple[str, float, int]:
    """Score 24 keys via Pearson correlation against Krumhansl-Schmuckler profiles."""
    cv = chroma_vec - np.mean(chroma_vec)
    cv_std = np.std(chroma_vec) + EPS
    best_score = -1e9
    second_best = -1e9
    best_root = 0
    best_mode = "major"
    for mode, profile in profiles:
        p = profile - np.mean(profile)
        p_std = np.std(p) + EPS
        for root in range(12):
            rotated = np.roll(p, root)
            score = float(np.dot(cv, rotated) / (cv_std * p_std * 12.0))
            if score > best_score:
                second_best = best_score
                best_score = score
                best_root = root
                best_mode = mode
            elif score > second_best:
                second_best = score
    confidence = float(np.clip((best_score - second_best) * 5.0, 0.0, 1.0))
    return f"{NOTE_NAMES[best_root]} {best_mode}", confidence, best_root


def _detect_beat_chords(y_harm: np.ndarray, sr: int, hop: int) -> list[tuple[int, str]]:
    """Detect the best-fitting chord for each beat. Returns list of (root, mode)."""
    chroma = librosa.feature.chroma_cqt(y=y_harm, sr=sr, hop_length=hop, bins_per_octave=36)
    _, beat_frames = librosa.beat.beat_track(y=y_harm, sr=sr, hop_length=hop)
    if beat_frames.size < 2:
        return []
    # Beat-sync: median chroma across each beat window
    chroma_sync = librosa.util.sync(chroma, beat_frames, aggregate=np.median)  # (12, n_beats)
    chords: list[tuple[int, str]] = []
    for i in range(chroma_sync.shape[1]):
        cv = chroma_sync[:, i]
        cv = cv / (np.max(cv) + EPS)
        # Cosine similarity against all 24 triad templates
        scores = CHORD_TEMPLATES @ cv
        norms = np.linalg.norm(CHORD_TEMPLATES, axis=1) * (np.linalg.norm(cv) + EPS)
        scores = scores / (norms + EPS)
        best_idx = int(np.argmax(scores))
        chord_root = best_idx % 12
        chord_mode = "major" if best_idx < 12 else "minor"
        chords.append((chord_root, chord_mode))
    return chords


def _key_from_chord_progression(chords: list[tuple[int, str]]) -> tuple[str, float, int]:
    """
    Score all 24 keys from the detected chord sequence using four signals:
      1. Diatonic coverage: fraction of chords diatonic to the key.
      2. First-chord bias: strong bonus if the first chord is the tonic.
      3. Last-chord bias: bonus if the song resolves back to the tonic at the end.
      4. Cadential resolutions: count how many chord transitions resolve TO the tonic
         via common cadences (v→i, VII→i, iv→i, IV→I, ii→I).
    """
    if not chords:
        return "C major", 0.0, 0

    from collections import Counter
    n = len(chords)
    chord_counts = Counter(chords)
    total = float(n)
    first = chords[0]
    last = chords[-1]

    # Build transition list (from_chord, to_chord)
    transitions: list[tuple[tuple[int,str], tuple[int,str]]] = [
        (chords[i], chords[i+1]) for i in range(n-1)
    ]

    best_score = -1e9
    second_best = -1e9
    best_root = 0
    best_mode = "major"

    for key_root in range(12):
        for key_mode in ("major", "minor"):
            diatonic = set(_diatonic_chords(key_root, key_mode))
            tonic = (key_root, key_mode)

            # 1. Diatonic fit
            diatonic_hits = sum(cnt for chord, cnt in chord_counts.items() if chord in diatonic)
            diatonic_score = diatonic_hits / total

            # 2. First-chord bias: tonic is very commonly the opening chord
            first_score = 1.0 if first == tonic else 0.0

            # 3. Last-chord bias: resolution back to tonic at phrase end
            last_score = 1.0 if last == tonic else 0.0

            # 4. Cadential resolution: transitions that resolve TO the tonic
            # For minor: v→i, V→i (dominant minor/major to tonic)
            #            VII→i  (subtonic resolution, natural minor hallmark)
            #            iv→i   (plagal)
            # For major: V→I, IV→I, ii→I
            if key_mode == "minor":
                resolution_sources = {
                    ((key_root+7)%12, "minor"),   # v (natural)
                    ((key_root+7)%12, "major"),   # V (harmonic minor)
                    ((key_root+10)%12, "major"),  # VII (subtonic) ← key G minor vs D minor differentiator
                    ((key_root+5)%12, "minor"),   # iv (plagal)
                }
            else:
                resolution_sources = {
                    ((key_root+7)%12, "major"),   # V
                    ((key_root+5)%12, "major"),   # IV (plagal)
                    ((key_root+2)%12, "minor"),   # ii
                }
            cadence_hits = sum(1 for frm, to in transitions if to == tonic and frm in resolution_sources)
            cadence_score = cadence_hits / max(1.0, float(n - 1))

            # Weighted combination — first/last chord are the strongest single-chord clues
            score = (diatonic_score * 0.35
                     + first_score  * 0.30
                     + last_score   * 0.15
                     + cadence_score * 0.20)

            if score > best_score:
                second_best = best_score
                best_score = score
                best_root = key_root
                best_mode = key_mode
            elif score > second_best:
                second_best = score

    confidence = float(np.clip((best_score - second_best) * 10.0, 0.0, 1.0))
    return f"{NOTE_NAMES[best_root]} {best_mode}", confidence, best_root


def _infer_key(y: np.ndarray, sr: int) -> tuple[str, float, int]:
    """
    Multi-window majority-vote key detection.
    Split the track into overlapping 10-second windows, run KS on each,
    then take the plurality winner weighted by per-window confidence.
    This is far more robust on real mixed tracks than any single-pass method.
    """
    hop = 512
    profiles = [("major", MAJOR_PROFILE), ("minor", MINOR_PROFILE)]

    # Aggressive harmonic separation to remove drums before any chroma work
    y_harm = librosa.effects.harmonic(y, margin=16.0)

    # ── Multi-window KS voting ────────────────────────────────────────────────
    # Split into overlapping 10-second windows. Each window votes for a key
    # weighted by its KS confidence. Plurality winner is the final answer.
    # This survives chord changes, vamps, and noisy sections far better than
    # any single-pass analysis.
    chroma_full = librosa.feature.chroma_cqt(y=y_harm, sr=sr, hop_length=hop, bins_per_octave=36)
    n_total = chroma_full.shape[1]

    win_frames = max(int(10.0 * sr / hop), n_total // 4)
    step_frames = win_frames // 2

    vote_scores: dict[tuple[int, str], float] = {}
    starts = list(range(0, max(1, n_total - win_frames + 1), step_frames))
    if not starts:
        starts = [0]

    rms_harm = librosa.feature.rms(y=y_harm, frame_length=2048, hop_length=hop)[0]

    for start in starts:
        end = min(start + win_frames, n_total)
        seg = chroma_full[:, start:end]
        if seg.shape[1] < 8:
            continue
        rms_seg = rms_harm[start:min(end, rms_harm.shape[0])]
        n_seg = min(seg.shape[1], rms_seg.shape[0])
        w = rms_seg[:n_seg] / (np.sum(rms_seg[:n_seg]) + EPS)
        cv = np.sum(seg[:, :n_seg] * w[None, :], axis=1)
        cv /= np.sum(cv) + EPS

        key_name_w, conf_w, root_w = _pearson_key_score(cv, profiles)
        mode_w = "minor" if key_name_w.endswith("minor") else "major"
        key_tuple = (root_w, mode_w)
        vote_scores[key_tuple] = vote_scores.get(key_tuple, 0.0) + conf_w

    if not vote_scores:
        # Last-resort single-pass
        n = min(n_total, rms_harm.shape[0])
        w = rms_harm[:n] / (np.sum(rms_harm[:n]) + EPS)
        cv = np.sum(chroma_full[:, :n] * w[None, :], axis=1)
        cv /= np.sum(cv) + EPS
        return _pearson_key_score(cv, profiles)

    best_key = max(vote_scores, key=lambda k: vote_scores[k])
    total_votes = sum(vote_scores.values())
    best_votes = vote_scores[best_key]
    others = [v for k, v in vote_scores.items() if k != best_key]
    second_votes = max(others) if others else 0.0
    confidence = float(np.clip((best_votes - second_votes) / (total_votes + EPS) * 3.0, 0.0, 1.0))

    best_root, best_mode = best_key
    return f"{NOTE_NAMES[best_root]} {best_mode}", confidence, best_root


def _scale_pitch_classes(root: int, mode: str) -> set[int]:
    if mode == "minor":
        degrees = [0, 2, 3, 5, 7, 8, 10]
    else:
        degrees = [0, 2, 4, 5, 7, 9, 11]
    return {(root + d) % 12 for d in degrees}


def _vocal_pitch_key_fit(y: np.ndarray, sr: int, root: int, mode: str, key_conf: float) -> tuple[float, float, float]:
    # Strict voicing threshold reduces noise/silence frames that inflate spread
    f0, voiced_flag, voiced_prob = librosa.pyin(
        y,
        fmin=float(librosa.note_to_hz("C2")),
        fmax=float(librosa.note_to_hz("C6")),
        sr=sr,
        fill_na=None,
    )
    # High voicing confidence gate (0.75) + valid pitch
    voiced = (voiced_prob >= 0.75) & np.isfinite(f0) & (f0 > 0)
    voiced_ratio = float(np.mean(voiced_prob >= 0.5)) if voiced_prob is not None else 0.0
    if not np.any(voiced):
        return 0.0, 0.0, voiced_ratio

    f0v = f0[voiced]
    midi = librosa.hz_to_midi(f0v)

    # Remove tracking outliers: frames more than 1 octave from the 25th–75th percentile range
    q25, q75 = float(np.percentile(midi, 25)), float(np.percentile(midi, 75))
    iqr = q75 - q25
    midi = midi[(midi >= q25 - 12.0) & (midi <= q75 + 12.0)]
    if midi.size == 0:
        return 0.0, 0.0, voiced_ratio

    # Spread = IQR-based robust std (avoids outlier inflation)
    # IQR in semitones → cents; scale to approximate std equivalent
    spread = float(iqr * 100.0 * 0.7413)  # 0.7413 = 1/(2*Φ⁻¹(0.75)) converts IQR→σ

    pc = np.mod(np.rint(midi), 12).astype(int)
    scale = _scale_pitch_classes(root, mode)
    in_key = np.array([p in scale for p in pc], dtype=bool)

    # Allow brief chromatic passing tones; penalize sustained out-of-key spans.
    off = (~in_key).astype(float)
    if off.size > 6:
        kernel = np.ones(7, dtype=float) / 7.0
        sustained_off = np.convolve(off, kernel, mode="same") > 0.8
        off_ratio = float(np.mean(sustained_off))
    else:
        off_ratio = float(np.mean(off))

    if key_conf < 0.35:
        off_ratio *= 0.35

    return off_ratio, spread, voiced_ratio


def _rhythm_template(onsets: np.ndarray, tempo: float) -> tuple[np.ndarray, dict[str, float]]:
    if onsets.size < 3 or tempo <= 1.0:
        return np.array([], dtype=float), {}

    beat = 60.0 / tempo
    ioi = np.diff(onsets)
    ioi = ioi[(ioi > 0.04) & (ioi < 2.0)]
    if ioi.size == 0:
        return np.array([], dtype=float), {}

    candidates = np.array([
        beat / 4.0,      # 16th
        beat / 3.0,      # 8th triplet
        beat / 2.0,      # 8th
        2.0 * beat / 3.0,
        beat,            # quarter
        1.5 * beat,
        2.0 * beat,      # half
    ], dtype=float)

    nearest = np.array([candidates[np.argmin(np.abs(candidates - d))] for d in ioi], dtype=float)
    uniq, counts = np.unique(np.round(nearest, 5), return_counts=True)
    order = np.argsort(counts)[::-1]
    top = uniq[order][: min(4, uniq.size)]
    template = {f"dur_{i + 1}": float(t) for i, t in enumerate(top)}
    return top, template


def _timing_metrics(vocal_onsets: np.ndarray, ref_onsets: np.ndarray, tempo: float, rhythm_bins: np.ndarray) -> tuple[float, float, float, float]:
    if vocal_onsets.size == 0 or tempo <= 1.0:
        return 0.0, 0.0, 0.0, 0.0

    grid = 60.0 / tempo / 4.0  # 16th-note grid
    err = np.abs((vocal_onsets / grid) - np.round(vocal_onsets / grid)) * grid
    timing_median_ms = float(np.median(err) * 1000.0)
    timing_bad_ratio = float(np.mean(err > 0.040))

    rhythm_bad_ratio = 0.0
    if rhythm_bins.size > 0 and vocal_onsets.size > 2:
        ioi = np.diff(vocal_onsets)
        if ioi.size > 0:
            dist = np.min(np.abs(ioi[:, None] - rhythm_bins[None, :]), axis=1)
            rhythm_bad_ratio = float(np.mean(dist > 0.045))

    groove_fit_ratio = 0.0
    if ref_onsets.size > 0:
        tol = max(0.025, grid * 0.30)
        hits = 0
        for t in vocal_onsets:
            if np.min(np.abs(ref_onsets - t)) <= tol:
                hits += 1
        groove_fit_ratio = float(hits / max(1, vocal_onsets.size))

    return timing_median_ms, timing_bad_ratio, rhythm_bad_ratio, groove_fit_ratio


def analyze_vocal(path: Path, fallback_bpm: float, track_name: str, reference_path: Path | None = None) -> dict:
    y, sr = _safe_load_mono(path)

    if reference_path is not None and reference_path.exists():
        y_ref, sr_ref = _safe_load_mono(reference_path)
        if sr_ref != sr:
            y_ref = librosa.resample(y_ref, orig_sr=sr_ref, target_sr=sr)
    else:
        y_ref = y

    duration_sec = float(len(y) / sr)
    peak = float(np.max(np.abs(y)))
    clipping_ratio = float(np.mean(np.abs(y) >= 0.999))

    inferred_tempo = _infer_tempo(y_ref, sr, fallback_bpm)
    key_name, key_conf, key_root = _infer_key(y_ref, sr)
    key_mode = "minor" if key_name.endswith("minor") else "major"

    rms = librosa.feature.rms(y=y, frame_length=2048, hop_length=512)[0]
    rms = np.maximum(rms, EPS)
    noise_floor_db = _db(float(np.percentile(rms, 10.0)))
    median_level_db = _db(float(np.median(rms)))
    snr_proxy_db = float(median_level_db - noise_floor_db)

    vocal_onsets = librosa.onset.onset_detect(y=y, sr=sr, units="time", backtrack=False)
    ref_onsets = librosa.onset.onset_detect(y=y_ref, sr=sr, units="time", backtrack=False)
    rhythm_bins, rhythm_template = _rhythm_template(vocal_onsets, inferred_tempo)
    timing_median_ms, timing_bad_ratio, rhythm_bad_ratio, groove_fit_ratio = _timing_metrics(
        vocal_onsets, ref_onsets, inferred_tempo, rhythm_bins
    )

    pitch_outlier_ratio, pitch_std_cents, pitch_voiced_ratio = _vocal_pitch_key_fit(y, sr, key_root, key_mode, key_conf)

    score = 100.0
    score -= min(40.0, clipping_ratio * 8000.0)
    if peak > 0.98:
        score -= 8.0
    if snr_proxy_db < 14.0:
        score -= min(24.0, (14.0 - snr_proxy_db) * 2.0)
    # Pitch spread: catches all-over-the-place intonation
    if pitch_std_cents > 80.0 and key_conf >= 0.25:
        score -= min(15.0, (pitch_std_cents - 80.0) * 0.18)
    # Off-key ratio: two-tier so genuinely bad pitch gets real penalisation.
    # Tier 1 catches moderate issues; tier 2 stacks for heavily out-of-key takes.
    if pitch_outlier_ratio > 0.10 and key_conf >= 0.25:
        score -= min(25.0, (pitch_outlier_ratio - 0.10) * 110.0)
    if pitch_outlier_ratio > 0.28 and key_conf >= 0.35:
        score -= min(14.0, (pitch_outlier_ratio - 0.28) * 100.0)
    # Timing: median grid error is the most direct signal. Good vocalists with
    # natural feel sit around 30-40ms; truly late/early takes push well past 45ms.
    if timing_median_ms > 45.0:
        score -= min(18.0, (timing_median_ms - 45.0) * 0.6)
    # Extra penalty when >65% of onsets miss the 40ms grid window — indicates
    # erratic placement rather than intentional groove/swing.
    if timing_bad_ratio > 0.65:
        score -= min(10.0, (timing_bad_ratio - 0.65) * 28.0)
    if rhythm_bad_ratio > 0.40:
        score -= min(6.0, (rhythm_bad_ratio - 0.40) * 15.0)
    if groove_fit_ratio < 0.45:
        score -= min(10.0, (0.45 - groove_fit_ratio) * 20.0)

    score = float(np.clip(score, 0.0, 100.0))

    suggestions: list[str] = []
    if clipping_ratio > 0.001 or peak > 0.98:
        suggestions.append("Input is clipping. Lower interface/mic gain by 6-10 dB and leave headroom (peaks below -6 dBFS).")
    if snr_proxy_db < 14.0:
        suggestions.append("Recording sounds noisy/roomy. Move closer to mic, reduce room noise, and use a pop filter.")
    if pitch_std_cents > 80.0 and pitch_outlier_ratio > 0.20 and key_conf >= 0.25:
        suggestions.append("Pitch consistency could be tightened. Do short punch-ins and monitor with the backing at a lower level.")
    elif pitch_outlier_ratio > 0.22 and key_conf >= 0.35:
        suggestions.append("Some sustained notes sit outside the key. Keep stylistic accidentals, but tighten long held notes.")
    elif pitch_outlier_ratio > 0.10 and key_conf >= 0.40:
        suggestions.append("Occasional out-of-key notes detected. Check intonation on sustained and held notes.")
    if groove_fit_ratio < 0.50:
        suggestions.append("Vocal phrasing is not locking to the arrangement groove. Practice against drums/bass and punch-in phrase by phrase.")
    elif timing_bad_ratio > 0.65:
        suggestions.append("Timing is erratic — large portions miss the beat grid. Record to a click with a subdivided count-in.")
    elif timing_median_ms > 50.0:
        suggestions.append("Timing is slightly loose against the click. Try recording to a click with a subdivided count-in.")
    if rhythm_bad_ratio > 0.40:
        suggestions.append("Rhythm pattern deviates from the phrase shape. Practice the phrase subdivisions before re-taking.")
    if key_conf < 0.30:
        suggestions.append("Song key confidence is low; pitch-accuracy feedback may not be fully reliable for this take.")
    if duration_sec < 3.0:
        suggestions.append("Take is very short. Record a longer phrase for more reliable quality analysis.")
    if not suggestions:
        suggestions.append("Take quality is generally good. Focus on expression and light comping.")

    if score >= 80.0:
        verdict = "Good"
    elif score >= 60.0:
        verdict = "Usable with fixes"
    else:
        verdict = "Poor - re-record recommended"

    return {
        "track_name": track_name,
        "input_path": str(path),
        "reference_path": str(reference_path) if reference_path is not None else "",
        "duration_sec": duration_sec,
        "score": score,
        "verdict": verdict,
        "inferred_tempo_bpm": inferred_tempo,
        "inferred_key": key_name,
        "key_confidence": key_conf,
        "rhythm_template_sec": rhythm_template,
        "metrics": {
            "peak_dbfs": _db(peak),
            "clipping_ratio": clipping_ratio,
            "noise_floor_dbfs": noise_floor_db,
            "snr_proxy_db": snr_proxy_db,
            "pitch_voiced_ratio": pitch_voiced_ratio,
            "pitch_std_cents": pitch_std_cents,
            "pitch_outlier_ratio": pitch_outlier_ratio,
            "timing_median_ms": timing_median_ms,
            "timing_bad_ratio": timing_bad_ratio,
            "rhythm_bad_ratio": rhythm_bad_ratio,
            "groove_fit_ratio": groove_fit_ratio,
        },
        "suggestions": suggestions,
    }


def format_report(result: dict) -> str:
    m = result["metrics"]
    lines = [
        f"Vocal Check: {result['track_name']}",
        f"Quality Score: {result['score']:.1f}/100 ({result['verdict']})",
        f"Inferred Tempo: {result['inferred_tempo_bpm']:.1f} BPM",
        f"Inferred Key: {result['inferred_key']} (confidence {result['key_confidence']:.2f})",
        "",
        "Diagnostics:",
        f"- Peak: {m['peak_dbfs']:.1f} dBFS",
        f"- Clipping ratio: {m['clipping_ratio'] * 100:.2f}%",
        f"- Noise floor: {m['noise_floor_dbfs']:.1f} dBFS",
        f"- SNR proxy: {m['snr_proxy_db']:.1f} dB",
        f"- Pitch spread: {m['pitch_std_cents']:.1f} cents",
        f"- Sustained off-key ratio: {m['pitch_outlier_ratio'] * 100:.1f}%",
        f"- Timing median error: {m['timing_median_ms']:.1f} ms",
        f"- Timing bad-hit ratio: {m['timing_bad_ratio'] * 100:.1f}%",
        f"- Rhythm template miss ratio: {m['rhythm_bad_ratio'] * 100:.1f}%",
        f"- Groove fit to arrangement: {m['groove_fit_ratio'] * 100:.1f}%",
        "",
        "Rhythm Template (first pass):",
    ]

    rt = result.get("rhythm_template_sec", {})
    if rt:
        for k, v in rt.items():
            lines.append(f"- {k}: {v:.3f}s")
    else:
        lines.append("- Not enough onset data to infer template")

    lines.append("")
    lines.append("Suggestions:")
    for s in result["suggestions"]:
        lines.append(f"- {s}")
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description="Analyze a vocal take for quality, timing, pitch, clipping, and musical fit.")
    parser.add_argument("--input", type=Path, required=True, help="Path to WAV file to analyze.")
    parser.add_argument("--output", type=Path, required=True, help="Path to output text report.")
    parser.add_argument("--bpm", type=float, default=120.0, help="Fallback BPM if tempo inference is unstable.")
    parser.add_argument("--reference", type=Path, default=None, help="Optional arrangement/reference mix used for tempo/key/groove inference.")
    parser.add_argument("--track-name", type=str, default="Selected Track", help="Label shown in report.")
    args = parser.parse_args()

    result = analyze_vocal(args.input, args.bpm, args.track_name, args.reference)
    report_text = format_report(result)

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(report_text, encoding="utf-8")
    args.output.with_suffix(".json").write_text(json.dumps(result, indent=2), encoding="utf-8")

    print(report_text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
