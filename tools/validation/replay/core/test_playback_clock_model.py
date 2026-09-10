from __future__ import annotations

import unittest

from .playback_clock_model import (
    SOFT_START_MIN_INTERVAL_US,
    SOFT_START_US,
    WARMUP_US,
    FrameCursorModel,
    PlaybackClockModel,
)


class PlaybackClockModelTest(unittest.TestCase):
    def test_resume_warmup_and_soft_start(self):
        clock = PlaybackClockModel(normal_interval_us=33_333)
        clock.resume(1_000_000)
        self.assertEqual(clock.frame_interval_us, SOFT_START_MIN_INTERVAL_US)
        self.assertEqual(clock.poll(1_000_000 + WARMUP_US - 1), "warmup")
        self.assertEqual(clock.poll(1_000_000 + WARMUP_US), "decode")

        clock.decode_succeeded(1_000_000 + WARMUP_US)
        self.assertEqual(clock.poll(1_000_000 + WARMUP_US + 199_999), "wait")
        self.assertEqual(clock.poll(1_000_000 + SOFT_START_US), "decode")
        self.assertEqual(clock.frame_interval_us, 33_333)

    def test_late_success_reanchors_without_catchup(self):
        clock = PlaybackClockModel(normal_interval_us=100_000)
        clock.resume(0)
        late_us = WARMUP_US + 900_000
        self.assertEqual(clock.poll(late_us), "decode")
        clock.decode_succeeded(late_us)
        self.assertEqual(clock.deadline_us, late_us + SOFT_START_MIN_INTERVAL_US)
        self.assertEqual(clock.poll(late_us + 1), "wait")

    def test_pause_blocks_and_resume_replaces_stale_deadline(self):
        clock = PlaybackClockModel(normal_interval_us=100_000)
        clock.resume(0)
        clock.pause()
        self.assertEqual(clock.poll(10_000_000), "paused")
        clock.resume(10_000_000)
        self.assertEqual(clock.deadline_us, 10_000_000 + WARMUP_US)
        self.assertEqual(clock.poll(10_000_000), "warmup")

    def test_stream_end_makes_first_loop_frame_immediately_due(self):
        clock = PlaybackClockModel(normal_interval_us=100_000)
        clock.resume(0)
        clock.stream_ended(800_000)
        self.assertEqual(clock.poll(800_000), "decode")

    def test_cursor_claims_before_decode_and_reset_loops(self):
        cursor = FrameCursorModel(frame_count=2)
        self.assertEqual(cursor.claim_next(), 0)
        # Even if frame zero's decode failed, the next claim is frame one.
        self.assertEqual(cursor.claim_next(), 1)
        self.assertIsNone(cursor.claim_next())
        cursor.reset()
        self.assertEqual(cursor.claim_next(), 0)

    def test_invalid_intervals_and_counts_fail_fast(self):
        with self.assertRaises(ValueError):
            PlaybackClockModel(normal_interval_us=0)
        with self.assertRaises(ValueError):
            FrameCursorModel(frame_count=-1)


if __name__ == "__main__":
    unittest.main()
