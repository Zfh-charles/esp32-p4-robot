"""Host contract for a PSRAM-free, abort-to-base standby life overlay."""

from dataclasses import dataclass


@dataclass(frozen=True)
class FlashBandCost:
    width: int
    rows: int
    frame_count: int

    def validate(self) -> None:
        if self.width <= 0 or not 1 <= self.rows <= 48:
            raise ValueError("overlay must be a positive, at-most-48-row band")
        if not 3 <= self.frame_count <= 8:
            raise ValueError("overlay choreography must contain 3..8 frames")

    @property
    def bytes_per_frame(self) -> int:
        return self.width * self.rows * 2

    @property
    def psram_read_bytes(self) -> int:
        return 0

    @property
    def psram_write_bytes(self) -> int:
        return 0


@dataclass
class IdleFlashOverlay:
    frame_count: int
    interval_ms: int
    rest_ms: int
    step: int = 0

    def tick(self, *, idle: bool, budget_ok: bool) -> tuple[str, int | None, int]:
        """Return (event, frame, next_delay_ms); final frame is canonical base."""
        if not idle:
            was_active = self.step > 0
            self.step = 0
            return ("abort_to_base" if was_active else "inactive", self.frame_count - 1 if was_active else None, self.rest_ms)
        if not budget_ok:
            was_active = self.step > 0
            self.step = 0
            return ("abort_to_base" if was_active else "drop", self.frame_count - 1 if was_active else None, self.rest_ms)
        frame = self.step
        self.step += 1
        if self.step == self.frame_count:
            self.step = 0
            return "done", frame, self.rest_ms
        return "show", frame, self.interval_ms
