"""Mix Readiness Gate -- analyses per-bus stem WAVs and emits plain-language corrective moves.

Checks performed
----------------
1.  Vocals/Music masking (2-5 kHz presence band)
2.  Drums/Music low-end buildup (20-120 Hz)
3.  Vocals low-end mud (rumble below 200 Hz on Vocals stem)
4.  Overall loudness balance (Vocals should be the loudest or near-loudest bus)
5.  Stereo width conflict (Drums and Music both very wide can smear the image)
6.  Master bus headroom (peak too close to 0 dBFS)
7.  Dynamic range / compression level per bus (crest factor)
8.  Master integrated loudness (too quiet or already squashed)
9.  Vocal sibilance / harshness (5-10 kHz buildup on Vocals bus)
10. Music bus muddiness (200-500 Hz buildup on Music/Drums)
11. Mono compatibility (Master stereo width vs. folded-to-mono LUFS loss)
12. Vocal air / brightness (high-frequency content above 10 kHz)
13. Ducking suggestion (vocal-over-music presence with no apparent sidechain relief)
"""
from __future__ import annotations

import textwrap
import json
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Dict, List, Optional

import librosa
import numpy as np

EPSILON = 1e-12
_BUS_NAMES = ("Drums", "Music", "Vocals", "Master")


# ---------------------------------------------------------------------------
# Data types
# ---------------------------------------------------------------------------

@dataclass
class BusSnapshot:
    name: str
    peak_dbfs: float
    rms_dbfs: float         # NEW: average RMS level
    crest_factor_db: float  # NEW: peak - RMS (lower = more compressed)
    lufs: float
    low_ratio: float        # 20-120 Hz fraction of total power
    mud_ratio: float        # NEW: 200-500 Hz fraction
    presence_ratio: float   # 2-5 kHz fraction
    sibilance_ratio: float  # NEW: 5-10 kHz fraction
    high_ratio: float       # 8 kHz+ fraction
    stereo_width: float     # 0-1 (0 = mono, 1 = fully decorrelated)
    mono_lufs_loss: float   # NEW: LUFS drop when folded to mono (>3 dB = phase issues)


@dataclass
class ReadinessIssue:
    severity: str          # "error" | "warning" | "info"
    bus_pair: str          # e.g. "Vocals/Music" or "Master"
    category: str          # "masking" | "low_end" | "headroom" | "balance" | "mud"
    action: str
    rationale: str


@dataclass
class ReadinessReport:
    gate_passed: bool
    score: int             # 0-100
    issues: List[ReadinessIssue]
    bus_snapshots: List[BusSnapshot]

    def to_dict(self) -> Dict:
        return {
            "gate_passed": self.gate_passed,
            "score": self.score,
            "issues": [asdict(i) for i in self.issues],
            "bus_snapshots": [asdict(s) for s in self.bus_snapshots],
        }


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _dbfs(value: float) -> float:
    return float(20.0 * np.log10(max(abs(value), EPSILON)))


def _band_ratio(power_per_bin: np.ndarray, freqs: np.ndarray, lo: float, hi: float) -> float:
    total = float(np.sum(power_per_bin) + EPSILON)
    band = float(np.sum(power_per_bin[(freqs >= lo) & (freqs < hi)]))
    return band / total


def _stereo_width(audio: np.ndarray) -> float:
    """Return decorrelation coefficient: 0 = mono, 1 = fully decorrelated."""
    if audio.ndim < 2 or audio.shape[0] < 2:
        return 0.0
    L = audio[0].astype(float)
    R = audio[1].astype(float)
    mid = (L + R) * 0.5
    side = (L - R) * 0.5
    mid_rms = float(np.sqrt(np.mean(mid ** 2)) + EPSILON)
    side_rms = float(np.sqrt(np.mean(side ** 2)) + EPSILON)
    return float(np.clip(side_rms / (mid_rms + side_rms), 0.0, 1.0))


