#!/usr/bin/env python3
"""Estimate the immutable speech-core bank footprint of a Character Pack.

The runtime safety batch intentionally excludes pose, eye, life, enter and exit
assets.  Only the canonical base and four mouth levels are required so an
emotion switch during speech becomes a pointer/index change instead of SD I/O.
"""

from __future__ import annotations

import argparse
import json
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any


CANONICAL_EMOTIONS = ("standby", "happy", "sad", "angry", "loving", "neutral")
MOUTH_LEVELS = ("closed", "small", "medium", "large")


@dataclass(frozen=True)
class EmotionBudget:
    emotion: str
    base_bytes: int
    mouth_bytes: int
    total_bytes: int
    base_expected_bytes: int
    mouth_expected_bytes: int
    valid: bool
    errors: tuple[str, ...]


def _load_manifest(emotion_dir: Path) -> dict[str, Any]:
    return json.loads((emotion_dir / "manifest.json").read_text(encoding="utf-8"))


def _asset_size(emotion_dir: Path, relative: str, label: str, errors: list[str]) -> int:
    path = emotion_dir / relative
    if not path.is_file():
        errors.append(f"missing {label}: {relative}")
        return 0
    return path.stat().st_size


def measure_emotion(pack_root: Path, emotion: str) -> EmotionBudget:
    emotion_dir = pack_root / emotion
    errors: list[str] = []
    try:
        manifest = _load_manifest(emotion_dir)
    except (OSError, json.JSONDecodeError) as exc:
        return EmotionBudget(emotion, 0, 0, 0, 0, 0, False, (str(exc),))

    render = manifest.get("render", {})
    if render.get("mode") != "layered":
        errors.append("render.mode is not layered")

    base = render.get("hold_base", {})
    base_rel = base.get("rgb565")
    base_expected = int(base.get("width", 0)) * int(base.get("height", 0)) * 2
    base_bytes = _asset_size(emotion_dir, base_rel, "hold_base", errors) if base_rel else 0
    if not base_rel:
        errors.append("hold_base.rgb565 missing from manifest")
    elif base_bytes != base_expected:
        errors.append(f"hold_base size {base_bytes} != expected {base_expected}")

    roi = render.get("mouth_roi", {})
    one_mouth_expected = int(roi.get("width", 0)) * int(roi.get("height", 0)) * 2
    mouth_expected = one_mouth_expected * len(MOUTH_LEVELS)
    mouth_bytes = 0
    mouth_map = render.get("mouth_levels", {})
    for level in MOUTH_LEVELS:
        entry = mouth_map.get(level, {})
        relative = entry.get("rgb565")
        if not relative:
            errors.append(f"mouth level {level} missing from manifest")
            continue
        size = _asset_size(emotion_dir, relative, f"mouth.{level}", errors)
        mouth_bytes += size
        if size != one_mouth_expected:
            errors.append(f"mouth.{level} size {size} != expected {one_mouth_expected}")

    total = base_bytes + mouth_bytes
    return EmotionBudget(
        emotion=emotion,
        base_bytes=base_bytes,
        mouth_bytes=mouth_bytes,
        total_bytes=total,
        base_expected_bytes=base_expected,
        mouth_expected_bytes=mouth_expected,
        valid=not errors,
        errors=tuple(errors),
    )


def build_report(pack_root: Path) -> dict[str, Any]:
    emotions = [measure_emotion(pack_root, emotion) for emotion in CANONICAL_EMOTIONS]
    total = sum(item.total_bytes for item in emotions)
    return {
        "schema": "speech-core-bank-budget-v1",
        "pack_root": str(pack_root.resolve()),
        "scope": "canonical base + closed/small/medium/large mouth only",
        "emotions": [asdict(item) for item in emotions],
        "total_bytes": total,
        "total_mib": round(total / (1024 * 1024), 3),
        "valid": all(item.valid for item in emotions),
        "excluded": ["pose", "eye", "life", "enter", "exit", "mjpeg"],
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("pack_root", type=Path)
    parser.add_argument("--json", action="store_true", help="emit machine-readable JSON")
    args = parser.parse_args()
    report = build_report(args.pack_root)
    if args.json:
        print(json.dumps(report, ensure_ascii=False, indent=2))
    else:
        for item in report["emotions"]:
            print(
                f"{item['emotion']:8s} base={item['base_bytes']:7d} "
                f"mouth={item['mouth_bytes']:6d} total={item['total_bytes']:7d} "
                f"valid={int(item['valid'])}"
            )
        print(f"TOTAL {report['total_bytes']} bytes ({report['total_mib']} MiB)")
    return 0 if report["valid"] else 2


if __name__ == "__main__":
    raise SystemExit(main())
