"""A monotonic clock controlled by a replay test."""

from __future__ import annotations


class FakeClock:
    def __init__(self, start_ms: int = 0) -> None:
        if start_ms < 0:
            raise ValueError("start_ms must be non-negative")
        self._now_ms = start_ms

    @property
    def now_ms(self) -> int:
        return self._now_ms

    def advance_to(self, target_ms: int) -> int:
        if target_ms < self._now_ms:
            raise ValueError("FakeClock cannot move backwards")
        self._now_ms = target_ms
        return self._now_ms

    def advance_by(self, delta_ms: int) -> int:
        if delta_ms < 0:
            raise ValueError("delta_ms must be non-negative")
        return self.advance_to(self._now_ms + delta_ms)
