from __future__ import annotations

import hashlib
import json
import tempfile
import unittest
from pathlib import Path

from idle_flash_band_builder import BuildError, build_idle_flash_bands


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


class IdleFlashBandBuilderTest(unittest.TestCase):
    def make_pack(self, root: Path, *, approval: str = "profile", close: bool = True) -> None:
        width, height = 8, 8
        clip = root / "standby"
        track = clip / "life" / "track_7"
        track.mkdir(parents=True)
        base = bytes(range(width * height * 2))
        (clip / "hold.rgb565").write_bytes(base)
        frames = []
        for index, value in enumerate((0x11, 0x55, 0x99, None)):
            if value is None:
                patch = b"".join(
                    base[(row * width + 2) * 2:(row * width + 6) * 2]
                    for row in range(4, 6)
                )
                if not close:
                    patch = bytes([0xEE]) + patch[1:]
            else:
                patch = bytes([value]) * (4 * 2 * 2)
            path = track / f"{index:02d}.rgb565"
            path.write_bytes(patch)
            frames.append({
                "sequence": index,
                "source_frame": 10 + index,
                "rgb565": f"life/track_7/{index:02d}.rgb565",
                "rgb565_sha256": digest(patch),
            })
        manifest = {
            "schema": "dialogue-emotion-clip-v2",
            "emotion": "standby",
            "render": {
                "hold_base": {"rgb565": "hold.rgb565", "width": width, "height": height},
                "life_layer": {
                    "composite_mode": "canonical_base_precomposited_v1",
                    "generation_id": "synthetic-generation",
                    "base_rgb565_sha256": digest(base),
                    "tracks": [{
                        "id": 7,
                        "approval": approval,
                        "semantic": "body_breathe",
                        "roi": {"x": 2, "y": 4, "width": 4, "height": 2},
                        "frames": frames,
                        "choreography": {
                            "schema": "idle-life-cluster-v1",
                            "keyframes": [1, 2, 3],
                            "frame_interval_ms": 240,
                            "rest_ms": {"min": 4000, "max": 7000},
                            "busy_policy": "abort_to_base_no_replay",
                            "returns_to_base": True,
                        },
                    }],
                },
            },
        }
        (clip / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
        pack = {
            "schema": "dialogue-emotion-pack-v2",
            "runtime_contract": {
                "screen": {"width": width, "height": height, "pixel_format": "rgb565-le"},
                "max_rows_per_tick": 4,
            },
            "clips": {"standby": "standby/manifest.json"},
        }
        (root / "pack_manifest.json").write_text(json.dumps(pack), encoding="utf-8")

    def test_builds_full_width_opaque_bands_and_exact_base_closure(self) -> None:
        with tempfile.TemporaryDirectory() as value:
            root, output = Path(value) / "pack", Path(value) / "out"
            root.mkdir()
            self.make_pack(root)
            report = build_idle_flash_bands(root, output, character_id="robot-a")
            self.assertEqual(report["character_id"], "robot-a")
            self.assertEqual((report["width"], report["height"]), (8, 2))
            self.assertEqual(len(report["frames"]), 3)
            self.assertEqual(report["resource_bytes"], 8 * 2 * 2 * 3)
            base = (root / "standby" / "hold.rgb565").read_bytes()
            base_band = base[4 * 8 * 2:6 * 8 * 2]
            first = (output / "idl0.rgb").read_bytes()
            self.assertEqual(first[:4], base_band[:4])
            self.assertEqual(first[12:16], base_band[12:16])
            self.assertEqual((output / "idl2.rgb").read_bytes(), base_band)
            self.assertTrue(json.loads((output / "idlelife.json").read_text())["opaque_full_width"])

    def test_requires_explicit_cross_ip_identity(self) -> None:
        with tempfile.TemporaryDirectory() as value:
            root = Path(value) / "pack"
            root.mkdir()
            self.make_pack(root)
            with self.assertRaisesRegex(BuildError, "character_id is required"):
                build_idle_flash_bands(root, Path(value) / "out")

    def test_rejects_unapproved_motion_track(self) -> None:
        with tempfile.TemporaryDirectory() as value:
            root = Path(value) / "pack"
            root.mkdir()
            self.make_pack(root, approval="auto_unreviewed")
            with self.assertRaisesRegex(BuildError, "profile-approved"):
                build_idle_flash_bands(root, Path(value) / "out", character_id="robot")

    def test_rejects_non_closing_choreography(self) -> None:
        with tempfile.TemporaryDirectory() as value:
            root = Path(value) / "pack"
            root.mkdir()
            self.make_pack(root, close=False)
            with self.assertRaisesRegex(BuildError, "close exactly"):
                build_idle_flash_bands(root, Path(value) / "out", character_id="robot")


if __name__ == "__main__":
    unittest.main()
