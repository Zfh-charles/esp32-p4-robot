"""MJPEG frame I/O helpers shared by dialogue emotion builder tools."""

from __future__ import annotations

import hashlib
import io
from dataclasses import dataclass

import numpy as np
from PIL import Image


@dataclass(frozen=True)
class Frame:
    offset: int
    data: bytes


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def scan_mjpeg(data: bytes) -> list[Frame]:
    frames: list[Frame] = []
    cursor = 0
    while True:
        soi = data.find(b"\xff\xd8", cursor)
        if soi < 0:
            break
        eoi = data.find(b"\xff\xd9", soi + 2)
        if eoi < 0:
            raise ValueError(f"truncated JPEG at byte {soi}")
        frames.append(Frame(soi, data[soi : eoi + 2]))
        cursor = eoi + 2
    if not frames:
        raise ValueError("no JPEG frames found")
    return frames


def rgb(frame: Frame) -> np.ndarray:
    with Image.open(io.BytesIO(frame.data)) as im:
        return np.asarray(im.convert("RGB"), dtype=np.uint8)


def gray_small(frame: Frame, size: tuple[int, int]) -> np.ndarray:
    with Image.open(io.BytesIO(frame.data)) as im:
        return np.asarray(im.convert("L").resize(size, Image.Resampling.BILINEAR), dtype=np.float32)
