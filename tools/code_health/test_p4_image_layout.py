import struct
import unittest

from check_p4_image_layout import parse_segments, validate_p4_layout


def make_image(segments):
    image = bytearray(24)
    image[0] = 0xE9
    image[1] = len(segments)
    for load_address, length in segments:
        image.extend(struct.pack("<II", load_address, length))
        image.extend(bytes(length))
    return bytes(image)


class P4ImageLayoutTest(unittest.TestCase):
    def test_accepts_small_padding(self):
        segments = parse_segments(
            make_image([(0x40190020, 0xFFD0), (0x4FF00000, 0x20), (0x40000020, 4)])
        )
        self.assertEqual(validate_p4_layout(segments).length, 0x20)

    def test_rejects_nearly_64k_padding(self):
        segments = parse_segments(
            make_image([(0x40190020, 4), (0x4FF00000, 0xFFFC), (0x40000020, 4)])
        )
        with self.assertRaisesRegex(ValueError, "pathological"):
            validate_p4_layout(segments)

    def test_rejects_truncated_segment(self):
        image = bytearray(make_image([(0x40190020, 4)]))
        del image[-1]
        with self.assertRaisesRegex(ValueError, "truncated"):
            parse_segments(bytes(image))


if __name__ == "__main__":
    unittest.main()
