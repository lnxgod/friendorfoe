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
    def test_handoff_clears_force_download_and_watchdog_resets_exact_mac(self) -> None:
        calls = []
        transcript = (
            "MAC: E0:72:A1:F9:47:FC\n"
            "Wrote 00000000, mask 00000001 to 6000812c\n"
            "Hard resetting with a watchdog...\n"
        )
        device = UsbDevice(
            "E0:72:A1:F9:47:FC", "/dev/cu.x", "ESP32-S3",
            "v0.2", "8MB", "8MB",
        )

        FlashEngine(lambda command: calls.append(command) or transcript) \
            .handoff_to_application(device)

        command = calls[0]
        self.assertIn("--no-stub", command)
        self.assertEqual(command[command.index("--before") + 1], "no_reset")
        self.assertEqual(
            command[command.index("--after") + 1],
            "watchdog_reset",
        )
        self.assertEqual(
            command[command.index("write_mem"):],
            ["write_mem", "0x6000812c", "0x0", "0x1"],
        )
        self.assertLess(command.index("--no-stub"), command.index("write_mem"))

    def test_handoff_accepts_esptool_5_force_download_receipt(self) -> None:
        transcript = (
            "MAC: E0:72:A1:F9:47:FC\n"
            "Wrote 0x00000000 with mask 0x00000001 to 0x6000812c.\n"
            "Hard resetting with a watchdog...\n"
        )
        device = UsbDevice(
            "E0:72:A1:F9:47:FC", "/dev/cu.x", "ESP32-S3",
            "v0.2", "8MB", "8MB",
        )

        FlashEngine(lambda _command: transcript).handoff_to_application(device)

    def test_handoff_rejects_incomplete_or_wrong_receipts(self) -> None:
        device = UsbDevice(
            "E0:72:A1:F9:47:FC", "/dev/cu.x", "ESP32-S3",
            "v0.2", "8MB", "8MB",
        )
        failures = (
            (
                "wrong MAC",
                "MAC: E0:72:A1:F9:49:84\n"
                "Wrote 00000000, mask 00000001 to 6000812c\n"
                "Hard resetting with a watchdog...\n",
            ),
            (
                "missing clear",
                "MAC: E0:72:A1:F9:47:FC\n"
                "Hard resetting with a watchdog...\n",
            ),
            (
                "missing reset",
                "MAC: E0:72:A1:F9:47:FC\n"
                "Wrote 00000000, mask 00000001 to 6000812c\n",
            ),
            (
                "duplicate clear",
                "MAC: E0:72:A1:F9:47:FC\n"
                "Wrote 00000000, mask 00000001 to 6000812c\n"
                "Wrote 00000000, mask 00000001 to 6000812c\n"
                "Hard resetting with a watchdog...\n",
            ),
        )

        for name, transcript in failures:
            with self.subTest(name=name):
                with self.assertRaises(FlashError):
                    FlashEngine(lambda command: transcript).handoff_to_application(
                        device
                    )

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
        self.assertNotIn("--verify", calls[1])
        before = calls[1].index("--before")
        self.assertEqual(calls[1][before + 1], "no_reset")

    def test_accepts_esptool_5_explicit_readback_receipts(self) -> None:
        calls = []

        def run(command):
            calls.append(command)
            if "write_flash" in command:
                return "MAC: E0:72:A1:F9:47:FC\nHash of data verified."
            if "verify_flash" in command:
                return (
                    "MAC: E0:72:A1:F9:47:FC\n"
                    + "Verification successful (digest matched).\n" * 3
                )
            return "MAC: E0:72:A1:F9:47:FC\n"

        device = UsbDevice(
            "E0:72:A1:F9:47:FC", "/dev/cu.x", "ESP32-S3",
            "v0.2", "8MB", "8MB",
        )
        evidence = FlashEngine(run).flash_and_verify(
            device, FakeBundle(), "uplink"
        )

        self.assertTrue(evidence.write_verified)
        self.assertTrue(evidence.readback_verified)
        write = next(command for command in calls if "write_flash" in command)
        self.assertNotIn("--verify", write)

    def test_rejects_wrong_mac_before_readback(self) -> None:
        calls = []
        def run(command):
            calls.append(command)
            return "MAC: E0:72:A1:F9:49:84\nHash of data verified."
        device = UsbDevice("E0:72:A1:F9:47:FC", "p", "ESP32-S3", "v0.2", "8MB", "8MB")
        with self.assertRaisesRegex(FlashError, "different"):
            FlashEngine(run).flash_and_verify(device, FakeBundle(), "uplink")
        self.assertEqual(len(calls), 1)

    def test_public_flash_failures_use_fixed_role_aliases(self) -> None:
        def run(command):
            if "write_flash" in command:
                return (
                    "MAC: E0:72:A1:F9:49:84\n"
                    "Hash of data verified."
                )
            return "MAC: E0:72:A1:F9:47:FC\n"

        device = UsbDevice(
            "E0:72:A1:F9:47:FC",
            "/dev/cu.x",
            "ESP32-S3",
            "v0.2",
            "8MB",
            "8MB",
        )
        with self.assertRaises(FlashError) as raised:
            FlashEngine(run).flash_and_verify(
                device, FakeBundle(), "uplink"
            )
        self.assertIn("UPLINK", str(raised.exception))
        self.assertNotIn(device.mac, str(raised.exception))


if __name__ == "__main__": unittest.main()
