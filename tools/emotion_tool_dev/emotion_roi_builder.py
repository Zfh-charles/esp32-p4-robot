#!/usr/bin/env python3
"""Build fixed-ROI MJPEG emotion assets for the ESP32-P4 firmware."""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import sys
from dataclasses import dataclass
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw


BASE_EMOTIONS = ("standby", "neutral", "happy", "sad", "angry", "loving")


@dataclass(frozen=True)
class Box:
    x: int
    y: int
    w: int
    h: int

    @property
    def area(self) -> int:
        return self.w * self.h

    def as_dict(self) -> dict[str, int]:
        return {"x": self.x, "y": self.y, "width": self.w, "height": self.h}


def split_mjpeg(data: bytes) -> list[bytes]:
    """Split a raw concatenated-JPEG MJPEG stream without trusting embedded lengths."""
    frames: list[bytes] = []
    pos = 0
    while True:
        soi = data.find(b"\xff\xd8", pos)
        if soi < 0:
            break
        eoi = data.find(b"\xff\xd9", soi + 2)
        if eoi < 0:
            raise ValueError(f"truncated JPEG beginning at byte {soi}")
        frames.append(data[soi : eoi + 2])
        pos = eoi + 2
    if not frames:
        raise ValueError("no JPEG frames found")
    return frames


def decode_rgb(jpeg: bytes) -> np.ndarray:
    with Image.open(io.BytesIO(jpeg)) as image:
        return np.asarray(image.convert("RGB"), dtype=np.uint8)


