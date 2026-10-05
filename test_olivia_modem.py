import unittest
import importlib.util
from pathlib import Path

import numpy as np

_CODEC_PATH = Path(__file__).with_name("olivia_modem.py")
_SPEC = importlib.util.spec_from_file_location("real_olivia_modem", _CODEC_PATH)
assert _SPEC is not None and _SPEC.loader is not None
_MODULE = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(_MODULE)
OliviaModem = _MODULE.OliviaModem


class OliviaModemTests(unittest.TestCase):
    def round_trip(self, message: str, chunk_size: int = 1024) -> str:
        encoder = OliviaModem()
        decoder = OliviaModem()
        samples = encoder.modulate(message)
        decoded = []
        for start in range(0, len(samples), chunk_size):
            text = decoder.demodulate(samples[start : start + chunk_size])
            if text:
                decoded.append(text)
        return "".join(decoded)

    def test_default_configuration(self):
        modem = OliviaModem()
        self.assertEqual(modem.tones, 8)
        self.assertEqual(modem.bandwidth, 250)
        self.assertEqual(modem.sample_rate, 8000)

    def test_rejects_unsupported_configuration(self):
        with self.assertRaises(ValueError):
            OliviaModem(3, 250, 8000)
        with self.assertRaises(ValueError):
            OliviaModem(8, 300, 8000)

    def test_supports_requested_configurations(self):
        for tones in (2, 4, 8, 16, 32, 64, 128, 256):
            for bandwidth in (125, 250, 500, 1000, 2000):
                modem = OliviaModem(tones, bandwidth, 8000)
                self.assertEqual(modem.tones, tones)
                self.assertEqual(modem.bandwidth, bandwidth)

    def test_empty_message_produces_finite_samples(self):
        samples = OliviaModem().modulate("")
        self.assertGreater(samples.size, 0)
        self.assertEqual(samples.dtype, np.float32)
        self.assertTrue(np.isfinite(samples).all())

    def test_round_trip_short_message(self):
        self.assertEqual(self.round_trip("Hello"), "Hello")

    def test_round_trip_with_other_tone_count_and_bandwidth(self):
        encoder = OliviaModem(16, 500)
        decoder = OliviaModem(16, 500)
        samples = encoder.modulate("Hello")
        self.assertEqual(decoder.demodulate(samples), "Hello")

    def test_round_trip_punctuation_and_spaces(self):
        message = "Hello, station A! CQ CQ 73?"
        self.assertEqual(self.round_trip(message), message)

    def test_round_trip_newlines(self):
        message = "Hello, station B\nCQ CQ 73?\n"
        self.assertEqual(self.round_trip(message), message)

    def test_round_trip_consecutive_newlines(self):
        message = "first\n\nthird"
        self.assertEqual(self.round_trip(message), message)

    def test_round_trip_boundary_characters(self):
        message = "".join(chr(value) for value in (0, 1, 63, 64, 126, 127))
        self.assertEqual(self.round_trip(message), message)

    def test_non_latin_text_is_replaced_deterministically(self):
        self.assertEqual(self.round_trip("café ☕"), "caf\0 ?")

    def test_long_message_round_trip(self):
        message = "Olivia MFSK over simulated QMX+; " * 20
        self.assertEqual(self.round_trip(message), message)

    def test_long_realistic_message_round_trip(self):
        message = (
            "Lorem ipsum dolor sit amet, consectetur adipiscing elit. "
            "Sed do eiusmod tempor incididunt ut labore et dolore magna "
            "aliqua enim ad minim veniam."
        )
        self.assertEqual(self.round_trip(message), message)

    def test_decoder_handles_one_sample_chunks(self):
        message = "Chunked RX"
        self.assertEqual(self.round_trip(message, chunk_size=1), message)

    def test_decoder_retains_partial_frame(self):
        modem = OliviaModem()
        samples = OliviaModem().modulate("ABC")
        frame_size = 64 * modem._symbol_samples
        self.assertEqual(modem.demodulate(samples[: frame_size - 1]), "")
        self.assertEqual(modem.demodulate(samples[frame_size - 1 :]), "ABC")


if __name__ == "__main__":
    unittest.main()