def _lufs(audio: np.ndarray, sr: int) -> float:
    try:
        import pyloudnorm as pyln
        mono = audio.mean(axis=0) if audio.ndim == 2 else audio
        meter = pyln.Meter(sr)
        return float(meter.integrated_loudness(mono))
    except Exception:
        # Fallback: approximate LUFS from RMS
        mono = audio.mean(axis=0) if audio.ndim == 2 else audio
        rms = float(np.sqrt(np.mean(mono ** 2)) + EPSILON)
        return float(20.0 * np.log10(rms)) - 3.0  # rough approximation


def _snapshot(name: str, path: Path) -> Optional[BusSnapshot]:
    """Load a WAV and extract a BusSnapshot. Returns None if file is silent/empty."""
    try:
        audio, sr = librosa.load(path.as_posix(), sr=None, mono=False)
    except Exception:
        return None

    if audio.size == 0:
        return None

    mono = audio.mean(axis=0) if audio.ndim == 2 else audio
    peak = float(np.max(np.abs(mono)))
    if peak < EPSILON:
        return None  # silent bus -- skip

    peak_db = _dbfs(peak)
    rms = float(np.sqrt(np.mean(mono ** 2)))
    rms_db = _dbfs(rms)
    crest_factor_db = peak_db - rms_db

    lufs_stereo = _lufs(audio, sr)

    # Mono fold loudness for mono compatibility check
    if audio.ndim == 2 and audio.shape[0] >= 2:
        mono_fold = audio.mean(axis=0)
        lufs_mono = _lufs(mono_fold[np.newaxis, :], sr)
        mono_lufs_loss = lufs_stereo - lufs_mono  # positive = gets quieter in mono
    else:
        mono_lufs_loss = 0.0

    stft = librosa.stft(mono, n_fft=4096, hop_length=1024)
    mag = np.abs(stft)
    freqs = librosa.fft_frequencies(sr=sr, n_fft=4096)
    power_per_bin = np.mean(mag, axis=1)

    low_ratio       = _band_ratio(power_per_bin, freqs, 20.0, 120.0)
    mud_ratio       = _band_ratio(power_per_bin, freqs, 200.0, 500.0)
    presence_ratio  = _band_ratio(power_per_bin, freqs, 2000.0, 5000.0)
    sibilance_ratio = _band_ratio(power_per_bin, freqs, 5000.0, 10000.0)
    high_ratio      = _band_ratio(power_per_bin, freqs, 8000.0, float(sr / 2))
    width           = _stereo_width(audio)

    return BusSnapshot(
        name=name,
        peak_dbfs=peak_db,
        rms_dbfs=rms_db,
        crest_factor_db=crest_factor_db,
        lufs=lufs_stereo,
        low_ratio=low_ratio,
        mud_ratio=mud_ratio,
        presence_ratio=presence_ratio,
        sibilance_ratio=sibilance_ratio,
        high_ratio=high_ratio,
        stereo_width=width,
        mono_lufs_loss=mono_lufs_loss,
    )


# ---------------------------------------------------------------------------
# Rules
# ---------------------------------------------------------------------------

def _check_vocals_music_masking(snaps: Dict[str, BusSnapshot]) -> List[ReadinessIssue]:
    issues: List[ReadinessIssue] = []
    vox = snaps.get("Vocals")
    music = snaps.get("Music")
    if vox is None or music is None:
        return issues

    overlap = 1.0 - abs(vox.presence_ratio - music.presence_ratio)
    if overlap > 0.85:
        issues.append(ReadinessIssue(
            severity="error",
            bus_pair="Vocals/Music",
            category="masking",
            action=(
                "Carve 2-5 kHz in the Music bus by 3-5 dB with a narrow bell EQ, "
                "or apply a dynamic EQ that ducks those frequencies when vocals are present."
            ),
            rationale=(
                f"Vocals and Music bus share {overlap:.0%} of their 2-5 kHz presence energy. "
                "This level of overlap typically causes vocal intelligibility loss in dense arrangements."
            ),
        ))
    elif overlap > 0.72:
        issues.append(ReadinessIssue(
            severity="warning",
            bus_pair="Vocals/Music",
            category="masking",
            action=(
                "Consider a gentle 2-3 dB presence dip (2-4 kHz) on the Music bus "
                "and verify vocal clarity at full mix volume."
            ),
            rationale=(
                f"Vocals and Music bus presence overlap is {overlap:.0%} — borderline. "
                "Check in mono to confirm vocal sits above the mix."
            ),
        ))
    return issues


