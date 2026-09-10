#!/usr/bin/env python3
from __future__ import annotations

import copy
import json
import unittest
from pathlib import Path

from validate_trace import validate_trace

HERE = Path(__file__).resolve().parent
FIXTURES = HERE / "fixtures"


def load(name: str) -> dict:
    return json.loads((FIXTURES / name).read_text(encoding="utf-8"))


class TraceContractTest(unittest.TestCase):
    def test_valid_fixtures(self) -> None:
        for name in ("valid_long_tts.json", "valid_exit_second_wake.json"):
            with self.subTest(name=name):
                self.assertEqual([], validate_trace(load(name)))

    def test_non_monotonic_fixture_is_rejected(self) -> None:
        joined = "\n".join(validate_trace(load("invalid_non_monotonic.json")))
        self.assertIn("strictly increasing", joined)
        self.assertIn("monotonic non-decreasing", joined)

    def test_privacy_cause_and_oracles_are_rejected(self) -> None:
        joined = "\n".join(validate_trace(load("invalid_privacy_and_cause.json")))
        self.assertIn("sanitized", joined)
        self.assertIn("must start with 'boot_trace_'", joined)
        self.assertIn("references missing event seq 99", joined)
        self.assertIn("M+ expected", joined)
        self.assertIn("M- expected", joined)

    def test_unknown_schema_mutation_is_rejected(self) -> None:
        trace = copy.deepcopy(load("valid_long_tts.json"))
        trace["schema_version"] = "p4.trace.v2"
        self.assertTrue(any("schema_version" in error for error in validate_trace(trace)))

    def test_duplicate_oracle_id_mutation_is_rejected(self) -> None:
        trace = copy.deepcopy(load("valid_long_tts.json"))
        trace["expected"]["m_minus"][0]["id"] = trace["expected"]["m_plus"][0]["id"]
        self.assertTrue(any("duplicate oracle id" in error for error in validate_trace(trace)))


if __name__ == "__main__":
    unittest.main()
