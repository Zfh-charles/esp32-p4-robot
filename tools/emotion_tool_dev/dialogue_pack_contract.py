#!/usr/bin/env python3
"""Validate the SD dialogue-v2 pack against the firmware loading contract."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
from typing import Any


EMOTIONS = ("standby", "neutral", "happy", "sad", "angry", "loving")
MOUTH_LEVELS = ("closed", "small", "medium", "large")
EYE_LEVELS = ("open", "half", "closed")
RENDER_MODES = {"full_frame_clip", "fixed_roi_clip", "layered", "static"}
SCREEN_WIDTH = 480
SCREEN_HEIGHT = 480
MAX_BAND_ROWS = 48
MAX_LIFE_TRACKS = 2
MAX_LIFE_FRAMES = 12
MAX_TRANSITION_FRAMES = 8
GENERIC_LIFE_SEMANTICS = {None, "", "auto_unreviewed", "profile_approved"}


def _sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _load_json(path: Path, errors: list[str], label: str) -> dict[str, Any] | None:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        errors.append(f"{label}: invalid JSON ({exc})")
        return None
    if not isinstance(value, dict):
        errors.append(f"{label}: JSON root must be an object")
        return None
    return value


def _safe_file(root: Path, rel: Any, errors: list[str], label: str) -> Path | None:
    if not isinstance(rel, str) or not rel or "\\" in rel:
        errors.append(f"{label}: invalid relative path")
        return None
    candidate = (root / rel).resolve()
    try:
        candidate.relative_to(root.resolve())
    except ValueError:
        errors.append(f"{label}: path escapes pack root ({rel})")
        return None
    if not candidate.is_file():
        errors.append(f"{label}: missing file ({rel})")
        return None
    return candidate


def _integer(value: Any) -> bool:
    return isinstance(value, int) and not isinstance(value, bool)


def _rect(value: Any, width: int, height: int, errors: list[str], label: str,
          max_rows: int | None = None) -> tuple[int, int, int, int] | None:
    if not isinstance(value, dict):
        errors.append(f"{label}: ROI must be an object")
        return None
    values = [value.get(k) for k in ("x", "y", "width", "height")]
    if not all(_integer(v) for v in values):
        errors.append(f"{label}: ROI fields must be integers")
        return None
    x, y, w, h = values
    if x < 0 or y < 0 or w < 1 or h < 1 or x + w > width or y + h > height:
        errors.append(f"{label}: ROI outside {width}x{height} ({x},{y} {w}x{h})")
        return None
    if max_rows is not None and h > max_rows:
        errors.append(f"{label}: {h} rows exceeds per-tick budget {max_rows}")
        return None
    return x, y, w, h


def _check_rgb565(folder: Path, rel: Any, width: int, height: int,
                  errors: list[str], label: str, expected_hash: Any = None) -> Path | None:
    path = _safe_file(folder, rel, errors, label)
    if path is None:
        return None
    expected = width * height * 2
    if path.stat().st_size != expected:
        errors.append(f"{label}: RGB565 size {path.stat().st_size}, expected {expected}")
        return None
    if expected_hash is not None:
        if not isinstance(expected_hash, str) or _sha256_bytes(path.read_bytes()) != expected_hash.lower():
            errors.append(f"{label}: SHA-256 mismatch")
            return None
    return path


def _validate_transition(folder: Path, layer: Any, layer_name: str, width: int, height: int,
                         hold_hash: str, errors: list[str]) -> None:
    if layer is None:
        return
    if not isinstance(layer, dict):
        errors.append(f"{folder.name}.{layer_name}: layer must be an object")
        return
    label = f"{folder.name}.{layer_name}"
    if layer.get("base_rgb565_sha256") != hold_hash:
        errors.append(f"{label}: canonical base hash mismatch")
    if layer.get("max_bands_per_tick", 1) != 1:
        errors.append(f"{label}: max_bands_per_tick must be 1")
    frames = layer.get("frames")
    if not isinstance(frames, list) or not 1 <= len(frames) <= MAX_TRANSITION_FRAMES:
        errors.append(f"{label}: frame count must be 1..{MAX_TRANSITION_FRAMES}")
        return
    for index, item in enumerate(frames):
        item_label = f"{label}[{index}]"
        if not isinstance(item, dict):
            errors.append(f"{item_label}: frame must be an object")
            continue
        roi = _rect(item.get("roi"), width, height, errors, item_label, MAX_BAND_ROWS)
        if roi:
            _check_rgb565(folder, item.get("rgb565"), roi[2], roi[3], errors,
                          item_label, item.get("rgb565_sha256"))


def _validate_clip(pack_root: Path, emotion: str, rel: Any, capabilities: set[str],
                   errors: list[str], warnings: list[str]) -> dict[str, Any] | None:
    manifest_path = _safe_file(pack_root, rel, errors, f"clips.{emotion}")
    if manifest_path is None:
        return None
    folder = manifest_path.parent
    manifest = _load_json(manifest_path, errors, emotion)
    if manifest is None:
        return None
    if manifest.get("schema") != "dialogue-emotion-clip-v2":
        errors.append(f"{emotion}: unsupported clip schema")
    if manifest.get("emotion") != emotion:
        errors.append(f"{emotion}: manifest emotion does not match clip slot")

    source = manifest.get("source")
    if not isinstance(source, dict):
        errors.append(f"{emotion}: source must be an object")
        return None
    width, height = source.get("width"), source.get("height")
    if width != SCREEN_WIDTH or height != SCREEN_HEIGHT:
        errors.append(f"{emotion}: firmware requires {SCREEN_WIDTH}x{SCREEN_HEIGHT} source")
        return None
    stream_path = _safe_file(folder, source.get("file"), errors, f"{emotion}.source")
    stream_data = stream_path.read_bytes() if stream_path else b""
    if stream_path:
        if source.get("bytes") != len(stream_data):
            errors.append(f"{emotion}.source: byte count mismatch")
        if source.get("sha256") != _sha256_bytes(stream_data):
            errors.append(f"{emotion}.source: SHA-256 mismatch")

    frames = manifest.get("frames")
    if not isinstance(frames, list) or len(frames) != source.get("frame_count"):
        errors.append(f"{emotion}: frame index count mismatch")
    elif stream_path:
        cursor = 0
        for index, item in enumerate(frames):
            label = f"{emotion}.frames[{index}]"
            if not isinstance(item, dict):
                errors.append(f"{label}: index must be an object")
                continue
            offset, size = item.get("offset"), item.get("size")
            if not _integer(offset) or not _integer(size) or offset != cursor or size <= 0:
                errors.append(f"{label}: non-contiguous or invalid offset/size")
                continue
            end = offset + size
            if end > len(stream_data):
                errors.append(f"{label}: frame exceeds stream")
                continue
            chunk = stream_data[offset:end]
            if not (chunk.startswith(b"\xff\xd8") and chunk.endswith(b"\xff\xd9")):
                errors.append(f"{label}: invalid JPEG boundary")
            if item.get("sha256") != _sha256_bytes(chunk):
                errors.append(f"{label}: SHA-256 mismatch")
            cursor = end
        if cursor != len(stream_data):
            errors.append(f"{emotion}: frame index does not cover complete stream")

    render = manifest.get("render")
    if not isinstance(render, dict):
        errors.append(f"{emotion}: render must be an object")
        return None
    mode = render.get("mode")
    if mode not in RENDER_MODES:
        errors.append(f"{emotion}: unsupported render mode {mode!r}")
        return None
    if mode not in capabilities:
        errors.append(f"{emotion}: render mode {mode} absent from pack capabilities")
    fallback = render.get("fallback")
    if fallback not in RENDER_MODES:
        errors.append(f"{emotion}: invalid fallback mode")

    summary = {"emotion": emotion, "mode": mode, "life_tracks": 0,
               "life_choreographies": 0, "unreviewed_life_tracks": 0}
    if mode != "layered":
        return summary

    mouth_roi = _rect(render.get("mouth_roi"), width, height, errors, f"{emotion}.mouth_roi")
    hold = render.get("hold_base")
    if not isinstance(hold, dict):
        errors.append(f"{emotion}: hold_base must be an object")
        return summary
    hold_w, hold_h = hold.get("width"), hold.get("height")
    if hold_w != width or hold_h != height:
        errors.append(f"{emotion}: hold_base must match source dimensions")
        return summary
    hold_path = _check_rgb565(folder, hold.get("rgb565"), width, height, errors,
                              f"{emotion}.hold_base")
    _safe_file(folder, hold.get("file"), errors, f"{emotion}.hold_base_preview")
    hold_hash = _sha256_bytes(hold_path.read_bytes()) if hold_path else ""

    levels = render.get("mouth_levels")
    if not isinstance(levels, dict) or set(levels) != set(MOUTH_LEVELS):
        errors.append(f"{emotion}: mouth_levels must be exactly {','.join(MOUTH_LEVELS)}")
    elif mouth_roi:
        for level in MOUTH_LEVELS:
            item = levels[level]
            if not isinstance(item, dict):
                errors.append(f"{emotion}.mouth.{level}: level must be an object")
                continue
            _safe_file(folder, item.get("file"), errors, f"{emotion}.mouth.{level}.preview")
            _check_rgb565(folder, item.get("rgb565"), mouth_roi[2], mouth_roi[3], errors,
                          f"{emotion}.mouth.{level}")

    eye_roi_value = render.get("eye_roi")
    eye_levels = render.get("eye_levels")
    if eye_roi_value is not None or eye_levels is not None:
        eye_roi = _rect(eye_roi_value, width, height, errors, f"{emotion}.eye_roi")
        if not isinstance(eye_levels, dict) or set(eye_levels) != set(EYE_LEVELS):
            errors.append(f"{emotion}: eye_levels must be exactly {','.join(EYE_LEVELS)}")
        elif eye_roi:
            for level in EYE_LEVELS:
                item = eye_levels[level]
                if not isinstance(item, dict):
                    errors.append(f"{emotion}.eye.{level}: level must be an object")
                    continue
                _safe_file(folder, item.get("file"), errors, f"{emotion}.eye.{level}.preview")
                _check_rgb565(folder, item.get("rgb565"), eye_roi[2], eye_roi[3], errors,
                              f"{emotion}.eye.{level}")

    pose_bank = render.get("pose_bank", {})
    if isinstance(pose_bank, dict):
        count = pose_bank.get("count", 1)
        poses = pose_bank.get("poses", [])
        if not _integer(count) or count < 1 or count > 2 or not isinstance(poses, list) or len(poses) != count:
            errors.append(f"{emotion}: firmware pose bank supports only one or two complete poses")

    life = render.get("life_layer")
    if life is not None:
        if not isinstance(life, dict):
            errors.append(f"{emotion}.life: layer must be an object")
        else:
            if life.get("base_rgb565_sha256") != hold_hash:
                errors.append(f"{emotion}.life: canonical base hash mismatch")
            if life.get("max_tracks_per_tick") != 1:
                errors.append(f"{emotion}.life: max_tracks_per_tick must be 1")
            if life.get("schedule") != "interleave_latest_drop_old":
                errors.append(f"{emotion}.life: schedule must drop stale frames")
            tracks = life.get("tracks")
            if not isinstance(tracks, list) or len(tracks) > MAX_LIFE_TRACKS:
                errors.append(f"{emotion}.life: firmware supports at most {MAX_LIFE_TRACKS} tracks")
            else:
                summary["life_tracks"] = len(tracks)
                seen_ids: set[int] = set()
                for track_index, track in enumerate(tracks):
                    label = f"{emotion}.life[{track_index}]"
                    if not isinstance(track, dict):
                        errors.append(f"{label}: track must be an object")
                        continue
                    track_id = track.get("id")
                    if not _integer(track_id) or track_id in seen_ids:
                        errors.append(f"{label}: track id must be a unique integer")
                    else:
                        seen_ids.add(track_id)
                    if track.get("approval") != "profile":
                        summary["unreviewed_life_tracks"] += 1
                        warnings.append(f"{label}: auto motion track is not semantically approved")
                    semantic = track.get("semantic")
                    choreography = track.get("choreography")
                    if choreography is not None:
                        if track.get("approval") != "profile" or semantic in GENERIC_LIFE_SEMANTICS:
                            errors.append(f"{label}: choreography requires a specific profile-approved semantic")
                        if not isinstance(choreography, dict):
                            errors.append(f"{label}: choreography must be an object")
                        else:
                            if choreography.get("schema") != "idle-life-cluster-v1":
                                errors.append(f"{label}: unsupported choreography schema")
                            keyframes = choreography.get("keyframes")
                            if (not isinstance(keyframes, list) or not 3 <= len(keyframes) <= 4 or
                                    any(not _integer(v) for v in keyframes)):
                                errors.append(f"{label}: choreography keyframes must contain 3..4 integer indices")
                            interval = choreography.get("frame_interval_ms")
                            if not _integer(interval) or not 160 <= interval <= 400:
                                errors.append(f"{label}: choreography frame_interval_ms must be 160..400")
                            rest = choreography.get("rest_ms")
                            if (not isinstance(rest, dict) or not _integer(rest.get("min")) or
                                    not _integer(rest.get("max")) or not 4000 <= rest["min"] <= rest["max"] <= 7000):
                                errors.append(f"{label}: choreography rest_ms must stay within 4000..7000")
                            if choreography.get("busy_policy") != "abort_to_base_no_replay":
                                errors.append(f"{label}: choreography must abort to base without replay")
                            if choreography.get("returns_to_base") is not True:
                                errors.append(f"{label}: choreography must declare returns_to_base=true")
                    roi = _rect(track.get("roi"), width, height, errors, label, MAX_BAND_ROWS)
                    track_frames = track.get("frames")
                    if not isinstance(track_frames, list) or not 2 <= len(track_frames) <= MAX_LIFE_FRAMES:
                        errors.append(f"{label}: frame count must be 2..{MAX_LIFE_FRAMES}")
                    elif roi:
                        frame_hashes: list[str | None] = []
                        for frame_index, item in enumerate(track_frames):
                            frame_label = f"{label}[{frame_index}]"
                            if not isinstance(item, dict):
                                errors.append(f"{frame_label}: frame must be an object")
                                continue
                            _check_rgb565(folder, item.get("rgb565"), roi[2], roi[3], errors,
                                          frame_label, item.get("rgb565_sha256"))
                            frame_hashes.append(item.get("rgb565_sha256"))
                            mask = _safe_file(folder, item.get("mask_a8"), errors,
                                              frame_label + ".mask")
                            if mask and mask.stat().st_size != roi[2] * roi[3]:
                                errors.append(f"{frame_label}: A8 mask size mismatch")
                            elif mask and item.get("mask_sha256") != _sha256_bytes(mask.read_bytes()):
                                errors.append(f"{frame_label}: A8 mask SHA-256 mismatch")
                        if choreography is not None and isinstance(choreography, dict):
                            keyframes = choreography.get("keyframes")
                            if isinstance(keyframes, list) and all(_integer(v) for v in keyframes):
                                if any(v < 0 or v >= len(track_frames) for v in keyframes):
                                    errors.append(f"{label}: choreography keyframe outside track")
                                elif keyframes[-1] != len(track_frames) - 1:
                                    errors.append(f"{label}: choreography must finish on the final base frame")
                            first, last = track_frames[0], track_frames[-1]
                            if (frame_hashes[0] != frame_hashes[-1] or
                                    first.get("peak_alpha") != 0 or last.get("peak_alpha") != 0 or
                                    first.get("active_ratio") != 0.0 or last.get("active_ratio") != 0.0):
                                errors.append(f"{label}: choreography track must close exactly on canonical base")
                            else:
                                summary["life_choreographies"] += 1

    _validate_transition(folder, render.get("enter_layer"), "enter_layer",
                         width, height, hold_hash, errors)
    _validate_transition(folder, render.get("release_layer"), "release_layer",
                         width, height, hold_hash, errors)
    return summary


def validate_pack(pack_root: Path) -> dict[str, Any]:
    pack_root = pack_root.resolve()
    errors: list[str] = []
    warnings: list[str] = []
    manifest = _load_json(pack_root / "pack_manifest.json", errors, "pack")
    if manifest is None:
        return {"ok": False, "errors": errors, "warnings": warnings, "clips": []}
    if manifest.get("schema") != "dialogue-emotion-pack-v2" or manifest.get("version") != 2:
        errors.append("pack: unsupported schema/version")
    capabilities_value = manifest.get("capabilities")
    if (not isinstance(capabilities_value, list) or
            any(v not in RENDER_MODES for v in capabilities_value) or
            len(set(capabilities_value)) != len(capabilities_value)):
        errors.append("pack: capabilities must be unique known render modes")
        capabilities: set[str] = set()
    else:
        capabilities = set(capabilities_value)
    clips = manifest.get("clips")
    if not isinstance(clips, dict) or set(clips) != set(EMOTIONS):
        errors.append(f"pack: clips must be exactly {','.join(EMOTIONS)}")
        clips = clips if isinstance(clips, dict) else {}
    runtime = manifest.get("runtime_contract")
    if runtime is None:
        warnings.append("pack: runtime_contract missing; accepted as legacy v2")
    elif not isinstance(runtime, dict):
        errors.append("pack: runtime_contract must be an object")
    else:
        if runtime.get("screen") != {"width": SCREEN_WIDTH, "height": SCREEN_HEIGHT,
                                      "pixel_format": "rgb565-le"}:
            errors.append("pack: runtime screen/pixel format mismatch")
        if runtime.get("max_rows_per_tick") != MAX_BAND_ROWS:
            errors.append(f"pack: max_rows_per_tick must be {MAX_BAND_ROWS}")
        if runtime.get("max_layers_per_tick") != 1:
            errors.append("pack: max_layers_per_tick must be 1")
    fallback = manifest.get("legacy_fallback")
    if not isinstance(fallback, dict) or fallback.get("enabled") is not True:
        errors.append("pack: legacy fallback must remain enabled")
    elif fallback.get("root") is None:
        warnings.append("pack: legacy fallback root is implicit; use /sdcard/mjpeg")

    summaries = []
    for emotion in EMOTIONS:
        summary = _validate_clip(pack_root, emotion, clips.get(emotion), capabilities,
                                 errors, warnings)
        if summary:
            summaries.append(summary)
    return {"ok": not errors, "errors": errors, "warnings": warnings,
            "clips": summaries,
            "firmware_contract": {"screen": f"{SCREEN_WIDTH}x{SCREEN_HEIGHT}",
                                  "max_rows_per_tick": MAX_BAND_ROWS,
                                  "max_layers_per_tick": 1}}


def main() -> int:
    parser = argparse.ArgumentParser(description="Validate a dialogue-emotion pack v2")
    parser.add_argument("pack", type=Path)
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()
    report = validate_pack(args.pack)
    text = json.dumps(report, ensure_ascii=False, indent=2)
    if args.report:
        args.report.write_text(text + "\n", encoding="utf-8")
    print(text)
    return 0 if report["ok"] else 2


if __name__ == "__main__":
    raise SystemExit(main())