def _check_low_end_buildup(snaps: Dict[str, BusSnapshot]) -> List[ReadinessIssue]:
    issues: List[ReadinessIssue] = []
    drums = snaps.get("Drums")
    music = snaps.get("Music")

    if drums is not None and music is not None:
        combined_low = drums.low_ratio + music.low_ratio
        if combined_low > 0.40:
            issues.append(ReadinessIssue(
                severity="error",
                bus_pair="Drums/Music",
                category="low_end",
                action=(
                    "High-pass the Music bus at 80-100 Hz (unless it carries bass guitar). "
                    "If it does carry bass, sidechain the bass to the kick with 3-5 dB gain reduction."
                ),
                rationale=(
                    f"Drums ({drums.low_ratio:.0%}) + Music ({music.low_ratio:.0%}) sub-120 Hz content "
                    f"sum to {combined_low:.0%} of total power — this will translate as muddy low end on small speakers."
                ),
            ))
        elif combined_low > 0.28:
            issues.append(ReadinessIssue(
                severity="warning",
                bus_pair="Drums/Music",
                category="low_end",
                action=(
                    "Verify low-end balance at reduced monitoring levels. "
                    "A high-pass on non-bass Music tracks above 60 Hz often cleans up translation."
                ),
                rationale=(
                    f"Combined sub-120 Hz energy is {combined_low:.0%}. "
                    "Borderline — may become muddy on consumer playback systems."
                ),
            ))
    return issues


def _check_vocals_mud(snaps: Dict[str, BusSnapshot]) -> List[ReadinessIssue]:
    issues: List[ReadinessIssue] = []
    vox = snaps.get("Vocals")
    if vox is None:
        return issues

    if vox.low_ratio > 0.15:
        issues.append(ReadinessIssue(
            severity="warning",
            bus_pair="Vocals",
            category="mud",
            action=(
                "Apply a high-pass filter on the Vocals bus at 80-120 Hz "
                "(or per-track at 100 Hz). Lead vocals rarely need energy below 120 Hz."
            ),
            rationale=(
                f"Vocals bus carries {vox.low_ratio:.0%} of power below 120 Hz. "
                "Low-end bleed on vocals competes with kick and bass and adds muddiness."
            ),
        ))
    return issues


def _check_headroom(snaps: Dict[str, BusSnapshot]) -> List[ReadinessIssue]:
    issues: List[ReadinessIssue] = []
    master = snaps.get("Master")
    if master is None:
        return issues

    if master.peak_dbfs > -0.5:
        issues.append(ReadinessIssue(
            severity="error",
            bus_pair="Master",
            category="headroom",
            action=(
                "Pull the Master bus gain down by at least 2 dB before bouncing. "
                "Target a mix peak of -3 to -6 dBFS to leave headroom for mastering."
            ),
            rationale=(
                f"Master bus peak is {master.peak_dbfs:.1f} dBFS — too close to 0 dBFS. "
                "Inter-sample peaks will clip during lossy encoding (MP3/AAC)."
            ),
        ))
    elif master.peak_dbfs > -3.0:
        issues.append(ReadinessIssue(
            severity="warning",
            bus_pair="Master",
            category="headroom",
            action="Consider lowering the master fader by 1-2 dB to give the mastering stage room to work.",
            rationale=(
                f"Master bus peak is {master.peak_dbfs:.1f} dBFS. "
                "Ideally leave -3 to -6 dB of headroom before mastering."
            ),
        ))
    return issues


