#!/usr/bin/env python3
"""Build indexed dialogue-emotion v2 assets from concatenated JPEG MJPEG files."""

from __future__ import annotations

import argparse
import csv
import io
import json
import shutil
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

from dialogue_mjpeg_io import Frame, gray_small, rgb, scan_mjpeg, sha256
from dialogue_pack_contract import validate_pack


EMOTIONS = ("standby", "neutral", "happy", "sad", "angry", "loving")
MOUTH_LEVELS = ("closed", "small", "medium", "large")
EYE_LEVELS = ("open", "half", "closed")
LIFE_TRACK_ROWS = 48
LIFE_SEQUENCE_OFFSETS = (0, 1, 2, 3, 4, 3, 2, 1, 0)
RELEASE_TRACK_ROWS = 48
RELEASE_EMOTIONS = ("sad", "angry")


def load_profile(path: Path | None) -> dict:
    if path is None:
        return {}
    with path.open("r", encoding="utf-8") as f:
        value = json.load(f)
    if value.get("schema") not in (None, "dialogue-emotion-profile-v1"):
        raise ValueError("unsupported profile schema")
    return value


def segments(count: int, override: dict | None) -> dict[str, dict[str, int]]:
    if override:
        result = {}
        for name in ("enter", "hold", "exit"):
            part = override[name]
            start, end = int(part["start"]), int(part["end"])
            if not (0 <= start <= end < count):
                raise ValueError(f"invalid {name} range {start}..{end} for {count} frames")
            result[name] = {"start": start, "end": end}
        return result
    a = max(1, count // 3)
    b = max(a + 1, (count * 2) // 3)
    b = min(b, count - 1)
    return {
        "enter": {"start": 0, "end": a - 1},
        "hold": {"start": a, "end": b - 1},
        "exit": {"start": b, "end": count - 1},
    }


def clamp_rect(rect: dict, width: int, height: int) -> dict[str, int]:
    x = max(0, min(width - 1, int(rect["x"])))
    y = max(0, min(height - 1, int(rect["y"])))
    w = max(8, min(width - x, int(rect.get("width", rect.get("w", 0)))))
    h = max(8, min(height - y, int(rect.get("height", rect.get("h", 0)))))
    return {"x": x, "y": y, "width": w, "height": h}


def suggest_mouth(frames: list[Frame], hold: dict[str, int], width: int, height: int) -> tuple[dict, dict]:
    sw, sh = 120, 120
    ids = np.linspace(hold["start"], hold["end"], min(20, hold["end"] - hold["start"] + 1), dtype=int)
    samples = np.stack([gray_small(frames[int(i)], (sw, sh)) for i in ids])
    motion = np.mean(np.abs(np.diff(samples, axis=0)), axis=0) if len(samples) > 1 else np.zeros((sh, sw))
    # Constrain the search to the central/lower face, while still allowing profile overrides.
    x0, x1 = int(sw * .28), int(sw * .72)
    y0, y1 = int(sh * .40), int(sh * .62)
    ww, wh = max(12, int(sw * .24)), max(8, int(sh * .12))
    best = (-1.0, x0, y0)
    for y in range(y0, max(y0 + 1, y1 - wh + 1), 3):
        for x in range(x0, max(x0 + 1, x1 - ww + 1), 3):
            inner = float(motion[y:y + wh, x:x + ww].mean())
            pad = 4
            outer = motion[max(0, y-pad):min(sh, y+wh+pad), max(0, x-pad):min(sw, x+ww+pad)]
            cx = (x + ww / 2) / sw
            cy = (y + wh / 2) / sh
            center_penalty = 10.0 * abs(cx - .50) + 8.0 * abs(cy - .48)
            score = inner - .25 * float(outer.mean()) - center_penalty
            if score > best[0]:
                best = (score, x, y)
    _, x, y = best
    rect = {
        "x": int(round(x * width / sw)), "y": int(round(y * height / sh)),
        "width": int(round(ww * width / sw)), "height": int(round(wh * height / sh)),
    }
    confidence = min(1.0, max(0.0, best[0] / 12.0))
    return clamp_rect(rect, width, height), {"motion_score": round(best[0], 3), "confidence": round(confidence, 3)}


def crop_array(a: np.ndarray, rect: dict) -> np.ndarray:
    x, y, w, h = rect["x"], rect["y"], rect["width"], rect["height"]
    return a[y:y+h, x:x+w]


def choose_mouth_frames(frames: list[Frame], hold: dict, rect: dict) -> tuple[dict[str, int], list[float]]:
    ids = list(range(hold["start"], hold["end"] + 1))
    arrays = [crop_array(rgb(frames[i]).astype(np.float32), rect) for i in ids]
    median = np.median(np.stack(arrays), axis=0)
    activity = np.asarray([float(np.mean(np.abs(a - median))) for a in arrays])
    order = np.argsort(activity)
    picks = {}
    for name, q in zip(MOUTH_LEVELS, (0.05, 0.38, 0.68, 0.95)):
        pos = int(round(q * (len(order) - 1)))
        picks[name] = ids[int(order[pos])]
    return picks, [round(float(v), 3) for v in activity]


def choose_second_pose(frames: list[Frame], hold: dict, rect: dict, primary: int,
                       activity: list[float], override: int | None) -> tuple[int, float]:
    if override is not None:
        chosen = int(override)
        if not (hold["start"] <= chosen <= hold["end"]):
            raise ValueError("pose_1_frame must be inside hold segment")
    else:
        ids = list(range(hold["start"], hold["end"] + 1))
        cutoff = float(np.quantile(np.asarray(activity), 0.40))
        candidates = [i for i, a in zip(ids, activity) if a <= cutoff and i != primary]
        primary_small = gray_small(frames[primary], (96, 96))
        scored = [(float(np.mean(np.abs(gray_small(frames[i], (96, 96)) - primary_small))), i)
                  for i in candidates]
        usable = [v for v in scored if 2.0 <= v[0] <= 18.0]
        chosen = max(usable or scored or [(0.0, primary)])[1]
    delta = float(np.mean(np.abs(gray_small(frames[chosen], (96, 96)) -
                                 gray_small(frames[primary], (96, 96)))))
    return chosen, round(delta, 3)


def risk_report(frames: list[Frame], picks: dict[str, int], rect: dict, width: int, height: int) -> dict:
    base = rgb(frames[picks["closed"]]).astype(np.float32)
    x, y, w, h = rect["x"], rect["y"], rect["width"], rect["height"]
    pad = max(6, min(w, h) // 8)
    x0, y0, x1, y1 = max(0, x-pad), max(0, y-pad), min(width, x+w+pad), min(height, y+h+pad)
    mask = np.ones((y1-y0, x1-x0), dtype=bool)
    mask[y-y0:y-y0+h, x-x0:x-x0+w] = False
    border_scores = []
    global_scores = []
    for level in MOUTH_LEVELS[1:]:
        arr = rgb(frames[picks[level]]).astype(np.float32)
        delta = np.mean(np.abs(arr - base), axis=2)
        border_scores.append(float(delta[y0:y1, x0:x1][mask].mean()) if mask.any() else 0.0)
        global_scores.append(float(delta.mean()))
    seam = max(border_scores, default=0.0)
    global_motion = max(global_scores, default=0.0)
    safe = seam <= 8.0 and global_motion <= 12.0
    return {
        "layered_safe": safe,
        "recommended_render_mode": "layered" if safe else "full_frame_clip",
        "seam_border_delta": round(seam, 3),
        "global_frame_delta": round(global_motion, 3),
        "reason": "mouth patch boundary is stable" if safe else "motion extends beyond the mouth patch; review or use full-frame fallback",
    }


def mouth_composite(base: np.ndarray, candidate: np.ndarray, rect: dict,
                    feather_px: int, align_px: int,
                    activity_expand_px: int = 7) -> tuple[np.ndarray, dict]:
    """Align a mouth candidate and keep only softly dilated local activity."""
    x, y, w, h = rect["x"], rect["y"], rect["width"], rect["height"]
    base_patch = base[y:y+h, x:x+w].astype(np.float32)
    feather = max(2, min(int(feather_px), max(2, min(w, h) // 3)))
    yy, xx = np.mgrid[0:h, 0:w]
    edge = np.minimum.reduce((xx, yy, w - 1 - xx, h - 1 - yy)).astype(np.float32)
    edge_alpha = np.clip(edge / float(feather), 0.0, 1.0)
    edge_alpha = edge_alpha * edge_alpha * (3.0 - 2.0 * edge_alpha)
    border = edge_alpha < 0.55

    best_score, best_patch, best_shift = float("inf"), None, (0, 0)
    for dy in range(-align_px, align_px + 1):
        for dx in range(-align_px, align_px + 1):
            sx, sy = x + dx, y + dy
            if sx < 0 or sy < 0 or sx + w > candidate.shape[1] or sy + h > candidate.shape[0]:
                continue
            patch = candidate[sy:sy+h, sx:sx+w].astype(np.float32)
            score = float(np.mean(np.abs(patch[border] - base_patch[border])))
            if score < best_score:
                best_score, best_patch, best_shift = score, patch, (dx, dy)
    if best_patch is None:
        best_patch = candidate[y:y+h, x:x+w].astype(np.float32)
    # Suppress low-level whole-frame/JPEG drift inside the rectangular ROI;
    # keep only a softly expanded mouth activity island.
    motion = np.mean(np.abs(best_patch - base_patch), axis=2)
    activity = np.clip((motion - 4.0) / 12.0, 0.0, 1.0)
    expand = max(3, min(21, int(activity_expand_px)))
    if expand % 2 == 0:
        expand += 1
    activity_img = Image.fromarray(np.uint8(np.rint(activity * 255.0)), "L")
    activity_img = activity_img.filter(ImageFilter.MaxFilter(expand)).filter(
        ImageFilter.GaussianBlur(radius=max(4.0, feather * 0.70)))
    activity_alpha = np.asarray(activity_img, dtype=np.float32) / 255.0
    # Mouth assets frequently include moving neckline/hair at the ROI corners.
    # A soft central-upper prior rejects those pixels without introducing a
    # new hard contour. Profiles can still move/resize the ROI per material.
    nx = (xx - (w - 1) * 0.50) / max(1.0, w * 0.43)
    ny = (yy - (h - 1) * 0.32) / max(1.0, h * 0.52)
    spatial = np.clip(1.0 - (nx * nx + ny * ny), 0.0, 1.0)
    spatial = spatial * spatial * (3.0 - 2.0 * spatial)
    alpha = edge_alpha * activity_alpha * spatial
    composed = best_patch * alpha[:, :, None] + base_patch * (1.0 - alpha[:, :, None])
    composed = np.clip(np.rint(composed), 0, 255).astype(np.uint8)
    delta = np.mean(np.abs(composed.astype(np.float32) - base_patch), axis=2)
    rings = {}
    for name, lo, hi in (("outer", 0, 2), ("near", 2, 5), ("inner", 5, 9)):
        ring = (edge >= lo) & (edge < hi)
        rings[name] = round(float(delta[ring].mean()) if ring.any() else 0.0, 3)
    seam_score = max(rings["outer"], rings["near"] * 0.60, rings["inner"] * 0.35)
    transition = (alpha > 0.05) & (alpha < 0.95)
    transition_p95 = float(np.percentile(delta[transition], 95)) if transition.any() else 0.0
    alpha_step = np.maximum(
        np.pad(np.abs(np.diff(alpha, axis=1)), ((0, 0), (0, 1))),
        np.pad(np.abs(np.diff(alpha, axis=0)), ((0, 1), (0, 0))),
    )
    alpha_step_p95 = float(np.percentile(alpha_step[transition], 95)) if transition.any() else 0.0
    return composed, {"dx": best_shift[0], "dy": best_shift[1],
                      "border_match_delta": round(best_score, 3),
                      "activity_expand_px": expand,
                      "composite_edge_delta": rings["outer"],
                      "composite_ring_delta": rings,
                      "composite_seam_score": round(seam_score, 3),
                      "transition_delta_p95": round(transition_p95, 3),
                      "alpha_step_p95": round(alpha_step_p95, 4)}


def save_seam_heatmap(base: np.ndarray, patches: dict[str, np.ndarray], rect: dict,
                      target: Path) -> None:
    """Visual audit of residual patch-vs-canonical-base deltas."""
    x, y, w, h = rect["x"], rect["y"], rect["width"], rect["height"]
    base_patch = base[y:y+h, x:x+w].astype(np.float32)
    peak = np.maximum.reduce([
        np.mean(np.abs(p.astype(np.float32) - base_patch), axis=2) for p in patches.values()
    ])
    heat = np.uint8(np.clip(peak * 12.0, 0, 255))
    heat_rgb = np.zeros((h, w, 3), dtype=np.uint8)
    heat_rgb[:, :, 0] = heat
    heat_rgb[:, :, 1] = np.uint8(np.clip(255 - np.abs(heat.astype(np.int16) - 128) * 2, 0, 255))
    heat_rgb[:, :, 2] = 255 - heat
    Image.fromarray(heat_rgb, "RGB").resize(
        (w * 3, h * 3), Image.Resampling.NEAREST).save(target)


def save_patch_array(patch: np.ndarray, target: Path, quality: int) -> None:
    Image.fromarray(patch, "RGB").save(target, "JPEG", quality=quality, optimize=True)


def save_rgb565_array(patch: np.ndarray, target: Path) -> None:
    """Firmware loads these directly — no on-device JPEG decode for mouth/eye."""
    r = (patch[:, :, 0].astype(np.uint16) >> 3)
    g = (patch[:, :, 1].astype(np.uint16) >> 2)
    b = (patch[:, :, 2].astype(np.uint16) >> 3)
    pix = ((r << 11) | (g << 5) | b).astype("<u2")
    target.write_bytes(pix.tobytes())


def load_rgb565_array(source: Path, width: int, height: int) -> np.ndarray:
    """Load a generated RGB565 surface back to RGB888 for deterministic recomposition."""
    pix = np.frombuffer(source.read_bytes(), dtype="<u2").reshape((height, width))
    r = ((pix >> 11) & 0x1f).astype(np.uint8)
    g = ((pix >> 5) & 0x3f).astype(np.uint8)
    b = (pix & 0x1f).astype(np.uint8)
    return np.stack(((r << 3) | (r >> 2), (g << 2) | (g >> 4),
                     (b << 3) | (b >> 2)), axis=2)


def _aligned_span(active: np.ndarray, limit: int, alignment: int = 8) -> tuple[int, int]:
    ids = np.flatnonzero(active)
    if ids.size == 0:
        return 0, limit
    start = max(0, (int(ids[0]) // alignment) * alignment)
    end = min(limit, ((int(ids[-1]) + 1 + alignment - 1) // alignment) * alignment)
    if end - start < 64:
        center = (start + end) // 2
        start = max(0, center - 32)
        end = min(limit, start + 64)
        start = max(0, end - 64)
    return start, end


def build_life_layer(frames: list[Frame], hold: dict, base_frame: int, base: np.ndarray,
                     mouth_rect: dict, eye_rect: dict, out: Path, emotion: str,
                     track_policy: dict | None = None) -> dict:
    """Generate same-generation, pre-cropped RGB565+alpha life tracks."""
    track_policy = track_policy or {}
    approved_raw = track_policy.get("approved_auto_tracks")
    approved_tracks = None
    if approved_raw is not None:
        if (not isinstance(approved_raw, list) or
                any(not isinstance(v, int) or v not in (0, 1) for v in approved_raw) or
                len(set(approved_raw)) != len(approved_raw)):
            raise ValueError("life_track_policy.approved_auto_tracks must be unique track ids 0/1")
        approved_tracks = set(approved_raw)
    semantic_labels = track_policy.get("semantic_labels", {})
    if not isinstance(semantic_labels, dict):
        raise ValueError("life_track_policy.semantic_labels must be an object")
    height, width = base.shape[:2]
    direction = 1 if base_frame + 4 <= hold["end"] else -1
    ids = [max(hold["start"], min(hold["end"], base_frame + direction * n))
           for n in LIFE_SEQUENCE_OFFSETS]
    arrays = [rgb(frames[i]) for i in ids]
    delta_stack = np.stack([
        np.mean(np.abs(a.astype(np.float32) - base.astype(np.float32)), axis=2)
        for a in arrays
    ])
    motion = np.mean(delta_stack, axis=0)
    for rect in (mouth_rect, eye_rect):
        x, y, w, h = rect["x"], rect["y"], rect["width"], rect["height"]
        motion[y:y+h, x:x+w] = 0.0

    track_ranges = ((max(8, height // 12), min(height, height // 2)),
                    (max(0, height // 2), min(height, height - height // 10)))
    life_root = out / "life"
    life_root.mkdir(exist_ok=True)
    previews = [base.copy() for _ in arrays]
    tracks = []
    for track_id, (search_y0, search_y1) in enumerate(track_ranges):
        # Auto motion is only a candidate detector.  A profile may approve the
        # semantically useful tracks (for example body breathing) and reject a
        # visually distracting hair/accessory-only track without firmware hacks.
        if approved_tracks is not None and track_id not in approved_tracks:
            continue
        if search_y1 - search_y0 < LIFE_TRACK_ROWS:
            continue
        row_score = motion.mean(axis=1)
        best_y, best_score = search_y0, -1.0
        for y in range(search_y0, search_y1 - LIFE_TRACK_ROWS + 1, 4):
            score = float(row_score[y:y + LIFE_TRACK_ROWS].mean())
            if score > best_score:
                best_y, best_score = y, score
        band_motion = motion[best_y:best_y + LIFE_TRACK_ROWS]
        threshold = max(2.5, float(np.quantile(band_motion, 0.70)))
        x0, x1 = _aligned_span(np.any(band_motion >= threshold, axis=0), width)
        roi = {"x": x0, "y": best_y, "width": x1 - x0, "height": LIFE_TRACK_ROWS}
        track_dir = life_root / f"track_{track_id}"
        track_dir.mkdir(exist_ok=True)
        entries = []
        for seq, (frame_id, candidate, delta) in enumerate(zip(ids, arrays, delta_stack)):
            patch = candidate[best_y:best_y + LIFE_TRACK_ROWS, x0:x1]
            local_delta = delta[best_y:best_y + LIFE_TRACK_ROWS, x0:x1]
            alpha = np.clip((local_delta - 2.0) * (255.0 / 14.0), 0, 220).astype(np.uint8)
            yy, xx = np.mgrid[0:LIFE_TRACK_ROWS, 0:x1-x0]
            edge = np.minimum.reduce((xx, yy, x1-x0-1-xx, LIFE_TRACK_ROWS-1-yy))
            alpha = (alpha.astype(np.float32) * np.clip(edge / 8.0, 0.0, 1.0)).astype(np.uint8)
            alpha = np.array(Image.fromarray(alpha, "L").filter(ImageFilter.GaussianBlur(2.0)),
                             dtype=np.uint8, copy=True)
            if seq in (0, len(arrays) - 1):
                alpha.fill(0)
            # Precompose every life frame against the canonical hold base.  The
            # device can then replace the ROI deterministically instead of
            # accumulating alpha over the previous canvas frame (which leaves
            # ghosts when the final zero-alpha frame is reached).
            base_crop = base[best_y:best_y + LIFE_TRACK_ROWS, x0:x1].astype(np.float32)
            a = alpha.astype(np.float32)[:, :, None] / 255.0
            composite = np.clip(
                patch.astype(np.float32) * a + base_crop * (1.0 - a), 0, 255).astype(np.uint8)
            rgb_path = track_dir / f"{seq:02d}.rgb565"
            mask_path = track_dir / f"{seq:02d}.a8"
            save_rgb565_array(composite, rgb_path)
            mask_path.write_bytes(alpha.tobytes())
            previews[seq][best_y:best_y + LIFE_TRACK_ROWS, x0:x1] = composite
            entries.append({"sequence": seq, "source_frame": frame_id,
                            "rgb565": f"life/track_{track_id}/{seq:02d}.rgb565",
                            "rgb565_sha256": sha256(rgb_path.read_bytes()),
                            "mask_a8": f"life/track_{track_id}/{seq:02d}.a8",
                            "mask_sha256": sha256(mask_path.read_bytes()),
                            "peak_alpha": int(alpha.max()),
                            "active_ratio": round(float(np.count_nonzero(alpha)) / alpha.size, 4)})
        semantic = semantic_labels.get(str(track_id), "auto_unreviewed")
        track = {"id": track_id, "roi": roi, "motion_score": round(best_score, 3),
                 "semantic": semantic,
                 "approval": "profile" if approved_tracks is not None else "auto_unreviewed",
                 "frames": entries}
        if approved_tracks is not None and semantic not in ("", "auto_unreviewed", "profile_approved"):
            # Four perceptual keyframes preserve the full PC-authored rise/peak/fall/base
            # gesture while capping P4 idle traffic to four <=48-row commits.
            last = len(entries) - 1
            keyframes = [1, len(entries) // 2, max(1, last - 1), last]
            keyframes = list(dict.fromkeys(keyframes))
            if len(keyframes) >= 3:
                track["choreography"] = {
                    "schema": "idle-life-cluster-v1",
                    "keyframes": keyframes,
                    "frame_interval_ms": 240,
                    "rest_ms": {"min": 4200, "max": 6900},
                    "busy_policy": "abort_to_base_no_replay",
                    "returns_to_base": True,
                }
        tracks.append(track)

    generation_seed = (sha256(base.tobytes()) + emotion + json.dumps(ids) +
                       json.dumps([t["roi"] for t in tracks], sort_keys=True)).encode("utf-8")
    generation_id = sha256(generation_seed)[:20]
    Image.fromarray(base, "RGB").save(life_root / "base_preview.png")
    preview_images = [Image.fromarray(p, "RGB") for p in previews]
    preview_images[0].save(life_root / "life_preview.gif", save_all=True,
                           append_images=preview_images[1:], duration=120, loop=0)
    return {"schema": "dialogue-life-layer-v1", "generation_id": generation_id,
            "composite_mode": "canonical_base_precomposited_v1",
            "base_rgb565_sha256": sha256((out / "hold_base.rgb565").read_bytes()),
            "enabled_default": emotion in ("standby", "neutral", "happy", "loving"),
            "sequence_ms": 120, "sequence": ids, "max_tracks_per_tick": 1,
            "schedule": "interleave_latest_drop_old", "tracks": tracks,
            "preview": "life/life_preview.gif"}


def _release_rect(rect: dict, width: int, height: int) -> dict[str, int]:
    """Keep a facial release patch inside one display row-band."""
    w = min(width, int(rect["width"]))
    h = min(RELEASE_TRACK_ROWS, int(rect["height"]), height)
    x = max(0, min(width - w, int(rect["x"])))
    center_y = int(rect["y"]) + int(rect["height"]) // 2
    y = max(0, min(height - h, center_y - h // 2))
    return {"x": x, "y": y, "width": w, "height": h}


def _release_composite(base: np.ndarray, candidate: np.ndarray, rect: dict) -> np.ndarray:
    """Precompose an exit crop against the emotion hold base with soft edges."""
    x, y, w, h = rect["x"], rect["y"], rect["width"], rect["height"]
    src = candidate[y:y+h, x:x+w].astype(np.float32)
    dst = base[y:y+h, x:x+w].astype(np.float32)
    yy, xx = np.mgrid[0:h, 0:w]
    edge = np.minimum.reduce((xx, yy, w - 1 - xx, h - 1 - yy)).astype(np.float32)
    alpha = np.clip(edge / 8.0, 0.0, 1.0)
    alpha = alpha * alpha * (3.0 - 2.0 * alpha)
    return np.clip(np.rint(src * alpha[:, :, None] + dst * (1.0 - alpha[:, :, None])),
                   0, 255).astype(np.uint8)


def build_release_layer(frames: list[Frame], exit_seg: dict, base_frame: int,
                        base: np.ndarray, mouth_rect: dict, eye_rect: dict,
                        out: Path, emotion: str) -> dict | None:
    """Generate a cheap strong-emotion release: close mouth, then relax eye/mouth bands."""
    if emotion not in RELEASE_EMOTIONS:
        return None
    height, width = base.shape[:2]
    mouth = _release_rect(mouth_rect, width, height)
    eye = _release_rect(eye_rect, width, height)
    mid = int(round(exit_seg["start"] + (exit_seg["end"] - exit_seg["start"]) * 0.55))
    end = int(round(exit_seg["start"] + (exit_seg["end"] - exit_seg["start"]) * 0.92))
    # One patch per worker tick.  First restore the canonical closed mouth,
    # then alternate eye/mouth so no tick submits two display bands.
    schedule = (("mouth", base_frame, mouth), ("eye", mid, eye),
                ("mouth", mid, mouth), ("eye", end, eye), ("mouth", end, mouth))
    root = out / "release"
    root.mkdir(exist_ok=True)
    preview = base.copy()
    previews = []
    entries = []
    for seq, (region, frame_id, rect) in enumerate(schedule):
        candidate = base if frame_id == base_frame else rgb(frames[frame_id])
        patch = _release_composite(base, candidate, rect)
        target = root / f"{seq:02d}_{region}.rgb565"
        save_rgb565_array(patch, target)
        x, y, w, h = rect["x"], rect["y"], rect["width"], rect["height"]
        preview[y:y+h, x:x+w] = patch
        previews.append(Image.fromarray(preview.copy(), "RGB"))
        entries.append({"sequence": seq, "region": region, "source_frame": frame_id,
                        "roi": rect, "rgb565": f"release/{target.name}",
                        "rgb565_sha256": sha256(target.read_bytes())})
    previews[0].save(root / "release_preview.gif", save_all=True,
                     append_images=previews[1:], duration=90, loop=0)
    generation_seed = (emotion + sha256(base.tobytes()) + json.dumps(entries, sort_keys=True)).encode("utf-8")
    return {"schema": "dialogue-release-layer-v1",
            "generation_id": sha256(generation_seed)[:20],
            "composite_mode": "canonical_base_precomposited_v1",
            "base_rgb565_sha256": sha256((out / "hold_base.rgb565").read_bytes()),
            "enabled_default": True, "interval_ms": 90,
            "max_bands_per_tick": 1, "frames": entries,
            "preview": "release/release_preview.gif"}


def _split_transition_rois(rects: list[tuple[str, dict]]) -> list[tuple[str, dict]]:
    """Split feature islands so every transition tick remains within one 48-row band."""
    result = []
    for name, rect in rects:
        y, h = int(rect["y"]), int(rect["height"])
        if h <= RELEASE_TRACK_ROWS:
            result.append((name, dict(rect)))
            continue
        top_h = h // 2
        result.append((name + "_top", {**rect, "height": top_h}))
        result.append((name + "_bottom", {**rect, "y": y + top_h,
                                           "height": h - top_h}))
    return result


def _build_hub_track(start: np.ndarray, end: np.ndarray,
                     rects: list[tuple[str, dict]], root: Path,
                     prefix: str, interval_ms: int = 75) -> dict:
    """Two-stage eased feature transition; final patches are exact target pixels."""
    root.mkdir(exist_ok=True)
    entries, previews = [], []
    canvas = start.copy()
    regions = _split_transition_rois(rects)
    for stage, alpha in enumerate((0.5, 1.0)):
        eased = alpha * alpha * (3.0 - 2.0 * alpha)
        for region, rect in regions:
            x, y, w, h = (int(rect[k]) for k in ("x", "y", "width", "height"))
            if alpha >= 1.0:
                patch = end[y:y+h, x:x+w].copy()
            else:
                a = start[y:y+h, x:x+w].astype(np.float32)
                b = end[y:y+h, x:x+w].astype(np.float32)
                patch = np.clip(np.rint(a * (1.0 - eased) + b * eased), 0, 255).astype(np.uint8)
            target = root / f"{len(entries):02d}_{region}.rgb565"
            save_rgb565_array(patch, target)
            canvas[y:y+h, x:x+w] = patch
            previews.append(Image.fromarray(canvas.copy(), "RGB"))
            entries.append({"sequence": len(entries), "stage": stage, "region": region,
                            "roi": rect, "rgb565": f"{prefix}/{target.name}",
                            "rgb565_sha256": sha256(target.read_bytes())})
    previews[0].save(root / f"{prefix}_preview.gif", save_all=True,
                     append_images=previews[1:], duration=interval_ms, loop=0)
    return {"schema": "dialogue-common-hub-track-v1", "interval_ms": interval_ms,
            "max_bands_per_tick": 1, "frames": entries,
            "final_exact_target": True,
            "preview": f"{prefix}/{prefix}_preview.gif"}


def canonicalize_strong_emotion_hub(pack_root: Path, emotion: str,
                                    quality: int) -> None:
    """Rebase angry/sad expression islands onto standby for reversible local transitions."""
    folder = pack_root / emotion
    manifest_path = folder / "manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    standby_manifest = json.loads((pack_root / "standby" / "manifest.json").read_text(encoding="utf-8"))
    width, height = int(manifest["source"]["width"]), int(manifest["source"]["height"])
    standby = load_rgb565_array(pack_root / "standby" / "hold_base.rgb565", width, height)
    native = load_rgb565_array(folder / "hold_base.rgb565", width, height)
    mouth_rect = dict(standby_manifest["render"]["mouth_roi"])
    eye_rect = dict(standby_manifest["render"]["eye_roi"])

    # Find the source feature inside a generous alignment window, but feather it
    # into the standby geometry.  The rest of the 480x480 canvas stays identical.
    hub = standby.copy()
    eye_patch, eye_align = mouth_composite(standby, native, eye_rect, 14, 32)
    mouth_patch, mouth_align = mouth_composite(standby, native, mouth_rect, 14, 32)
    for patch, rect in ((eye_patch, eye_rect), (mouth_patch, mouth_rect)):
        x, y, w, h = (int(rect[k]) for k in ("x", "y", "width", "height"))
        hub[y:y+h, x:x+w] = patch

    frames = scan_mjpeg((folder / "frames.mjpeg").read_bytes())
    mouth_levels, eye_levels = {}, {}
    for level, item in manifest["render"]["mouth_levels"].items():
        candidate = rgb(frames[int(item["frame"])])
        mouth_levels[level], _ = mouth_composite(hub, candidate, mouth_rect, 14, 32)
        save_patch_array(mouth_levels[level], folder / item["file"], quality)
        save_rgb565_array(mouth_levels[level], folder / item["rgb565"])
    for level, item in manifest["render"]["eye_levels"].items():
        candidate = rgb(frames[int(item["frame"])])
        eye_levels[level], _ = mouth_composite(hub, candidate, eye_rect, 14, 32)
        save_patch_array(eye_levels[level], folder / item["file"], quality)
        save_rgb565_array(eye_levels[level], folder / item["rgb565"])

    Image.fromarray(hub, "RGB").save(folder / "hold_base.jpg", "JPEG",
                                      quality=quality, optimize=True)
    save_rgb565_array(hub, folder / "hold_base.rgb565")
    rects = [("eye", eye_rect), ("mouth", mouth_rect)]
    enter = _build_hub_track(standby, hub, rects, folder / "hub_enter", "hub_enter")
    release = _build_hub_track(hub, standby, rects, folder / "hub_exit", "hub_exit")
    base_hash = sha256((folder / "hold_base.rgb565").read_bytes())
    enter["base_rgb565_sha256"] = base_hash
    release["base_rgb565_sha256"] = base_hash
    release["final_standby_rgb565_sha256"] = sha256(
        (pack_root / "standby" / "hold_base.rgb565").read_bytes())

    render = manifest["render"]
    render["mouth_roi"], render["eye_roi"] = mouth_rect, eye_rect
    render["hold_base"]["frame"] = -1
    render["hold_base"]["generated"] = "standby_common_hub_v1"
    render["pose_bank"] = {"version": 1, "count": 1,
                           "poses": [{"id": 0, "base_frame": -1, "root": "."}]}
    render["life_layer"] = None
    render["enter_layer"] = enter
    render["release_layer"] = release
    render["common_hub"] = {"schema": "standby-common-hub-v1",
                            "standby_rgb565_sha256": release["final_standby_rgb565_sha256"],
                            "eye_alignment": eye_align, "mouth_alignment": mouth_align}
    manifest["analysis"]["common_hub"] = {
        "global_canvas_preserved": True, "transition_regions": [v for _, v in rects],
        "entry_frames": len(enter["frames"]), "exit_frames": len(release["frames"])}
    manifest_path.write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8")


def save_composite_preview(base: np.ndarray, patches: dict[str, np.ndarray], rect: dict,
                           target: Path, label: str) -> None:
    views = []
    x, y, w, h = rect["x"], rect["y"], rect["width"], rect["height"]
    for level in MOUTH_LEVELS:
        view = base.copy()
        view[y:y+h, x:x+w] = patches[level]
        views.append(Image.fromarray(view, "RGB"))
    canvas = Image.new("RGB", (base.shape[1] * 2, base.shape[0] * 2))
    draw = ImageDraw.Draw(canvas)
    for i, view in enumerate(views):
        ox, oy = (i % 2) * base.shape[1], (i // 2) * base.shape[0]
        canvas.paste(view, (ox, oy))
        draw.rectangle((ox + x, oy + y, ox + x + w - 1, oy + y + h - 1), outline=(0, 255, 0), width=2)
        draw.rectangle((ox + 8, oy + 8, ox + 180, oy + 34), fill=(0, 0, 0))
        draw.text((ox + 14, oy + 13), f"{label} {MOUTH_LEVELS[i]}", fill=(255, 255, 255))
    canvas.save(target, "PNG")


def suggest_eye(frames: list[Frame], hold: dict[str, int], width: int, height: int) -> tuple[dict, dict]:
    sw, sh = 120, 120
    ids = np.linspace(hold["start"], hold["end"], min(16, hold["end"] - hold["start"] + 1), dtype=int)
    samples = np.stack([gray_small(frames[int(i)], (sw, sh)) for i in ids])
    motion = np.mean(np.abs(np.diff(samples, axis=0)), axis=0) if len(samples) > 1 else np.zeros((sh, sw))
    x0, x1 = int(sw * 0.22), int(sw * 0.78)
    y0, y1 = int(sh * 0.22), int(sh * 0.42)
    ww, wh = max(20, int(sw * 0.40)), max(10, int(sh * 0.12))
    best = (-1.0, x0, y0)
    for y in range(y0, max(y0 + 1, y1 - wh + 1), 3):
        for x in range(x0, max(x0 + 1, x1 - ww + 1), 3):
            score = float(motion[y:y + wh, x:x + ww].mean())
            if score > best[0]:
                best = (score, x, y)
    _, x, y = best
    rect = {
        "x": int(round(x * width / sw)),
        "y": int(round(y * height / sh)),
        "width": int(round(ww * width / sw)),
        "height": int(round(wh * height / sh)),
    }
    return clamp_rect(rect, width, height), {"motion_score": round(best[0], 3)}


def choose_eye_frames(frames: list[Frame], hold: dict, rect: dict) -> dict[str, int]:
    ids = list(range(hold["start"], hold["end"] + 1))
    arrays = [crop_array(rgb(frames[i]).astype(np.float32), rect) for i in ids]
    # Darker = more closed for typical eye crops.
    brightness = np.asarray([float(a.mean()) for a in arrays])
    order = np.argsort(brightness)  # low → closed
    return {
        "closed": ids[int(order[0])],
        "half": ids[int(order[len(order) // 2])],
        "open": ids[int(order[-1])],
    }


def save_preview(frame: Frame, rect: dict, target: Path, label: str) -> None:
    with Image.open(io.BytesIO(frame.data)) as im:
        canvas = im.convert("RGB")
    draw = ImageDraw.Draw(canvas)
    x, y, w, h = rect["x"], rect["y"], rect["width"], rect["height"]
    draw.rectangle((x, y, x+w-1, y+h-1), outline=(255, 0, 0), width=4)
    draw.rectangle((8, 8, 8 + max(120, len(label)*8), 34), fill=(0, 0, 0))
    draw.text((14, 13), label, fill=(255, 255, 255))
    canvas.save(target, "PNG")


def frame_index(frames: list[Frame], duration_ms: int) -> list[dict]:
    return [{"frame": i, "offset": f.offset, "size": len(f.data), "duration_ms": duration_ms,
             "sha256": sha256(f.data)} for i, f in enumerate(frames)]


def build_one(source: Path, out: Path, profile: dict, duration_ms: int, quality: int) -> tuple[dict, dict]:
    data = source.read_bytes()
    frames = scan_mjpeg(data)
    first = rgb(frames[0])
    height, width = first.shape[:2]
    for i in sorted(set((0, len(frames)//2, len(frames)-1))):
        a = rgb(frames[i])
        if a.shape[:2] != (height, width):
            raise ValueError(f"mixed dimensions at frame {i}")
    seg = segments(len(frames), profile.get("segments"))
    suggested_rect, detection = suggest_mouth(frames, seg["hold"], width, height)
    rect = clamp_rect(profile.get("mouth_roi", suggested_rect), width, height)
    picks, activity = choose_mouth_frames(frames, seg["hold"], rect)
    if "mouth_frames" in profile:
        for level in MOUTH_LEVELS:
            picks[level] = int(profile["mouth_frames"][level])
            if not (seg["hold"]["start"] <= picks[level] <= seg["hold"]["end"]):
                raise ValueError(f"{level} frame must be inside hold segment")
    risk = risk_report(frames, picks, rect, width, height)
    source_risk = dict(risk)
    base = rgb(frames[picks["closed"]])
    pose_1_frame, pose_delta = choose_second_pose(
        frames, seg["hold"], rect, picks["closed"], activity, profile.get("pose_1_frame"))
    pose_bases = [base, rgb(frames[pose_1_frame])]
    feather_px = int(profile.get("mouth_feather_px", 10))
    align_px = max(0, min(8, int(profile.get("mouth_align_px", 4))))
    # Normalize pose 1 to the same closed-mouth semantic state before building its atlas.
    pose1_closed, _ = mouth_composite(pose_bases[1], base, rect, feather_px, align_px)
    x, y, w, h = rect["x"], rect["y"], rect["width"], rect["height"]
    pose_bases[1] = pose_bases[1].copy()
    pose_bases[1][y:y+h, x:x+w] = pose1_closed
    pose_mouth, pose_alignment = [], []
    level_feather_scale = profile.get("mouth_level_feather_scale", {})
    level_activity_expand = profile.get("mouth_activity_expand_px", {})
    for pose_index, pose_base in enumerate(pose_bases):
        mouth_patches, alignment = {}, {}
        for level in MOUTH_LEVELS:
            candidate = pose_base if level == "closed" else rgb(frames[picks[level]])
            scale = max(1.0, min(2.5, float(level_feather_scale.get(level, 1.0))))
            level_feather = max(2, int(round(feather_px * scale)))
            level_expand = int(level_activity_expand.get(level, 7))
            mouth_patches[level], alignment[level] = mouth_composite(
                pose_base, candidate, rect, level_feather, align_px, level_expand)
        pose_mouth.append(mouth_patches)
        pose_alignment.append(alignment)
    mouth_patches, alignment = pose_mouth[0], pose_alignment[0]
    composite_edge = max(v["composite_edge_delta"] for v in alignment.values())
    composite_seam = max(v["composite_seam_score"] for v in alignment.values())
    risk = dict(risk)
    risk.update({"source_layered_safe": source_risk["layered_safe"],
                 "source_seam_border_delta": source_risk["seam_border_delta"],
                 "composite_edge_delta": composite_edge,
                 "composite_seam_score": composite_seam,
                 "layered_safe": composite_seam <= 3.0,
                 "recommended_render_mode": "layered" if composite_seam <= 3.0 else "full_frame_clip",
                 "reason": "mouth patches use activity-masked canonical-base feathering"})
    requested_mode = profile.get("render_mode", "auto")
    if requested_mode == "auto" and profile.get("force_layered"):
        requested_mode = "layered"
    mode = risk["recommended_render_mode"] if requested_mode == "auto" else requested_mode
    if mode not in ("full_frame_clip", "fixed_roi_clip", "layered", "static"):
        raise ValueError(f"unsupported render mode: {mode}")
    if mode == "layered" and not risk["layered_safe"]:
        risk = dict(risk)
        risk["forced_layered"] = True
        risk["reason"] = (risk.get("reason", "") +
                          "; layered forced by profile/CLI — review mouth_roi before SD deploy")

    out.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, out / "frames.mjpeg")
    patches = out / "mouth"
    patches.mkdir(exist_ok=True)
    for level in MOUTH_LEVELS:
        save_patch_array(mouth_patches[level], patches / f"{level}.jpg", quality)
        save_rgb565_array(mouth_patches[level], patches / f"{level}.rgb565")

    eye_rect, eye_det = suggest_eye(frames, seg["hold"], width, height)
    eye_rect = clamp_rect(profile.get("eye_roi", eye_rect), width, height)
    eye_picks = choose_eye_frames(frames, seg["hold"], eye_rect)
    if "eye_frames" in profile:
        for level in EYE_LEVELS:
            eye_picks[level] = int(profile["eye_frames"][level])
    eyes = out / "eye"
    eyes.mkdir(exist_ok=True)
    pose_eyes = []
    eye_feather_px = int(profile.get("eye_feather_px", 8))
    for pose_base in pose_bases:
        eye_set = {}
        for level in EYE_LEVELS:
            eye_set[level], _ = mouth_composite(
                pose_base, rgb(frames[eye_picks[level]]), eye_rect, eye_feather_px, align_px)
        pose_eyes.append(eye_set)
    for level in EYE_LEVELS:
        eye_patch = pose_eyes[0][level]
        save_patch_array(eye_patch, eyes / f"{level}.jpg", quality)
        save_rgb565_array(eye_patch, eyes / f"{level}.rgb565")

    hold_base = out / "hold_base.jpg"
    hold_base.write_bytes(frames[picks["closed"]].data)
    save_rgb565_array(base, out / "hold_base.rgb565")
    save_composite_preview(base, mouth_patches, rect, out / "mouth_roi_preview.png", source.stem)
    save_seam_heatmap(base, mouth_patches, rect, out / "mouth_seam_heatmap.png")
    save_preview(frames[eye_picks["open"]], eye_rect, out / "eye_roi_preview.png", f"{source.stem}: eye ROI")

    pose_1_dir = out / "pose_1"
    (pose_1_dir / "mouth").mkdir(parents=True, exist_ok=True)
    (pose_1_dir / "eye").mkdir(exist_ok=True)
    Image.fromarray(pose_bases[1], "RGB").save(pose_1_dir / "hold_base.jpg", "JPEG", quality=quality, optimize=True)
    save_rgb565_array(pose_bases[1], pose_1_dir / "hold_base.rgb565")
    for level in MOUTH_LEVELS:
        save_patch_array(pose_mouth[1][level], pose_1_dir / "mouth" / f"{level}.jpg", quality)
        save_rgb565_array(pose_mouth[1][level], pose_1_dir / "mouth" / f"{level}.rgb565")
    for level in EYE_LEVELS:
        save_patch_array(pose_eyes[1][level], pose_1_dir / "eye" / f"{level}.jpg", quality)
        save_rgb565_array(pose_eyes[1][level], pose_1_dir / "eye" / f"{level}.rgb565")
    save_composite_preview(pose_bases[1], pose_mouth[1], rect,
                           out / "mouth_roi_preview_pose_1.png", f"{source.stem} pose1")

    life_layer = build_life_layer(frames, seg["hold"], picks["closed"], base,
                                  rect, eye_rect, out, source.stem,
                                  profile.get("life_track_policy"))
    release_layer = None
    if not profile.get("skip_release_layer"):
        release_layer = build_release_layer(frames, seg["exit"], picks["closed"], base,
                                            rect, eye_rect, out, source.stem)

    manifest = {
        "schema": "dialogue-emotion-clip-v2", "emotion": source.stem,
        "source": {"file": "frames.mjpeg", "bytes": len(data), "sha256": sha256(data),
                   "width": width, "height": height, "frame_count": len(frames)},
        "render": {"mode": mode, "fallback": "full_frame_clip", "mouth_roi": rect,
                   "hold_base": {"frame": picks["closed"], "file": "hold_base.jpg",
                                 "rgb565": "hold_base.rgb565", "width": width, "height": height},
                   "mouth_composite": {"method": "canonical_base_activity_feather_v2",
                                       "feather_px": feather_px, "alignment_px": align_px,
                                       "level_feather_scale": level_feather_scale,
                                       "level_activity_expand_px": level_activity_expand,
                                       "levels": alignment},
                   "mouth_levels": {
                       k: {"frame": picks[k], "file": f"mouth/{k}.jpg", "rgb565": f"mouth/{k}.rgb565"}
                       for k in MOUTH_LEVELS
                   },
                   "eye_roi": eye_rect,
                   "eye_levels": {
                       k: {"frame": eye_picks[k], "file": f"eye/{k}.jpg", "rgb565": f"eye/{k}.rgb565"}
                       for k in EYE_LEVELS
                   },
                   "pose_bank": {"version": 1, "count": 2, "switch_min_ms": 6000,
                                 "switch_max_ms": 12000, "blink_masked": True,
                                 "poses": [
                                     {"id": 0, "base_frame": picks["closed"], "root": "."},
                                     {"id": 1, "base_frame": pose_1_frame, "root": "pose_1"}
                                 ]},
                   "life_layer": life_layer},
        "segments": seg, "frames": frame_index(frames, duration_ms),
        "analysis": {"mouth_detection": detection, "mouth_activity": activity, "risk": risk,
                     "eye_detection": eye_det,
                     "pose_bank": {"pose_1_frame": pose_1_frame, "pose_delta": pose_delta}},
    }
    if release_layer:
        manifest["render"]["release_layer"] = release_layer
    (out / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8")
    generated = {"segments": seg, "mouth_roi": rect, "mouth_frames": picks,
                 "eye_roi": eye_rect, "eye_frames": eye_picks,
                 "pose_1_frame": pose_1_frame,
                 "mouth_level_feather_scale": level_feather_scale,
                 "mouth_activity_expand_px": level_activity_expand,
                 "render_mode": mode, "auto_suggested_mouth_roi": suggested_rect}
    if "life_track_policy" in profile:
        generated["life_track_policy"] = profile["life_track_policy"]
    return manifest, generated


def validate_clip(folder: Path, manifest: dict) -> list[str]:
    errors: list[str] = []
    data = (folder / manifest["source"]["file"]).read_bytes()
    if sha256(data) != manifest["source"]["sha256"]:
        errors.append("stream SHA-256 mismatch")
    hold = manifest["render"].get("hold_base", {})
    hold_rgb = folder / hold.get("rgb565", "")
    expected_hold = int(hold.get("width", 0)) * int(hold.get("height", 0)) * 2
    if not hold_rgb.is_file() or hold_rgb.stat().st_size != expected_hold:
        errors.append("missing or invalid canonical hold_base rgb565")
    for item in manifest["frames"]:
        chunk = data[item["offset"]:item["offset"] + item["size"]]
        if not (chunk.startswith(b"\xff\xd8") and chunk.endswith(b"\xff\xd9")):
            errors.append(f"frame {item['frame']} JPEG boundary invalid")
        if sha256(chunk) != item["sha256"]:
            errors.append(f"frame {item['frame']} hash mismatch")
    for level, item in manifest["render"]["mouth_levels"].items():
        if not (folder / item["file"]).is_file():
            errors.append(f"missing mouth patch {level}")
        rgb565 = item.get("rgb565")
        if rgb565 and not (folder / rgb565).is_file():
            errors.append(f"missing mouth rgb565 {level}")
    for level, item in manifest["render"].get("eye_levels", {}).items():
        if not (folder / item["file"]).is_file():
            errors.append(f"missing eye patch {level}")
        rgb565 = item.get("rgb565")
        if rgb565 and not (folder / rgb565).is_file():
            errors.append(f"missing eye rgb565 {level}")
    pose_bank = manifest["render"].get("pose_bank", {})
    for pose in pose_bank.get("poses", [])[1:]:
        root = folder / pose["root"]
        if (root / "hold_base.rgb565").stat().st_size != expected_hold:
            errors.append(f"invalid pose base {pose['id']}")
        for level in MOUTH_LEVELS:
            if not (root / "mouth" / f"{level}.rgb565").is_file():
                errors.append(f"missing pose {pose['id']} mouth {level}")
        for level in EYE_LEVELS:
            if not (root / "eye" / f"{level}.rgb565").is_file():
                errors.append(f"missing pose {pose['id']} eye {level}")
    life = manifest["render"].get("life_layer", {})
    if life:
        if life.get("base_rgb565_sha256") != sha256(hold_rgb.read_bytes()):
            errors.append("life layer base hash mismatch")
        for track in life.get("tracks", []):
            roi = track.get("roi", {})
            expected = int(roi.get("width", 0)) * int(roi.get("height", 0))
            if int(roi.get("height", 0)) > LIFE_TRACK_ROWS:
                errors.append(f"life track {track.get('id')} exceeds row budget")
            for item in track.get("frames", []):
                rgb_path = folder / item["rgb565"]
                mask_path = folder / item["mask_a8"]
                if not rgb_path.is_file() or rgb_path.stat().st_size != expected * 2:
                    errors.append(f"invalid life RGB565 {rgb_path.name}")
                if not mask_path.is_file() or mask_path.stat().st_size != expected:
                    errors.append(f"invalid life mask {mask_path.name}")
    for layer_name in ("enter_layer", "release_layer"):
        release = manifest["render"].get(layer_name)
        if not release:
            continue
        if release.get("base_rgb565_sha256") != sha256(hold_rgb.read_bytes()):
            errors.append(f"{layer_name} base hash mismatch")
        frames_out = release.get("frames", [])
        if not (1 <= len(frames_out) <= 8):
            errors.append(f"{layer_name} frame count invalid")
        for item in frames_out:
            roi = item.get("roi", {})
            rh = int(roi.get("height", 0))
            expected = int(roi.get("width", 0)) * rh * 2
            rgb_path = folder / item.get("rgb565", "")
            if rh <= 0 or rh > RELEASE_TRACK_ROWS:
                errors.append(f"{layer_name} ROI exceeds {RELEASE_TRACK_ROWS} rows")
            if not rgb_path.is_file() or rgb_path.stat().st_size != expected:
                errors.append(f"missing or invalid {layer_name} patch {item.get('rgb565')}")
    return errors


def main() -> int:
    ap = argparse.ArgumentParser(description="Build dialogue emotion manifest v2 and volume-driven mouth assets")
    ap.add_argument("--input", required=True, type=Path)
    ap.add_argument("--output", required=True, type=Path)
    ap.add_argument("--profile", type=Path)
    ap.add_argument("--frame-duration-ms", type=int, default=160)
    ap.add_argument("--quality", type=int, default=82)
    ap.add_argument("--force-layered", action="store_true",
                    help="Force render_mode=layered even when risk says unsafe (review ROI!)")
    ap.add_argument("--max-mouth-soften", type=float, default=1.0,
                    help="PC-only P-seam candidate: soften medium/large mouth for four non-strong emotions")
    ap.add_argument("--skip-strong-hub", action="store_true",
                    help="Do not generate the rejected angry/sad common-hub experiment")
    ap.add_argument("--skip-release-layer", action="store_true",
                    help="Keep the v5 contract: do not emit the rejected angry/sad release track")
    ap.add_argument("--life-approved-tracks", action="append", default=[], metavar="EMOTION:IDS[:SEMANTIC]",
                    help="Semantic gate, e.g. standby:1:body_breathe or happy:1:body_weight_shift")
    args = ap.parse_args()
    if args.input.resolve() == args.output.resolve() or args.input.resolve() in args.output.resolve().parents:
        raise ValueError("output must not be the input directory or inside it")
    profile = load_profile(args.profile)
    for spec in args.life_approved_tracks:
        parts = spec.split(":", 2)
        if len(parts) < 2 or parts[0] not in EMOTIONS:
            raise ValueError("--life-approved-tracks expects EMOTION:IDS[:SEMANTIC] with a known emotion")
        emotion, ids_raw = parts[0], parts[1]
        semantic = parts[2].strip() if len(parts) == 3 else "profile_approved"
        if len(parts) == 3 and (not semantic or not semantic.replace("_", "").isalnum()):
            raise ValueError("life semantic must be a non-empty identifier")
        try:
            ids = [] if not ids_raw else [int(v) for v in ids_raw.split(",")]
        except ValueError as exc:
            raise ValueError("life track ids must be comma-separated integers 0/1") from exc
        item = profile.setdefault("emotions", {}).setdefault(emotion, {})
        item["life_track_policy"] = {
            "approved_auto_tracks": ids,
            "semantic_labels": {str(v): semantic for v in ids},
        }
    if not 1.0 <= args.max_mouth_soften <= 2.0:
        raise ValueError("--max-mouth-soften must be in 1.0..2.0")
    if args.max_mouth_soften > 1.0:
        medium_scale = 1.0 + (args.max_mouth_soften - 1.0) * 0.5
        medium_expand = int(round(7 + (args.max_mouth_soften - 1.0) * 4))
        large_expand = int(round(7 + (args.max_mouth_soften - 1.0) * 8))
        for value_name, value in (("medium", medium_expand), ("large", large_expand)):
            if value % 2 == 0:
                value += 1
            if value_name == "medium":
                medium_expand = value
            else:
                large_expand = value
        for emotion in ("standby", "neutral", "happy", "loving"):
            item = profile.setdefault("emotions", {}).setdefault(emotion, {})
            item["mouth_level_feather_scale"] = {
                "medium": round(medium_scale, 3), "large": args.max_mouth_soften}
            item["mouth_activity_expand_px"] = {
                "medium": medium_expand, "large": large_expand}
    if args.skip_release_layer:
        for emotion in EMOTIONS:
            profile.setdefault("emotions", {}).setdefault(emotion, {})["skip_release_layer"] = True
    if args.force_layered:
        profile["force_layered"] = True
        for emo in EMOTIONS:
            profile.setdefault("emotions", {}).setdefault(emo, {})["render_mode"] = "layered"
    args.output.mkdir(parents=True, exist_ok=True)
    clips, generated, rows = {}, {"schema": "dialogue-emotion-profile-v1", "emotions": {}}, []
    for emotion in EMOTIONS:
        source = args.input / f"{emotion}.mjpeg"
        if not source.is_file():
            raise FileNotFoundError(source)
        manifest, suggestion = build_one(source, args.output / emotion,
                                         profile.get("emotions", {}).get(emotion, {}),
                                         args.frame_duration_ms, args.quality)
        clips[emotion] = f"{emotion}/manifest.json"
        generated["emotions"][emotion] = suggestion
        risk = manifest["analysis"]["risk"]
        rows.append({"emotion": emotion, "frames": manifest["source"]["frame_count"],
                     "mode": manifest["render"]["mode"], "layered_safe": risk["layered_safe"],
                     "source_seam_delta": risk["source_seam_border_delta"],
                     "composite_edge_delta": risk["composite_edge_delta"],
                     "composite_seam_score": risk["composite_seam_score"],
                     "global_delta": risk["global_frame_delta"]})
    if not args.skip_strong_hub:
        for emotion in RELEASE_EMOTIONS:
            canonicalize_strong_emotion_hub(args.output, emotion, args.quality)
    pack = {
        "schema": "dialogue-emotion-pack-v2", "version": 2,
        "runtime_contract": {
            "screen": {"width": 480, "height": 480, "pixel_format": "rgb565-le"},
            "asset_root": "/sdcard/dialogue_v2",
            "max_rows_per_tick": LIFE_TRACK_ROWS,
            "max_layers_per_tick": 1,
            "delivery": "latest_drop_old",
        },
        "state_machine": {"states": ["standby", "enter", "hold", "exit"],
                          "emotion_slots": ["requested", "pending", "committed"],
                          "tts_flow": ["enter", "hold", "exit"],
                          "mouth_signal": {"source": "smoothed_audio_rms", "levels": list(MOUTH_LEVELS),
                                           "delivery": "latest_value", "lvgl_from_audio_task": False}},
        "capabilities": ["full_frame_clip", "fixed_roi_clip", "layered", "static"],
        "legacy_fallback": {"enabled": True, "root": "/sdcard/mjpeg",
                            "files": [f"{e}.mjpeg" for e in EMOTIONS]},
        "clips": clips,
    }
    (args.output / "pack_manifest.json").write_text(json.dumps(pack, ensure_ascii=False, indent=2), encoding="utf-8")
    (args.output / "profile.generated.json").write_text(json.dumps(generated, ensure_ascii=False, indent=2), encoding="utf-8")
    with (args.output / "analysis_report.csv").open("w", newline="", encoding="utf-8-sig") as f:
        writer = csv.DictWriter(f, fieldnames=rows[0].keys())
        writer.writeheader(); writer.writerows(rows)
    errors = []
    for emotion, rel in clips.items():
        manifest = json.loads((args.output / rel).read_text(encoding="utf-8"))
        errors.extend(f"{emotion}: {e}" for e in validate_clip(args.output / emotion, manifest))
    validation = {"ok": not errors, "errors": errors, "clips": len(clips)}
    contract = validate_pack(args.output)
    errors.extend(f"contract: {e}" for e in contract["errors"])
    validation = {"ok": not errors, "errors": errors, "clips": len(clips),
                  "contract_warnings": contract["warnings"]}
    (args.output / "contract_report.json").write_text(
        json.dumps(contract, ensure_ascii=False, indent=2), encoding="utf-8")
    (args.output / "validation_report.json").write_text(json.dumps(validation, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps({"output": str(args.output), "validation": validation, "analysis": rows}, ensure_ascii=False, indent=2))
    return 0 if not errors else 2


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)
