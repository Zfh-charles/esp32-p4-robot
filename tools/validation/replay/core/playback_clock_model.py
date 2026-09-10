"""G4 extraction oracle for existing MJPEG clock/cursor semantics.

This deterministic model deliberately mirrors the s1gd control flow. It does
not model JPEG hardware, SD, PSRAM, FreeRTOS scheduling or display latency.
"""

from __future__ import annotations

from dataclasses import dataclass


WARMUP_US = 300_000
SOFT_START_US = 2_000_000
SOFT_START_MIN_INTERVAL_US = 200_000


@dataclass
class PlaybackClockModel:
    normal_interval_us: int
    paused: bool = True
    frame_interval_us: int = 0
    warmup_until_us: int = 0
    soft_start_until_us: int = 0
    deadline_us: int = 0

    def __post_init__(self) -> None:
        if self.normal_interval_us <= 0:
            raise ValueError("normal interval must be positive")
        self.frame_interval_us = self.normal_interval_us

    def pause(self) -> None:
        self.paused = True

    def resume(self, now_us: int, normal_interval_us: int | None = None) -> None:
        if normal_interval_us is not None:
            if normal_interval_us <= 0:
                raise ValueError("normal interval must be positive")
            self.normal_interval_us = normal_interval_us
        self.warmup_until_us = now_us + WARMUP_US
        self.soft_start_until_us = now_us + SOFT_START_US
        self.deadline_us = self.warmup_until_us
        self.frame_interval_us = max(
            self.normal_interval_us, SOFT_START_MIN_INTERVAL_US
        )
        self.paused = False

    def poll(self, now_us: int) -> str:
        if self.paused:
            return "paused"
        if now_us < self.warmup_until_us:
            return "warmup"
        if now_us >= self.soft_start_until_us:
            self.frame_interval_us = self.normal_interval_us
        return "decode" if now_us >= self.deadline_us else "wait"

    def decode_succeeded(self, now_us: int) -> None:
        if self.paused:
            raise RuntimeError("decode success while paused")
        # Existing player intentionally reanchors instead of catching up.
        self.deadline_us = now_us + self.frame_interval_us

    def stream_ended(self, now_us: int) -> None:
        # Existing loop resets the cursor and makes frame zero immediately due.
        self.deadline_us = now_us


@dataclass
class FrameCursorModel:
    frame_count: int
    index: int = 0

    def __post_init__(self) -> None:
        if self.frame_count < 0:
            raise ValueError("frame count must be non-negative")

    def claim_next(self) -> int | None:
        if self.index >= self.frame_count:
            return None
        claimed = self.index
        # Mirrors current_frame_index++ before hardware decode. A failed decode
        # therefore skips this frame; changing that needs a separate marker.
        self.index += 1
        return claimed

    def reset(self) -> None:
        self.index = 0
