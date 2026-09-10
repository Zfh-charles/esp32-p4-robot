#!/usr/bin/env python3
"""Compile a legacy dialogue-emotion v2 pack into a Character Pack v3 sidecar.

The compiler is deliberately read-only with respect to v2 media.  It selects
only profile-approved capabilities and hashes the existing precomposed assets;
it never decodes or re-encodes the source MJPEG streams.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path
from typing import Any


CANONICAL_EMOTIONS = ("standby", "happy", "sad", "angry", "loving", "neutral")
FEATURE_LAYER = {
    "mouth": "mouth_face",
    "eyes": "eyes_brows",
    "body_life": "body_life",
}


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def canonical_json(value: Any) -> bytes:
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":")).encode("utf-8")


def load_json(path: Path) -> dict[str, Any]:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise ValueError(f"{path}: root must be an object")
    return value


def safe_asset(pack_root: Path, emotion_root: Path, relative: str) -> tuple[str, Path]:
    if not isinstance(relative, str) or not relative or "\\" in relative:
        raise ValueError(f"unsafe asset path {relative!r}")
    candidate = (emotion_root / relative).resolve()
    root = pack_root.resolve()
    try:
        rel = candidate.relative_to(root)
    except ValueError as exc:
        raise ValueError(f"asset escapes pack root: {relative}") from exc
    if not candidate.is_file():
        raise ValueError(f"missing asset: {rel.as_posix()}")
    return rel.as_posix(), candidate


def asset_entry(pack_root: Path, emotion_root: Path, relative: str, pose_id: str) -> dict[str, str]:
    rel, path = safe_asset(pack_root, emotion_root, relative)
    return {"path": rel, "sha256": sha256_file(path), "pose_id": pose_id}


def validate_profile(profile: dict[str, Any]) -> None:
    if profile.get("schema") != "character-profile-v1" or profile.get("version") != 1:
        raise ValueError("unsupported character profile schema/version")
    character = profile.get("character")
    if not isinstance(character, dict):
        raise ValueError("profile.character must be an object")
    for key in ("id", "display_name", "archetype", "canvas"):
        if key not in character:
            raise ValueError(f"profile.character.{key} is required")
    emotions = profile.get("canonical_emotions")
    if not isinstance(emotions, dict) or set(emotions) != set(CANONICAL_EMOTIONS):
        raise ValueError("profile must define exactly six canonical emotions")
    for emotion in CANONICAL_EMOTIONS:
        item = emotions[emotion]
        if not isinstance(item, dict):
            raise ValueError(f"profile {emotion} must be an object")
        features = item.get("features")
        if not isinstance(features, dict) or set(features) != {"mouth", "eyes", "body_life", "transition"}:
            raise ValueError(f"profile {emotion}.features must define mouth/eyes/body_life/transition")
        if not all(isinstance(value, bool) for value in features.values()):
            raise ValueError(f"profile {emotion}.features values must be boolean")
        approved = item.get("approved_layers", [])
        if not isinstance(approved, list) or len(approved) != len(set(approved)):
            raise ValueError(f"profile {emotion}.approved_layers must be unique")
        for feature, layer in FEATURE_LAYER.items():
            if layer in approved and not features[feature]:
                raise ValueError(f"profile {emotion}: {layer} approved while {feature}=false")
        if any(layer in approved for layer in ("enter", "exit")) and not features["transition"]:
            raise ValueError(f"profile {emotion}: transition layer approved while transition=false")


def collect_level_layer(pack_root: Path, emotion_root: Path, render: dict[str, Any],
                        source_key: str, layer_name: str) -> dict[str, Any]:
    levels = render.get(source_key)
    if not isinstance(levels, dict) or not levels:
        raise ValueError(f"{emotion_root.name}: approved {layer_name} has no {source_key}")
    assets = []
    for pose_id, item in sorted(levels.items()):
        if not isinstance(item, dict) or "rgb565" not in item:
            raise ValueError(f"{emotion_root.name}: invalid {source_key}.{pose_id}")
        assets.append(asset_entry(pack_root, emotion_root, item["rgb565"], str(pose_id)))
    return {"name": layer_name, "composition": "precomposed_opaque", "assets": assets}


def collect_life_layer(pack_root: Path, emotion_root: Path, render: dict[str, Any],
                       approved_ids: list[int]) -> dict[str, Any]:
    life = render.get("life_layer")
    if not isinstance(life, dict):
        raise ValueError(f"{emotion_root.name}: body_life approved but life_layer is absent")
    tracks = {int(track.get("id")): track for track in life.get("tracks", []) if isinstance(track, dict)}
    assets = []
    for track_id in approved_ids:
        track = tracks.get(track_id)
        if track is None:
            raise ValueError(f"{emotion_root.name}: approved life track {track_id} is absent")
        if track.get("approval") != "profile" or track.get("semantic") in (None, "auto_unreviewed"):
            raise ValueError(f"{emotion_root.name}: life track {track_id} lacks semantic approval")
        for frame in track.get("frames", []):
            assets.append(asset_entry(
                pack_root, emotion_root, frame["rgb565"],
                f"track_{track_id}_{int(frame.get('sequence', 0)):02d}",
            ))
    if not assets:
        raise ValueError(f"{emotion_root.name}: approved body_life produced no assets")
    return {"name": "body_life", "composition": "precomposed_opaque", "assets": assets}


def collect_transition_layer(pack_root: Path, emotion_root: Path, render: dict[str, Any],
                             layer_name: str) -> dict[str, Any]:
    source_key = "enter_layer" if layer_name == "enter" else "release_layer"
    source = render.get(source_key)
    if not isinstance(source, dict):
        raise ValueError(f"{emotion_root.name}: approved {layer_name} has no {source_key}")
    assets = []
    for index, frame in enumerate(source.get("frames", [])):
        assets.append(asset_entry(pack_root, emotion_root, frame["rgb565"], f"{layer_name}_{index:02d}"))
    if not assets:
        raise ValueError(f"{emotion_root.name}: approved {layer_name} produced no assets")
    return {"name": layer_name, "composition": "precomposed_opaque", "assets": assets}


def compile_character_pack(pack_root: Path, profile: dict[str, Any]) -> tuple[dict[str, Any], dict[str, Any]]:
    validate_profile(profile)
    legacy = load_json(pack_root / "pack_manifest.json")
    if legacy.get("schema") != "dialogue-emotion-pack-v2" or legacy.get("version") != 2:
        raise ValueError("input must be a dialogue-emotion-pack-v2")
    clips = legacy.get("clips", {})
    output_emotions: dict[str, Any] = {}
    coverage: list[dict[str, Any]] = []
    source_fingerprints: dict[str, str] = {}
    total_asset_bytes = 0
    canvas = profile["character"]["canvas"]

    for canonical in CANONICAL_EMOTIONS:
        spec = profile["canonical_emotions"][canonical]
        source_emotion = spec.get("source_emotion", canonical)
        manifest_rel = clips.get(source_emotion)
        if not isinstance(manifest_rel, str):
            raise ValueError(f"{canonical}: source emotion {source_emotion!r} is absent")
        manifest_path = (pack_root / manifest_rel).resolve()
        emotion_root = manifest_path.parent
        manifest = load_json(manifest_path)
        if (manifest.get("source", {}).get("width"), manifest.get("source", {}).get("height")) != (canvas["width"], canvas["height"]):
            raise ValueError(
                f"{canonical}: source canvas does not match character profile "
                f"{canvas['width']}x{canvas['height']}"
            )
        source_rel, source_path = safe_asset(pack_root, emotion_root, manifest["source"]["file"])
        actual_source_sha = sha256_file(source_path)
        if actual_source_sha != manifest["source"].get("sha256"):
            raise ValueError(f"{canonical}: source MJPEG hash mismatch")
        source_fingerprints[canonical] = actual_source_sha

        render = manifest.get("render", {})
        base_rel, base_path = safe_asset(pack_root, emotion_root, render["hold_base"]["rgb565"])
        layers = []
        approved = spec.get("approved_layers", [])
        if "mouth_face" in approved:
            layers.append(collect_level_layer(pack_root, emotion_root, render, "mouth_levels", "mouth_face"))
        if "eyes_brows" in approved:
            layers.append(collect_level_layer(pack_root, emotion_root, render, "eye_levels", "eyes_brows"))
        if "body_life" in approved:
            ids = spec.get("approved_life_tracks", [])
            if not isinstance(ids, list) or not all(isinstance(value, int) for value in ids):
                raise ValueError(f"{canonical}.approved_life_tracks must be integer ids")
            layers.append(collect_life_layer(pack_root, emotion_root, render, ids))
        for name in ("enter", "exit"):
            if name in approved:
                layers.append(collect_transition_layer(pack_root, emotion_root, render, name))

        backend = spec.get("backend") or ("composite_sprite" if layers else "static_seed")
        resource_claims = list(spec.get("resource_claims", []))
        item = {
            "state": spec.get("state", "supported"),
            "backend": backend,
            "canonical_base": {"asset": base_rel, "sha256": sha256_file(base_path)},
            "features": dict(spec["features"]),
            "layers": layers,
            "resource_claims": resource_claims,
            "review": dict(spec.get("review", {"status": "pending", "notes": "P1 review pending"})),
        }
        output_emotions[canonical] = item
        asset_bytes = base_path.stat().st_size
        for layer in layers:
            for asset in layer["assets"]:
                asset_bytes += (pack_root / asset["path"]).stat().st_size
        total_asset_bytes += asset_bytes
        risk = manifest.get("analysis", {}).get("risk", {})
        coverage.append({
            "canonical": canonical, "source_emotion": source_emotion,
            "state": item["state"], "backend": backend,
            "approved_layers": [layer["name"] for layer in layers],
            "mouth_roi_consumed": "mouth_face" in approved,
            "eye_roi_consumed": "eyes_brows" in approved,
            "source_mjpeg": source_rel, "source_sha256": actual_source_sha,
            "layered_safe": risk.get("layered_safe"),
            "seam_score": risk.get("composite_seam_score"),
            "edge_delta": risk.get("composite_edge_delta"),
            "source_border_delta": risk.get("source_seam_border_delta"),
            "global_frame_delta": risk.get("global_frame_delta"),
            "approved_life_tracks": list(spec.get("approved_life_tracks", [])),
            "asset_bytes": asset_bytes,
        })

    source_sha = hashlib.sha256(canonical_json(source_fingerprints)).hexdigest()
    profile_sha = hashlib.sha256(canonical_json(profile)).hexdigest()
    generation_id = hashlib.sha256((source_sha + profile_sha).encode("ascii")).hexdigest()[:20]
    result = {
        "schema": "character-pack-v3", "version": 3,
        "pack": {"id": profile["pack_id"], "generation_id": generation_id, "source_sha256": source_sha},
        "character": profile["character"],
        "canonical_emotions": output_emotions,
    }
    report = {
        "schema": "character-compile-report-v1", "ok": True,
        "pack_id": profile["pack_id"], "generation_id": generation_id,
        "profile_sha256": profile_sha, "source_sha256": source_sha,
        "source_mjpeg_unchanged": True, "canonical_coverage": coverage,
        "resource_summary": {"referenced_asset_bytes": total_asset_bytes},
    }
    return result, report


def validate_v3(document: dict[str, Any]) -> list[str]:
    contract_root = Path(__file__).resolve().parents[1] / "tools" / "product_contracts"
    sys.path.insert(0, str(contract_root))
    from validate_contracts import validate_document
    return validate_document(document)


def main() -> int:
    parser = argparse.ArgumentParser(description="Compile a v2 dialogue pack into a profile-driven Character Pack v3 sidecar")
    parser.add_argument("--v2-pack", required=True, type=Path)
    parser.add_argument("--character-profile", required=True, type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()
    profile = load_json(args.character_profile)
    document, report = compile_character_pack(args.v2_pack, profile)
    errors = validate_v3(document)
    report["ok"] = not errors
    report["errors"] = errors
    output = args.output or args.v2_pack / "character_pack_v3.json"
    report_path = args.report or args.v2_pack / "character_compile_report.json"
    output.write_text(json.dumps(document, ensure_ascii=False, indent=2), encoding="utf-8")
    report_path.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps({"ok": not errors, "output": str(output), "report": str(report_path), "errors": errors}, ensure_ascii=False, indent=2))
    return 0 if not errors else 2


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)
