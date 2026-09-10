#!/usr/bin/env python3

import json
import tempfile
import unittest
from pathlib import Path

import mjpeg_strangler_guard as guard


LEGACY_REL = "main/boards/ep-chat-p4-ml307/emotion_video_player.c"
ROLES = ("index_reader", "frame_source", "decoder", "playback_clock", "present_sink")
RUNTIME_CONFIG_REL = "main/boards/ep-chat-p4-ml307/mjpeg_runtime_selection.h"
RUNTIME_MACROS = {
    "index_reader": "EMOTION_VIDEO_USE_INDEX_READER",
    "frame_source": "EMOTION_VIDEO_USE_FRAME_SOURCE",
    "decoder": "EMOTION_VIDEO_USE_DECODER_STAGE",
    "playback_clock": "EMOTION_VIDEO_USE_PLAYBACK_CLOCK",
    "present_sink": "EMOTION_VIDEO_USE_PRESENT_SINK",
}


def legacy_fixture() -> str:
    return r"""
#define MAX_FRAMES_PER_VIDEO 300
static int build_frame_index(void) {
  index_result = mjpeg_index_reader_build(
  index_result = mjpeg_index_reader_build_legacy(
      cache->buffer, cache->size, cache->frame_index,
      MAX_FRAMES_PER_VIDEO, &frame_count, index_reader_yield, NULL);
  cache->frame_count = (int)frame_count;
  cache->index_built = true;
}
static int hw_decode_frame_optimized(void) {
  mjpeg_frame_source_view_t frame_source = {
    .buffer = current_cache->buffer,
    .entries = current_cache->frame_index,
  };
  result = mjpeg_frame_source_claim_next(&frame_source, &frame_cursor, &frame_claim);
  result = mjpeg_frame_source_claim_next_legacy(&frame_source, &frame_cursor, &frame_claim);
  if (claim_result != MJPEG_FRAME_SOURCE_OK) return bad;
  current_frame_index = (int)frame_cursor;
  frame_data = (uint8_t *)frame_claim.data;
  xSemaphoreGive(player->cache_mutex);
  if (!player->input_buffer) alloc_input();
  memcpy(player->input_buffer, frame_data, frame_size);
  if (!player->output_buffer) alloc_output();
  if (!player->hw_dec_handle) open_decoder();
  vc_ret = esp_video_dec_process(handle, in, out);
  if (vc_ret == ESP_VC_ERR_BUF_NOT_ENOUGH) {
    esp_video_dec_get_frame_info(handle, info);
    esp_video_codec_free(output);
    esp_video_codec_align_alloc(align, needed, actual);
    vc_ret = esp_video_dec_process(handle, in, out);
  }
  if (vc_ret != ESP_VC_ERR_OK) return bad;
  if (out_frame.decoded_size > 0 && player->frame_cb) player->frame_cb(data);
}
static void decode_task(void) {
  poll = mjpeg_playback_clock_player_poll(binding);
  if (clock_poll.action == MJPEG_CLOCK_ACTION_DECODE) {
  ret = hw_decode_frame_optimized(player);
  effect = mjpeg_playback_clock_player_on_decode_result(binding);
  if (ret == ESP_OK) effect.delay_one_tick;
  else if (ret == ESP_ERR_NOT_FOUND) handle_emotion_playback_complete(player);
  else {
    if (effect.enter_error_state) change_state(player, EMOTION_VIDEO_STATE_ERROR);
  }
  }
}
int emotion_video_player_decode_one_rgb565(void) {
  saved_cb = player->frame_cb;
  player->frame_cb = NULL;
  hw_decode_frame_optimized(player);
  player->frame_cb = saved_cb;
}
int emotion_video_player_decode_next_n_rgb565(void) {
  saved_cb = player->frame_cb;
  player->frame_cb = NULL;
  ret = hw_decode_frame_optimized(player);
  if (ret == ESP_ERR_NOT_FOUND) {
    reset_current_emotion_position(player);
    ret = hw_decode_frame_optimized(player);
  }
  player->frame_cb = saved_cb;
}
int emotion_video_player_decode_at_rgb565(void) {
  idx = frame_index % fc;
  current_frame_index = (int)idx;
  saved_cb = player->frame_cb;
  player->frame_cb = NULL;
  ret = hw_decode_frame_optimized(player);
  player->frame_cb = saved_cb;
}
"""


class MjpegStranglerGuardTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.legacy = self.root / LEGACY_REL
        self.legacy.parent.mkdir(parents=True)
        self.legacy.write_text(legacy_fixture(), encoding="utf-8")
        self.manifest = self.root / "manifest.json"
        self._write_manifest()

    def tearDown(self):
        self.temp.cleanup()

    def _write_manifest(
        self, *, evidence="pending", components=None, runtime="legacy"
    ):
        runtime_config = self.root / RUNTIME_CONFIG_REL
        runtime_config.parent.mkdir(parents=True, exist_ok=True)
        runtime_value = 1 if runtime == "production" else 0
        runtime_config.write_text(
            "".join(
                f"#define {macro} {runtime_value}\n"
                for macro in RUNTIME_MACROS.values()
            ),
            encoding="utf-8",
        )
        resolved_components = (
            components if components is not None else {role: None for role in ROLES}
        )
        payload = {
            "schema_version": 3,
            "legacy_source": LEGACY_REL,
            "runtime_config": RUNTIME_CONFIG_REL,
            "required_roles": list(ROLES),
            "components": resolved_components,
            "production_contracts": {
                role: {
                    "path": resolved_components[role],
                    "status": "pass",
                    "host_gate": f"test_{role}_host",
                }
                for role in ROLES
            },
            "runtime_selection": {role: runtime for role in ROLES},
            "evidence": {
                "baseline_session_v2": {"status": evidence},
                "g4_runtime_v2": {"status": evidence},
                "g4_soak_v3": {"status": evidence},
            },
            "runtime_evidence": {
                role: {"status": evidence} for role in ROLES
            },
        }
        self.manifest.write_text(json.dumps(payload), encoding="utf-8")

    def test_ordered_legacy_contract_passes(self):
        report = guard.evaluate(self.root, self.manifest)
        self.assertTrue(report.semantic_anchors_pass)
        self.assertEqual("legacy_guarded", report.phase)
        self.assertFalse(report.g5_delete_ready)
        self.assertFalse(report.g5_governance_ready)
        self.assertTrue(report.runtime_config_sync)

    def test_misordered_claim_is_rejected(self):
        source = legacy_fixture().replace(
            "current_frame_index = (int)frame_cursor;\n  frame_data = (uint8_t *)frame_claim.data;\n  xSemaphoreGive(player->cache_mutex);",
            "xSemaphoreGive(player->cache_mutex);\n  current_frame_index = (int)frame_cursor;\n  frame_data = (uint8_t *)frame_claim.data;",
        )
        self.legacy.write_text(source, encoding="utf-8")
        report = guard.evaluate(self.root, self.manifest)
        self.assertFalse(report.semantic_anchors_pass)
        self.assertEqual("legacy_drift", report.phase)
        self.assertIn("legacy_semantic_anchor_mismatch", report.blockers)

    def test_second_resize_retry_is_rejected(self):
        source = legacy_fixture().replace(
            "if (vc_ret != ESP_VC_ERR_OK) return bad;",
            "vc_ret = esp_video_dec_process(handle, in, out);\n"
            "  if (vc_ret != ESP_VC_ERR_OK) return bad;",
        )
        self.legacy.write_text(source, encoding="utf-8")
        report = guard.evaluate(self.root, self.manifest)
        result = next(x for x in report.anchor_results if x.name == "decode_resize_present_contract")
        self.assertFalse(result.passed)
        self.assertIn("count mismatch", result.detail)

    def test_claimed_evidence_cannot_hide_missing_split(self):
        self._write_manifest(evidence="pass")
        report = guard.evaluate(self.root, self.manifest)
        self.assertFalse(report.production_split)
        self.assertFalse(report.g5_delete_ready)
        self.assertIn("g4_production_split_missing", report.blockers)
        self.assertIn("g4_runtime_selection_incomplete", report.blockers)

    def test_dual_path_is_ready_only_with_components_and_evidence(self):
        components = {}
        for role in ROLES:
            rel = f"main/boards/ep-chat-p4-ml307/mjpeg/{role}.c"
            path = self.root / rel
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("/* strangler component */\n", encoding="utf-8")
            components[role] = rel
        self._write_manifest(
            evidence="pass", components=components, runtime="production"
        )
        report = guard.evaluate(self.root, self.manifest)
        self.assertEqual("dual_path", report.phase)
        self.assertTrue(report.production_split)
        self.assertTrue(report.production_contracts_ready)
        self.assertTrue(report.g5_delete_ready)
        self.assertTrue(report.g5_governance_ready)
        self.assertFalse(report.g5_certified_mixed_ready)
        self.assertEqual(ROLES, report.migrated_roles)
        self.assertEqual((), report.blockers)

    def test_runtime_manifest_drift_is_rejected(self):
        runtime_config = self.root / RUNTIME_CONFIG_REL
        runtime_config.write_text(
            runtime_config.read_text(encoding="utf-8").replace(
                "#define EMOTION_VIDEO_USE_DECODER_STAGE 0",
                "#define EMOTION_VIDEO_USE_DECODER_STAGE 1",
            ),
            encoding="utf-8",
        )
        report = guard.evaluate(self.root, self.manifest)
        self.assertFalse(report.runtime_config_sync)
        self.assertIn("g4_runtime_config_manifest_mismatch", report.blockers)

    def test_real_repository_has_five_components_and_certified_mixed_closeout(self):
        repo_root = Path(__file__).resolve().parents[2]
        manifest = Path(__file__).with_name("mjpeg_strangler_manifest.json")
        report = guard.evaluate(repo_root, manifest)
        self.assertEqual("dual_path_guarded", report.phase)
        self.assertTrue(report.semantic_anchors_pass)
        self.assertTrue(report.production_split)
        self.assertFalse(report.runtime_ready)
        self.assertTrue(report.runtime_config_sync)
        self.assertFalse(report.g5_delete_ready)
        self.assertTrue(report.g5_certified_mixed_ready)
        self.assertTrue(report.g5_governance_ready)
        self.assertEqual(
            ("index_reader", "frame_source", "decoder", "playback_clock", "present_sink"),
            report.migrated_roles,
        )
        self.assertNotIn("g4_production_split_missing", report.blockers)
        self.assertIn("g4_runtime_selection_incomplete", report.blockers)
        self.assertIn("g4_decoder_runtime_evidence_failed", report.blockers)
        self.assertIn("g4_playback_clock_runtime_evidence_failed", report.blockers)
        self.assertEqual(
            ("decoder", "playback_clock"), report.certified_legacy_roles
        )
        self.assertEqual((), report.closeout_blockers)
        self.assertEqual("pass", report.runtime_evidence_status["present_sink"])
        self.assertNotIn("g4_present_sink_runtime_evidence_pending", report.blockers)
        self.assertNotIn("g4_decoder_runtime_evidence_missing", report.blockers)
        self.assertNotIn("baseline_session_v2_pending", report.blockers)
        self.assertNotIn("g4_v2_missing", report.blockers)
        self.assertNotIn("g4_v3_missing", report.blockers)

    def test_real_repository_mixed_closeout_needs_no_further_runtime_change(self):
        repo_root = Path(__file__).resolve().parents[2]
        manifest_path = Path(__file__).with_name("mjpeg_strangler_manifest.json")
        payload = json.loads(manifest_path.read_text(encoding="utf-8"))
        self.assertEqual(
            "legacy", payload["runtime_selection"]["playback_clock"]
        )
        report = guard.evaluate(repo_root, manifest_path)
        self.assertFalse(report.g5_delete_ready)
        self.assertTrue(report.g5_certified_mixed_ready)
        self.assertTrue(report.g5_governance_ready)
        self.assertEqual(
            ("decoder", "playback_clock"), report.certified_legacy_roles
        )
        self.assertEqual((), report.closeout_blockers)
        self.assertIn("g4_runtime_selection_incomplete", report.blockers)
        self.assertIn("g4_decoder_runtime_evidence_failed", report.blockers)
        self.assertIn("g4_playback_clock_runtime_evidence_failed", report.blockers)

    def test_certified_mixed_backend_can_close_governance_without_deletion(self):
        components = {}
        for role in ROLES:
            rel = f"main/boards/ep-chat-p4-ml307/mjpeg/{role}.c"
            path = self.root / rel
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("/* strangler component */\n", encoding="utf-8")
            components[role] = rel
        self._write_manifest(
            evidence="pass", components=components, runtime="production"
        )
        payload = json.loads(self.manifest.read_text(encoding="utf-8"))
        for role in ("decoder", "playback_clock"):
            payload["runtime_selection"][role] = "legacy"
            payload["runtime_evidence"][role] = {"status": "fail"}
        payload["legacy_exceptions"] = {
            role: {
                "decision": "permanent_legacy",
                "status": "approved",
                "reason": "two runtime markers failed the production path",
                "j5_stop": True,
                "failure_markers": [f"{role}_a", f"{role}_b"],
                "fallback_marker": "known_good",
                "fallback_status": "pass",
            }
            for role in ("decoder", "playback_clock")
        }
        self.manifest.write_text(json.dumps(payload), encoding="utf-8")
        runtime_config = self.root / RUNTIME_CONFIG_REL
        config = runtime_config.read_text(encoding="utf-8")
        config = config.replace(
            "#define EMOTION_VIDEO_USE_DECODER_STAGE 1",
            "#define EMOTION_VIDEO_USE_DECODER_STAGE 0",
        ).replace(
            "#define EMOTION_VIDEO_USE_PLAYBACK_CLOCK 1",
            "#define EMOTION_VIDEO_USE_PLAYBACK_CLOCK 0",
        )
        runtime_config.write_text(config, encoding="utf-8")

        report = guard.evaluate(self.root, self.manifest)
        self.assertFalse(report.g5_delete_ready)
        self.assertTrue(report.g5_certified_mixed_ready)
        self.assertTrue(report.g5_governance_ready)
        self.assertEqual(("decoder", "playback_clock"), report.certified_legacy_roles)
        self.assertEqual((), report.closeout_blockers)

    def test_legacy_exception_requires_two_unique_failure_markers(self):
        components = {}
        for role in ROLES:
            rel = f"main/boards/ep-chat-p4-ml307/mjpeg/{role}.c"
            path = self.root / rel
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("/* strangler component */\n", encoding="utf-8")
            components[role] = rel
        self._write_manifest(
            evidence="pass", components=components, runtime="production"
        )
        payload = json.loads(self.manifest.read_text(encoding="utf-8"))
        payload["runtime_selection"]["decoder"] = "legacy"
        payload["runtime_evidence"]["decoder"] = {"status": "fail"}
        payload["legacy_exceptions"] = {
            "decoder": {
                "decision": "permanent_legacy",
                "status": "approved",
                "reason": "insufficient repeated evidence",
                "j5_stop": True,
                "failure_markers": ["only_one"],
                "fallback_marker": "known_good",
                "fallback_status": "pass",
            }
        }
        self.manifest.write_text(json.dumps(payload), encoding="utf-8")
        runtime_config = self.root / RUNTIME_CONFIG_REL
        runtime_config.write_text(
            runtime_config.read_text(encoding="utf-8").replace(
                "#define EMOTION_VIDEO_USE_DECODER_STAGE 1",
                "#define EMOTION_VIDEO_USE_DECODER_STAGE 0",
            ),
            encoding="utf-8",
        )

        report = guard.evaluate(self.root, self.manifest)
        self.assertFalse(report.g5_governance_ready)
        self.assertIn(
            "g5_decoder_legacy_exception_invalid", report.closeout_blockers
        )

    def test_decoder_production_stage_and_wrapper_preserve_boundaries(self):
        repo_root = Path(__file__).resolve().parents[2]
        manifest_path = Path(__file__).with_name("mjpeg_strangler_manifest.json")
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        contract = manifest["production_contracts"]["decoder"]
        self.assertEqual("pass", contract["status"])
        self.assertEqual(contract["path"], manifest["components"]["decoder"])
        self.assertEqual("legacy", manifest["runtime_selection"]["decoder"])

        source = (repo_root / contract["path"]).read_text(encoding="utf-8")
        self.assertEqual(source.count("ops->process("), 2)
        self.assertEqual(source.count("memcpy(resources->input_buffer"), 1)
        for forbidden in (
            "xSemaphore",
            "frame_cb",
            "WdtContend",
            "taskYIELD",
            "lv_",
            "esp_video",
            "heap_caps",
        ):
            with self.subTest(forbidden=forbidden):
                self.assertNotIn(forbidden, source)

        wrapper = (repo_root / LEGACY_REL).read_text(encoding="utf-8")
        adapter_source = (
            repo_root
            / "main/boards/ep-chat-p4-ml307/mjpeg_decoder_player_adapter.c"
        ).read_text(encoding="utf-8")
        helper = guard.function_body(
            adapter_source, "mjpeg_decoder_player_adapter_run"
        )
        self.assertIsNotNone(helper)
        assert helper is not None
        ordered = (
            "mjpeg_decoder_stage_run(",
            "*adapter->input_buffer = owner.input_buffer;",
            "*adapter->input_buffer_size = (uint32_t)owner.input_buffer_size;",
            "*adapter->output_buffer = owner.output_buffer;",
            "*adapter->output_buffer_size = (uint32_t)owner.output_buffer_size;",
            "*adapter->decoder_handle = (esp_video_dec_handle_t)owner.decoder_handle;",
            "adapter->frame_info->res.width = owner.frame_width;",
            "adapter->frame_info->res.height = owner.frame_height;",
            "map_status(status)",
            'WdtContendBreadcrumb("pre_frame_cb")',
            "adapter->frame_cb(",
            'WdtContendBreadcrumb("post_frame_cb")',
            "WdtContendNoteMjpegFrame(",
            'WdtContendBreadcrumb("post_frame_note")',
            "taskYIELD();",
            'WdtContendBreadcrumb("post_yield")',
        )
        cursor = 0
        for anchor in ordered:
            with self.subTest(anchor=anchor):
                position = helper.find(anchor, cursor)
                self.assertGreaterEqual(position, 0)
                cursor = position + len(anchor)

        optimized = guard.function_body(wrapper, "hw_decode_frame_optimized")
        self.assertIsNotNone(optimized)
        assert optimized is not None
        self.assertIn("#if EMOTION_VIDEO_USE_DECODER_STAGE", optimized)
        self.assertIn(
            "return mjpeg_decoder_player_adapter_run(&adapter, frame_data, frame_size);",
            optimized,
        )
        self.assertEqual(optimized.count("esp_video_dec_process("), 2)

        adapter = (
            repo_root
            / "main/boards/ep-chat-p4-ml307/mjpeg_decoder_stage_esp.c"
        ).read_text(encoding="utf-8")
        self.assertEqual(adapter.count("esp_video_dec_process("), 1)
        for forbidden in ("frame_cb", "WdtContend", "xSemaphore", "taskYIELD", "lv_"):
            with self.subTest(adapter_forbidden=forbidden):
                self.assertNotIn(forbidden, adapter)

    def test_playback_clock_contract_and_legacy_r0_are_wired(self):
        repo_root = Path(__file__).resolve().parents[2]
        manifest_path = Path(__file__).with_name("mjpeg_strangler_manifest.json")
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        contract = manifest["production_contracts"]["playback_clock"]
        self.assertEqual("pass", contract["status"])
        self.assertEqual(contract["path"], manifest["components"]["playback_clock"])
        self.assertEqual("legacy", manifest["runtime_selection"]["playback_clock"])

        source = (repo_root / contract["path"]).read_text(encoding="utf-8")
        self.assertIn("decode_error_count > 10U", source)
        self.assertIn("MJPEG_CLOCK_HANG_THRESHOLD_US", source)
        for forbidden in (
            "xSemaphore",
            "vTask",
            "esp_timer",
            "hw_decode",
            "frame_cb",
            "WdtContend",
            "lv_",
            "heap_caps",
            "current_frame_index",
        ):
            with self.subTest(forbidden=forbidden):
                self.assertNotIn(forbidden, source)

        legacy = guard.function_body(source, "mjpeg_playback_clock_legacy_poll")
        self.assertIsNotNone(legacy)
        assert legacy is not None
        self.assertIn("MJPEG_CLOCK_ACTION_DECODE", legacy)
        self.assertNotIn("return poll_impl", legacy)

        wrapper = (repo_root / LEGACY_REL).read_text(encoding="utf-8")
        task = guard.function_body(wrapper, "decode_task")
        self.assertIsNotNone(task)
        assert task is not None
        for anchor in (
            "#if EMOTION_VIDEO_USE_PLAYBACK_CLOCK",
            "mjpeg_playback_clock_player_poll_direct(",
            "mjpeg_playback_clock_player_on_decode_result_direct(",
            "effect.delay_one_tick",
            "effect.enter_error_state",
        ):
            self.assertIn(anchor, task)

        adapter = (
            repo_root
            / "main/boards/ep-chat-p4-ml307/mjpeg_playback_clock_player_adapter.c"
        ).read_text(encoding="utf-8")
        for anchor in (
            "mjpeg_playback_clock_poll(&state, now_us)",
            "mjpeg_playback_clock_legacy_poll(&state, now_us)",
            "mjpeg_playback_clock_on_decode_result(",
            "mjpeg_playback_clock_legacy_on_decode_result(",
            "store_state(binding, &state)",
            "mjpeg_playback_clock_player_poll_direct(",
            "mjpeg_playback_clock_player_on_decode_result_direct(",
        ):
            self.assertIn(anchor, adapter)

    def test_present_sink_production_preserves_observation_without_display_driver(self):
        repo_root = Path(__file__).resolve().parents[2]
        manifest_path = Path(__file__).with_name("mjpeg_strangler_manifest.json")
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        contract = manifest["production_contracts"]["present_sink"]
        self.assertEqual("pass", contract["status"])
        self.assertEqual(contract["path"], manifest["components"]["present_sink"])
        self.assertEqual("production", manifest["runtime_selection"]["present_sink"])

        source = (repo_root / contract["path"]).read_text(encoding="utf-8")
        self.assertEqual(source.count("ops->window_active("), 3)
        self.assertEqual(source.count("ops->note_frame("), 2)
        for anchor in (
            '"pre_frame_cb"',
            '"post_frame_cb"',
            '"post_frame_note"',
            '"post_yield"',
        ):
            self.assertIn(anchor, source)
        for forbidden in (
            "xSemaphore",
            "vTask",
            "lv_",
            "esp_lcd",
            "heap_caps",
            "esp_video",
            "current_frame_index",
            "frame_deadline_us",
        ):
            with self.subTest(forbidden=forbidden):
                self.assertNotIn(forbidden, source)

        adapter = (
            repo_root
            / "main/boards/ep-chat-p4-ml307/mjpeg_present_sink_esp.c"
        ).read_text(encoding="utf-8")
        for anchor in (
            "EMOTION_VIDEO_USE_PRESENT_SINK",
            "mjpeg_present_sink_present(&frame, &k_sink_ops",
            'WdtContendBreadcrumb("pre_frame_cb")',
            'WdtContendBreadcrumb("post_yield")',
        ):
            self.assertIn(anchor, adapter)


if __name__ == "__main__":
    unittest.main()
