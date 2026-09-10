from __future__ import annotations

import argparse
import hashlib
import json
import re
from pathlib import Path


SCHEMA = "idle-flash-band-v1"
CHOREOGRAPHY_SCHEMA = "idle-life-cluster-v1"
PIXEL_FORMAT = "rgb565-le"
MAX_ROWS = 48


class BuildError(ValueError):
    pass


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _read_json(path: Path) -> dict:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise BuildError(f"cannot read JSON {path}: {exc}") from exc
    if not isinstance(value, dict):
        raise BuildError(f"JSON root must be an object: {path}")
    return value


def _safe_child(root: Path, relative: str) -> Path:
    if not isinstance(relative, str) or not relative:
        raise BuildError("asset path must be a non-empty string")
    target = (root / relative).resolve()
    try:
        target.relative_to(root.resolve())
    except ValueError as exc:
        raise BuildError(f"asset path escapes clip root: {relative}") from exc
    return target


def _read_verified(root: Path, entry: dict, key: str, hash_key: str) -> bytes:
    path = _safe_child(root, entry.get(key))
    try:
        data = path.read_bytes()
    except OSError as exc:
        raise BuildError(f"cannot read asset {path}: {exc}") from exc
    expected = entry.get(hash_key)
    if expected and sha256(data) != expected:
        raise BuildError(f"hash mismatch for {path}")
    return data


def _identity(pack: dict, profile: dict, explicit: str | None, pack_root: Path) -> str:
    value = explicit or pack.get("character_id") or profile.get("character_id")
    if not value:
        raise BuildError(
            "character_id is required for a pack without identity metadata; pass --character-id"
        )
    value = re.sub(r"[^A-Za-z0-9_.-]+", "_", str(value)).strip("_.-")
    if not value:
        raise BuildError(f"invalid character_id for {pack_root}")
    return value


def _select_track(life: dict, semantic: str) -> dict:
    matches = [
        item for item in life.get("tracks", [])
        if item.get("approval") == "profile"
        and item.get("semantic") == semantic
        and isinstance(item.get("choreography"), dict)
        and item["choreography"].get("schema") == CHOREOGRAPHY_SCHEMA
    ]
    if len(matches) != 1:
        raise BuildError(
            f"expected exactly one profile-approved {semantic!r} choreography, got {len(matches)}"
        )
    return matches[0]