def _check_balance(snaps: Dict[str, BusSnapshot]) -> List[ReadinessIssue]:
    issues: List[ReadinessIssue] = []
    vox = snaps.get("Vocals")
    drums = snaps.get("Drums")
    music = snaps.get("Music")

    if vox is not None and drums is not None and music is not None:
        # Vocals should be louder or within 2 dB of Drums+Music average
        backing_lufs = max(drums.lufs, music.lufs)
        if vox.lufs < backing_lufs - 4.0:
            issues.append(ReadinessIssue(
                severity="warning",
                bus_pair="Vocals/Backing",
                category="balance",
                action=(
                    f"Vocals are {backing_lufs - vox.lufs:.1f} LUFS quieter than the loudest backing bus. "
                    "Raise the Vocals bus fader or compress the lead vocal to bring it forward."
                ),
                rationale=(
                    "Vocal intelligibility depends on the lead vocal sitting above the backing arrangement. "
                    "The current balance risks the vocal getting lost in the mix."
                ),
            ))
    return issues


def _check_stereo_width(snaps: Dict[str, BusSnapshot]) -> List[ReadinessIssue]:
    issues: List[ReadinessIssue] = []
    drums = snaps.get("Drums")
    music = snaps.get("Music")
    if drums is not None and music is not None:
        if drums.stereo_width > 0.55 and music.stereo_width > 0.55:
            issues.append(ReadinessIssue(
                severity="info",
                bus_pair="Drums/Music",
                category="stereo",
                action=(
                    "Both Drums and Music buses are wide. Consider narrowing one of them "
                    "to leave space for the other, or keep bass/kick strictly mono below 200 Hz."
                ),
                rationale=(
                    f"Drums width={drums.stereo_width:.2f}, Music width={music.stereo_width:.2f}. "
                    "Competing wide sources reduce perceived depth and can cause phase issues on mono playback."
                ),
            ))
    return issues



def _check_dynamics(snaps: Dict[str, BusSnapshot]) -> List[ReadinessIssue]:
    """Flag buses that are over- or under-compressed based on crest factor."""
    issues: List[ReadinessIssue] = []
    # Typical crest factors: Drums 8-16 dB, Music 6-14 dB, Vocals 6-12 dB
    targets = {
        "Drums":  (8.0,  18.0),
        "Music":  (6.0,  16.0),
        "Vocals": (5.0,  14.0),
        "Master": (4.0,  12.0),
    }
    for bus_name, (lo, hi) in targets.items():
        snap = snaps.get(bus_name)
        if snap is None:
            continue
        cf = snap.crest_factor_db
        if cf < lo:
            issues.append(ReadinessIssue(
                severity="warning",
                bus_pair=bus_name,
                category="dynamics",
                action=(
                    f"The {bus_name} bus crest factor is {cf:.1f} dB -- below the typical floor of {lo:.0f} dB. "
                    "The bus sounds squashed. Try backing off the compressor threshold or ratio, "
                    "or raise the attack time so transients breathe through."
                ),
                rationale=(
                    f"Crest factor (peak minus RMS) of {cf:.1f} dB indicates heavy limiting or over-compression. "
                    "Overly compressed buses lose punch and energy, especially on Drums and Vocals."
                ),
            ))
        elif cf > hi:
            issues.append(ReadinessIssue(
                severity="info",
                bus_pair=bus_name,
                category="dynamics",
                action=(
                    f"The {bus_name} bus crest factor is {cf:.1f} dB -- higher than typical ({hi:.0f} dB max). "
                    "Consider adding a bus compressor (4:1, medium attack ~20 ms, fast release) "
                    "to glue the elements together and control dynamics."
                ),
                rationale=(
                    f"High crest factor ({cf:.1f} dB) means wide dynamic swings. "
                    "Without compression the bus may feel inconsistent in perceived loudness."
                ),
            ))
    return issues


