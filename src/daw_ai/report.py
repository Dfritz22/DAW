from __future__ import annotations

import json
from pathlib import Path

from .models import AnalysisResult


def write_json(result: AnalysisResult, output_dir: Path) -> Path:
    output_dir.mkdir(parents=True, exist_ok=True)
    out = output_dir / "analysis.json"
    out.write_text(json.dumps(result.to_dict(), indent=2), encoding="utf-8")
    return out


def write_markdown(result: AnalysisResult, output_dir: Path, top_n: int = 5) -> Path:
    output_dir.mkdir(parents=True, exist_ok=True)
    out = output_dir / "report.md"

    lines: list[str] = []
    lines.append("# DAW AI Analysis Report")
    lines.append("")

    lines.append("## Track Summary")
    lines.append("")
    lines.append("| Track | Peak dBFS | RMS dBFS | LUFS-I | Crest dB |")
    lines.append("|---|---:|---:|---:|---:|")
    for t in result.tracks:
        lines.append(
            f"| {t.name} | {t.peak_dbfs:.2f} | {t.rms_dbfs:.2f} | {t.lufs_integrated:.2f} | {t.crest_db:.2f} |"
        )

    lines.append("")
    lines.append("## Top Recommendations")
    lines.append("")

    for idx, rec in enumerate(result.recommendations[:top_n], start=1):
        lines.append(f"{idx}. [{rec.scope}] {rec.target} | {rec.category} | confidence {rec.confidence:.2f}")
        lines.append(f"   - Action: {rec.action}")
        lines.append(f"   - Why: {rec.rationale}")

    if not result.recommendations:
        lines.append("No significant issues detected by current heuristics.")

    out.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return out
