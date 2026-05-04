from __future__ import annotations

import argparse
from pathlib import Path

import json

from .auto_mix import run_iterative_auto_mix, write_unmixed_render
from .effects import generate_effects_report
from .features import extract_track_features
from .io import filter_selected, find_audio_files, parse_groups
from .mastering import LOUDNESS_PRESETS, master_file
from .models import AnalysisResult
from .report import write_json, write_markdown
from .rules import (
    generate_group_recommendations,
    generate_track_recommendations,
    rank_recommendations,
)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Offline DAW AI analyzer (MVP)")
    parser.add_argument("--input-dir", type=Path, required=False, default=None, help="Directory containing stem files")
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=Path("analysis_out"),
        help="Directory for analysis outputs",
    )
    parser.add_argument(
        "--select",
        nargs="+",
        default=None,
        help="Optional track filenames to analyze (space-separated)",
    )
    parser.add_argument(
        "--group",
        action="append",
        default=None,
        help="Group definition format: group_name:track1.wav,track2.wav",
    )
    parser.add_argument(
        "--auto-mix",
        action="store_true",
        help="Run iterative auto-mix demo and render preview/final mixes.",
    )
    parser.add_argument(
        "--mix-mode",
        choices=["bad", "decent", "best"],
        default="best",
        help="Mix profile to render for testing: intentionally bad, decent, or best iterative.",
    )
    parser.add_argument(
        "--iterations",
        type=int,
        default=8,
        help="Number of iterative adjustment passes for --mix-mode best.",
    )
    parser.add_argument(
        "--preview-sec",
        type=float,
        default=30.0,
        help="Seconds to render for each iteration preview file.",
    )
    parser.add_argument(
        "--render-unmixed",
        action="store_true",
        help="Render a raw sum of all tracks with no mix moves applied.",
    )
    parser.add_argument(
        "--reference-file",
        type=Path,
        default=None,
        help="Optional mixed or mastered reference file to steer iterative auto-mix scoring.",
    )
    parser.add_argument(
        "--suggest-effects",
        action="store_true",
        help="Analyse each track and output a recommended effects chain (reverb, delay, saturation, etc.).",
    )
    parser.add_argument(
        "--master",
        action="store_true",
        help="Run the mastering chain on the final mix (after --auto-mix, or on --master-input).",
    )
    parser.add_argument(
        "--target-lufs",
        type=float,
        default=-14.0,
        help="Loudness target for mastering in LUFS (default -14 for streaming). Presets: spotify=-14, apple=-16, cd=-12.",
    )
    parser.add_argument(
        "--master-ceiling-db",
        type=float,
        default=-1.0,
        help="True-peak limiter ceiling in dBFS (default -1.0).",
    )
    parser.add_argument(
        "--master-width",
        type=float,
        default=1.15,
        help="Stereo width scalar for mastering M/S stage (default 1.15).",
    )
    parser.add_argument(
        "--master-input",
        type=Path,
        default=None,
        help="Master an existing WAV file instead of using the auto-mix output.",
    )
    parser.add_argument(
        "--mix-readiness",
        type=Path,
        default=None,
        metavar="STEMS_DIR",
        help="Run the Mix Readiness gate on a directory of per-bus WAV stems (Drums.wav, Music.wav, Vocals.wav, Master.wav).",
    )
    return parser