def _check_master_loudness(snaps: Dict[str, BusSnapshot]) -> List[ReadinessIssue]:
    """Check if the master bus integrated LUFS is in a useful pre-master range."""
    issues: List[ReadinessIssue] = []
    master = snaps.get("Master")
    if master is None:
        return issues
    lufs = master.lufs
    if lufs < -24.0:
        issues.append(ReadinessIssue(
            severity="warning",
            bus_pair="Master",
            category="loudness",
            action=(
                f"Master integrated loudness is {lufs:.1f} LUFS -- very quiet for a pre-master mix. "
                "Raise bus faders or add makeup gain so the mix sits around -18 to -12 LUFS "
                "before sending to mastering."
            ),
            rationale=(
                "A very quiet mix forces the mastering engineer to add a lot of gain, "
                "amplifying noise floor and reducing headroom for dynamic processing."
            ),
        ))
    elif lufs > -8.0:
        issues.append(ReadinessIssue(
            severity="error",
            bus_pair="Master",
            category="loudness",
            action=(
                f"Master integrated loudness is {lufs:.1f} LUFS -- the mix is already heavily limited. "
                "Remove or ease any master-bus limiter or compressor before bouncing a mix stem. "
                "The mastering stage needs dynamic headroom to work."
            ),
            rationale=(
                "Pre-master mixes above -9 LUFS usually indicate a limiter is brick-walling the mix. "
                "This leaves no room for mastering loudness processing."
            ),
        ))
    return issues


def _check_harshness(snaps: Dict[str, BusSnapshot]) -> List[ReadinessIssue]:
    """Check for excessive sibilance / harshness on Vocals and Drums."""
    issues: List[ReadinessIssue] = []
    for bus_name, threshold, action_hint in [
        ("Vocals", 0.18, "A de-esser on the Vocals bus targeting 6-9 kHz, 3-5 dB reduction, "
                         "or a gentle high-shelf cut of 1-2 dB above 8 kHz will reduce harshness."),
        ("Drums",  0.22, "A narrow 1-2 dB cut around 6-8 kHz on the Drums bus can tame snare "
                         "ring and cymbal harshness without dulling the transients."),
    ]:
        snap = snaps.get(bus_name)
        if snap is None:
            continue
        if snap.sibilance_ratio > threshold:
            issues.append(ReadinessIssue(
                severity="warning",
                bus_pair=bus_name,
                category="harshness",
                action=action_hint,
                rationale=(
                    f"{bus_name} bus carries {snap.sibilance_ratio:.0%} of energy in the 5-10 kHz range "
                    f"(threshold: {threshold:.0%}). Elevated sibilance causes listener fatigue on "
                    "headphones and consumer earbuds."
                ),
            ))
    return issues


def _check_mud(snaps: Dict[str, BusSnapshot]) -> List[ReadinessIssue]:
    """Check for 200-500 Hz buildup across the mix that causes muddiness."""
    issues: List[ReadinessIssue] = []
    # Look at Music and Drums buses independently
    for bus_name, threshold in [("Music", 0.30), ("Drums", 0.26)]:
        snap = snaps.get(bus_name)
        if snap is None:
            continue
        if snap.mud_ratio > threshold:
            issues.append(ReadinessIssue(
                severity="warning",
                bus_pair=bus_name,
                category="mud",
                action=(
                    f"The {bus_name} bus has a 200-500 Hz buildup ({snap.mud_ratio:.0%}). "
                    "Apply a narrow bell cut of 2-4 dB centered around 300-400 Hz, "
                    "or low-mid shelf starting at 350 Hz (-2 dB). "
                    "Solo the bus and sweep to find the offending frequency."
                ),
                rationale=(
                    "The 200-500 Hz range ('mud zone') is where most mix congestion happens. "
                    "Cutting here on supporting buses opens up clarity for kick, bass, and vocals."
                ),
            ))
    return issues


