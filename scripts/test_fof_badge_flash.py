#!/usr/bin/env python3
"""Small stdlib tests for badge-only flasher guardrails."""

from __future__ import annotations

import binascii
import contextlib
import hashlib
import inspect
import io
import json
import struct
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

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


def _scanner_status(platform: dict, version: str, *,
                    hardware_id: str = "E0:72:A1:F9:48:58",
                    slot: str = "ble") -> dict:
    ble_primary = slot == "ble"
    expected_profile = "ble_primary" if ble_primary else "wifi_primary"
    return {
        "recovery_mode": "normal",
        "safe_mode": False,
        "usb_control_alive": True,
        "scanner_uart_alive": True,
        "scanners": [{
            "uart": slot,
            "connected": True,
            "board": platform["scanner_name"],
            "firmware_name": platform["scanner_name"],
            "app_project": platform["scanner_project"],
            "hardware_type": platform["hardware_type"],
            "hardware_id": hardware_id,
            "ver": version,
            "rollback_pending": False,
            "recovery_mode": "normal",
            "health": "ok",
            "ota_state": "idle",
            "slot_role": expected_profile,
            "expected_scan_profile": expected_profile,
            "scan_profile": expected_profile,
            "role_acked": True,
            "ble_scanning": ble_primary,
            "ble_host_active": ble_primary,
            "ble_host_synced": ble_primary,
            "wifi_paused": ble_primary,
        }],
    }


def _stage_receipt(platform: dict, version: str, data: bytes, slot_mask: int,
                   *, generation: int | None = None) -> dict:
    receipt = {
        "ok": True,
        "target": platform["scanner_name"],
        "name": platform["scanner_name"],
        "app_project": platform["scanner_project"],
        "project": platform["scanner_project"],
        "hardware_type": platform["hardware_type"],
        "hardware": platform["hardware_type"],
        "version": version,
        "size": len(data),
        "crc32": binascii.crc32(data) & 0xFFFFFFFF,
        "sha256": hashlib.sha256(data).hexdigest(),
        "slot_mask": slot_mask,
    }
    if generation is not None:
        receipt["generation"] = generation
    return receipt


