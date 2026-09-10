#!/usr/bin/env python3

import tempfile
import unittest
from pathlib import Path

import run_gate


class ValidationGateTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.repo = Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    @staticmethod
    def check(check_id, source, *, timeout=15, when_path=None):
        check = {
            "id": check_id,
            "label": check_id,
            "tier": "T0",
            "scopes": ["fast"],
            "timeout_seconds": timeout,
            "required": True,
            "command": ["{python}", "-c", source],
        }
        if when_path is not None:
            check["when_path"] = when_path
            check["optional_when_missing"] = True
        return check

    @staticmethod
    def manifest(*checks):
        return {
            "schema_version": 1,
            "manifest_version": "test-v1",
            "checks": list(checks),
        }

    def test_pass_and_optional_missing_path_skip(self):
        report = run_gate.execute_manifest(
            self.manifest(
                self.check("pass", "print('ok')"),
                self.check("future", "raise SystemExit(99)", when_path="future/contracts"),
            ),
            "fast",
            self.repo,
        )
        self.assertEqual(report["overall_status"], "PASS")
        self.assertEqual([item["status"] for item in report["results"]], ["PASS", "SKIP"])

    def test_failure_does_not_stop_following_checks(self):
        report = run_gate.execute_manifest(
            self.manifest(
                self.check("fail", "raise SystemExit(7)"),
                self.check("still_runs", "print('after failure')"),
            ),
            "fast",
            self.repo,
        )
        self.assertEqual(report["overall_status"], "FAIL")
        self.assertEqual([item["status"] for item in report["results"]], ["FAIL", "PASS"])
        self.assertEqual(report["failed_required"], ["fail"])

    def test_timeout_is_a_required_failure(self):
        report = run_gate.execute_manifest(
            self.manifest(self.check("slow", "import time; time.sleep(2)", timeout=0.05)),
            "fast",
            self.repo,
        )
        self.assertEqual(report["results"][0]["status"], "TIMEOUT")
        self.assertEqual(report["overall_status"], "FAIL")

    def test_report_is_atomic_valid_json(self):
        report = run_gate.execute_manifest(
            self.manifest(self.check("pass", "print('ok')")), "fast", self.repo
        )
        output = self.repo / "report.json"
        run_gate.write_report(output, report)
        loaded = run_gate.read_json(output)
        self.assertEqual(loaded["schema_version"], 1)
        self.assertEqual(loaded["overall_status"], "PASS")
        self.assertFalse(output.with_name("report.json.tmp").exists())


if __name__ == "__main__":
    unittest.main()
