#!/usr/bin/env python3
"""Dependency-free semantic validator for P0 product contracts."""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path
from typing import Any, Callable


CANONICAL_EMOTIONS = {
    "standby", "happy", "sad", "angry", "loving", "neutral"
}
BACKENDS = {
    "full_frame_safe", "composite_sprite", "band_delta", "static_seed"
}
CAPABILITY_STATES = {"supported", "degraded", "rejected"}
RESOURCE_CLASSES = {
    "realtime_audio", "control", "safety", "perception", "inference",
    "visual", "storage", "telemetry",
}
QUEUE_POLICIES = {
    "latest_value", "drop_old", "coalesce", "run_to_completion"
}
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")


def _object(value: Any, errors: list[str], label: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        errors.append(f"{label}: must be an object")
        return {}
    return value


def _non_negative_int(value: Any, errors: list[str], label: str) -> None:
    if not isinstance(value, int) or isinstance(value, bool) or value < 0:
        errors.append(f"{label}: must be a non-negative integer")


def _positive_int(value: Any, errors: list[str], label: str) -> None:
    if not isinstance(value, int) or isinstance(value, bool) or value <= 0:
        errors.append(f"{label}: must be a positive integer")


def _safe_relative(value: Any, errors: list[str], label: str) -> None:
    if not isinstance(value, str) or not value or "\\" in value:
        errors.append(f"{label}: must be a non-empty POSIX relative path")
        return
    path = Path(value)
    if path.is_absolute() or ".." in path.parts:
        errors.append(f"{label}: path must not escape the pack")


def _sha256(value: Any, errors: list[str], label: str) -> None:
    if not isinstance(value, str) or not SHA256_RE.fullmatch(value):
        errors.append(f"{label}: must be a lowercase SHA-256")


def _unique_names(items: Any, errors: list[str], label: str) -> list[dict[str, Any]]:
    if not isinstance(items, list):
        errors.append(f"{label}: must be an array")
        return []
    result: list[dict[str, Any]] = []
    names: set[str] = set()
    for index, raw in enumerate(items):
        item = _object(raw, errors, f"{label}[{index}]")
        name = item.get("name")
        if not isinstance(name, str) or not name:
            errors.append(f"{label}[{index}].name: must be non-empty")
        elif name in names:
            errors.append(f"{label}: duplicate name {name!r}")
        else:
            names.add(name)
        result.append(item)
    return result


def validate_character_pack(doc: dict[str, Any]) -> list[str]:
    errors: list[str] = []
    if doc.get("schema") != "character-pack-v3" or doc.get("version") != 3:
        errors.append("character: unsupported schema/version")
    pack = _object(doc.get("pack"), errors, "pack")
    for key in ("id", "generation_id"):
        if not isinstance(pack.get(key), str) or not pack[key]:
            errors.append(f"pack.{key}: must be non-empty")
    _sha256(pack.get("source_sha256"), errors, "pack.source_sha256")
    character = _object(doc.get("character"), errors, "character")
    for key in ("id", "display_name", "archetype"):
        if not isinstance(character.get(key), str) or not character[key]:
            errors.append(f"character.{key}: must be non-empty")
    canvas = _object(character.get("canvas"), errors, "character.canvas")
    _positive_int(canvas.get("width"), errors, "character.canvas.width")
    _positive_int(canvas.get("height"), errors, "character.canvas.height")

    emotions = _object(doc.get("canonical_emotions"), errors, "canonical_emotions")
    if set(emotions) != CANONICAL_EMOTIONS:
        missing = sorted(CANONICAL_EMOTIONS - set(emotions))
        extra = sorted(set(emotions) - CANONICAL_EMOTIONS)
        errors.append(f"canonical_emotions: exact six required; missing={missing} extra={extra}")
    for name in sorted(CANONICAL_EMOTIONS & set(emotions)):
        item = _object(emotions[name], errors, f"canonical_emotions.{name}")
        state = item.get("state")
        if state not in CAPABILITY_STATES:
            errors.append(f"{name}.state: unsupported state {state!r}")
        backend = item.get("backend")
        if state != "rejected" and backend not in BACKENDS:
            errors.append(f"{name}.backend: supported/degraded emotion needs a backend")
        review = _object(item.get("review"), errors, f"{name}.review")
        if review.get("status") not in {"approved", "pending", "rejected"}:
            errors.append(f"{name}.review.status: invalid")
        base = item.get("canonical_base")
        if state != "rejected":
            base = _object(base, errors, f"{name}.canonical_base")
            _safe_relative(base.get("asset"), errors, f"{name}.canonical_base.asset")
            _sha256(base.get("sha256"), errors, f"{name}.canonical_base.sha256")
        layers = item.get("layers", [])
        if not isinstance(layers, list):
            errors.append(f"{name}.layers: must be an array")
            layers = []
        layer_names: set[str] = set()
        for index, raw in enumerate(layers):
            layer = _object(raw, errors, f"{name}.layers[{index}]")
            layer_name = layer.get("name")
            if layer_name in layer_names:
                errors.append(f"{name}.layers: duplicate layer {layer_name!r}")
            elif isinstance(layer_name, str):
                layer_names.add(layer_name)
            if layer.get("composition") != "precomposed_opaque":
                errors.append(f"{name}.{layer_name}: runtime alpha/composition is forbidden")
            for asset_index, asset in enumerate(layer.get("assets", [])):
                asset = _object(asset, errors, f"{name}.{layer_name}.assets[{asset_index}]")
                _safe_relative(asset.get("path"), errors, f"{name}.{layer_name}.asset.path")
                _sha256(asset.get("sha256"), errors, f"{name}.{layer_name}.asset.sha256")
                if not isinstance(asset.get("pose_id"), str) or not asset["pose_id"]:
                    errors.append(f"{name}.{layer_name}.asset.pose_id: must be non-empty")
        features = _object(item.get("features", {}), errors, f"{name}.features")
        if features.get("mouth") is False and "mouth_face" in layer_names:
            errors.append(f"{name}: mouth_face layer present while mouth=false")
    return errors


def validate_device_capability(doc: dict[str, Any]) -> list[str]:
    errors: list[str] = []
    if doc.get("schema") != "device-capability-v1" or doc.get("version") != 1:
        errors.append("device: unsupported schema/version")
    board = _object(doc.get("board"), errors, "board")
    for key in ("id", "soc", "revision"):
        if not isinstance(board.get(key), str) or not board[key]:
            errors.append(f"board.{key}: must be non-empty")
    memory = _object(doc.get("memory"), errors, "memory")
    for key in ("internal_sram_bytes", "psram_bytes"):
        _non_negative_int(memory.get(key), errors, f"memory.{key}")
    domains = _unique_names(doc.get("resource_domains"), errors, "resource_domains")
    domain_names = {x.get("name") for x in domains}
    capabilities = _unique_names(doc.get("capabilities"), errors, "capabilities")
    allowed_kinds = {
        "audio", "display", "storage", "network", "camera", "mmwave",
        "environment", "temperature", "edge_accelerator",
    }
    for item in capabilities:
        if item.get("kind") not in allowed_kinds:
            errors.append(f"capability {item.get('name')!r}: invalid kind")
        if not isinstance(item.get("present"), bool):
            errors.append(f"capability {item.get('name')!r}.present: must be boolean")
        for domain in item.get("resource_domains", []):
            if domain not in domain_names:
                errors.append(f"capability {item.get('name')!r}: unknown domain {domain!r}")
    return errors


def validate_resource_claim(doc: dict[str, Any]) -> list[str]:
    errors: list[str] = []
    if doc.get("schema") != "resource-claim-v1" or doc.get("version") != 1:
        errors.append("claim: unsupported schema/version")
    if doc.get("class") not in RESOURCE_CLASSES:
        errors.append("claim.class: invalid")
    for key in ("name", "producer"):
        if not isinstance(doc.get(key), str) or not doc[key]:
            errors.append(f"claim.{key}: must be non-empty")
    timing = _object(doc.get("timing"), errors, "timing")
    _positive_int(timing.get("deadline_ms"), errors, "timing.deadline_ms")
    _positive_int(timing.get("wcet_us"), errors, "timing.wcet_us")
    memory = _object(doc.get("memory"), errors, "memory")
    for key in ("internal_sram_bytes", "psram_bytes"):
        _non_negative_int(memory.get(key), errors, f"memory.{key}")
    burst = _object(doc.get("burst"), errors, "burst")
    for key in ("psram_read_bytes", "psram_write_bytes"):
        _non_negative_int(burst.get(key), errors, f"burst.{key}")
    policy = _object(doc.get("policy"), errors, "policy")
    droppable = policy.get("droppable")
    if not isinstance(droppable, bool):
        errors.append("policy.droppable: must be boolean")
    if policy.get("queue") not in QUEUE_POLICIES:
        errors.append("policy.queue: invalid")
    chain = policy.get("degradation_chain")
    if not isinstance(chain, list) or not chain or not all(isinstance(x, str) and x for x in chain):
        errors.append("policy.degradation_chain: must be a non-empty string array")
    if doc.get("class") == "realtime_audio" and droppable is not False:
        errors.append("realtime_audio: must not be droppable")
    if doc.get("class") == "visual" and droppable is not True:
        errors.append("visual: must be droppable")
    return errors


def validate_visual_event(doc: dict[str, Any]) -> list[str]:
    errors: list[str] = []
    if doc.get("schema") != "visual-intent-commit-v1" or doc.get("version") != 1:
        errors.append("visual: unsupported schema/version")
    kind = doc.get("kind")
    if kind not in {"visual_intent", "visual_commit"}:
        errors.append("visual.kind: invalid")
    if doc.get("emotion") not in CANONICAL_EMOTIONS:
        errors.append("visual.emotion: invalid canonical emotion")
    _non_negative_int(doc.get("generation"), errors, "visual.generation")
    _positive_int(doc.get("deadline_ms"), errors, "visual.deadline_ms")
    if kind == "visual_commit":
        if doc.get("backend") not in BACKENDS:
            errors.append("visual.backend: invalid")
        if doc.get("composition") != "precomposed_opaque":
            errors.append("visual.commit: runtime alpha/composition is forbidden")
        _sha256(doc.get("base_sha256"), errors, "visual.base_sha256")
        if doc.get("drop_policy") != "drop_expired_no_replay":
            errors.append("visual.drop_policy: stale commits must not replay")
    return errors


VALIDATORS: dict[str, Callable[[dict[str, Any]], list[str]]] = {
    "character-pack-v3": validate_character_pack,
    "device-capability-v1": validate_device_capability,
    "resource-claim-v1": validate_resource_claim,
    "visual-intent-commit-v1": validate_visual_event,
}


def validate_document(doc: Any) -> list[str]:
    if not isinstance(doc, dict):
        return ["document root must be an object"]
    validator = VALIDATORS.get(doc.get("schema"))
    if validator is None:
        return [f"unsupported schema {doc.get('schema')!r}"]
    return validator(doc)


def validate_path(path: Path) -> dict[str, Any]:
    try:
        doc = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        return {"path": str(path), "ok": False, "errors": [f"invalid JSON: {exc}"]}
    errors = validate_document(doc)
    return {"path": str(path), "ok": not errors, "errors": errors}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("paths", nargs="*", type=Path)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent
    paths = args.paths
    expected: dict[Path, bool] = {}
    if args.self_test or not paths:
        valid = sorted((root / "examples" / "valid").glob("*.json"))
        invalid = sorted((root / "examples" / "invalid").glob("*.json"))
        paths = valid + invalid
        expected = {p: True for p in valid} | {p: False for p in invalid}
    reports = [validate_path(path) for path in paths]
    expectation_errors = []
    for report in reports:
        path = Path(report["path"])
        if path in expected and report["ok"] != expected[path]:
            expectation_errors.append(
                f"{path.name}: expected ok={expected[path]}, got {report['ok']}"
            )
    output = {
        "ok": not expectation_errors and all(
            report["ok"] == expected.get(Path(report["path"]), report["ok"])
            for report in reports
        ),
        "reports": reports,
        "expectation_errors": expectation_errors,
    }
    print(json.dumps(output, ensure_ascii=False, indent=2))
    return 0 if output["ok"] else 2


if __name__ == "__main__":
    raise SystemExit(main())
