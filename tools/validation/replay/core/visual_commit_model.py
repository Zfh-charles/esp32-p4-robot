"""Deterministic G3c queue semantics; this is a test oracle, not production code."""

from __future__ import annotations

from dataclasses import dataclass
from enum import Enum

from .clock import FakeClock


MAX_ROWS = 48
MAX_BURST_BYTES = 480 * MAX_ROWS * 2


class IntentKind(str, Enum):
    MOUTH = "mouth"
    IDLE_LIFE = "idle_life"


class Budget(str, Enum):
    FROZEN = "frozen"
    STATIC_ONLY = "static_only"
    MOUTH_REDUCED = "mouth_reduced"
    MOUTH_ONLY = "mouth_only"
    TRANSITION = "transition"


@dataclass(frozen=True)
class Intent:
    kind: IntentKind
    sequence: int
    generation: int
    created_at_ms: int
    deadline_ms: int
    max_rows: int = MAX_ROWS
    burst_bytes: int = MAX_BURST_BYTES


@dataclass(frozen=True)
class Decision:
    action: str
    sequence: int | None
    reason: str
    backend: str = "none"


class VisualCommitModel:
    """One-slot latest-value scheduler with consume-on-evaluate semantics.

    Every evaluation consumes the candidate, including denial. This makes busy,
    expired and under-budget frames impossible to replay later as stale motion.
    """

    def __init__(self, clock: FakeClock | None = None) -> None:
        self.clock = clock or FakeClock()
        self.generation = 0
        self.pending: Intent | None = None
        self.last_sequence = -1
        self.history: list[Decision] = []

    def _record(self, decision: Decision) -> Decision:
        self.history.append(decision)
        return decision

    def set_generation(self, generation: int) -> None:
        if generation < self.generation:
            raise ValueError("visual generation must be monotonic")
        self.generation = generation

    def submit(self, intent: Intent) -> Decision:
        if intent.sequence <= self.last_sequence:
            return self._record(Decision("drop", intent.sequence, "out_of_order"))
        self.last_sequence = intent.sequence
        if self.pending is not None:
            self._record(Decision("drop", self.pending.sequence, "superseded"))
        self.pending = intent
        return self._record(Decision("queued", intent.sequence, "latest"))

    def evaluate(self, budget: Budget, *, resource_busy: bool = False) -> Decision:
        intent = self.pending
        self.pending = None
        if intent is None:
            return self._record(Decision("none", None, "empty"))

        age_ms = self.clock.now_ms - intent.created_at_ms
        if age_ms < 0:
            return self._record(Decision("drop", intent.sequence, "future_timestamp"))
        if age_ms > intent.deadline_ms:
            return self._record(Decision("drop", intent.sequence, "expired"))
        if intent.generation != self.generation:
            return self._record(Decision("drop", intent.sequence, "stale_generation"))
        if intent.max_rows > MAX_ROWS or intent.burst_bytes > MAX_BURST_BYTES:
            return self._record(Decision("drop", intent.sequence, "claim_cap"))
        if resource_busy:
            return self._record(Decision("drop", intent.sequence, "resource_busy"))

        if intent.kind is IntentKind.MOUTH:
            if budget not in {Budget.MOUTH_REDUCED, Budget.MOUTH_ONLY}:
                return self._record(Decision("drop", intent.sequence, "mouth_budget"))
            return self._record(
                Decision("commit", intent.sequence, "admitted", "layered_band")
            )

        if budget is not Budget.TRANSITION:
            return self._record(Decision("drop", intent.sequence, "life_budget"))
        return self._record(
            Decision("commit", intent.sequence, "admitted", "precomposed_band")
        )
