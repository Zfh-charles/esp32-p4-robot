from __future__ import annotations

import unittest

from .mjpeg_decode_pipeline_model import DecodePlan, MjpegDecodePipelineModel
from .mjpeg_frame_source_model import (
    EOI,
    MIN_FRAME_SIZE,
    SOI,
    CachedFrameSourceModel,
    build_frame_index,
)


def jpeg() -> bytes:
    return SOI + b"x" * (MIN_FRAME_SIZE - 4) + EOI


def pipeline(frame_count: int = 2) -> MjpegDecodePipelineModel:
    data = jpeg() * frame_count
    return MjpegDecodePipelineModel(
        CachedFrameSourceModel(data, build_frame_index(data))
    )


class MjpegDecodePipelineModelTest(unittest.TestCase):
    def test_loop_success_orders_claim_decode_present_and_stats(self):
        model = pipeline()

        outcome = model.loop_tick()

        self.assertEqual((outcome.status, outcome.frame_index, outcome.presented), ("ok", 0, True))
        self.assertEqual(
            model.effects,
            [
                ("source.claim", 0),
                ("decoder.process", 0),
                ("present.frame", 0),
                ("decoder.ok", 0),
                ("loop.success", 0),
            ],
        )
        self.assertEqual((model.current_frame, model.decode_error_count), (1, 0))

    def test_decoder_failure_consumes_claim_and_next_tick_skips_frame(self):
        model = pipeline()

        failed = model.loop_tick(DecodePlan(first_result="fail"))
        succeeded = model.loop_tick()

        self.assertEqual((failed.status, failed.frame_index), ("fail", 0))
        self.assertEqual((succeeded.status, succeeded.frame_index), ("ok", 1))
        self.assertEqual(model.current_frame, 1)
        self.assertEqual(model.decode_error_count, 0)

    def test_buffer_resize_retries_once_before_present(self):
        model = pipeline(1)

        outcome = model.loop_tick(
            DecodePlan(first_result="buffer_not_enough", retry_result="ok")
        )

        self.assertEqual(outcome.status, "ok")
        self.assertEqual(
            [effect[0] for effect in model.effects],
            [
                "source.claim",
                "decoder.process",
                "decoder.get_frame_info",
                "output.reallocate",
                "decoder.retry",
                "present.frame",
                "decoder.ok",
                "loop.success",
            ],
        )

    def test_second_buffer_shortage_is_generic_failure(self):
        model = pipeline(1)
        outcome = model.loop_tick(
            DecodePlan(
                first_result="buffer_not_enough",
                retry_result="buffer_not_enough",
            )
        )

        self.assertEqual(outcome.status, "fail")
        self.assertEqual(
            [name for name, _ in model.effects].count("decoder.get_frame_info"), 1
        )
        self.assertNotIn(("present.frame", 0), model.effects)

    def test_resize_faults_preserve_advanced_source_cursor(self):
        for plan, expected in (
            (DecodePlan(first_result="buffer_not_enough", frame_info_ok=False), "fail"),
            (DecodePlan(first_result="buffer_not_enough", resize_alloc_ok=False), "no_mem"),
            (DecodePlan(input_alloc_ok=False), "no_mem"),
            (DecodePlan(output_alloc_ok=False), "no_mem"),
            (DecodePlan(decoder_open_ok=False), "fail"),
        ):
            with self.subTest(plan=plan):
                model = pipeline()
                self.assertEqual(model.loop_tick(plan).status, expected)
                self.assertEqual(model.source.cursor, 1)

    def test_zero_decoded_size_is_success_without_present(self):
        model = pipeline(1)
        model.decode_error_count = 4

        outcome = model.loop_tick(DecodePlan(decoded_size=0))

        self.assertEqual((outcome.status, outcome.presented), ("ok", False))
        self.assertIn(("present.skipped", 0), model.effects)
        self.assertEqual((model.current_frame, model.decode_error_count), (1, 0))

    def test_loop_eof_resets_but_defers_frame_zero_to_next_tick(self):
        model = pipeline(1)
        self.assertEqual(model.loop_tick().frame_index, 0)

        eof = model.loop_tick()

        self.assertEqual(eof.status, "not_found")
        self.assertEqual(model.source.cursor, 0)
        self.assertEqual(model.current_frame, 1)
        self.assertEqual(model.loop_tick().frame_index, 0)

    def test_eleventh_consecutive_failure_enters_error_state(self):
        model = pipeline(11)
        for _ in range(10):
            model.loop_tick(DecodePlan(first_result="fail"))
            self.assertEqual(model.state, "playing")

        model.loop_tick(DecodePlan(first_result="fail"))

        self.assertEqual((model.decode_error_count, model.state), (11, "error"))
        self.assertEqual(
            [name for name, _ in model.effects].count("loop.error_state"), 1
        )

    def test_bypass_suppresses_present_and_does_not_touch_loop_stats(self):
        model = pipeline(3)
        model.current_frame = 7
        model.decode_error_count = 2

        outcome = model.bypass_decode_at(5)

        self.assertEqual((outcome.status, outcome.frame_index, outcome.presented), ("ok", 2, False))
        self.assertIn(("present.skipped", 2), model.effects)
        self.assertEqual((model.current_frame, model.decode_error_count), (7, 2))

    def test_bypass_next_n_retries_frame_zero_immediately_after_eof(self):
        model = pipeline(1)
        self.assertEqual(model.bypass_decode_next_n(1).frame_index, 0)

        wrapped = model.bypass_decode_next_n(0)

        self.assertEqual((wrapped.status, wrapped.frame_index), ("ok", 0))
        self.assertIn(("bypass.reset", None), model.effects)
        self.assertFalse(wrapped.presented)

    def test_invalid_plans_fail_fast(self):
        with self.assertRaises(ValueError):
            DecodePlan(first_result="maybe")
        with self.assertRaises(ValueError):
            DecodePlan(retry_result="maybe")
        with self.assertRaises(ValueError):
            DecodePlan(decoded_size=-1)


if __name__ == "__main__":
    unittest.main()