class BadgeFlashGuardrailTests(unittest.TestCase):
    def test_badge_serial_exposes_safe_reconnect(self) -> None:
        self.assertTrue(hasattr(flash.BadgeSerial, "reconnect"))

    def test_badge_serial_reconnect_closes_waits_reopens_and_pings(self) -> None:
        badge = flash.BadgeSerial("/dev/old-uplink", dry_run=False)
        events: list[str] = []
        badge._close_serial = lambda: events.append("close")  # type: ignore[method-assign]
        badge._open_serial = lambda: events.append("open")  # type: ignore[method-assign]
        badge._wait_ping_once = (  # type: ignore[method-assign]
            lambda timeout_s: events.append(f"ping:{timeout_s}")
        )

        with mock.patch.object(
            flash,
            "wait_for_port",
            return_value="/dev/new-uplink",
        ) as wait_port:
            badge.reconnect(timeout_s=12)

        self.assertEqual(events, ["close", "open", "ping:12"])
        self.assertEqual(badge.port, "/dev/new-uplink")
        wait_port.assert_called_once_with("/dev/old-uplink", timeout_s=12)

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

    def test_version_match_rejects_truncated_release_identity(self) -> None:
        self.assertFalse(
            flash.versions_match(
                "0.64.68-badge", "0.64.68-badge-live-follow"
            )
        )

    def test_badge_network_firmware_transport_is_refused(self) -> None:
        with self.assertRaisesRegex(flash.FlashError, "USB.*UART"):
            flash.require_usb_firmware_transport("ap")
        with self.assertRaisesRegex(flash.FlashError, "USB.*UART"):
            flash.require_usb_firmware_transport("lan")
        flash.require_usb_firmware_transport("usb")

    def test_same_version_relay_is_skipped_by_default(self) -> None:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        status = _scanner_status(platform, "v0.64.32")

        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(
                flash.choose_relay_slots(status, platform, ["ble"], "0.64.32",
                                         recovery_rewrite_same_version=False,
                                         label="test"),
                [],
            )

    def test_same_version_relay_requires_explicit_recovery_flag(self) -> None:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        status = _scanner_status(platform, "v0.64.32")

        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(
                flash.choose_relay_slots(status, platform, ["ble"], "0.64.32",
                                         recovery_rewrite_same_version=True,
                                         label="test"),
                ["ble"],
            )

    def test_downgrade_is_rejected_before_staging(self) -> None:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        status = _scanner_status(platform, "0.64.69", slot="wifi")

        with self.assertRaisesRegex(flash.FlashError, "downgrade"):
            flash.reject_scanner_downgrades(status, ["wifi"], "0.64.68")

    def test_automatic_preflight_classifies_newer_peer_without_aborting_older(self) -> None:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        status = _scanner_status(
            platform, "0.64.70-badge-next", slot="wifi",
            hardware_id="14:C1:9F:52:CA:B0",
        )
        status["scanners"].extend(
            _scanner_status(
                platform, "0.64.68-badge-live-follow", slot="ble"
            )["scanners"]
        )

        self.assertEqual(
            flash.scanner_update_newer_slots(
                status,
                ["ble", "wifi"],
                "0.64.69-badge-defcon34",
                require_connected=False,
            ),
            {"wifi"},
        )

    def test_automatic_preflight_can_stage_with_temporarily_offline_slot(self) -> None:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        status = _scanner_status(
            platform, "0.64.68-badge-live-follow", slot="wifi"
        )

        self.assertEqual(
            flash.scanner_update_newer_slots(
                status,
                ["ble", "wifi"],
                "0.64.69-badge-defcon34",
                require_connected=False,
            ),
            set(),
        )

    def test_equal_numeric_core_with_different_suffix_fails_closed(self) -> None:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        status = _scanner_status(
            platform, "0.64.68-field-hotfix", slot="wifi"
        )

        with self.assertRaisesRegex(flash.FlashError, "unordered"):
            flash.reject_scanner_downgrades(
                status, ["wifi"], "0.64.68-badge-live-follow"
            )

    def test_usb_stage_includes_and_verifies_sha256(self) -> None:
        platform = dict(flash.PLATFORMS["badge-trio-xiao-s3"])
        version = "0.64.68-badge-live-follow"
        with tempfile.TemporaryDirectory() as temp_dir:
            image = Path(temp_dir) / "scanner.bin"
            data = _firmware_image(
                platform["scanner_project"],
                version,
                platform["scanner_name"],
                platform["hardware_type"],
            )
            image.write_bytes(data)
            platform["scanner_bin"] = image
            sha256 = hashlib.sha256(data).hexdigest()
            crc32 = binascii.crc32(data) & 0xFFFFFFFF

            class FakeRawSerial:
                def write(self, _data: bytes) -> None:
                    return None

                def flush(self) -> None:
                    return None

            badge = flash.BadgeSerial("/dev/null", dry_run=False)
            badge.ser = FakeRawSerial()
            lines: list[str] = []
            replies = [
                _stage_receipt(platform, version, data, 1),
                _stage_receipt(
                    platform, version, data, 1, generation=17
                ),
            ]
            badge.write_line = lines.append  # type: ignore[method-assign]
            badge.read_prefixed_json = (  # type: ignore[method-assign]
                lambda *_args, **_kwargs: replies.pop(0)
            )

            with contextlib.redirect_stdout(io.StringIO()):
                badge.stage_scanner_firmware(platform, version, ["ble"])

            begin = json.loads(lines[0].removeprefix("FOF_CTL:"))
            self.assertEqual(begin["sha256"], sha256)
            self.assertEqual(begin["slot_mask"], 1)

    def test_usb_stage_rejects_incomplete_begin_receipt_before_first_byte(self) -> None:
        platform = dict(flash.PLATFORMS["badge-trio-xiao-s3"])
        version = "0.64.69-badge-defcon34"
        with tempfile.TemporaryDirectory() as temp_dir:
            image = Path(temp_dir) / "scanner.bin"
            data = _firmware_image(
                platform["scanner_project"],
                version,
                platform["scanner_name"],
                platform["hardware_type"],
            )
            image.write_bytes(data)
            platform["scanner_bin"] = image

            class FakeRawSerial:
                def __init__(self) -> None:
                    self.writes: list[bytes] = []

                def write(self, payload: bytes) -> None:
                    self.writes.append(payload)

                def flush(self) -> None:
                    return None

            raw = FakeRawSerial()
            badge = flash.BadgeSerial("/dev/null", dry_run=False)
            badge.ser = raw
            badge.write_line = lambda _line: None  # type: ignore[method-assign]
            incomplete = _stage_receipt(platform, version, data, 1)
            del incomplete["slot_mask"]
            replies = [
                incomplete,
                _stage_receipt(
                    platform, version, data, 1, generation=18
                ),
            ]
            badge.read_prefixed_json = (  # type: ignore[method-assign]
                lambda *_args, **_kwargs: replies.pop(0)
            )

            with self.assertRaisesRegex(
                flash.FlashError, "begin.*slot_mask"
            ):
                badge.stage_scanner_firmware(platform, version, ["ble"])

            self.assertEqual(raw.writes, [])

    def test_usb_stage_returns_generation_bound_final_receipt(self) -> None:
        platform = dict(flash.PLATFORMS["badge-trio-xiao-s3"])
        version = "0.64.69-badge-defcon34"
        with tempfile.TemporaryDirectory() as temp_dir:
            image = Path(temp_dir) / "scanner.bin"
            data = _firmware_image(
                platform["scanner_project"],
                version,
                platform["scanner_name"],
                platform["hardware_type"],
            )
            image.write_bytes(data)
            platform["scanner_bin"] = image
            final_receipt = _stage_receipt(
                platform, version, data, 3, generation=44
            )
            badge = flash.BadgeSerial("/dev/null", dry_run=False)
            badge.ser = SimpleNamespace(
                write=lambda _data: None, flush=lambda: None
            )
            badge.write_line = lambda _line: None  # type: ignore[method-assign]
            replies = [
                _stage_receipt(platform, version, data, 3),
                final_receipt,
            ]
            badge.read_prefixed_json = (  # type: ignore[method-assign]
                lambda *_args, **_kwargs: replies.pop(0)
            )

            with contextlib.redirect_stdout(io.StringIO()):
                got = badge.stage_scanner_firmware(
                    platform, version, ["ble", "wifi"]
                )

            self.assertEqual(got, final_receipt)

    def test_usb_stage_revalidates_target_identity_before_first_byte(self) -> None:
        platform = dict(flash.PLATFORMS["badge-trio-xiao-s3"])
        version = "0.64.68-badge-live-follow"
        with tempfile.TemporaryDirectory() as temp_dir:
            image = Path(temp_dir) / "scanner.bin"
            image.write_bytes(_firmware_image(
                "fof_scanner",
                version,
                platform["scanner_name"],
                platform["hardware_type"],
            ))
            platform["scanner_bin"] = image
            badge = flash.BadgeSerial("/dev/null", dry_run=False)
            lines: list[str] = []
            badge.write_line = lines.append  # type: ignore[method-assign]
            data = image.read_bytes()
            badge.ser = SimpleNamespace(write=lambda _data: None, flush=lambda: None)
            replies = [
                {"ok": True},
                {
                    "ok": True,
                    "name": platform["scanner_name"],
                    "version": version,
                    "size": len(data),
                    "crc32": binascii.crc32(data) & 0xFFFFFFFF,
                },
            ]
            badge.read_prefixed_json = (  # type: ignore[method-assign]
                lambda *_args, **_kwargs: replies.pop(0)
            )

            with self.assertRaisesRegex(flash.FlashError, "project"):
                badge.stage_scanner_firmware(platform, version, ["ble", "wifi"])

            self.assertEqual(lines, [])

    def test_usb_flow_stages_once_without_manual_relay(self) -> None:
        platform = dict(flash.PLATFORMS["badge-trio-xiao-s3"])
        target_version = "0.64.68-badge-live-follow"
        before = _scanner_status(platform, "0.64.67", slot="ble")
        args = SimpleNamespace(
            port="/dev/fake-uplink",
            platform="badge-trio-xiao-s3",
            dry_run=False,
            skip_command_probe=False,
            recovery_rewrite_same_version=False,
        )
        stage_proof = {
            "generation": 1,
            "sha256": "a" * 64,
            "slot_mask": 1,
        }

        class FakeBadge:
            staged = 0
            relayed = 0
            staged_slots: list[str] = []

            def __init__(self, *_args, **_kwargs) -> None:
                pass

            def __enter__(self):
                return self

            def __exit__(self, *_args) -> None:
                return None

            def wait_ping(self) -> None:
                return None

            def status(self) -> dict:
                return {
                    "version": target_version,
                    "firmware_name": platform["uplink_name"],
                    "app_project": platform["uplink_project"],
                    "hardware_type": platform["hardware_type"],
                }

            def stage_scanner_firmware(self, _platform, _version, slots) -> dict:
                FakeBadge.staged += 1
                FakeBadge.staged_slots = list(slots)
                return stage_proof

            def relay_scanner(self, *_args, **_kwargs) -> None:
                FakeBadge.relayed += 1

        wait_calls: list[tuple[dict[str, str], dict | None]] = []

        def fake_wait(_badge, _platform, _slots, _version, *,
                      expected_hardware_ids,
                      expected_stage_receipt=None,
                      **_kwargs) -> None:
            wait_calls.append((expected_hardware_ids, expected_stage_receipt))

        with mock.patch.object(flash, "BadgeSerial", FakeBadge), \
             mock.patch.object(flash, "wait_for_scanner_status_usb",
                               return_value=before), \
             mock.patch.object(flash, "wait_for_scanners_usb",
                               side_effect=fake_wait):
            with contextlib.redirect_stdout(io.StringIO()):
                flash.usb_flow(args, platform, False, ["ble"], target_version)

        self.assertEqual(FakeBadge.staged, 1)
        self.assertEqual(FakeBadge.relayed, 0)
        self.assertEqual(FakeBadge.staged_slots, ["ble"])
        self.assertEqual(
            wait_calls,
            [({"ble": "e0:72:a1:f9:48:58"}, stage_proof)],
        )

    def test_scanner_only_usb_flow_requires_current_uplink_before_staging(self) -> None:
        platform = dict(flash.PLATFORMS["badge-trio-xiao-s3"])
        target_version = "0.64.69-badge-defcon34"
        scanner_status = _scanner_status(platform, "0.64.68", slot="ble")
        args = SimpleNamespace(
            port="/dev/fake-uplink",
            platform="badge-trio-xiao-s3",
            dry_run=False,
            skip_command_probe=False,
            recovery_rewrite_same_version=False,
        )

        class FakeBadge:
            staged = False

            def __init__(self, *_args, **_kwargs) -> None:
                pass

            def __enter__(self):
                return self

            def __exit__(self, *_args) -> None:
                return None

            def wait_ping(self) -> None:
                return None

            def status(self) -> dict:
                return {
                    "version": "0.64.68-badge-live-follow",
                    "firmware_name": platform["uplink_name"],
                    "app_project": platform["uplink_project"],
                    "hardware_type": platform["hardware_type"],
                }

            def stage_scanner_firmware(self, *_args) -> dict:
                FakeBadge.staged = True
                return {"generation": 1}

        with mock.patch.object(flash, "BadgeSerial", FakeBadge), \
             mock.patch.object(
                 flash, "wait_for_scanner_status_usb",
                 return_value=scanner_status,
             ), \
             mock.patch.object(flash, "wait_for_scanners_usb"):
            with self.assertRaisesRegex(flash.FlashError, "uplink version"):
                flash.usb_flow(
                    args, platform, False, ["ble"], target_version
                )

        self.assertFalse(FakeBadge.staged)

    def test_usb_uplink_flash_refuses_downgrade_before_uart_flash(self) -> None:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        args = SimpleNamespace(
            port="/dev/fake-uplink",
            platform="badge-trio-xiao-s3",
            dry_run=False,
            recovery_rewrite_same_version=False,
        )
        status = {
            "version": "0.64.69",
            "firmware_name": platform["uplink_name"],
            "app_project": platform["uplink_project"],
            "hardware_type": platform["hardware_type"],
        }

        class FakeBadge:
            def __init__(self, *_args, **_kwargs) -> None:
                pass

            def __enter__(self):
                return self

            def __exit__(self, *_args) -> None:
                return None

            def wait_ping(self) -> None:
                return None

            def status(self) -> dict:
                return status

        with mock.patch.object(flash, "BadgeSerial", FakeBadge), \
             mock.patch.object(flash, "flash_uplink_usb") as uplink_flash, \
             mock.patch.object(flash, "wait_for_port",
                               return_value="/dev/fake-uplink"):
            with self.assertRaisesRegex(flash.FlashError, "downgrade"):
                flash.usb_flow(args, platform, True, [], "0.64.68")

        uplink_flash.assert_not_called()

    def test_network_path_is_refused_before_scanner_status_or_upload(self) -> None:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        status = _scanner_status(platform, "0.64.69", slot="wifi")
        args = SimpleNamespace(
            transport="ap",
            host=None,
            node=None,
            backend="http://127.0.0.1:8000",
            port=None,
            platform="badge-trio-xiao-s3",
            dry_run=False,
            network_ttl_s=900,
            skip_command_probe=False,
            skip_current=False,
            recovery_rewrite_same_version=False,
        )

        with mock.patch.object(flash, "wait_http_status", return_value={}), \
             mock.patch.object(flash, "wait_for_scanner_status_network",
                               return_value=status), \
             mock.patch.object(flash, "wait_for_scanners_network"):
            with self.assertRaisesRegex(flash.FlashError, "USB.*UART"):
                flash.network_flow(
                    args, platform, False, ["wifi"], "0.64.68"
                )

    def test_network_uplink_path_is_refused_before_http_ota(self) -> None:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        args = SimpleNamespace(
            transport="ap",
            host=None,
            node=None,
            backend="http://127.0.0.1:8000",
            port=None,
            platform="badge-trio-xiao-s3",
            dry_run=False,
            network_ttl_s=900,
            recovery_rewrite_same_version=False,
        )
        status = {
            "version": "0.64.69",
            "firmware_name": platform["uplink_name"],
            "app_project": platform["uplink_project"],
            "hardware_type": platform["hardware_type"],
        }

        with mock.patch.object(flash, "wait_http_status",
                               return_value=status), \
             mock.patch.object(flash, "flash_uplink_network") as uplink_ota:
            with self.assertRaisesRegex(flash.FlashError, "USB.*UART"):
                flash.network_flow(
                    args, platform, True, [], "0.64.68"
                )

        uplink_ota.assert_not_called()

    def test_network_path_cannot_bypass_usb_uart_transport_on_raw_uart(self) -> None:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        status = {
            "scanners": [{
                "uart": "wifi",
                "connected": False,
                "uart_raw_seen": True,
                "uart_raw_bytes": 2048,
            }]
        }
        args = SimpleNamespace(
            transport="ap",
            host=None,
            node=None,
            backend="http://127.0.0.1:8000",
            port=None,
            platform="badge-trio-xiao-s3",
            dry_run=False,
            network_ttl_s=900,
            skip_command_probe=False,
            skip_current=False,
            recovery_rewrite_same_version=False,
        )

        with mock.patch.object(flash, "wait_http_status", return_value={}), \
             mock.patch.object(flash, "wait_for_scanner_status_network",
                               return_value=status), \
             mock.patch.object(flash, "upload_scanner_network"), \
             mock.patch.object(flash, "relay_scanner_network"), \
             mock.patch.object(flash, "wait_for_scanners_network"):
            with self.assertRaisesRegex(flash.FlashError, "USB.*UART"):
                flash.network_flow(
                    args, platform, False, ["wifi"], "0.64.68"
                )

    def test_network_upgrade_cannot_reach_relay(self) -> None:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        status = _scanner_status(platform, "0.64.67", slot="wifi")
        args = SimpleNamespace(
            transport="ap",
            host=None,
            node=None,
            backend="http://127.0.0.1:8000",
            port=None,
            platform="badge-trio-xiao-s3",
            dry_run=False,
            network_ttl_s=900,
            skip_command_probe=False,
            recovery_rewrite_same_version=False,
        )

        with mock.patch.object(flash, "wait_http_status", return_value={}), \
             mock.patch.object(flash, "wait_for_scanner_status_network",
                               return_value=status), \
             mock.patch.object(flash, "upload_scanner_network"), \
             mock.patch.object(flash, "relay_scanner_network") as relay, \
             mock.patch.object(flash, "wait_for_scanners_network"):
            with self.assertRaisesRegex(flash.FlashError, "USB.*UART"):
                flash.network_flow(
                    args, platform, False, ["wifi"], "0.64.68"
                )

        relay.assert_not_called()

    def test_direct_scanner_flash_requires_uplink_preflight(self) -> None:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        args = SimpleNamespace(
            manual_scanner="wifi",
            port="/dev/fake-scanner",
            verify_port=None,
            platform="badge-trio-xiao-s3",
            dry_run=False,
            recovery_rewrite_same_version=False,
        )

        with mock.patch.object(flash, "flash_scanner_usb") as direct_flash:
            with self.assertRaisesRegex(
                flash.FlashError, "cannot prove downgrade safety"
            ):
                flash.manual_scanner_flow(args, platform, "0.64.68")

        direct_flash.assert_not_called()

    def test_direct_scanner_flash_rejects_downgrade_before_flash(self) -> None:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        status = _scanner_status(platform, "0.64.69", slot="wifi")
        args = SimpleNamespace(
            manual_scanner="wifi",
            port="/dev/fake-scanner",
            verify_port="/dev/fake-uplink",
            platform="badge-trio-xiao-s3",
            dry_run=False,
            recovery_rewrite_same_version=False,
        )

        class FakeBadge:
            def __init__(self, *_args, **_kwargs) -> None:
                pass

            def __enter__(self):
                return self

            def __exit__(self, *_args) -> None:
                return None

            def wait_ping(self) -> None:
                return None

            def status(self) -> dict:
                return status

        with mock.patch.object(flash, "BadgeSerial", FakeBadge), \
             mock.patch.object(flash, "flash_scanner_usb") as direct_flash, \
             mock.patch.object(flash, "wait_for_scanners_usb"):
            with self.assertRaisesRegex(flash.FlashError, "downgrade"):
                flash.manual_scanner_flow(args, platform, "0.64.68")

        direct_flash.assert_not_called()

    def test_direct_scanner_verification_does_not_require_auto_coordinator_generation(self) -> None:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        version = "0.64.69-badge-defcon34"

        class FakeBadge:
            def status(self) -> dict:
                return _scanner_status(platform, version, slot="ble")

        with mock.patch.object(flash, "verify_auto_update_convergence") as auto_verify:
            flash.wait_for_scanners_usb(
                FakeBadge(),
                platform,
                ["ble"],
                version,
                timeout_s=1,
                require_auto_update=False,
            )

        auto_verify.assert_not_called()

    def test_scanner_wait_accepts_exact_stage_receipt_proof(self) -> None:
        parameters = inspect.signature(flash.wait_for_scanners_usb).parameters

        self.assertIn("expected_stage_receipt", parameters)

    def test_scanner_wait_refuses_auto_proof_without_stage_receipt(self) -> None:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]

        with self.assertRaisesRegex(flash.FlashError, "stage receipt"):
            flash.wait_for_scanners_usb(
                SimpleNamespace(),
                platform,
                ["ble"],
                "0.64.69-badge-defcon34",
                timeout_s=0,
            )

    def test_scanner_wait_passes_stage_receipt_to_auto_verifier(self) -> None:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        version = "0.64.69-badge-defcon34"
        status = _scanner_status(platform, version, slot="ble")
        receipt = _stage_receipt(
            platform, version, b"scanner-image", 1, generation=8
        )

        class FakeBadge:
            def status(self) -> dict:
                return status

        with mock.patch.object(
            flash, "verify_auto_update_convergence"
        ) as auto_verify:
            flash.wait_for_scanners_usb(
                FakeBadge(),
                platform,
                ["ble"],
                version,
                timeout_s=1,
                expected_stage_receipt=receipt,
            )

        auto_verify.assert_called_once_with(
            status,
            ["ble"],
            expected_stage_receipt=receipt,
        )

    def test_scanner_wait_accepts_newer_only_from_bound_coordinator_state(self) -> None:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        version = "0.64.69-badge-defcon34"
        status = _scanner_status(platform, "0.64.70-badge-next", slot="wifi")
        receipt = _stage_receipt(
            platform, version, b"scanner-image", 2, generation=8
        )
        status["firmware_store"] = {
            "stored": True,
            "target": receipt["target"],
            "app_project": receipt["app_project"],
            "hardware_type": receipt["hardware_type"],
            "version": receipt["version"],
            "size": receipt["size"],
            "crc32": receipt["crc32"],
            "sha256": receipt["sha256"],
            "generation": 8,
            "auto_update": {
                "generation": 8,
                "target_slot_mask": 2,
                "pending_mask": 0,
                "worker_running": False,
                "readiness_probes": [0, 1],
                "scanners": [
                    {"slot": 0, "attempts": 0, "state": "excluded"},
                    {"slot": 1, "attempts": 0, "state": "current"},
                ],
            },
        }

        class FakeBadge:
            def status(self) -> dict:
                return status

        with mock.patch.object(flash.time, "sleep"), \
             mock.patch.object(flash.time, "time", side_effect=[0, 0, 2, 2]):
            with self.assertRaisesRegex(flash.FlashError, "version"):
                flash.wait_for_scanners_usb(
                    FakeBadge(),
                    platform,
                    ["wifi"],
                    version,
                    timeout_s=1,
                    expected_stage_receipt=receipt,
                    allowed_newer_slots={"wifi"},
                )

        status["firmware_store"]["auto_update"]["scanners"][1]["state"] = (
            "newer_skipped"
        )
        flash.wait_for_scanners_usb(
            FakeBadge(),
            platform,
            ["wifi"],
            version,
            timeout_s=1,
            expected_stage_receipt=receipt,
        )

    def test_scanner_wait_reconnects_after_transient_status_failure(self) -> None:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        version = "0.64.69-badge-defcon34"
        converged = _scanner_status(platform, version, slot="ble")

        class FakeBadge:
            def __init__(self) -> None:
                self.status_calls = 0
                self.reconnect_calls = 0

            def status(self) -> dict:
                self.status_calls += 1
                if self.status_calls == 1:
                    raise flash.FlashError("USB serial disconnected")
                return converged

            def reconnect(self, timeout_s: int = 15) -> None:
                self.reconnect_calls += 1

        badge = FakeBadge()
        with mock.patch.object(flash.time, "sleep"):
            flash.wait_for_scanners_usb(
                badge,
                platform,
                ["ble"],
                version,
                timeout_s=1,
                require_auto_update=False,
            )

        self.assertEqual(badge.reconnect_calls, 1)

    def test_scanner_preflight_reconnects_after_transient_status_failure(self) -> None:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        ready = _scanner_status(
            platform, "0.64.68-badge-live-follow", slot="ble"
        )

        class FakeBadge:
            def __init__(self) -> None:
                self.status_calls = 0
                self.reconnect_calls = 0

            def status(self) -> dict:
                self.status_calls += 1
                if self.status_calls == 1:
                    raise flash.FlashError("USB serial disconnected")
                return ready

            def reconnect(self, timeout_s: int = 15) -> None:
                self.reconnect_calls += 1

        badge = FakeBadge()
        with mock.patch.object(flash.time, "sleep"):
            got = flash.wait_for_scanner_status_usb(
                badge, ["ble"], timeout_s=1
            )

        self.assertEqual(got, ready)
        self.assertEqual(badge.reconnect_calls, 1)

    def test_scanner_preflight_waits_past_connected_boot_placeholder(self) -> None:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        booting = {
            "scanners": [{
                "uart": "ble",
                "connected": True,
                "slot_role": "ble_primary",
                "role_acked": False,
                "firmware_name": "",
                "app_project": "",
                "hardware_type": "",
                "hardware_id": "",
                "ver": "",
            }]
        }
        ready = _scanner_status(
            platform, "0.64.68-badge-live-follow", slot="ble"
        )
        self.assertFalse(
            flash.scanner_status_has_relay_path(booting, ["ble"])
        )

        class FakeBadge:
            def __init__(self) -> None:
                self.statuses = [booting, ready]
                self.status_calls = 0

            def status(self) -> dict:
                self.status_calls += 1
                return self.statuses.pop(0)

        badge = FakeBadge()
        with mock.patch.object(flash.time, "sleep"):
            got = flash.wait_for_scanner_status_usb(
                badge, ["ble"], timeout_s=1
            )

        self.assertEqual(got, ready)
        self.assertEqual(badge.status_calls, 2)

    def test_scanner_convergence_requires_exact_identity_and_mac(self) -> None:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        version = "0.64.68-badge-live-follow"
        status = _scanner_status(platform, version)

        flash.verify_scanners(
            status,
            platform,
            ["ble"],
            version,
            expected_hardware_ids={"ble": "e0:72:a1:f9:48:58"},
        )

        status["scanners"][0]["app_project"] = "fof_scanner"
        with self.assertRaisesRegex(flash.FlashError, "project"):
            flash.verify_scanners(
                status,
                platform,
                ["ble"],
                version,
                expected_hardware_ids={"ble": "e0:72:a1:f9:48:58"},
            )

    def test_scanner_convergence_rejects_mac_swap(self) -> None:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        version = "0.64.68-badge-live-follow"
        status = _scanner_status(platform, version)

        with self.assertRaisesRegex(flash.FlashError, "hardware id"):
            flash.verify_scanners(
                status,
                platform,
                ["ble"],
                version,
                expected_hardware_ids={"ble": "14:c1:9f:52:ca:b0"},
            )

    def test_scanner_convergence_rejects_duplicate_requested_macs(self) -> None:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        version = "0.64.69-badge-defcon34"
        status = _scanner_status(platform, version, slot="ble")
        status["scanners"].extend(
            _scanner_status(
                platform,
                version,
                slot="wifi",
                hardware_id="E0:72:A1:F9:48:58",
            )["scanners"]
        )

        with self.assertRaisesRegex(flash.FlashError, "unique"):
            flash.verify_scanners(status, platform, ["ble", "wifi"], version)

    def test_scanner_convergence_requires_exact_untruncated_version(self) -> None:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        version = "0.64.68-badge-live-follow"
        status = _scanner_status(platform, "0.64.68-badge")

        with self.assertRaisesRegex(flash.FlashError, "version"):
            flash.verify_scanners(status, platform, ["ble"], version)

    def test_scanner_convergence_allows_only_a_proven_still_newer_skip(self) -> None:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        target = "0.64.69-badge-defcon34"
        status = _scanner_status(platform, "0.64.70-badge-next", slot="wifi")

        flash.verify_scanners(
            status,
            platform,
            ["wifi"],
            target,
            allowed_newer_slots={"wifi"},
        )
        with self.assertRaisesRegex(flash.FlashError, "version"):
            flash.verify_scanners(status, platform, ["wifi"], target)

        status["scanners"][0]["ver"] = "0.64.68-badge-live-follow"
        with self.assertRaisesRegex(flash.FlashError, "newer"):
            flash.verify_scanners(
                status,
                platform,
                ["wifi"],
                target,
                allowed_newer_slots={"wifi"},
            )

    def test_scanner_convergence_requires_rollback_clear_and_normal_health(self) -> None:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        version = "0.64.68-badge-live-follow"
        status = _scanner_status(platform, version)
        status["scanners"][0]["rollback_pending"] = True

        with self.assertRaisesRegex(flash.FlashError, "rollback"):
            flash.verify_scanners(status, platform, ["ble"], version)

        status["scanners"][0]["rollback_pending"] = False
        status["scanners"][0]["health"] = "cmd_wait"
        with self.assertRaisesRegex(flash.FlashError, "health"):
            flash.verify_scanners(status, platform, ["ble"], version)

    def test_scanner_convergence_requires_exact_slot_role_and_live_radios(self) -> None:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        version = "0.64.69-badge-defcon34"

        for slot in ("ble", "wifi"):
            status = _scanner_status(platform, version, slot=slot)
            flash.verify_scanners(status, platform, [slot], version)

            status["scanners"][0]["role_acked"] = False
            with self.assertRaisesRegex(flash.FlashError, "role"):
                flash.verify_scanners(status, platform, [slot], version)

        ble = _scanner_status(platform, version, slot="ble")
        ble["scanners"][0]["ble_scanning"] = False
        with self.assertRaisesRegex(flash.FlashError, "radio"):
            flash.verify_scanners(ble, platform, ["ble"], version)

        wifi = _scanner_status(platform, version, slot="wifi")
        wifi["scanners"][0]["wifi_paused"] = True
        with self.assertRaisesRegex(flash.FlashError, "radio"):
            flash.verify_scanners(wifi, platform, ["wifi"], version)

    def test_auto_update_convergence_binds_terminal_state_to_staged_generation(self) -> None:
        status = {
            "firmware_store": {
                "stored": True,
                "generation": 42,
                "auto_update": {
                    "generation": 42,
                    "target_slot_mask": 3,
                    "pending_mask": 0,
                    "worker_running": False,
                    "readiness_probes": [1, 2],
                    "scanners": [
                        {"slot": 0, "attempts": 1, "state": "converged"},
                        {"slot": 1, "attempts": 0, "state": "current"},
                    ],
                },
            },
        }

        flash.verify_auto_update_convergence(status, ["ble", "wifi"])

        status["firmware_store"]["auto_update"]["generation"] = 41
        with self.assertRaisesRegex(flash.FlashError, "generation"):
            flash.verify_auto_update_convergence(status, ["ble", "wifi"])

    def test_auto_update_convergence_accepts_newer_skipped_terminal(self) -> None:
        status = {
            "firmware_store": {
                "stored": True,
                "generation": 42,
                "auto_update": {
                    "generation": 42,
                    "target_slot_mask": 3,
                    "pending_mask": 0,
                    "worker_running": False,
                    "readiness_probes": [1, 1],
                    "scanners": [
                        {"slot": 0, "attempts": 1, "state": "converged"},
                        {"slot": 1, "attempts": 0, "state": "newer_skipped"},
                    ],
                },
            },
        }

        flash.verify_auto_update_convergence(status, ["ble", "wifi"])

    def test_auto_update_verifier_accepts_exact_stage_receipt_proof(self) -> None:
        parameters = inspect.signature(
            flash.verify_auto_update_convergence
        ).parameters

        self.assertIn("expected_stage_receipt", parameters)

    def test_auto_update_convergence_binds_store_to_exact_stage_receipt(self) -> None:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        version = "0.64.69-badge-defcon34"
        receipt = _stage_receipt(
            platform, version, b"scanner-image", 1, generation=42
        )
        status = {
            "firmware_store": {
                "stored": True,
                "target": receipt["target"],
                "app_project": receipt["app_project"],
                "hardware_type": receipt["hardware_type"],
                "version": receipt["version"],
                "sha256": receipt["sha256"],
                "size": receipt["size"],
                "crc32": receipt["crc32"],
                "generation": receipt["generation"],
                "auto_update": {
                    "generation": receipt["generation"],
                    "target_slot_mask": 1,
                    "pending_mask": 0,
                    "worker_running": False,
                    "readiness_probes": [1, 0],
                    "scanners": [
                        {"slot": 0, "attempts": 1, "state": "converged"},
                        {"slot": 1, "attempts": 0, "state": "excluded"},
                    ],
                },
            },
        }

        flash.verify_auto_update_convergence(
            status, ["ble"], expected_stage_receipt=receipt
        )

        for field, replacement in (
            ("generation", 43),
            ("sha256", "0" * 64),
            ("target", "scanner-s3-combo"),
        ):
            with self.subTest(field=field):
                changed = json.loads(json.dumps(status))
                changed["firmware_store"][field] = replacement
                if field == "generation":
                    changed["firmware_store"]["auto_update"]["generation"] = replacement
                with self.assertRaisesRegex(
                    flash.FlashError, f"stage receipt.*{field}"
                ):
                    flash.verify_auto_update_convergence(
                        changed,
                        ["ble"],
                        expected_stage_receipt=receipt,
                    )

        wrong_mask_receipt = dict(receipt)
        wrong_mask_receipt["slot_mask"] = 2
        with self.assertRaisesRegex(flash.FlashError, "stage receipt.*slot_mask"):
            flash.verify_auto_update_convergence(
                status,
                ["ble"],
                expected_stage_receipt=wrong_mask_receipt,
            )

    def test_auto_update_convergence_proves_exact_requested_slot_mask(self) -> None:
        status = {
            "firmware_store": {
                "stored": True,
                "generation": 9,
                "auto_update": {
                    "generation": 9,
                    "target_slot_mask": 1,
                    "pending_mask": 0,
                    "worker_running": False,
                    "readiness_probes": [1, 0],
                    "scanners": [
                        {"slot": 0, "attempts": 1, "state": "converged"},
                        {"slot": 1, "attempts": 0, "state": "excluded"},
                    ],
                },
            },
        }

        flash.verify_auto_update_convergence(status, ["ble"])

        auto_update = status["firmware_store"]["auto_update"]
        auto_update["target_slot_mask"] = 3
        with self.assertRaisesRegex(flash.FlashError, "slot mask"):
            flash.verify_auto_update_convergence(status, ["ble"])

        auto_update["target_slot_mask"] = 1
        auto_update["scanners"][1]["state"] = "current"
        with self.assertRaisesRegex(flash.FlashError, "excluded"):
            flash.verify_auto_update_convergence(status, ["ble"])

    def test_auto_update_convergence_rejects_pending_or_nonterminal_work(self) -> None:
        status = {
            "firmware_store": {
                "stored": True,
                "generation": 7,
                "auto_update": {
                    "generation": 7,
                    "target_slot_mask": 2,
                    "pending_mask": 0,
                    "worker_running": False,
                    "readiness_probes": [0, 1],
                    "scanners": [
                        {"slot": 0, "attempts": 0, "state": "excluded"},
                        {"slot": 1, "attempts": 1, "state": "converged"},
                    ],
                },
            },
        }

        auto_update = status["firmware_store"]["auto_update"]
        auto_update["pending_mask"] = 2
        with self.assertRaisesRegex(flash.FlashError, "pending"):
            flash.verify_auto_update_convergence(status, ["wifi"])

        auto_update["pending_mask"] = 0
        auto_update["worker_running"] = True
        with self.assertRaisesRegex(flash.FlashError, "worker"):
            flash.verify_auto_update_convergence(status, ["wifi"])

        auto_update["worker_running"] = False
        auto_update["scanners"][1]["state"] = "ready_queued"
        with self.assertRaisesRegex(flash.FlashError, "terminal"):
            flash.verify_auto_update_convergence(status, ["wifi"])

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
