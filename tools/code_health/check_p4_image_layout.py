#!/usr/bin/env python3
"""Reject ESP32-P4 images with a pathological pre-IROM RAM padding segment."""

from __future__ import annotations

import argparse
import struct
from dataclasses import dataclass
from pathlib import Path


ESP_IMAGE_MAGIC = 0xE9
ESP_IMAGE_HEADER_SIZE = 24
SEGMENT_HEADER_SIZE = 8
P4_IROM_LOAD_ADDRESS = 0x40000020
P4_PADDING_LOAD_ADDRESS = 0x4FF00000
MAX_SAFE_PADDING_BYTES = 0x1000


@dataclass(frozen=True)
class Segment:
    load_address: int
    length: int
    file_offset: int


def parse_segments(image: bytes) -> list[Segment]:
    if len(image) < ESP_IMAGE_HEADER_SIZE or image[0] != ESP_IMAGE_MAGIC:
        raise ValueError("not an ESP application image")

    segment_count = image[1]
    offset = ESP_IMAGE_HEADER_SIZE
    segments: list[Segment] = []
    for index in range(segment_count):
        if offset + SEGMENT_HEADER_SIZE > len(image):
            raise ValueError(f"segment {index + 1} header is truncated")
        load_address, length = struct.unpack_from("<II", image, offset)
        data_offset = offset + SEGMENT_HEADER_SIZE
        end = data_offset + length
        if end > len(image):
            raise ValueError(f"segment {index + 1} data is truncated")
        # esptool's image_info reports the segment-header offset. Flash-mapped
        # segment headers must land at 0x...0018 so their payload begins at the
        # 64 KiB boundary (0x...0020 includes the 8-byte segment header).
        segments.append(Segment(load_address, length, offset))
        offset = end
    return segments


def validate_p4_layout(segments: list[Segment]) -> Segment:
    for index, segment in enumerate(segments):
        if segment.load_address != P4_IROM_LOAD_ADDRESS:
            continue
        if index == 0:
            raise ValueError("IROM segment has no preceding alignment segment")
        padding = segments[index - 1]
        if padding.load_address != P4_PADDING_LOAD_ADDRESS:
            raise ValueError(
                "unexpected segment before IROM: "
                f"load=0x{padding.load_address:08x}"
            )
        if padding.length > MAX_SAFE_PADDING_BYTES:
            raise ValueError(
                "pathological pre-IROM RAM padding: "
                f"0x{padding.length:x} bytes (limit 0x{MAX_SAFE_PADDING_BYTES:x})"
            )
        if segment.file_offset % 0x10000 != 0x18:
            raise ValueError(
                f"IROM file offset is not mapped-image aligned: 0x{segment.file_offset:x}"
            )
        return padding
    raise ValueError("P4 IROM segment was not found")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("image", type=Path)
    args = parser.parse_args()

    try:
        segments = parse_segments(args.image.read_bytes())
        padding = validate_p4_layout(segments)
    except (OSError, ValueError) as error:
        print(f"P4_IMAGE_LAYOUT FAIL {error}")
        return 1

    print(
        "P4_IMAGE_LAYOUT PASS "
        f"segments={len(segments)} padding=0x{padding.length:x} "
        f"limit=0x{MAX_SAFE_PADDING_BYTES:x}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
