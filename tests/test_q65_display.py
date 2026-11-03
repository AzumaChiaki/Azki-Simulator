"""Check production LCD conversion without a firmware or display server."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class Q65DisplayTest(unittest.TestCase):
    def test_native_precision_and_legacy_colors(self):
        compiler = shutil.which("clang") or shutil.which("cc")
        if not compiler:
            self.skipTest("C compiler required")
        (ROOT / "build").mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="q65-display-", dir=ROOT / "build") as tmp:
            binary = str(Path(tmp) / "display")
            subprocess.run([compiler, "-O1", "-g", "-fsanitize=address,undefined",
                            str(ROOT / "tests/test_q65_display.c"), "-o", binary],
                           check=True)
            result = subprocess.run([binary], check=True, capture_output=True, text=True)
            self.assertIn("256 native intensities and 65536 legacy colors preserved", result.stdout)


if __name__ == "__main__":
    unittest.main()
