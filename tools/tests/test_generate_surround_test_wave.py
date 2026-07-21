import struct
import unittest

from tools.generate_surround_test_wave import (
    BITS_PER_SAMPLE,
    CHANNEL_COUNT,
    CHANNEL_MASK_5_1_SURROUND,
    SAMPLE_RATE,
    build_wave_bytes,
)


class SurroundTestWaveTests(unittest.TestCase):
    def test_writes_extensible_5_1_surround_header(self):
        payload = build_wave_bytes(tone_seconds=0.02, silence_seconds=0.01)
        self.assertEqual(payload[:4], b"RIFF")
        self.assertEqual(payload[8:12], b"WAVE")
        self.assertEqual(payload[12:16], b"fmt ")
        self.assertEqual(struct.unpack_from("<I", payload, 16)[0], 40)
        self.assertEqual(struct.unpack_from("<H", payload, 20)[0], 0xFFFE)
        self.assertEqual(struct.unpack_from("<H", payload, 22)[0], CHANNEL_COUNT)
        self.assertEqual(struct.unpack_from("<I", payload, 24)[0], SAMPLE_RATE)
        self.assertEqual(struct.unpack_from("<H", payload, 34)[0], BITS_PER_SAMPLE)
        self.assertEqual(struct.unpack_from("<I", payload, 40)[0], CHANNEL_MASK_5_1_SURROUND)
        self.assertEqual(payload[60:64], b"data")

    def test_only_selected_channel_is_active_in_each_segment(self):
        tone_seconds = 0.01
        silence_seconds = 0.0
        payload = build_wave_bytes(tone_seconds=tone_seconds, silence_seconds=silence_seconds)
        data = payload[68:]
        segment_frames = round(tone_seconds * SAMPLE_RATE)
        probe_frame = segment_frames // 3
        for selected_channel in range(CHANNEL_COUNT):
            frame = selected_channel * segment_frames + probe_frame
            samples = struct.unpack_from("<6h", data, frame * 12)
            self.assertNotEqual(samples[selected_channel], 0)
            self.assertEqual(sum(abs(value) for index, value in enumerate(samples) if index != selected_channel), 0)

    def test_rejects_non_positive_tone_duration(self):
        with self.assertRaises(ValueError):
            build_wave_bytes(tone_seconds=0)


if __name__ == "__main__":
    unittest.main()