def run(
    input_dir: Path,
    output_dir: Path,
    select: list[str] | None,
    group_args: list[str] | None,
    auto_mix: bool,
    mix_mode: str,
    iterations: int,
    preview_sec: float,
    render_unmixed: bool,
    reference_file: Path | None,
    suggest_effects: bool = False,
    master: bool = False,
    target_lufs: float = -14.0,
    master_ceiling_db: float = -1.0,
    master_width: float = 1.15,
    master_input: Path | None = None,
    mix_readiness_dir: Path | None = None,
) -> int:
    # --mix-readiness is standalone — does not need --input-dir
    if mix_readiness_dir is not None:
        from .mix_readiness import run_mix_readiness, format_text_report
        report = run_mix_readiness(mix_readiness_dir)
        # First line is machine-readable for the C++ host; rest is human text
        print(f"GATE_PASSED={str(report.gate_passed).lower()}")
        print(format_text_report(report))
        return 0 if report.gate_passed else 1

    if input_dir is None:
        raise ValueError("--input-dir is required unless --mix-readiness is used.")
    files = find_audio_files(input_dir)
    files = filter_selected(files, select)

    if not files:
        raise FileNotFoundError(f"No supported audio files found in {input_dir}")

    tracks = [extract_track_features(path) for path in files]

    recommendations = []
    for track in tracks:
        recommendations.extend(generate_track_recommendations(track))

    groups = parse_groups(group_args)
    recommendations.extend(generate_group_recommendations(tracks, groups))
    recommendations = rank_recommendations(recommendations)

    result = AnalysisResult(tracks=tracks, recommendations=recommendations)

    json_path = write_json(result, output_dir)
    report_path = write_markdown(result, output_dir)

    print(f"Analyzed {len(tracks)} track(s)")
    print(f"Wrote {json_path}")
    print(f"Wrote {report_path}")

    if render_unmixed:
        unmixed = write_unmixed_render(files, output_dir)
        print(f"Wrote {unmixed['json_path']}")
        print(f"Wrote {unmixed['markdown_path']}")
        print(f"Wrote {unmixed['path']}")

    mix_summary: dict | None = None
    if auto_mix:
        mix_summary = run_iterative_auto_mix(
            paths=files,
            output_dir=output_dir,
            mix_mode=mix_mode,
            iterations=max(1, iterations),
            preview_sec=max(0.0, preview_sec),
            reference_file=reference_file,
        )
        print(f"Auto-mix mode: {mix_mode}")
        print(f"Wrote {mix_summary['json_path']}")
        print(f"Wrote {mix_summary['markdown_path']}")
        print(f"Wrote {mix_summary['final_mix_path']}")

    if suggest_effects:
        from .auto_mix import _infer_role  # reuse existing role inference

        roles = [_infer_role(f.name) for f in files]
        # Use mix metrics from the last auto-mix iteration if available
        mix_metrics_for_bus = mix_summary["iteration_logs"][-1]["metrics"] if mix_summary is not None else None
        effects_report = generate_effects_report(
            track_names=[f.name for f in files],
            track_features=tracks,
            roles=roles,
            mix_metrics=mix_metrics_for_bus,
        )
        output_dir.mkdir(parents=True, exist_ok=True)
        effects_json = output_dir / "effects_suggestions.json"
        effects_json.write_text(json.dumps(effects_report, indent=2), encoding="utf-8")
        print(f"Wrote {effects_json}")
        _print_effects_summary(effects_report)

    if master or master_input is not None:
        # Decide which WAV to master
        if master_input is not None:
            src_wav = master_input
        elif mix_summary is not None:
            src_wav = Path(mix_summary["final_mix_path"])
        else:
            # Fall back: look for best mix in output dir
            src_wav = output_dir / "mix_best_final.wav"
        if not src_wav.exists():
            print(f"Master: source file not found: {src_wav}")
        else:
            print(f"\nMastering {src_wav.name}  →  target {target_lufs:.1f} LUFS ...")
            msummary = master_file(
                input_path=src_wav,
                output_dir=output_dir,
                target_lufs=target_lufs,
                ceiling_db=master_ceiling_db,
                width=master_width,
            )
            m = msummary["metrics"]
            print(f"Wrote {msummary['output']}")
            print(f"  Pre-master LUFS : {m['pre_master_lufs']}")
            print(f"  Gain applied    : {m['lufs_gain_db']:+.1f} dB")
            print(f"  Final LUFS      : {m['final_lufs']}")
            print(f"  True-peak       : {m['final_peak_dbfs']:.2f} dBFS  (ceiling {m['ceiling_db']} dB)")

    return 0


def _print_effects_summary(report: dict) -> None:
    print("\n=== Effect Suggestions ===")
    for track in report.get("per_track", []):
        fx = track["effects"]
        if not fx:
            continue
        print(f"\n  {track['name']} ({track['role']}, crest {track['crest_db']:.1f} dB)")
        for s in fx:
            star = "*" if s["priority"] == "recommended" else " "
            print(f"    [{star}] {s['effect_type']:18s} {s['preset_name']}")
            print(f"         {s['reason']}")
    if "mix_bus" in report:
        print("\n  Mix Bus")
        for s in report["mix_bus"]:
            star = "*" if s["priority"] == "recommended" else " "
            print(f"    [{star}] {s['effect_type']:18s} {s['preset_name']}")
            print(f"         {s['reason']}")


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()
    return run(
        args.input_dir,
        args.output_dir,
        args.select,
        args.group,
        args.auto_mix,
        args.mix_mode,
        args.iterations,
        args.preview_sec,
        args.render_unmixed,
        args.reference_file,
        args.suggest_effects,
        args.master,
        args.target_lufs,
        args.master_ceiling_db,
        args.master_width,
        args.master_input,
        args.mix_readiness,
    )


if __name__ == "__main__":
    raise SystemExit(main())
