"""G4a oracle for the current MJPEG index and cached-frame source semantics.

This mirrors ``emotion_video_player.c`` without touching SD, PSRAM, the JPEG
decoder, FreeRTOS, or LVGL.  It intentionally preserves a few non-obvious
legacy behaviours so a later strangler extraction cannot change them by
accident: an empty scan is still a built index, a valid claim advances before
decode, and a bad index entry does not advance.
"""

from __future__ import annotations

from dataclasses import dataclass


SOI = b"\xff\xd8"
EOI = b"\xff\xd9"
MIN_FRAME_SIZE = 100
MAX_FRAME_SIZE = 2 * 1024 * 1024
MAX_FRAMES_PER_VIDEO = 300


@dataclass(frozen=True)
class FrameIndexEntry:
    offset: int
    size: int


@dataclass(frozen=True)
class FrameIndex:
    entries: tuple[FrameIndexEntry, ...]
    built: bool = True


@dataclass(frozen=True)
class FrameClaim:
    status: str
    frame_index: int | None = None
    entry: FrameIndexEntry | None = None


def build_frame_index(
    data: bytes,
    *,
    max_frames: int = MAX_FRAMES_PER_VIDEO,
) -> FrameIndex:
    """Scan SOI..EOI pairs exactly like the production cache index builder."""

    if not isinstance(data, bytes):
        raise TypeError("data must be bytes")
    if max_frames <= 0:
        raise ValueError("max_frames must be positive")

    entries: list[FrameIndexEntry] = []
    search_pos = 0
    while search_pos < len(data) and len(entries) < max_frames:
        start = data.find(SOI, search_pos)
        if start < 0:
            break
        end_marker = data.find(EOI, start + len(SOI))
        if end_marker < 0:
            break

        end = end_marker + len(EOI)
        frame_size = end - start
        if MIN_FRAME_SIZE <= frame_size <= MAX_FRAME_SIZE:
            entries.append(FrameIndexEntry(offset=start, size=frame_size))
        # Production skips the entire candidate even when its size is rejected.
        search_pos = end

    # Production sets index_built=true even when zero valid frames were found.
    return FrameIndex(entries=tuple(entries), built=True)


class CachedFrameSourceModel:
    """Model bounds checks, cursor ownership, EOF, and decode-at seek semantics."""

    def __init__(
        self,
        data: bytes,
        frame_index: FrameIndex,
        *,
        ready: bool = True,
    ) -> None:
        self.data = data
        self.frame_index = frame_index
        self.ready = ready
        self.cursor = 0

    def claim_next(self) -> FrameClaim:
        if not self.ready or not self.data:
            return FrameClaim("invalid_state")
        if not self.frame_index.built:
            return FrameClaim("invalid_state")
        if self.cursor >= len(self.frame_index.entries):
            return FrameClaim("not_found")

        frame_number = self.cursor
        entry = self.frame_index.entries[frame_number]
        if (
            entry.offset < 0
            or entry.size < 0
            or entry.offset >= len(self.data)
            or entry.offset + entry.size > len(self.data)
        ):
            # Production returns INVALID_SIZE before incrementing the cursor.
            return FrameClaim("invalid_size", frame_number, entry)

        # Production increments current_frame_index before allocation/copy/JPEG
        # decode, so a later decoder failure skips this frame.
        self.cursor += 1
        return FrameClaim("ok", frame_number, entry)

    def reset(self) -> None:
        self.cursor = 0

    def seek_and_claim(self, requested_index: int) -> FrameClaim:
        if requested_index < 0:
            raise ValueError("requested_index must be non-negative")
        frame_count = len(self.frame_index.entries)
        if not self.ready or not self.frame_index.built or frame_count == 0:
            return FrameClaim("invalid_state")
        # Mirrors decode_at_rgb565: each synchronous request overwrites the
        # previous cursor, wraps modulo frame count, then claims exactly once.
        self.cursor = requested_index % frame_count
        return self.claim_next()
