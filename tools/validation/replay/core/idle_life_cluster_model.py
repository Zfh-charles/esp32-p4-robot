"""Host model for the fail-closed standby life-cluster scheduler."""

from dataclasses import dataclass


@dataclass
class IdleLifeCluster:
    keyframes: tuple[int, ...]
    interval_ms: int
    rest_min_ms: int
    rest_max_ms: int
    step: int = 0
    repair_pending: bool = False
    repair_attempts: int = 0
    generation: int = 0

    def target(self) -> int:
        return self.keyframes[-1] if self.repair_pending else self.keyframes[self.step]

    def _rest(self) -> int:
        self.generation += 1
        span = self.rest_max_ms - self.rest_min_ms + 1
        return self.rest_min_ms + (self.generation * 1103) % span

    def tick(self, presented: bool) -> tuple[str, int]:
        if presented:
            self.repair_attempts = 0
            if self.repair_pending:
                self.repair_pending = False
                self.step = 0
                return "repair_done", self._rest()
            self.step += 1
            if self.step == len(self.keyframes):
                self.step = 0
                return "done", self._rest()
            return "continue", self.interval_ms

        self.repair_pending = self.repair_pending or self.step > 0
        self.step = 0
        if self.repair_pending and self.repair_attempts < 2:
            self.repair_attempts += 1
            return "abort_repair", self.interval_ms
        return "drop", self._rest()