def _check_mono_compatibility(snaps: Dict[str, BusSnapshot]) -> List[ReadinessIssue]:
    """Flag Master bus stereo width or mono fold loudness loss."""
    issues: List[ReadinessIssue] = []
    master = snaps.get("Master")
    if master is None:
        return issues
    if master.mono_lufs_loss > 3.0:
        issues.append(ReadinessIssue(
            severity="warning",
            bus_pair="Master",
            category="mono_compat",
            action=(
                f"Folding to mono drops the master by {master.mono_lufs_loss:.1f} LUFS. "
                "Check for out-of-phase stereo layers -- pan a problematic stereo source to center "
                "and listen for comb filtering. Use M/S processing or a mid-boosted stereo image tool "
                "to reduce phase-related mono cancellation."
            ),
            rationale=(
                "A LUFS drop of more than 3 dB when summed to mono means significant phase cancellation. "
                "The mix will sound thin or hollow on mono playback (phone speakers, club systems)."
            ),
        ))
    elif master.stereo_width > 0.65:
        issues.append(ReadinessIssue(
            severity="info",
            bus_pair="Master",
            category="mono_compat",
            action=(
                "The Master bus is very wide (width score {:.2f}). ".format(master.stereo_width) +
                "Verify mono compatibility by A/B-ing with a mono fold. "
                "Consider a mid-side compressor or stereo width control keeping lows mono below 200 Hz."
            ),
            rationale=(
                "Very wide masters can lack punch in mono. "
                "Keeping sub-bass and kick in the center preserves impact on all playback systems."
            ),
        ))
    return issues


def _check_vocal_air(snaps: Dict[str, BusSnapshot]) -> List[ReadinessIssue]:
    """Check if Vocals bus lacks high-frequency air (above 10 kHz)."""
    issues: List[ReadinessIssue] = []
    vox = snaps.get("Vocals")
    if vox is None:
        return issues
    # high_ratio covers 8 kHz+; if it's very low, vocals may sound dull
    if vox.high_ratio < 0.04:
        issues.append(ReadinessIssue(
            severity="info",
            bus_pair="Vocals",
            category="air",
            action=(
                f"Vocals bus high-frequency content (8 kHz+) is only {vox.high_ratio:.1%}. "
                "Add a gentle air shelf: +1 to +2 dB at 10-12 kHz (low Q). "
                "This adds presence and shimmer without harshness."
            ),
            rationale=(
                "Vocals with very little energy above 8 kHz often sound boxed-in or over-filtered. "
                "A small air boost restores the natural breath and consonant openness of a vocal performance."
            ),
        ))
    return issues


def _check_ducking_suggestion(snaps: Dict[str, BusSnapshot]) -> List[ReadinessIssue]:
    """Suggest sidechain ducking when Vocals and Music compete at similar LUFS and high presence."""
    issues: List[ReadinessIssue] = []
    vox = snaps.get("Vocals")
    music = snaps.get("Music")
    if vox is None or music is None:
        return issues

    lufs_gap = abs(vox.lufs - music.lufs)
    both_loud = vox.lufs > -18.0 and music.lufs > -18.0
    presence_overlap = 1.0 - abs(vox.presence_ratio - music.presence_ratio)

    if both_loud and lufs_gap < 3.0 and presence_overlap > 0.70:
        issues.append(ReadinessIssue(
            severity="info",
            bus_pair="Vocals/Music",
            category="ducking",
            action=(
                "Vocals and Music buses are within {:.1f} LUFS of each other with high presence overlap. ".format(lufs_gap) +
                "Set up a sidechain compressor on the Music bus triggered by the Vocals bus: "
                "2:1 ratio, fast attack (5-10 ms), medium release (80-120 ms), -2 to -3 dB gain reduction. "
                "This creates automatic space for the vocal when it sings."
            ),
            rationale=(
                "When two loud buses share the same frequency range, manual level-riding or a "
                "sidechain ducker is the cleanest way to maintain vocal intelligibility without "
                "permanently sacrificing the music bus level."
            ),
        ))
    return issues


