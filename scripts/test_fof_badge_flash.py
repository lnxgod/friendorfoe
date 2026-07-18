#!/usr/bin/env python3
"""Small stdlib tests for badge-only flasher guardrails."""

from __future__ import annotations

import contextlib
import io
import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fof_badge_flash as flash


def _firmware_image(project: str, version: str, *markers: str) -> bytes:
    image = bytearray(0x20 + 112)
    image[0] = 0xE9
    struct.pack_into("<I", image, 0x20, 0xABCD5432)
    image[0x30:0x50] = version.encode("ascii").ljust(32, b"\x00")
    image[0x50:0x70] = project.encode("ascii").ljust(32, b"\x00")
    for marker in markers:
        image.extend(b"\x00" + marker.encode("ascii") + b"\x00")
    return bytes(image)


class BadgeFlashGuardrailTests(unittest.TestCase):
    def test_badge_platform_declares_exact_target_project_and_hardware(self) -> None:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]

        self.assertEqual(platform["scanner_name"], "scanner-s3-combo-fof_badge")
        self.assertEqual(platform["scanner_project"], "fof_badge_scanner")
        self.assertEqual(platform["uplink_name"], "uplink-s3-fof_badge")
        self.assertEqual(platform["uplink_project"], "fof_badge_uplink")
        self.assertEqual(platform["hardware_type"], "seeed_xiao_esp32s3")

    def test_badge_scanner_image_identity_is_verified_before_flash(self) -> None:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        version = "0.64.68-badge-live-follow"
        with tempfile.TemporaryDirectory() as temp_dir:
            image = Path(temp_dir) / "scanner.bin"
            image.write_bytes(_firmware_image(
                "fof_badge_scanner",
                version,
                "scanner-s3-combo-fof_badge",
                "seeed_xiao_esp32s3",
            ))

            flash.validate_firmware_artifact(
                image,
                target=platform["scanner_name"],
                project=platform["scanner_project"],
                hardware=platform["hardware_type"],
                version=version,
            )

    def test_badge_scanner_image_rejects_wrong_project_or_missing_hardware(self) -> None:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        version = "0.64.68-badge-live-follow"
        with tempfile.TemporaryDirectory() as temp_dir:
            image = Path(temp_dir) / "scanner.bin"
            image.write_bytes(_firmware_image(
                "fof_scanner",
                version,
                "scanner-s3-combo-fof_badge",
            ))

            with self.assertRaisesRegex(flash.FlashError, "project"):
                flash.validate_firmware_artifact(
                    image,
                    target=platform["scanner_name"],
                    project=platform["scanner_project"],
                    hardware=platform["hardware_type"],
                    version=version,
                )

            image.write_bytes(_firmware_image(
                "fof_badge_scanner",
                version,
                "scanner-s3-combo-fof_badge",
            ))
            with self.assertRaisesRegex(flash.FlashError, "hardware"):
                flash.validate_firmware_artifact(
                    image,
                    target=platform["scanner_name"],
                    project=platform["scanner_project"],
                    hardware=platform["hardware_type"],
                    version=version,
                )

    def test_artifact_guard_runs_before_manual_scanner_flash(self) -> None:
        source = Path(flash.__file__).read_text()
        main_body = source[source.index("def main()") :]

        self.assertIn("validate_firmware_artifact", source)
        self.assertLess(
            main_body.index("require_artifacts(platform, False"),
            main_body.index("manual_scanner_flow(args"),
        )

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
