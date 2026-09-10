from __future__ import annotations

import unittest

from .clock import FakeClock
from .visual_commit_model import (
    MAX_BURST_BYTES,
    MAX_ROWS,
    Budget,
    Intent,
    IntentKind,
    VisualCommitModel,
)


class VisualCommitModelTest(unittest.TestCase):
    def make_intent(
        self,
        sequence: int,
        *,
        kind: IntentKind = IntentKind.MOUTH,
        generation: int = 1,
        created_at_ms: int = 0,
        deadline_ms: int = 120,
        max_rows: int = MAX_ROWS,
        burst_bytes: int = MAX_BURST_BYTES,
    ) -> Intent:
        return Intent(
            kind=kind,
            sequence=sequence,
            generation=generation,
            created_at_ms=created_at_ms,
            deadline_ms=deadline_ms,
            max_rows=max_rows,
            burst_bytes=burst_bytes,
        )

    def test_latest_value_overwrites_without_replay(self):
        model = VisualCommitModel(FakeClock())
        model.set_generation(1)
        model.submit(self.make_intent(1))
        model.submit(self.make_intent(2))

        committed = model.evaluate(Budget.MOUTH_ONLY)
        self.assertEqual((committed.action, committed.sequence), ("commit", 2))
        self.assertEqual(model.evaluate(Budget.MOUTH_ONLY).action, "none")
        self.assertIn("superseded", [decision.reason for decision in model.history])

    def test_expired_candidate_is_consumed_once(self):
        clock = FakeClock()
        model = VisualCommitModel(clock)
        model.set_generation(1)
        model.submit(self.make_intent(3, deadline_ms=119))
        clock.advance_to(120)

        self.assertEqual(model.evaluate(Budget.MOUTH_ONLY).reason, "expired")
        self.assertEqual(model.evaluate(Budget.MOUTH_ONLY).reason, "empty")

    def test_busy_and_budget_denial_never_backfill(self):
        model = VisualCommitModel(FakeClock())
        model.set_generation(1)
        model.submit(self.make_intent(4, kind=IntentKind.IDLE_LIFE, deadline_ms=7000))
        self.assertEqual(
            model.evaluate(Budget.TRANSITION, resource_busy=True).reason,
            "resource_busy",
        )
        self.assertEqual(model.evaluate(Budget.TRANSITION).reason, "empty")

        model.submit(self.make_intent(5))
        self.assertEqual(model.evaluate(Budget.STATIC_ONLY).reason, "mouth_budget")
        self.assertEqual(model.evaluate(Budget.MOUTH_ONLY).reason, "empty")

    def test_generation_and_sequence_are_monotonic_guards(self):
        model = VisualCommitModel(FakeClock())
        model.set_generation(2)
        model.submit(self.make_intent(8, generation=1))
        self.assertEqual(model.evaluate(Budget.MOUTH_ONLY).reason, "stale_generation")

        model.submit(self.make_intent(9, generation=2))
        self.assertEqual(model.submit(self.make_intent(8, generation=2)).reason, "out_of_order")
        self.assertEqual(model.evaluate(Budget.MOUTH_REDUCED).sequence, 9)
        with self.assertRaises(ValueError):
            model.set_generation(1)

    def test_claim_caps_and_backend_mapping(self):
        model = VisualCommitModel(FakeClock())
        model.set_generation(1)
        model.submit(self.make_intent(10, max_rows=MAX_ROWS + 1))
        self.assertEqual(model.evaluate(Budget.MOUTH_ONLY).reason, "claim_cap")

        model.submit(
            self.make_intent(11, kind=IntentKind.IDLE_LIFE, deadline_ms=7000)
        )
        decision = model.evaluate(Budget.TRANSITION)
        self.assertEqual((decision.action, decision.backend), ("commit", "precomposed_band"))


if __name__ == "__main__":
    unittest.main()
