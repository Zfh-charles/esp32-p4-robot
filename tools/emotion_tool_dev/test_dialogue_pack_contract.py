from __future__ import annotations

import hashlib
import json
import tempfile
import unittest
from pathlib import Path

from dialogue_pack_contract import EMOTIONS, MOUTH_LEVELS, validate_pack


class DialoguePackContractTest(unittest.TestCase):
    def make_pack(self, root: Path) -> None:
        clips = {}
        stream = b"\xff\xd8test\xff\xd9"
        hold = bytes(480 * 480 * 2)
        for emotion in EMOTIONS:
            folder = root / emotion
            (folder / "mouth").mkdir(parents=True)
            (folder / "frames.mjpeg").write_bytes(stream)
            (folder / "hold_base.rgb565").write_bytes(hold)
            (folder / "hold_base.jpg").write_bytes(b"preview")
            levels = {}
            for level in MOUTH_LEVELS:
                (folder / "mouth" / f"{level}.rgb565").write_bytes(bytes(8 * 8 * 2))
                (folder / "mouth" / f"{level}.jpg").write_bytes(b"preview")
                levels[level] = {"frame": 0, "file": f"mouth/{level}.jpg",
                                 "rgb565": f"mouth/{level}.rgb565"}
            manifest = {
                "schema": "dialogue-emotion-clip-v2",
                "emotion": emotion,
                "source": {"file": "frames.mjpeg", "bytes": len(stream),
                           "sha256": hashlib.sha256(stream).hexdigest(),
                           "width": 480, "height": 480, "frame_count": 1},
                "render": {"mode": "layered", "fallback": "full_frame_clip",
                           "mouth_roi": {"x": 200, "y": 220, "width": 8, "height": 8},
                           "hold_base": {"file": "hold_base.jpg", "rgb565": "hold_base.rgb565",
                                         "width": 480, "height": 480},
                           "mouth_levels": levels,
                           "pose_bank": {"count": 1, "poses": [{"id": 0, "root": "."}]},
                           "life_layer": None},
                "frames": [{"frame": 0, "offset": 0, "size": len(stream),
                            "duration_ms": 160, "sha256": hashlib.sha256(stream).hexdigest()}],
            }
            (folder / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
            clips[emotion] = f"{emotion}/manifest.json"
        pack = {
            "schema": "dialogue-emotion-pack-v2", "version": 2,
            "runtime_contract": {
                "screen": {"width": 480, "height": 480, "pixel_format": "rgb565-le"},
                "max_rows_per_tick": 48, "max_layers_per_tick": 1,
            },
            "capabilities": ["full_frame_clip", "fixed_roi_clip", "layered", "static"],
            "legacy_fallback": {"enabled": True, "root": "/sdcard/mjpeg",
                                "files": [f"{e}.mjpeg" for e in EMOTIONS]},
            "clips": clips,
        }
        (root / "pack_manifest.json").write_text(json.dumps(pack), encoding="utf-8")

    def test_valid_minimal_pack(self) -> None:
        with tempfile.TemporaryDirectory() as value:
            root = Path(value)
            self.make_pack(root)
            report = validate_pack(root)
            self.assertTrue(report["ok"], report["errors"])

    def test_rejects_path_escape(self) -> None:
        with tempfile.TemporaryDirectory() as value:
            root = Path(value)
            self.make_pack(root)
            pack = json.loads((root / "pack_manifest.json").read_text())
            pack["clips"]["happy"] = "../outside.json"
            (root / "pack_manifest.json").write_text(json.dumps(pack))
            report = validate_pack(root)
            self.assertFalse(report["ok"])
            self.assertTrue(any("escapes pack root" in e for e in report["errors"]))

    def test_rejects_out_of_bounds_roi(self) -> None:
        with tempfile.TemporaryDirectory() as value:
            root = Path(value)
            self.make_pack(root)
            path = root / "happy" / "manifest.json"
            manifest = json.loads(path.read_text())
            manifest["render"]["mouth_roi"]["y"] = 479
            path.write_text(json.dumps(manifest))
            report = validate_pack(root)
            self.assertFalse(report["ok"])
            self.assertTrue(any("ROI outside" in e for e in report["errors"]))

    def test_rejects_wrong_patch_size(self) -> None:
        with tempfile.TemporaryDirectory() as value:
            root = Path(value)
            self.make_pack(root)
            (root / "happy" / "mouth" / "large.rgb565").write_bytes(b"bad")
            report = validate_pack(root)
            self.assertFalse(report["ok"])
            self.assertTrue(any("RGB565 size" in e for e in report["errors"]))

    def add_closed_life_choreography(self, root: Path, *, break_closure: bool = False) -> None:
        folder = root / "standby"
        path = folder / "manifest.json"
        manifest = json.loads(path.read_text())
        life_dir = folder / "life" / "track_1"
        life_dir.mkdir(parents=True)
        frames = []
        values = (0, 32, 96, 0 if not break_closure else 7)
        for index, value in enumerate(values):
            rgb = bytes([value]) * (8 * 8 * 2)
            mask = bytes([0 if index in (0, 3) else 128]) * (8 * 8)
            rgb_path = life_dir / f"{index:02d}.rgb565"
            mask_path = life_dir / f"{index:02d}.a8"
            rgb_path.write_bytes(rgb)
            mask_path.write_bytes(mask)
            frames.append({
                "sequence": index,
                "rgb565": f"life/track_1/{index:02d}.rgb565",
                "rgb565_sha256": hashlib.sha256(rgb).hexdigest(),
                "mask_a8": f"life/track_1/{index:02d}.a8",
                "mask_sha256": hashlib.sha256(mask).hexdigest(),
                "peak_alpha": 0 if index in (0, 3) else 128,
                "active_ratio": 0.0 if index in (0, 3) else 1.0,
            })
        hold_hash = hashlib.sha256((folder / "hold_base.rgb565").read_bytes()).hexdigest()
        manifest["render"]["life_layer"] = {
            "base_rgb565_sha256": hold_hash,
            "max_tracks_per_tick": 1,
            "schedule": "interleave_latest_drop_old",
            "tracks": [{
                "id": 1,
                "approval": "profile",
                "semantic": "body_breathe",
                "roi": {"x": 200, "y": 360, "width": 8, "height": 8},
                "frames": frames,
                "choreography": {
                    "schema": "idle-life-cluster-v1",
                    "keyframes": [1, 2, 3],
                    "frame_interval_ms": 240,
                    "rest_ms": {"min": 4200, "max": 6900},
                    "busy_policy": "abort_to_base_no_replay",
                    "returns_to_base": True,
                },
            }],
        }
        path.write_text(json.dumps(manifest))

    def test_accepts_profile_approved_closed_life_choreography(self) -> None:
        with tempfile.TemporaryDirectory() as value:
            root = Path(value)
            self.make_pack(root)
            self.add_closed_life_choreography(root)
            report = validate_pack(root)
            self.assertTrue(report["ok"], report["errors"])
            standby = next(item for item in report["clips"] if item["emotion"] == "standby")
            self.assertEqual(standby["life_choreographies"], 1)

    def test_rejects_life_choreography_that_does_not_return_to_base(self) -> None:
        with tempfile.TemporaryDirectory() as value:
            root = Path(value)
            self.make_pack(root)
            self.add_closed_life_choreography(root, break_closure=True)
            report = validate_pack(root)
            self.assertFalse(report["ok"])
            self.assertTrue(any("close exactly on canonical base" in e for e in report["errors"]))


if __name__ == "__main__":
    unittest.main()
