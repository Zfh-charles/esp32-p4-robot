"""Stable, cancellable executor driven by :class:`FakeClock`."""

from __future__ import annotations

import heapq
from collections.abc import Callable
from dataclasses import dataclass, field

from .clock import FakeClock


@dataclass(order=True)
class _Task:
    due_ms: int
    order: int
    handle: int = field(compare=False)
    callback: Callable[[], None] = field(compare=False)


class FakeExecutor:
    """Executes equal-deadline work in schedule order, never wall-clock time."""

    def __init__(self, clock: FakeClock) -> None:
        self.clock = clock
        self._queue: list[_Task] = []
        self._next_handle = 1
        self._order = 0
        self._cancelled: set[int] = set()

    @property
    def pending_count(self) -> int:
        return sum(task.handle not in self._cancelled for task in self._queue)

    def schedule(self, delay_ms: int, callback: Callable[[], None]) -> int:
        if delay_ms < 0:
            raise ValueError("delay_ms must be non-negative")
        if not callable(callback):
            raise TypeError("callback must be callable")
        handle = self._next_handle
        self._next_handle += 1
        heapq.heappush(self._queue, _Task(self.clock.now_ms + delay_ms, self._order, handle, callback))
        self._order += 1
        return handle

    def cancel(self, handle: int) -> bool:
        if handle <= 0 or handle in self._cancelled:
            return False
        if not any(task.handle == handle for task in self._queue):
            return False
        self._cancelled.add(handle)
        return True

    def run_due(self, max_tasks: int = 10_000) -> int:
        ran = 0
        while self._queue and self._queue[0].due_ms <= self.clock.now_ms:
            if ran >= max_tasks:
                raise RuntimeError("FakeExecutor max_tasks exceeded")
            task = heapq.heappop(self._queue)
            if task.handle in self._cancelled:
                self._cancelled.remove(task.handle)
                continue
            task.callback()
            ran += 1
        return ran

    def run_until_idle(self, max_tasks: int = 10_000) -> int:
        ran = 0
        while self._queue:
            while self._queue and self._queue[0].handle in self._cancelled:
                task = heapq.heappop(self._queue)
                self._cancelled.remove(task.handle)
            if not self._queue:
                break
            self.clock.advance_to(self._queue[0].due_ms)
            ran += self.run_due(max_tasks=max_tasks - ran)
            if ran >= max_tasks and self.pending_count:
                raise RuntimeError("FakeExecutor max_tasks exceeded")
        return ran
