#!/usr/bin/env python3
"""Small stdlib tests for badge-only flasher guardrails."""

from __future__ import annotations

import contextlib
import io
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fof_badge_flash as flash


class BadgeFlashGuardrailTests(unittest.TestCase):
    def test_version_parser_accepts_v_prefix(self) -> None:
        self.assertTrue(flash.versions_match("v0.64.32", "0.64.32"))
        self.assertTrue(flash.versions_match("0.64.32", "v0.64.32"))

    def test_same_version_relay_rewrites_by_default(self) -> None:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        status = {
            "scanners": [{
                "uart": "ble",
                "connected": True,
                "board": platform["scanner_name"],
                "ver": "v0.64.32",
            }]
        }

        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(
                flash.choose_relay_slots(status, platform, ["ble"], "0.64.32",
                                         skip_current=False,
                                         label="test"),
                ["ble"],
            )

    def test_skip_current_opt_out_skips_same_version(self) -> None:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        status = {
            "scanners": [{
                "uart": "ble",
                "connected": True,
                "board": platform["scanner_name"],
                "ver": "v0.64.32",
            }]
        }

        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(
                flash.choose_relay_slots(status, platform, ["ble"], "0.64.32",
                                         skip_current=True,
                                         label="test"),
                [],
            )

    def test_missing_or_stale_slot_still_relays(self) -> None:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        status = {"scanners": []}
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(
                flash.choose_relay_slots(status, platform, ["wifi"], "0.64.32",
                                         skip_current=False,
                                         label="test"),
                ["wifi"],
            )

    def test_raw_uart_bad_status_is_recoverable(self) -> None:
        status = {
            "scanners": [{
                "uart": "wifi",
                "connected": False,
                "uart_raw_seen": True,
                "uart_raw_bytes": 2048,
                "uart_json_err": 12,
            }]
        }
        self.assertTrue(flash.scanner_status_has_relay_path(status, ["wifi"]))

    def test_missing_uart_path_is_physical_offline(self) -> None:
        status = {"scanners": [{"uart": "wifi", "connected": False}]}
        self.assertFalse(flash.scanner_status_has_relay_path(status, ["wifi"]))

    def test_relay_timeout_scales_with_firmware_size(self) -> None:
        self.assertGreaterEqual(flash.scanner_relay_timeout_s(1_200_000), 240)
        self.assertLessEqual(flash.scanner_relay_timeout_s(1_200_000), 900)

    def test_progress_lines_are_logged_while_waiting_for_final_relay(self) -> None:
        class FakeSerial:
            def __init__(self) -> None:
                self.lines = [
                    b'FOF_FW_RELAY_PROGRESS:{"uart":"ble","stage":"chunks","bytes":600000,"size":1200000,"percent":50,"chunks":586,"nacks":0,"retries":0,"elapsed_s":22}\n',
                    b'FOF_FW_RELAY:{"ok":true,"uart":"ble","stage":"done"}\n',
                ]

            def read(self, _n: int) -> bytes:
                return self.lines.pop(0) if self.lines else b""

        badge = flash.BadgeSerial("/dev/null", dry_run=False)
        badge.ser = FakeSerial()
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            body = badge.read_prefixed_json(
                "FOF_FW_RELAY:",
                2,
                progress_prefix="FOF_FW_RELAY_PROGRESS:",
            )
        self.assertTrue(body["ok"])
        self.assertIn("[relay] ble chunks 50%", out.getvalue())


if __name__ == "__main__":
    unittest.main()
