import json
import tempfile
import unittest
from pathlib import Path

from tools.validation.replay.speech_core_bank_budget import (
    CANONICAL_EMOTIONS,
    build_report,
)


class SpeechCoreBankBudgetTest(unittest.TestCase):
    def _make_pack(self, root: Path, *, truncate_large: bool = False) -> None:
        for emotion in CANONICAL_EMOTIONS:
            emotion_dir = root / emotion
            (emotion_dir / "mouth").mkdir(parents=True)
            manifest = {
                "render": {
                    "mode": "layered",
                    "mouth_roi": {"width": 4, "height": 2},
                    "hold_base": {
                        "rgb565": "hold_base.rgb565",
                        "width": 8,
                        "height": 6,
                    },
                    "mouth_levels": {
                        level: {"rgb565": f"mouth/{level}.rgb565"}
                        for level in ("closed", "small", "medium", "large")
                    },
                }
            }
            (emotion_dir / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
            (emotion_dir / "hold_base.rgb565").write_bytes(bytes(8 * 6 * 2))
            for level in ("closed", "small", "medium", "large"):
                size = 4 * 2 * 2
                if truncate_large and emotion == "happy" and level == "large":
                    size -= 2
                (emotion_dir / "mouth" / f"{level}.rgb565").write_bytes(bytes(size))

    def test_reports_exact_six_emotion_core_total(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            self._make_pack(root)
            report = build_report(root)
            self.assertTrue(report["valid"])
            self.assertEqual(report["total_bytes"], 6 * (8 * 6 * 2 + 4 * 4 * 2 * 2))
            self.assertEqual(len(report["emotions"]), 6)

    def test_rejects_truncated_asset(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            self._make_pack(root, truncate_large=True)
            report = build_report(root)
            self.assertFalse(report["valid"])
            happy = next(item for item in report["emotions"] if item["emotion"] == "happy")
            self.assertIn("mouth.large size", " ".join(happy["errors"]))


if __name__ == "__main__":
    unittest.main()
