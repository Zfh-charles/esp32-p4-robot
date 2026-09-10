from __future__ import annotations

import copy
import unittest

from .clock import FakeClock
from .executor import FakeExecutor
from .runner import run_trace


def fixture(events, effects, positive_type="UI.ShowStandby"):
    return {
        "schema_version": "p4.trace.v1",
        "trace_id": "core-test",
        "source": {"fw_marker": "boot_trace_test", "environment": "replay"},
        "sanitized": True,
        "events": events,
        "expected": {
            "effects": effects,
            "m_plus": [{"id": "positive", "target": "effect", "type": positive_type, "min_count": 1}],
            "m_minus": [{"id": "no_abort", "target": "effect", "type": "Audio.Abort", "max_count": 0}],
            "invariants": [{"id": "positive_count", "kind": "count", "target": "effect", "type": positive_type, "min_count": 1}],
        },
    }


class ClockExecutorTest(unittest.TestCase):
    def test_clock_is_monotonic(self):
        clock = FakeClock(4)
        self.assertEqual(clock.advance_by(6), 10)
        with self.assertRaises(ValueError):
            clock.advance_to(9)

    def test_executor_stable_order_cancel_and_due(self):
        clock = FakeClock()
        executor = FakeExecutor(clock)
        observed = []
        executor.schedule(10, lambda: observed.append("first"))
        cancelled = executor.schedule(10, lambda: observed.append("cancelled"))
        executor.schedule(10, lambda: observed.append("third"))
        self.assertTrue(executor.cancel(cancelled))
        clock.advance_to(9)
        self.assertEqual(executor.run_due(), 0)
        clock.advance_to(10)
        self.assertEqual(executor.run_due(), 2)
        self.assertEqual(observed, ["first", "third"])


class ReplayTest(unittest.TestCase):
    def test_exit_timer_retains_original_cause(self):
        events = [
            {"seq": 0, "t_ms": 0, "type": "Boot.Ready"},
            {"seq": 1, "t_ms": 10, "type": "Wake.Detected"},
            {"seq": 2, "t_ms": 20, "type": "Session.ExitRequested"},
        ]
        types_and_causes = [
            ("UI.ShowStandby", 0), ("Wake.Enable", 0),
            ("Wake.Disable", 1), ("Session.Begin", 1), ("Audio.OpenUplink", 1), ("UI.ShowListening", 1),
            ("Session.CloseRequest", 2), ("Session.Closed", 2), ("Wake.Enable", 2), ("UI.ShowStandby", 2),
        ]
        effects = [{"seq": i, "type": name, "caused_by_event_seq": cause}
                   for i, (name, cause) in enumerate(types_and_causes, start=1)]
        trace = fixture(events, effects)
        trace["expected"]["invariants"] = [{"id": "exit_order", "kind": "effect_order", "before_type": "Session.CloseRequest", "after_type": "Session.Closed"}]
        self.assertTrue(run_trace(trace).passed)

    def test_fault_duplicate_drop_and_delay(self):
        for action, delay, count in (("duplicate_next", 0, 1), ("drop_next", 0, 0), ("delay_next", 25, 1)):
            with self.subTest(action=action):
                events = [
                    {"seq": 0, "t_ms": 0, "type": "Boot.Ready"},
                    {"seq": 1, "t_ms": 1, "type": "Wake.Detected"},
                    {"seq": 2, "t_ms": 2, "type": "TTS.Start"},
                    {"seq": 3, "t_ms": 3, "type": "Fault.Inject", "payload": {"action": action, "target_type": "TTS.Chunk", "delay_ms": delay}},
                    {"seq": 4, "t_ms": 4, "type": "TTS.Chunk", "payload": {"chunk": "a"}},
                ]
                effects = [
                    {"seq": 1, "type": "UI.ShowStandby", "caused_by_event_seq": 0},
                    {"seq": 2, "type": "Wake.Enable", "caused_by_event_seq": 0},
                    {"seq": 3, "type": "Wake.Disable", "caused_by_event_seq": 1},
                    {"seq": 4, "type": "Session.Begin", "caused_by_event_seq": 1},
                    {"seq": 5, "type": "Audio.OpenUplink", "caused_by_event_seq": 1},
                    {"seq": 6, "type": "UI.ShowListening", "caused_by_event_seq": 1},
                    {"seq": 7, "type": "Audio.StartPlayback", "caused_by_event_seq": 2, "payload": {"generation": 1}},
                    {"seq": 8, "type": "Visual.SpeechBegin", "caused_by_event_seq": 2, "payload": {"generation": 1}},
                ]
                if count:
                    effects.append({"seq": 9, "type": "Audio.QueueChunk", "caused_by_event_seq": 4,
                                    "payload": {"chunk": "a", "generation": 1}})
                trace = fixture(events, effects, positive_type="Fault.Inject" if count == 0 else "Audio.QueueChunk")
                if count == 0:
                    trace["expected"]["m_plus"] = [{"id": "fault_seen", "target": "event", "type": "Fault.Inject", "min_count": 1}]
                    trace["expected"]["invariants"] = [{"id": "chunk_absent", "kind": "count", "target": "effect", "type": "Audio.QueueChunk", "min_count": 0, "max_count": 0}]
                result = run_trace(trace)
                self.assertTrue(result.passed, result.errors)

    def test_deterministic_result(self):
        trace = fixture(
            [{"seq": 0, "t_ms": 0, "type": "Boot.Ready"}],
            [{"seq": 1, "type": "UI.ShowStandby", "caused_by_event_seq": 0}, {"seq": 2, "type": "Wake.Enable", "caused_by_event_seq": 0}],
        )
        self.assertEqual(run_trace(copy.deepcopy(trace)).to_dict(), run_trace(copy.deepcopy(trace)).to_dict())

    def test_effect_and_oracle_mismatch_fail(self):
        trace = fixture(
            [{"seq": 0, "t_ms": 0, "type": "Boot.Ready"}],
            [{"seq": 1, "type": "UI.Wrong", "caused_by_event_seq": 0}],
        )
        trace["expected"]["m_plus"] = [{"id": "must_abort", "target": "effect", "type": "Audio.Abort", "min_count": 1}]
        result = run_trace(trace)
        self.assertFalse(result.passed)
        self.assertTrue(any("effect sequence mismatch" in error for error in result.errors))
        self.assertTrue(any("must_abort" in error for error in result.errors))


if __name__ == "__main__":
    unittest.main()
