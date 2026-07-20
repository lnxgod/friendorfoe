from __future__ import annotations

import unittest

from tools.badge_flasher.devices import DeviceBackend, DeviceError, parse_esptool_probe


PROBE = """Chip is ESP32-S3 (QFN56) (revision v0.2)
Features: WiFi, BLE, Embedded PSRAM 8MB (AP_3v3)
MAC: e0:72:a1:f9:47:fc
Detected flash size: 8MB
"""


class DeviceTests(unittest.TestCase):
    def test_parses_required_s3_identity(self) -> None:
        device = parse_esptool_probe(PROBE, "/dev/cu.usbmodem101", "0x14")
        self.assertEqual(device.mac, "E0:72:A1:F9:47:FC")
        self.assertEqual(device.chip, "ESP32-S3")
        self.assertEqual(device.revision, "v0.2")
        self.assertEqual(device.flash_size, "8MB")
        self.assertEqual(device.psram_size, "8MB")
        self.assertEqual(device.location_id, "0x14")

    def test_rejects_non_s3_or_missing_psram(self) -> None:
        with self.assertRaisesRegex(DeviceError, "ESP32-S3"):
            parse_esptool_probe(PROBE.replace("ESP32-S3", "ESP32-C3"), "p")
        with self.assertRaisesRegex(DeviceError, "PSRAM"):
            parse_esptool_probe(PROBE.replace("Embedded PSRAM 8MB", "No PSRAM"), "p")

    def test_lists_only_unique_cu_ports(self) -> None:
        values = {
            "/dev/cu.usbmodem*": ["/dev/cu.usbmodem2", "/dev/cu.usbmodem1"],
            "/dev/cu.usbserial*": ["/dev/cu.usbmodem1"],
        }
        backend = DeviceBackend(globber=lambda pattern: values.get(pattern, []))
        self.assertEqual(
            backend.list_candidate_ports(),
            ["/dev/cu.usbmodem1", "/dev/cu.usbmodem2"],
        )

    def test_rebind_follows_macs_when_ports_swap(self) -> None:
        rounds = iter([
            [
                PROBE.replace("f9:47:fc", "f9:49:84"),
                PROBE,
            ]
        ])
        outputs = next(rounds)
        backend = DeviceBackend(
            globber=lambda _pattern: ["/dev/cu.a", "/dev/cu.b"],
            command_runner=lambda _command: outputs.pop(0),
        )
        found = backend.rebind(
            {"E0:72:A1:F9:47:FC", "E0:72:A1:F9:49:84"}, timeout_s=0
        )
        self.assertEqual(found["E0:72:A1:F9:49:84"].port, "/dev/cu.a")
        self.assertEqual(found["E0:72:A1:F9:47:FC"].port, "/dev/cu.b")

    def test_rebind_rejects_unexpected_extra_s3(self) -> None:
        outputs = [PROBE, PROBE.replace("f9:47:fc", "f9:49:84")]
        backend = DeviceBackend(
            globber=lambda _pattern: ["/dev/cu.a", "/dev/cu.b"],
            command_runner=lambda _command: outputs.pop(0),
        )
        with self.assertRaisesRegex(DeviceError, "unexpected"):
            backend.rebind({"E0:72:A1:F9:47:FC"}, timeout_s=0)


if __name__ == "__main__":
    unittest.main()
