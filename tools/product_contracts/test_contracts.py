from __future__ import annotations

import copy
import json
import sys
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT))

from validate_contracts import CANONICAL_EMOTIONS, validate_document, validate_path


class ProductContractTests(unittest.TestCase):
    def test_schema_files_are_json_with_unique_ids(self) -> None:
        ids: set[str] = set()
        for path in sorted((ROOT / "schemas").glob("*.json")):
            doc = json.loads(path.read_text(encoding="utf-8"))
            self.assertEqual(doc.get("$schema"), "https://json-schema.org/draft/2020-12/schema")
            schema_id = doc.get("$id")
            self.assertIsInstance(schema_id, str)
            self.assertNotIn(schema_id, ids)
            ids.add(schema_id)
        self.assertEqual(len(ids), 4)

    def test_all_valid_examples_pass(self) -> None:
        paths = sorted((ROOT / "examples" / "valid").glob("*.json"))
        self.assertGreaterEqual(len(paths), 10)
        for path in paths:
            with self.subTest(path=path.name):
                self.assertEqual(validate_path(path)["errors"], [])

    def test_all_invalid_examples_fail(self) -> None:
        paths = sorted((ROOT / "examples" / "invalid").glob("*.json"))
        self.assertGreaterEqual(len(paths), 4)
        for path in paths:
            with self.subTest(path=path.name):
                self.assertFalse(validate_path(path)["ok"])

    def test_structurally_different_no_mouth_ip_is_valid(self) -> None:
        path = ROOT / "examples" / "valid" / "character_robot_no_mouth.json"
        doc = json.loads(path.read_text(encoding="utf-8"))
        self.assertEqual(set(doc["canonical_emotions"]), CANONICAL_EMOTIONS)
        self.assertTrue(all(not item["features"]["mouth"] for item in doc["canonical_emotions"].values()))
        self.assertEqual(validate_document(doc), [])

    def test_missing_canonical_emotion_is_rejected(self) -> None:
        path = ROOT / "examples" / "valid" / "character_current_ip.json"
        doc = json.loads(path.read_text(encoding="utf-8"))
        broken = copy.deepcopy(doc)
        del broken["canonical_emotions"]["angry"]
        errors = validate_document(broken)
        self.assertTrue(any("exact six required" in error for error in errors))

    def test_runtime_alpha_and_fake_mouth_are_rejected(self) -> None:
        path = ROOT / "examples" / "valid" / "character_robot_no_mouth.json"
        doc = json.loads(path.read_text(encoding="utf-8"))
        broken = copy.deepcopy(doc)
        broken["canonical_emotions"]["standby"]["layers"].append({
            "name": "mouth_face",
            "composition": "runtime_alpha",
            "assets": [],
        })
        errors = validate_document(broken)
        self.assertTrue(any("runtime alpha" in error for error in errors))
        self.assertTrue(any("mouth=false" in error for error in errors))

    def test_priority_semantics_are_enforced(self) -> None:
        audio = json.loads((ROOT / "examples" / "valid" / "claim_audio_capture.json").read_text(encoding="utf-8"))
        audio["policy"]["droppable"] = True
        self.assertTrue(any("must not be droppable" in error for error in validate_document(audio)))

        visual = json.loads((ROOT / "examples" / "valid" / "claim_visual_life.json").read_text(encoding="utf-8"))
        visual["policy"]["droppable"] = False
        self.assertTrue(any("must be droppable" in error for error in validate_document(visual)))


if __name__ == "__main__":
    unittest.main()
