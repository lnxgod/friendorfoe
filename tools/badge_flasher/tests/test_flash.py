from __future__ import annotations

import unittest

from tools.badge_flasher.flash import FlashEngine, FlashError
from tools.badge_flasher.models import UsbDevice


class FakeBundle:
    version = "0.64.76-badge-defcon34"
    root = __import__("pathlib").Path("/bundle")
    def layout(self, role):
        return {"flash_size": "8MB", "identity": {"version": self.version}, "parts": [
            {"offset": 0, "path": f"{role}/bootloader.bin"},
            {"offset": 0x8000, "path": f"{role}/partitions.bin"},
            {"offset": 0x10000, "path": f"{role}/firmware.bin"},
        ]}


class FlashTests(unittest.TestCase):
    def test_exact_offsets_and_sequential_write_verify_reset(self) -> None:
        calls = []
        def run(command):
            calls.append(command)
            if "write_flash" in command:
                return "MAC: E0:72:A1:F9:47:FC\nHash of data verified."
            if "verify_flash" in command:
                return "MAC: E0:72:A1:F9:47:FC\n" + "-- verify OK (digest matched)\n" * 3
            return "MAC: E0:72:A1:F9:47:FC\n"
        device = UsbDevice("E0:72:A1:F9:47:FC", "/dev/cu.x", "ESP32-S3", "v0.2", "8MB", "8MB")
        evidence = FlashEngine(run).flash_and_verify(device, FakeBundle(), "uplink")
        self.assertTrue(evidence.readback_verified)
        actions = [
            next(x for x in call if x in {"flash_id", "write_flash", "verify_flash", "run"})
            for call in calls
        ]
        self.assertEqual(actions, ["flash_id", "write_flash", "verify_flash", "run"])
        self.assertIn("0x10000", calls[1])
        self.assertIn("--erase-all", calls[1])
        before = calls[1].index("--before")
        self.assertEqual(calls[1][before + 1], "no_reset")

    def test_rejects_wrong_mac_before_readback(self) -> None:
        calls = []
        def run(command):
            calls.append(command)
            return "MAC: E0:72:A1:F9:49:84\nHash of data verified."
        device = UsbDevice("E0:72:A1:F9:47:FC", "p", "ESP32-S3", "v0.2", "8MB", "8MB")
        with self.assertRaisesRegex(FlashError, "different"):
            FlashEngine(run).flash_and_verify(device, FakeBundle(), "uplink")
        self.assertEqual(len(calls), 1)


if __name__ == "__main__": unittest.main()