def build_idle_flash_bands(
    pack_root: Path,
    output_dir: Path,
    *,
    character_id: str | None = None,
    emotion: str = "standby",
    semantic: str = "body_breathe",
    force: bool = False,
) -> dict:
    pack_root = pack_root.resolve()
    pack = _read_json(pack_root / "pack_manifest.json")
    profile_path = pack_root / "profile.generated.json"
    profile = _read_json(profile_path) if profile_path.exists() else {}
    identity = _identity(pack, profile, character_id, pack_root)

    contract = pack.get("runtime_contract", {})
    screen = contract.get("screen", {})
    width = int(screen.get("width", 0))
    height = int(screen.get("height", 0))
    if width <= 0 or height <= 0 or screen.get("pixel_format") != PIXEL_FORMAT:
        raise BuildError("pack must declare a positive rgb565-le screen")

    clip_rel = pack.get("clips", {}).get(emotion)
    if not clip_rel:
        raise BuildError(f"pack has no canonical clip {emotion!r}")
    clip_manifest_path = _safe_child(pack_root, clip_rel)
    clip_root = clip_manifest_path.parent
    clip = _read_json(clip_manifest_path)
    render = clip.get("render", {})
    hold = render.get("hold_base", {})
    life = render.get("life_layer")
    if not isinstance(life, dict):
        raise BuildError(f"{emotion} has no life_layer")
    if life.get("composite_mode") != "canonical_base_precomposited_v1":
        raise BuildError("flash bands require canonical_base_precomposited_v1")
    track = _select_track(life, semantic)

    roi = track.get("roi", {})
    x, y = int(roi.get("x", -1)), int(roi.get("y", -1))
    roi_w, rows = int(roi.get("width", 0)), int(roi.get("height", 0))
    max_rows = min(MAX_ROWS, int(contract.get("max_rows_per_tick", MAX_ROWS)))
    if x < 0 or y < 0 or roi_w <= 0 or rows <= 0:
        raise BuildError("life ROI must be positive")
    if rows > max_rows or x + roi_w > width or y + rows > height:
        raise BuildError(f"life ROI exceeds screen or {max_rows}-row budget")

    base = _safe_child(clip_root, hold.get("rgb565")).read_bytes()
    expected_base_size = width * height * 2
    if len(base) != expected_base_size:
        raise BuildError(f"hold base size {len(base)} != {expected_base_size}")
    base_hash = sha256(base)
    if life.get("base_rgb565_sha256") != base_hash:
        raise BuildError("life_layer base hash does not match canonical hold base")

    choreography = track["choreography"]
    keyframes = choreography.get("keyframes")
    frames = track.get("frames", [])
    if not isinstance(keyframes, list) or not 3 <= len(keyframes) <= 8:
        raise BuildError("choreography needs 3..8 keyframes")
    if choreography.get("busy_policy") != "abort_to_base_no_replay":
        raise BuildError("unsupported busy policy")
    if choreography.get("returns_to_base") is not True:
        raise BuildError("choreography must return to canonical base")

    row_bytes, roi_row_bytes = width * 2, roi_w * 2
    base_band = base[y * row_bytes:(y + rows) * row_bytes]
    base_roi = b"".join(
        base_band[row * row_bytes + x * 2:row * row_bytes + x * 2 + roi_row_bytes]
        for row in range(rows)
    )

    if output_dir.exists() and any(output_dir.iterdir()) and not force:
        raise BuildError(f"output directory is not empty: {output_dir}; pass --force")
    output_dir.mkdir(parents=True, exist_ok=True)
    if force:
        for old in output_dir.glob("idl*.rgb"):
            old.unlink()
        (output_dir / "idlelife.json").unlink(missing_ok=True)

    output_frames, final_patch = [], None
    for output_index, source_index in enumerate(keyframes):
        if not isinstance(source_index, int) or not 0 <= source_index < len(frames):
            raise BuildError(f"invalid keyframe index: {source_index}")
        entry = frames[source_index]
        patch = _read_verified(clip_root, entry, "rgb565", "rgb565_sha256")
        if len(patch) != roi_w * rows * 2:
            raise BuildError(f"life patch {source_index} has wrong size")
        band = bytearray(base_band)
        for row in range(rows):
            dst_start, src_start = row * row_bytes + x * 2, row * roi_row_bytes
            band[dst_start:dst_start + roi_row_bytes] = patch[src_start:src_start + roi_row_bytes]
        name, data = f"idl{output_index}.rgb", bytes(band)
        (output_dir / name).write_bytes(data)
        output_frames.append({
            "file": name,
            "bytes": len(data),
            "sha256": sha256(data),
            "source_sequence": source_index,
            "source_frame": entry.get("source_frame"),
        })
        final_patch = patch

    if final_patch != base_roi or output_frames[-1]["sha256"] != sha256(base_band):
        raise BuildError("last keyframe does not close exactly on canonical base band")

    report = {
        "schema": SCHEMA,
        "character_id": identity,
        "pack_schema": pack.get("schema"),
        "emotion": emotion,
        "semantic": semantic,
        "source_generation_id": life.get("generation_id"),
        "base_rgb565_sha256": base_hash,
        "pixel_format": PIXEL_FORMAT,
        "width": width,
        "height": rows,
        "y": y,
        "stride_bytes": row_bytes,
        "opaque_full_width": True,
        "max_rows_per_tick": max_rows,
        "frame_interval_ms": int(choreography.get("frame_interval_ms", 0)),
        "rest_ms": choreography.get("rest_ms"),
        "busy_policy": choreography.get("busy_policy"),
        "returns_to_base": True,
        "source_roi": {"x": x, "y": y, "width": roi_w, "height": rows},
        "frames": output_frames,
        "resource_bytes": sum(item["bytes"] for item in output_frames),
    }
    (output_dir / "idlelife.json").write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    return report


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Precompose an approved Character Pack life track into opaque full-width RGB565 bands."
    )
    parser.add_argument("pack", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--character-id")
    parser.add_argument("--emotion", default="standby")
    parser.add_argument("--semantic", default="body_breathe")
    parser.add_argument("--force", action="store_true")
    args = parser.parse_args()
    try:
        report = build_idle_flash_bands(
            args.pack, args.output, character_id=args.character_id,
            emotion=args.emotion, semantic=args.semantic, force=args.force,
        )
    except BuildError as exc:
        parser.error(str(exc))
    print(json.dumps(report, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
