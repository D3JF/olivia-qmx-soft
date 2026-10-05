import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


PROJECT = Path(__file__).parent
SCRIPT = PROJECT / "olivia_waveform.py"


class OliviaWaveformTests(unittest.TestCase):
    def run_generator(self, *args, input_text=None):
        environment = os.environ.copy()
        environment["QT_QPA_PLATFORM"] = "offscreen"
        return subprocess.run(
            [sys.executable, str(SCRIPT), *args],
            cwd=PROJECT,
            env=environment,
            input=input_text,
            text=True,
            capture_output=True,
            check=False,
        )

    def test_generates_jpeg_from_message_argument(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "argument.jpg"
            result = self.run_generator("Hello, World!", "-o", str(output))
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertTrue(output.read_bytes().startswith(b"\xff\xd8\xff"))

    def test_generates_jpeg_from_stdin(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "stdin.jpg"
            result = self.run_generator("-o", str(output), input_text="CQ CQ 73!\n")
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertTrue(output.read_bytes().startswith(b"\xff\xd8\xff"))

    def test_rejects_empty_input(self):
        result = self.run_generator(input_text="")
        self.assertNotEqual(result.returncode, 0)


if __name__ == "__main__":
    unittest.main()
