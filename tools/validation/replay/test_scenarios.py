from __future__ import annotations

import copy
import json
import unittest

from tools.validation.replay.core.runner import run_trace
from tools.validation.replay.run_scenarios import EXPECTED_SCENARIOS, default_paths, run_paths


class T2ScenarioSuiteTest(unittest.TestCase):
    def test_exact_six_scenarios_pass(self) -> None:
        report = run_paths(default_paths())
        self.assertEqual(set(EXPECTED_SCENARIOS), {item["scenario"] for item in report["results"]})
        self.assertEqual("PASS", report["overall_status"], report)
        self.assertEqual({"PASS": 6, "FAIL": 0}, report["counts"])

    def test_each_scenario_has_fault_and_oracles(self) -> None:
        for path in default_paths():
            trace = json.loads(path.read_text(encoding="utf-8"))
            with self.subTest(path=path.name):
                self.assertTrue(any(event["type"] == "Fault.Inject" for event in trace["events"]))
                self.assertTrue(trace["expected"]["m_plus"])
                self.assertTrue(trace["expected"]["m_minus"])
                self.assertTrue(trace["expected"]["invariants"])

    def test_replay_is_deterministic(self) -> None:
        for path in default_paths():
            trace = json.loads(path.read_text(encoding="utf-8"))
            with self.subTest(path=path.name):
                self.assertEqual(run_trace(trace).to_dict(), run_trace(trace).to_dict())

    def test_effect_mutation_is_killed(self) -> None:
        trace = json.loads((default_paths()[0]).read_text(encoding="utf-8"))
        mutated = copy.deepcopy(trace)
        mutated["expected"]["effects"].pop()
        result = run_trace(mutated)
        self.assertFalse(result.passed)
        self.assertTrue(any("effect sequence mismatch" in error for error in result.errors))


if __name__ == "__main__":
    unittest.main()
