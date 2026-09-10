#!/usr/bin/env python3

import json
import tempfile
import unittest
from pathlib import Path

import architecture_guard


class ArchitectureGuardTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        (self.root / "main").mkdir()
        self._write("main/application.cc", '#include "boards/p4/legacy.h"\n')
        self._write("main/application.h", "")
        self._write("main/hot_a.cc", "a\n")
        self.baseline = {
            "schema_version": 1,
            "default_new_file_max_lines": 3,
            "legacy_file_line_limits": {"main/application.cc": 1},
            "hotspot_files": ["main/hot_a.cc"],
            "hotspot_total_max_lines": 1,
            "board_conditional_limits": {
                "main/application.cc": 0,
                "main/application.h": 0,
            },
            "board_conditional_token": "CONFIG_BOARD_P4",
            "allowed_application_board_includes": ["boards/p4/legacy.h"],
            "clean_layer_roots": ["main/domain/", "main/use_cases/"],
            "clean_layer_max_lines": 3,
            "clean_layer_forbidden_include_fragments": ["freertos/", "boards/"],
        }

    def tearDown(self):
        self.temp.cleanup()

    def _write(self, rel: str, text: str) -> None:
        path = self.root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")

    def _rules(self):
        return {finding.rule for finding in architecture_guard.check(self.root, self.baseline).findings}

    def test_current_debt_is_accepted(self):
        self.assertTrue(architecture_guard.check(self.root, self.baseline).passed)

    def test_file_growth_is_rejected(self):
        self._write("main/application.cc", "a\nb\n")
        self.assertIn("file-size-ratchet", self._rules())

    def test_new_board_dependency_is_rejected(self):
        self._write(
            "main/application.cc",
            '#include "boards/p4/legacy.h"\n#include "boards/p4/new_driver.h"\n',
        )
        rules = self._rules()
        self.assertIn("core-board-dependency-ratchet", rules)

    def test_framework_leak_into_domain_is_rejected(self):
        self._write("main/domain/policy.h", "#include <freertos/FreeRTOS.h>\n")
        self.assertIn("clean-layer-dependency", self._rules())

    def test_hotspot_total_cannot_grow(self):
        self._write("main/hot_a.cc", "a\nb\n")
        self.assertIn("hotspot-total-ratchet", self._rules())


if __name__ == "__main__":
    unittest.main()