def align_box(x0: int, y0: int, x1: int, y1: int, width: int, height: int,
              padding: int, alignment: int) -> Box:
    x0 = max(0, x0 - padding)
    y0 = max(0, y0 - padding)
    x1 = min(width, x1 + padding)
    y1 = min(height, y1 + padding)
    x0 = (x0 // alignment) * alignment
    y0 = (y0 // alignment) * alignment
    x1 = min(width, ((x1 + alignment - 1) // alignment) * alignment)
    y1 = min(height, ((y1 + alignment - 1) // alignment) * alignment)
    return Box(x0, y0, max(alignment, x1 - x0), max(alignment, y1 - y0))


def changed_blocks(frame: np.ndarray, base: np.ndarray, pixel_threshold: int,
                   block: int, block_ratio: float) -> np.ndarray:
    """Reject scattered JPEG noise by requiring enough changed pixels in a block."""
    delta = np.max(np.abs(frame.astype(np.int16) - base.astype(np.int16)), axis=2)
    changed = delta >= pixel_threshold
    height, width = changed.shape
    bh = (height + block - 1) // block
    bw = (width + block - 1) // block
    padded = np.zeros((bh * block, bw * block), dtype=np.uint8)
    padded[:height, :width] = changed
    counts = padded.reshape(bh, block, bw, block).sum(axis=(1, 3))
    return counts >= max(1, int(block * block * block_ratio))


def detect_roi(decoded: list[np.ndarray], base: np.ndarray, args: argparse.Namespace) -> Box:
    height, width, _ = base.shape
    union = np.zeros(((height + args.block - 1) // args.block,
                      (width + args.block - 1) // args.block), dtype=bool)
    for frame in decoded:
        union |= changed_blocks(frame, base, args.pixel_threshold, args.block, args.block_ratio)
    ys, xs = np.nonzero(union)
    if not len(xs):
        cx, cy = width // 2, height // 2
        return align_box(cx - args.block, cy - args.block, cx + args.block,
                         cy + args.block, width, height, args.padding, args.alignment)
    x0 = int(xs.min()) * args.block
    y0 = int(ys.min()) * args.block
    x1 = min(width, (int(xs.max()) + 1) * args.block)
    y1 = min(height, (int(ys.max()) + 1) * args.block)
    return align_box(x0, y0, x1, y1, width, height, args.padding, args.alignment)


def encode_jpeg(rgb: np.ndarray, quality: int) -> bytes:
    output = io.BytesIO()
    Image.fromarray(rgb, "RGB").save(output, "JPEG", quality=quality, optimize=True,
                                      subsampling="4:2:0")
    return output.getvalue()


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def save_preview(frame: np.ndarray, roi: Box, path: Path) -> None:
    image = Image.fromarray(frame, "RGB")
    draw = ImageDraw.Draw(image)
    draw.rectangle((roi.x, roi.y, roi.x + roi.w - 1, roi.y + roi.h - 1),
                   outline=(255, 40, 40), width=3)
    image.save(path, "PNG")


def validate_output(output_dir: Path, reports: list[dict]) -> int:
    """Verify every indexed byte range, digest, JPEG boundary and decoded ROI size."""
    total = 0
    for report in reports:
        target_dir = output_dir / report["emotion"]
        stream = (target_dir / report["stream_file"]).read_bytes()
        if sha256(stream) != report["stream_sha256"]:
            raise ValueError(f'{report["emotion"]}: stream SHA-256 mismatch')
        expected_size = (report["roi"]["width"], report["roi"]["height"])
        for entry in report["frames"]:
            start = entry["offset"]
            frame = stream[start : start + entry["size"]]
            if not frame.startswith(b"\xff\xd8") or not frame.endswith(b"\xff\xd9"):
                raise ValueError(f'{report["emotion"]}: invalid JPEG at frame {entry["index"]}')
            if sha256(frame) != entry["sha256"]:
                raise ValueError(f'{report["emotion"]}: frame SHA-256 mismatch at {entry["index"]}')
            with Image.open(io.BytesIO(frame)) as image:
                if image.size != expected_size:
                    raise ValueError(
                        f'{report["emotion"]}: frame {entry["index"]} is {image.size}, '
                        f'expected {expected_size}')
                image.verify()
            total += 1
    return total


def process_emotion(name: str, source: Path, base: np.ndarray, output_root: Path,
                    args: argparse.Namespace) -> dict:
    source_bytes = source.read_bytes()
    jpeg_frames = split_mjpeg(source_bytes)
    decoded = [decode_rgb(frame) for frame in jpeg_frames]
    shape = decoded[0].shape
    if any(frame.shape != shape for frame in decoded):
        raise ValueError(f"{source.name}: inconsistent frame dimensions")
    if shape != base.shape:
        raise ValueError(f"{source.name}: {shape[1]}x{shape[0]} differs from standby base")

    roi = detect_roi(decoded, base, args)
    height, width, _ = shape
    target_dir = output_root / name
    target_dir.mkdir(parents=True, exist_ok=True)
    stream = bytearray()
    entries = []
    for index, frame in enumerate(decoded):
        crop = frame[roi.y : roi.y + roi.h, roi.x : roi.x + roi.w]
        encoded = encode_jpeg(crop, args.quality)
        offset = len(stream)
        stream.extend(encoded)
        entries.append({
            "index": index,
            "offset": offset,
            "size": len(encoded),
            "duration_ms": args.frame_duration_ms,
            "sha256": sha256(encoded),
        })

    stream_bytes = bytes(stream)
    (target_dir / "frames.mjpeg").write_bytes(stream_bytes)
    save_preview(decoded[len(decoded) // 3], roi, target_dir / "roi_preview.png")
    ratio = roi.area / (width * height)
    manifest = {
        "schema": "xiaozhi-emotion-roi-v1",
        "emotion": name,
        "canvas": {"width": width, "height": height, "format": "RGB565"},
        "roi": roi.as_dict(),
        "encoding": "jpeg-concatenated",
        "pixel_format_after_decode": "RGB565",
        "frame_count": len(entries),
        "default_frame_duration_ms": args.frame_duration_ms,
        "stream_file": "frames.mjpeg",
        "stream_size": len(stream_bytes),
        "stream_sha256": sha256(stream_bytes),
        "source_file": source.name,
        "source_size": len(source_bytes),
        "source_sha256": sha256(source_bytes),
        "roi_area_ratio": round(ratio, 6),
        "estimated_rgb565_bytes_per_frame": roi.area * 2,
        "estimated_reduction_vs_full_frame": round(1.0 - ratio, 6),
        "frames": entries,
    }
    (target_dir / "manifest.json").write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8")
    return manifest


def build(args: argparse.Namespace) -> int:
    input_dir = args.input.resolve()
    output_dir = args.output.resolve()
    if input_dir == output_dir or input_dir in output_dir.parents:
        raise ValueError("output must not be the input directory or a child of it")
    missing = [name for name in BASE_EMOTIONS if not (input_dir / f"{name}.mjpeg").is_file()]
    if missing and not args.allow_partial:
        raise FileNotFoundError("missing required emotions: " + ", ".join(missing))
    sources = [(name, input_dir / f"{name}.mjpeg") for name in BASE_EMOTIONS
               if (input_dir / f"{name}.mjpeg").is_file()]
    if not sources:
        raise FileNotFoundError(f"no base emotion .mjpeg files found in {input_dir}")

    standby_path = input_dir / "standby.mjpeg"
    if not standby_path.is_file():
        raise FileNotFoundError("standby.mjpeg is required as the common base")
    standby_frames = split_mjpeg(standby_path.read_bytes())
    seed_index = min(len(standby_frames) - 1, len(standby_frames) // 3)
    base = decode_rgb(standby_frames[seed_index])

    output_dir.mkdir(parents=True, exist_ok=True)
    base_jpeg = encode_jpeg(base, args.quality)
    (output_dir / "standby_base.jpg").write_bytes(base_jpeg)
    reports = []
    for name, source in sources:
        print(f"[build] {name}: {source.name}", flush=True)
        reports.append(process_emotion(name, source, base, output_dir, args))

    root_manifest = {
        "schema": "xiaozhi-emotion-pack-v1",
        "input_directory": str(input_dir),
        "base": {
            "file": "standby_base.jpg",
            "seed_frame_index": seed_index,
            "width": int(base.shape[1]),
            "height": int(base.shape[0]),
            "sha256": sha256(base_jpeg),
        },
        "detection": {
            "pixel_threshold": args.pixel_threshold,
            "block": args.block,
            "block_ratio": args.block_ratio,
            "padding": args.padding,
            "alignment": args.alignment,
        },
        "emotions": [{
            "emotion": item["emotion"],
            "manifest": f'{item["emotion"]}/manifest.json',
            "roi": item["roi"],
            "frame_count": item["frame_count"],
            "source_size": item["source_size"],
            "output_size": item["stream_size"],
            "estimated_reduction_vs_full_frame": item["estimated_reduction_vs_full_frame"],
        } for item in reports],
    }
    (output_dir / "pack_manifest.json").write_text(
        json.dumps(root_manifest, ensure_ascii=False, indent=2), encoding="utf-8")

    lines = [
        "emotion,frames,roi_x,roi_y,roi_w,roi_h,roi_area_percent,source_bytes,output_bytes",
    ]
    for item in reports:
        roi = item["roi"]
        lines.append(
            f'{item["emotion"]},{item["frame_count"]},{roi["x"]},{roi["y"]},'
            f'{roi["width"]},{roi["height"]},{item["roi_area_ratio"] * 100:.2f},'
            f'{item["source_size"]},{item["stream_size"]}')
    (output_dir / "report.csv").write_text("\n".join(lines) + "\n", encoding="utf-8-sig")
    verified_frames = validate_output(output_dir, reports)
    print(f"[verify] emotions={len(reports)} frames={verified_frames} OK")
    print(f"[done] output: {output_dir}")
    print(f"[done] report: {output_dir / 'report.csv'}")
    return 0


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Convert full-frame concatenated-JPEG emotions to fixed-ROI asset packs.")
    parser.add_argument("--input", type=Path, required=True, help="directory containing *.mjpeg")
    parser.add_argument("--output", type=Path, required=True, help="new output directory")
    parser.add_argument("--quality", type=int, default=75, choices=range(50, 101), metavar="50..100")
    parser.add_argument("--pixel-threshold", type=int, default=28,
                        help="RGB max-channel delta treated as changed")
    parser.add_argument("--block", type=int, default=8, help="noise rejection block size")
    parser.add_argument("--block-ratio", type=float, default=0.18,
                        help="fraction of changed pixels required per block")
    parser.add_argument("--padding", type=int, default=16, help="pixels added around detected ROI")
    parser.add_argument("--alignment", type=int, default=8, help="ROI coordinate/size alignment")
    parser.add_argument("--frame-duration-ms", type=int, default=160)
    parser.add_argument("--allow-partial", action="store_true")
    return parser.parse_args()


if __name__ == "__main__":
    try:
        sys.exit(build(parse_args()))
    except Exception as exc:
        print(f"error: {exc}", file=sys.stderr)
        sys.exit(2)
