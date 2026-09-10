#!/usr/bin/env python3

import unittest

import mjpeg_hot_path_stack_guard as guard


class MjpegHotPathStackGuardTest(unittest.TestCase):
    def test_stack_frame_parser_handles_riscv_prologue(self):
        self.assertEqual(
            128,
            guard.stack_frame_bytes("4000: 7119 addi sp,sp,-128\n"),
        )
        self.assertEqual(0, guard.stack_frame_bytes("4000: 8082 ret\n"))

    def test_clock_trampoline_exceeds_stable_budget(self):
        report = guard.evaluate_frames(
            "clock",
            {
                guard.CLOCK_ADAPTER: 128,
                guard.CLOCK_PRODUCTION: 16,
                guard.CLOCK_LEGACY: 32,
            },
            production_clock_calls_legacy=True,
        )
        self.assertEqual("FAIL", report["status"])
        self.assertEqual(176, report["observed_bytes"])

    def test_clock_direct_body_meets_stable_budget(self):
        report = guard.evaluate_frames(
            "clock",
            {
                guard.CLOCK_ADAPTER: 128,
                guard.CLOCK_PRODUCTION: 32,
            },
        )
        self.assertEqual("PASS", report["status"])
        self.assertEqual(160, report["observed_bytes"])

    def test_clock_trampoline_fails_even_if_small_object_frames_fit(self):
        report = guard.evaluate_frames(
            "clock",
            {
                guard.CLOCK_ADAPTER: 32,
                guard.CLOCK_PRODUCTION: 32,
                guard.CLOCK_LEGACY: 32,
            },
            production_clock_calls_legacy=True,
        )
        self.assertEqual("FAIL", report["status"])
        self.assertTrue(report["forbidden_clock_trampoline"])

    def test_clock_direct_binding_uses_max_of_separate_entries(self):
        report = guard.evaluate_frames(
            "clock-direct",
            {
                guard.CLOCK_DIRECT_POLL: 80,
                guard.CLOCK_DIRECT_RESULT: 48,
            },
        )
        self.assertEqual("PASS", report["status"])
        self.assertEqual(80, report["observed_bytes"])

    def test_clock_direct_binding_rejects_stage_callback(self):
        report = guard.evaluate_frames(
            "clock-direct",
            {
                guard.CLOCK_DIRECT_POLL: 32,
                guard.CLOCK_DIRECT_RESULT: 32,
            },
            direct_clock_calls_stage=True,
        )
        self.assertEqual("FAIL", report["status"])
        self.assertTrue(report["forbidden_direct_stage_call"])

    def test_decoder_layered_chain_is_rejected(self):
        report = guard.evaluate_frames(
            "decoder",
            dict(zip(guard.DECODER_CHAIN, (144, 128, 96, 112, 80))),
        )
        self.assertEqual("FAIL", report["status"])
        self.assertEqual(560, report["observed_bytes"])

    def test_decoder_direct_specialization_budget(self):
        report = guard.evaluate_frames(
            "decoder",
            dict(zip(guard.DECODER_CHAIN, (144, 32, 16, 16, 0))),
        )
        self.assertEqual("PASS", report["status"])
        self.assertEqual(208, report["observed_bytes"])

    def test_decoder_direct_binding_meets_total_and_entry_budgets(self):
        report = guard.evaluate_frames(
            "decoder-direct",
            {
                guard.DECODER_DIRECT_CALLER: 144,
                guard.DECODER_DIRECT_ENTRY: 64,
            },
        )
        self.assertEqual("PASS", report["status"])
        self.assertEqual(208, report["observed_bytes"])
        self.assertEqual(64, report["decoder_direct_frame_bytes"])

    def test_decoder_direct_binding_rejects_large_entry_even_if_total_fits(self):
        report = guard.evaluate_frames(
            "decoder-direct",
            {
                guard.DECODER_DIRECT_CALLER: 96,
                guard.DECODER_DIRECT_ENTRY: 80,
            },
        )
        self.assertEqual("FAIL", report["status"])
        self.assertTrue(report["decoder_direct_frame_exceeds_budget"])

    def test_decoder_direct_binding_rejects_stage_or_indirect_calls(self):
        stage_report = guard.evaluate_frames(
            "decoder-direct",
            {
                guard.DECODER_DIRECT_CALLER: 128,
                guard.DECODER_DIRECT_ENTRY: 48,
            },
            direct_decoder_calls_stage=True,
        )
        self.assertEqual("FAIL", stage_report["status"])
        self.assertTrue(stage_report["forbidden_decoder_stage_call"])

        indirect_report = guard.evaluate_frames(
            "decoder-direct",
            {
                guard.DECODER_DIRECT_CALLER: 128,
                guard.DECODER_DIRECT_ENTRY: 48,
            },
            direct_decoder_indirect_call=True,
        )
        self.assertEqual("FAIL", indirect_report["status"])
        self.assertTrue(indirect_report["forbidden_decoder_indirect_call"])

    def test_missing_symbol_fails_closed(self):
        report = guard.evaluate_frames(
            "decoder",
            {guard.DECODER_CHAIN[0]: 144},
        )
        self.assertEqual("FAIL", report["status"])
        self.assertIn(guard.DECODER_CHAIN[1], report["missing"])


if __name__ == "__main__":
    unittest.main()
