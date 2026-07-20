from __future__ import annotations

import unittest
from pathlib import Path


class BuilderSourceTests(unittest.TestCase):
    def test_builder_uses_platformio_flasher_offsets_and_revalidates_output(self) -> None:
        source = (Path(__file__).with_name("build_badge_factory_bundle.py")).read_text()
        self.assertIn('flasher_args.json', source)
        self.assertIn('load_bundle(output', source)
        self.assertIn('compiled_partition_offsets', source)
        self.assertIn('ota_data_initial.bin', source)

    def test_builder_does_not_trust_generated_app_offset(self) -> None:
        source = (Path(__file__).with_name("build_badge_factory_bundle.py")).read_text()
        self.assertNotIn('int(flasher["app"]["offset"], 0)', source)


if __name__ == "__main__":
    unittest.main()