# ---------------------------------------------------------------------------
# Main entry
# ---------------------------------------------------------------------------

def run_mix_readiness(stems_dir: Path) -> ReadinessReport:
    """
    Load bus stem WAVs from *stems_dir* (named Drums.wav, Music.wav, Vocals.wav, Master.wav)
    and return a ReadinessReport.
    """
    snaps: Dict[str, BusSnapshot] = {}
    for name in _BUS_NAMES:
        path = stems_dir / f"{name}.wav"
        if path.exists():
            s = _snapshot(name, path)
            if s is not None:
                snaps[name] = s

    if not snaps:
        return ReadinessReport(
            gate_passed=False,
            score=0,
            issues=[ReadinessIssue(
                severity="error",
                bus_pair="all",
                category="missing",
                action="No bus stem WAVs found. Ensure the DAW exported Drums.wav, Music.wav, Vocals.wav, Master.wav.",
                rationale="No audio data to analyse.",
            )],
            bus_snapshots=[],
        )

    issues: List[ReadinessIssue] = []
    issues.extend(_check_vocals_music_masking(snaps))
    issues.extend(_check_low_end_buildup(snaps))
    issues.extend(_check_vocals_mud(snaps))
    issues.extend(_check_headroom(snaps))
    issues.extend(_check_balance(snaps))
    issues.extend(_check_stereo_width(snaps))
    issues.extend(_check_dynamics(snaps))
    issues.extend(_check_master_loudness(snaps))
    issues.extend(_check_harshness(snaps))
    issues.extend(_check_mud(snaps))
    issues.extend(_check_mono_compatibility(snaps))
    issues.extend(_check_vocal_air(snaps))
    issues.extend(_check_ducking_suggestion(snaps))

    # Score: start at 100, deduct per issue severity
    score = 100
    for issue in issues:
        if issue.severity == "error":
            score -= 20
        elif issue.severity == "warning":
            score -= 8
        elif issue.severity == "info":
            score -= 3
    score = max(0, min(100, score))

    gate_passed = score >= 60 and not any(i.severity == "error" for i in issues)

    return ReadinessReport(
        gate_passed=gate_passed,
        score=score,
        issues=issues,
        bus_snapshots=list(snaps.values()),
    )


def format_text_report(report: ReadinessReport) -> str:
    """Return a plain-ASCII text report suitable for display in a Win32 MessageBox."""
    lines: List[str] = []
    status = "GATE PASSED" if report.gate_passed else "GATE NOT PASSED"
    lines.append(f"{status}   (Score: {report.score}/100)")
    lines.append("")

    if not report.issues:
        lines.append("No issues found - mix looks clean!")
    else:
        sev_order = {"error": 0, "warning": 1, "info": 2}
        sorted_issues = sorted(report.issues, key=lambda i: sev_order.get(i.severity, 9))
        for issue in sorted_issues:
            tag = {"error": "[ERROR]", "warning": "[WARN] ", "info":  "[INFO] "}.get(issue.severity, "[INFO] ")
            category_label = issue.category.replace("_", " ").upper()
            lines.append(f"{tag}  {issue.bus_pair}  [{category_label}]")
            # Wrap action text at 68 chars with 10-char indent
            wrapped = textwrap.fill(issue.action, width=68, initial_indent="          ",
                                    subsequent_indent="          ")
            lines.append(wrapped)
            lines.append("")

    return "\n".join(lines)
