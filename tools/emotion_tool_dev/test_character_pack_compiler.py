from __future__ import annotations

import copy
import hashlib
import json
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT))

from character_pack_compiler import CANONICAL_EMOTIONS, compile_character_pack, validate_profile, validate_v3


class CharacterPackCompilerTest(unittest.TestCase):
    def make_pack(self, root: Path, with_layers: bool, width: int = 480, height: int = 480) -> dict[str, str]:
        clips = {}
        before = {}
        for index, emotion in enumerate(CANONICAL_EMOTIONS):
            folder = root / emotion
            folder.mkdir(parents=True)
            stream = b"\xff\xd8" + emotion.encode("ascii") + b"\xff\xd9"
            (folder / "frames.mjpeg").write_bytes(stream)
            before[emotion] = hashlib.sha256(stream).hexdigest()
            base = bytes([index + 1]) * 32
            (folder / "hold_base.rgb565").write_bytes(base)
            render = {"hold_base": {"rgb565": "hold_base.rgb565"}}
            if with_layers:
                (folder / "mouth").mkdir()
                mouth = {}
                for level in ("closed", "small", "medium", "large"):
                    path = folder / "mouth" / f"{level}.rgb565"
                    path.write_bytes((emotion + level).encode("ascii"))
                    mouth[level] = {"rgb565": f"mouth/{level}.rgb565"}
                render["mouth_levels"] = mouth
                (folder / "eye").mkdir()
                eyes = {}
                for level in ("open", "half", "closed"):
                    path = folder / "eye" / f"{level}.rgb565"
                    path.write_bytes((emotion + level + "eye").encode("ascii"))
                    eyes[level] = {"rgb565": f"eye/{level}.rgb565"}
                render["eye_levels"] = eyes
                life_dir = folder / "life" / "track_1"
                life_dir.mkdir(parents=True)
                life_path = life_dir / "00.rgb565"
                life_path.write_bytes(b"life")
                render["life_layer"] = {"tracks": [{
                    "id": 1, "approval": "profile", "semantic": "body_breathe",
                    "frames": [{"sequence": 0, "rgb565": "life/track_1/00.rgb565"}],
                }]}
                for source_key in ("enter_layer", "release_layer"):
                    transition_dir = folder / source_key
                    transition_dir.mkdir()
                    transition_path = transition_dir / "00.rgb565"
                    transition_path.write_bytes((emotion + source_key).encode("ascii"))
                    render[source_key] = {"frames": [{"rgb565": f"{source_key}/00.rgb565"}]}
            manifest = {
                "schema": "dialogue-emotion-clip-v2", "emotion": emotion,
                "source": {"file": "frames.mjpeg", "sha256": before[emotion], "width": width, "height": height},
                "render": render,
                "analysis": {"risk": {"layered_safe": True, "composite_seam_score": 0.5}},
            }
            (folder / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
            clips[emotion] = f"{emotion}/manifest.json"
        (root / "pack_manifest.json").write_text(json.dumps({
            "schema": "dialogue-emotion-pack-v2", "version": 2, "clips": clips,
        }), encoding="utf-8")
        return before

    def load_profile(self, name: str) -> dict:
        return json.loads((ROOT / "profiles" / name).read_text(encoding="utf-8"))

    def test_current_ip_compiles_without_touching_mjpeg(self) -> None:
        with tempfile.TemporaryDirectory() as value:
            root = Path(value)
            before = self.make_pack(root, with_layers=True)
            pack_manifest_before = hashlib.sha256((root / "pack_manifest.json").read_bytes()).hexdigest()
            document, report = compile_character_pack(root, self.load_profile("current_ip_v1.json"))
            self.assertEqual(validate_v3(document), [])
            self.assertTrue(report["source_mjpeg_unchanged"])
            after = {emotion: hashlib.sha256((root / emotion / "frames.mjpeg").read_bytes()).hexdigest() for emotion in CANONICAL_EMOTIONS}
            self.assertEqual(before, after)
            self.assertEqual(pack_manifest_before, hashlib.sha256((root / "pack_manifest.json").read_bytes()).hexdigest())
            self.assertIn("mouth_face", [layer["name"] for layer in document["canonical_emotions"]["happy"]["layers"]])
            self.assertEqual(document["canonical_emotions"]["angry"]["layers"], [])
            happy_report = next(item for item in report["canonical_coverage"] if item["canonical"] == "happy")
            self.assertIn("seam_score", happy_report)
            self.assertIn("global_frame_delta", happy_report)
            self.assertGreater(report["resource_summary"]["referenced_asset_bytes"], 0)

    def test_optional_eye_and_transition_layers_are_profile_gated(self) -> None:
        with tempfile.TemporaryDirectory() as value:
            root = Path(value)
            self.make_pack(root, with_layers=True)
            profile = self.load_profile("current_ip_v1.json")
            neutral = profile["canonical_emotions"]["neutral"]
            neutral["features"]["transition"] = True
            neutral["approved_layers"] = ["mouth_face", "eyes_brows", "enter", "exit"]
            document, _ = compile_character_pack(root, profile)
            names = [layer["name"] for layer in document["canonical_emotions"]["neutral"]["layers"]]
            self.assertEqual(names, ["mouth_face", "eyes_brows", "enter", "exit"])
            self.assertEqual(validate_v3(document), [])

    def test_no_mouth_ip_does_not_consume_roi_or_layer_assets(self) -> None:
        with tempfile.TemporaryDirectory() as value:
            root = Path(value)
            self.make_pack(root, with_layers=False, width=320, height=320)
            document, report = compile_character_pack(root, self.load_profile("robot_no_mouth_v1.json"))
            self.assertEqual(validate_v3(document), [])
            self.assertTrue(report["source_mjpeg_unchanged"])
            for item in document["canonical_emotions"].values():
                self.assertFalse(item["features"]["mouth"])
                self.assertEqual(item["layers"], [])

    def test_profile_rejects_fake_mouth(self) -> None:
        profile = self.load_profile("robot_no_mouth_v1.json")
        broken = copy.deepcopy(profile)
        broken["canonical_emotions"]["standby"]["approved_layers"] = ["mouth_face"]
        with self.assertRaisesRegex(ValueError, "mouth_face approved while mouth=false"):
            validate_profile(broken)

    def test_profile_requires_exact_six(self) -> None:
        profile = self.load_profile("current_ip_v1.json")
        del profile["canonical_emotions"]["angry"]
        with self.assertRaisesRegex(ValueError, "exactly six"):
            validate_profile(profile)


if __name__ == "__main__":
    unittest.main()
