#!/usr/bin/env python3
"""Stdlib tests for the stubborn badge recovery catcher."""

from __future__ import annotations

import sys
import tempfile
import types
import unittest
from pathlib import Path
from unittest import mock

TOOLS_DIR = Path(__file__).resolve().parents[1] / "esp32" / "uplink" / "tools"
sys.path.insert(0, str(TOOLS_DIR))
if "serial" not in sys.modules:
    fake_serial = types.ModuleType("serial")
    fake_serial.Serial = object
    sys.modules["serial"] = fake_serial
import recover_fof_badge as recover


class BadgeRecoveryScriptTests(unittest.TestCase):
    def test_auto_detects_single_cu_usbmodem_port(self) -> None:
        def fake_glob(pattern: str) -> list[str]:
            if pattern == "/dev/cu.usbmodem*":
                return ["/dev/cu.usbmodem101"]
            if pattern == "/dev/tty.usbmodem*":
                return ["/dev/tty.usbmodem101"]
            return []

        with mock.patch.object(recover.glob, "glob", side_effect=fake_glob):
            self.assertEqual(recover.detect_usb_port(), "/dev/cu.usbmodem101")

    def test_auto_detect_rejects_ambiguous_cu_ports(self) -> None:
        def fake_glob(pattern: str) -> list[str]:
            if pattern == "/dev/cu.usbmodem*":
                return ["/dev/cu.usbmodem101", "/dev/cu.usbmodem201"]
            return []

        with mock.patch.object(recover.glob, "glob", side_effect=fake_glob):
            with self.assertRaisesRegex(RuntimeError, "multiple USB serial ports"):
                recover.detect_usb_port()

    def test_full_flash_command_uses_no_reset_and_all_images(self) -> None:
        with tempfile.TemporaryDirectory() as tmp_name:
            tmp = Path(tmp_name)
            build_dir = tmp / ".pio" / "build" / "badge"
            build_dir.mkdir(parents=True)
            for name in (
                "bootloader.bin",
                "partitions.bin",
                "ota_data_initial.bin",
                "firmware.bin",
            ):
                (build_dir / name).write_bytes(b"x")

            captured: dict[str, object] = {}

            def fake_run_stream(cmd: list[str], cwd: Path,
                                timeout: float | None = None) -> int:
                captured["cmd"] = cmd
                captured["cwd"] = cwd
                captured["timeout"] = timeout
                return 0

            with mock.patch.object(recover, "UPLINK_DIR", tmp), \
                 mock.patch.object(recover, "PENV_PYTHON", tmp / "python"), \
                 mock.patch.object(recover, "ESPTOOL", tmp / "esptool.py"), \
                 mock.patch.object(recover, "run_stream", side_effect=fake_run_stream):
                self.assertTrue(recover.esptool_write_flash(
                    "/dev/cu.usbmodem101", "badge", "no-reset", 0,
                    baud=460800, timeout_s=7))

            cmd = captured["cmd"]
            self.assertIsInstance(cmd, list)
            cmd_list = list(cmd)
            self.assertIn("--before", cmd_list)
            self.assertEqual(cmd_list[cmd_list.index("--before") + 1], "no-reset")
            self.assertIn("--connect-attempts", cmd_list)
            self.assertEqual(cmd_list[cmd_list.index("--connect-attempts") + 1], "0")
            self.assertIn("-z", cmd_list)
            self.assertIn("--flash-size", cmd_list)
            self.assertIn("8MB", cmd_list)
            for offset in ("0x0", "0x8000", "0xf000", "0x20000"):
                self.assertIn(offset, cmd_list)
            self.assertEqual(captured["timeout"], 7)

    def test_serial_helpers_request_status_clear_safe_mode_and_reboot(self) -> None:
        class FakeSerial:
            def __init__(self) -> None:
                self.writes: list[bytes] = []

            def write(self, data: bytes) -> int:
                self.writes.append(data)
                return len(data)

            def flush(self) -> None:
                pass

        ser = FakeSerial()
        recover.request_status(ser)  # type: ignore[arg-type]
        recover.clear_forced_safe_mode(ser)  # type: ignore[arg-type]
        recover.request_reboot(ser)  # type: ignore[arg-type]

        joined = b"".join(ser.writes)
        self.assertIn(b"FOF_STATUS", joined)
        self.assertIn(b'"cmd":"safe_mode"', joined)
        self.assertIn(b'"enabled":false', joined)
        self.assertIn(b'"reason":"post_flash_clear"', joined)
        self.assertIn(b"FOF_REBOOT", joined)

    def test_latest_status_from_text_returns_last_status(self) -> None:
        text = (
            'FOF_STATUS:{"mode":"usb_only","crash_count":3}\r\n'
            'noise\n'
            'FOF_STATUS:{"mode":"usb_only","crash_count":0,'
            '"recovery_mode":"normal"}\r\n'
        )
        status = recover.latest_status_from_text(text)
        self.assertIsNotNone(status)
        self.assertEqual(status["crash_count"], 0)
        self.assertEqual(status["recovery_mode"], "normal")


if __name__ == "__main__":
    unittest.main()
