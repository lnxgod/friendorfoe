#!/usr/bin/env python3
"""Stdlib tests for the badge debug HTTP bridge."""

from __future__ import annotations

import sys
import types
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
if "serial" not in sys.modules:
    fake_serial = types.ModuleType("serial")
    fake_serial.Serial = object
    sys.modules["serial"] = fake_serial
import fof_badge_debug_bridge as bridge


class FakeSerial:
    def __init__(self) -> None:
        self.writes: list[bytes] = []

    def write(self, data: bytes) -> int:
        self.writes.append(data)
        return len(data)

    def flush(self) -> None:
        pass


class BadgeDebugBridgeTests(unittest.TestCase):
    def test_detects_single_usbmodem_port(self) -> None:
        def fake_glob(pattern: str) -> list[str]:
            if pattern == "/dev/cu.usbmodem*":
                return ["/dev/cu.usbmodem101"]
            return []

        with mock.patch.object(bridge.glob, "glob", side_effect=fake_glob):
            self.assertEqual(bridge.detect_usb_port(), "/dev/cu.usbmodem101")

    def test_rejects_ambiguous_usbmodem_ports(self) -> None:
        def fake_glob(pattern: str) -> list[str]:
            if pattern == "/dev/cu.usbmodem*":
                return ["/dev/cu.usbmodem101", "/dev/cu.usbmodem201"]
            return []

        with mock.patch.object(bridge.glob, "glob", side_effect=fake_glob):
            with self.assertRaisesRegex(RuntimeError, "multiple USB modem ports"):
                bridge.detect_usb_port()

    def test_parse_prefixed_json(self) -> None:
        parsed = bridge.parse_prefixed_json('FOF_CTL_OK:{"ok":true}', "FOF_CTL_OK:")
        self.assertEqual(parsed, {"ok": True})
        self.assertIsNone(bridge.parse_prefixed_json("noise", "FOF_CTL_OK:"))

    def test_control_command_forwards_fof_ctl(self) -> None:
        state = bridge.BridgeState(serial_port="/dev/cu.usbmodem101")
        state.ser = FakeSerial()  # type: ignore[assignment]
        state.lines.put('FOF_CTL_OK:{"message":"display nav updated"}')

        response = state.send_control({"cmd": "display_nav", "action": "next"})

        self.assertTrue(response["ok"])
        joined = b"".join(state.ser.writes)  # type: ignore[union-attr]
        self.assertIn(b"FOF_CTL:", joined)
        self.assertIn(b'"cmd":"display_nav"', joined)

    def test_upload_streams_begin_command_and_raw_bytes(self) -> None:
        state = bridge.BridgeState(serial_port="/dev/cu.usbmodem101")
        state.ser = FakeSerial()  # type: ignore[assignment]
        state.lines.put('FOF_FW_UPLOAD:{"ok":true,"stage":"begin"}')
        state.lines.put('FOF_FW_UPLOAD:{"ok":true,"stage":"done","size":4}')

        response = state.upload_firmware("scanner", "test", b"ABCD")

        self.assertTrue(response["ok"])
        joined = b"".join(state.ser.writes)  # type: ignore[union-attr]
        self.assertIn(b'"cmd":"fw_upload_begin"', joined)
        self.assertIn(b'"size":4', joined)
        self.assertIn(b"ABCD", joined)


if __name__ == "__main__":
    unittest.main()
