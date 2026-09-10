from __future__ import annotations

import unittest

from .mjpeg_frame_source_model import (
    EOI,
    MAX_FRAME_SIZE,
    MAX_FRAMES_PER_VIDEO,
    MIN_FRAME_SIZE,
    SOI,
    CachedFrameSourceModel,
    FrameIndex,
    FrameIndexEntry,
    build_frame_index,
)


def jpeg_of_size(size: int, fill: bytes = b"x") -> bytes:
    if size < 4:
        raise ValueError("JPEG candidate must fit SOI and EOI")
    return SOI + fill * (size - 4) + EOI


class MjpegFrameSourceModelTest(unittest.TestCase):
    def test_scan_ignores_noise_and_records_exact_offsets(self):
        first = jpeg_of_size(MIN_FRAME_SIZE, b"a")
        second = jpeg_of_size(MIN_FRAME_SIZE + 7, b"b")
        data = b"noise" + first + b"gap" + second + b"tail"

        index = build_frame_index(data)

        self.assertTrue(index.built)
        self.assertEqual(
            index.entries,
            (
                FrameIndexEntry(5, len(first)),
                FrameIndexEntry(5 + len(first) + 3, len(second)),
            ),
        )

    def test_rejected_small_candidate_is_consumed_before_next_scan(self):
        too_small = jpeg_of_size(MIN_FRAME_SIZE - 1)
        valid = jpeg_of_size(MIN_FRAME_SIZE)
        data = too_small + b"." + valid

        index = build_frame_index(data)

        self.assertEqual(
            index.entries,
            (FrameIndexEntry(len(too_small) + 1, len(valid)),),
        )

    def test_oversized_candidate_is_skipped_and_later_frame_survives(self):
        oversized = jpeg_of_size(MAX_FRAME_SIZE + 1)
        valid = jpeg_of_size(MIN_FRAME_SIZE)

        index = build_frame_index(oversized + valid)

        self.assertEqual(index.entries, (FrameIndexEntry(len(oversized), len(valid)),))

    def test_truncated_tail_stops_scan_but_keeps_prior_frames(self):
        valid = jpeg_of_size(MIN_FRAME_SIZE)
        index = build_frame_index(valid + b"gap" + SOI + b"truncated")
        empty = build_frame_index(SOI + b"truncated")

        self.assertEqual(index.entries, (FrameIndexEntry(0, len(valid)),))
        self.assertTrue(empty.built)
        self.assertEqual(empty.entries, ())

    def test_frame_cap_matches_production_table_capacity(self):
        frame = jpeg_of_size(MIN_FRAME_SIZE)
        index = build_frame_index(frame * (MAX_FRAMES_PER_VIDEO + 1))
        self.assertEqual(len(index.entries), MAX_FRAMES_PER_VIDEO)

    def test_claim_advances_before_decode_and_eof_reset_loops(self):
        frame = jpeg_of_size(MIN_FRAME_SIZE)
        data = frame + frame
        source = CachedFrameSourceModel(data, build_frame_index(data))

        self.assertEqual(source.claim_next().frame_index, 0)
        # A decoder failure after this claim does not rewind the production cursor.
        self.assertEqual(source.claim_next().frame_index, 1)
        self.assertEqual(source.claim_next().status, "not_found")
        source.reset()
        self.assertEqual(source.claim_next().frame_index, 0)

    def test_bad_index_entry_repeats_without_advancing(self):
        data = jpeg_of_size(MIN_FRAME_SIZE)
        bad = FrameIndex((FrameIndexEntry(len(data) - 2, 10),))
        source = CachedFrameSourceModel(data, bad)

        self.assertEqual(source.claim_next().status, "invalid_size")
        self.assertEqual(source.cursor, 0)
        self.assertEqual(source.claim_next().status, "invalid_size")
        self.assertEqual(source.cursor, 0)

    def test_seek_wraps_and_latest_request_overwrites_cursor(self):
        frame = jpeg_of_size(MIN_FRAME_SIZE)
        data = frame * 3
        source = CachedFrameSourceModel(data, build_frame_index(data))

        self.assertEqual(source.seek_and_claim(5).frame_index, 2)
        # A later synchronous seek replaces the prior end cursor rather than
        # queueing or replaying the old request.
        self.assertEqual(source.seek_and_claim(3).frame_index, 0)
        self.assertEqual(source.cursor, 1)

    def test_unready_unbuilt_empty_and_invalid_arguments_fail_closed(self):
        frame = jpeg_of_size(MIN_FRAME_SIZE)
        built = build_frame_index(frame)
        self.assertEqual(CachedFrameSourceModel(frame, built, ready=False).claim_next().status, "invalid_state")
        self.assertEqual(CachedFrameSourceModel(frame, FrameIndex((), built=False)).claim_next().status, "invalid_state")
        self.assertEqual(CachedFrameSourceModel(b"", FrameIndex(())).claim_next().status, "invalid_state")
        self.assertEqual(CachedFrameSourceModel(frame, FrameIndex(())).seek_and_claim(0).status, "invalid_state")
        with self.assertRaises(ValueError):
            CachedFrameSourceModel(frame, built).seek_and_claim(-1)
        with self.assertRaises(TypeError):
            build_frame_index(bytearray(frame))  # type: ignore[arg-type]
        with self.assertRaises(ValueError):
            build_frame_index(frame, max_frames=0)


if __name__ == "__main__":
    unittest.main()
