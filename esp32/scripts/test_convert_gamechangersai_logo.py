from __future__ import annotations

import importlib.util
import tempfile
import unittest
from pathlib import Path


SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent.parent
CONVERTER_PATH = SCRIPT_DIR / "convert_gamechangersai_logo.py"
OFFICIAL_LOGO_PATH = (
    REPO_ROOT / "esp32" / "uplink" / "assets" / "gamechangersai-logo.png"
)


def load_converter():
    spec = importlib.util.spec_from_file_location(
        "convert_gamechangersai_logo", CONVERTER_PATH
    )
    if spec is None or spec.loader is None:
        raise RuntimeError("unable to load GameChangersAI logo converter")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class GameChangersLogoConverterTest(unittest.TestCase):
    def test_official_source_hash_is_exact_and_tampering_is_rejected(self):
        converter = load_converter()
        official = OFFICIAL_LOGO_PATH.read_bytes()

        self.assertEqual(
            "903d20f0b3d52c8b5b785686680cbb5e884ea17a5636fdf381e9752ade92efce",
            converter.EXPECTED_SHA256,
        )
        self.assertEqual(converter.EXPECTED_SHA256, converter.verify_source(official))
        with self.assertRaises(ValueError):
            converter.verify_source(official + b"changed")

    def test_generation_is_64_square_indexed_and_deterministic(self):
        converter = load_converter()

        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            header = tmp_path / "gamechangersai_logo.h"
            source = tmp_path / "gamechangersai_logo.c"
            result = converter.generate(OFFICIAL_LOGO_PATH, header, source, 64)
            first_header = header.read_bytes()
            first_source = source.read_bytes()

            self.assertEqual((64, 64, 4096), result)
            self.assertIn(b"GAMECHANGERSAI_LOGO_WIDTH 64u", first_header)
            self.assertIn(b"GAMECHANGERSAI_LOGO_HEIGHT 64u", first_header)
            self.assertIn(b"GAMECHANGERSAI_LOGO_PALETTE_SIZE 8u", first_header)
            self.assertIn(converter.SOURCE_URL.encode(), first_header)
            self.assertIn(converter.EXPECTED_SHA256.encode(), first_header)
            self.assertIn(b"const uint8_t gamechangersai_logo_levels", first_source)

            converter.generate(OFFICIAL_LOGO_PATH, header, source, 64)
            self.assertEqual(first_header, header.read_bytes())
            self.assertEqual(first_source, source.read_bytes())


if __name__ == "__main__":
    unittest.main()
