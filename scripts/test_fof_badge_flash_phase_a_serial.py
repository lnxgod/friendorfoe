#!/usr/bin/env python3
"""Phase A H2 tests for strict host serial receipt integration."""

from __future__ import annotations

import binascii
import contextlib
import hashlib
import io
import json
import struct
import sys
import tempfile
import time
import unittest
from pathlib import Path
from types import SimpleNamespace
from typing import Callable
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fof_badge_flash as flash
import secure_artifact_tree as artifact_tree


UINT32_MAX = (1 << 32) - 1
VERSION = "0.64.76-badge-defcon34"
OLD_VERSION = "0.64.75-badge-defcon34"
HARDWARE_ID = "e0:72:a1:f9:48:58"
UPDATE_SESSION = "0123456789ABCDEF"
CAPTURED_FINDMY_DETECTION = (
    b'FOF_DET:{"id":"BLE:17D27B12:FindMy Accessory",'
    b'"manufacturer":"FindMy Accessory","badge_label":"Find My",'
    b'"badge_class":"tracker",'
    b'"badge_entity_key":"PRIV:TAG:FP:17D27B12","source":6,'
    b'"confidence":0.649999976158142,"threat_score":36,"rssi":-51}\n'
)
CAPTURED_BLE_NEARBY_DETECTION = (
    b'FOF_DET:{"id":"BLE:7EE9619D:BLE Nearby",'
    b'"manufacturer":"BLE Nearby","badge_label":"","badge_class":"",'
    b'"badge_entity_key":"","source":6,'
    b'"confidence":0.18000000715255737,"threat_score":0,'
    b'"rssi":-70}\n'
)


def _usb_record(
    device: str = "/dev/fake",
) -> flash.UsbDescriptorRecord:
    """Synthetic immutable descriptor for host-only protocol tests."""
    return flash.UsbDescriptorRecord(
        device=device,
        vid=flash.ESPRESSIF_USB_SERIAL_JTAG_VID,
        pid=flash.ESPRESSIF_USB_SERIAL_JTAG_PID,
        serial_number="e0:72:a1:f9:47:fc",
        location="phase-a-host-fixture",
        stat_device=1,
        stat_inode=2,
        stat_rdev=3,
    )


class _TimedByteSerial:
    """Exact byte transport with deterministic monotonic arrival times."""

    def __init__(
        self,
        clock: SimpleNamespace,
        events: list[tuple[float, bytes]] | None = None,
    ) -> None:
        self.clock = clock
        self.events = list(events or [])
        self.writes: list[bytes] = []
        self.read_calls = 0
        self.closed = False

    def read(self, _size: int) -> bytes:
        self.read_calls += 1
        if not self.events:
            return b""
        advance, payload = self.events.pop(0)
        self.clock.now += advance
        return payload

    def write(self, payload: bytes) -> int:
        self.writes.append(bytes(payload))
        return len(payload)

    def flush(self) -> None:
        return None

    def close(self) -> None:
        self.closed = True


def _frame(prefix: str, payload: dict, *, crlf: bool = False) -> bytes:
    ending = b"\r\n" if crlf else b"\n"
    return (
        prefix.encode("ascii")
        + json.dumps(payload, separators=(",", ":")).encode("utf-8")
        + ending
    )


def _firmware_image(project: str, version: str, *markers: str) -> bytes:
    image = bytearray(0x20 + 112)
    image[0] = 0xE9
    struct.pack_into("<I", image, 0x20, 0xABCD5432)
    image[0x30:0x50] = version.encode("ascii").ljust(32, b"\x00")
    image[0x50:0x70] = project.encode("ascii").ljust(32, b"\x00")
    for marker in markers:
        image.extend(b"\x00" + marker.encode("ascii") + b"\x00")
    return bytes(image)


def _frozen_firmware_set(data: bytes) -> artifact_tree.FrozenArtifactSet:
    receipt_sha256 = "0" * 64
    member = artifact_tree.FrozenArtifactMember(
        logical_name="artifact.firmware",
        size=len(data),
        sha256=hashlib.sha256(data).hexdigest(),
        content=data,
    )
    aggregate = hashlib.sha256()
    aggregate.update(b"FOF-FROZEN-ARTIFACT-SET-v1\x00")
    aggregate.update(bytes.fromhex(receipt_sha256))
    aggregate.update((1).to_bytes(4, "big"))
    logical = member.logical_name.encode("utf-8")
    aggregate.update(len(logical).to_bytes(4, "big"))
    aggregate.update(logical)
    aggregate.update(member.size.to_bytes(8, "big"))
    aggregate.update(bytes.fromhex(member.sha256))
    aggregate.update(member.content)
    return artifact_tree.FrozenArtifactSet(
        receipt_sha256=receipt_sha256,
        members=(member,),
        aggregate_sha256=aggregate.hexdigest(),
    )


def _uplink_receipt(
    phase: str,
    *,
    total: int,
    received: int = 0,
    credit: int = 0,
    ok: bool = True,
    reboot_required: bool = False,
    error: str = "",
) -> dict:
    return {
        "ok": ok,
        "phase": phase,
        "partition": "ota_1",
        "received": received,
        "total": total,
        "credit_bytes": credit,
        "retryable": False,
        "reboot_required": reboot_required,
        "error": error,
    }


def _relay_terminal(*, size: int = 2048) -> dict:
    return {
        "ok": True,
        "phase": "final",
        "slot": "ble",
        "uart": "ble",
        "generation": 42,
        "hardware_id": HARDWARE_ID,
        "size": size,
        "bytes": size,
        "chunks": (size + flash.SCANNER_RELAY_CHUNK_BYTES - 1)
        // flash.SCANNER_RELAY_CHUNK_BYTES,
        "stage": "done",
        "done": True,
        "error": "",
    }


def _relay_progress(*, size: int = 2048) -> dict:
    return {
        "uart": "ble",
        "stage": "chunks",
        "bytes": size // 2,
        "size": size,
        "percent": 50,
        "chunks": 1,
        "nacks": 0,
        "retries": 0,
        "elapsed_s": 1,
        "error": "",
    }


def _stage_receipt(
    platform: dict,
    version: str,
    data: bytes,
    slot_mask: int,
    *,
    phase: str | None = None,
    received: int | None = None,
    credit: int | None = None,
    generation: int | None = None,
) -> dict:
    receipt = {
        "ok": True,
        "partition": "fw_scanner_s3",
        "size": len(data),
        "crc32": binascii.crc32(data) & 0xFFFFFFFF,
        "sha256": hashlib.sha256(data).hexdigest(),
        "target": platform["scanner_name"],
        "name": platform["scanner_name"],
        "app_project": platform["scanner_project"],
        "project": platform["scanner_project"],
        "hardware_type": platform["hardware_type"],
        "hardware": platform["hardware_type"],
        "version": version,
        "slot_mask": slot_mask,
    }
    if phase is not None:
        receipt.update({
            "flow_control": "credit-v1",
            "phase": phase,
            "received": received,
            "total": len(data),
            "credit_bytes": credit,
        })
    if generation is not None:
        receipt["generation"] = generation
    return receipt


def _uplink_status(version: str) -> dict:
    return {
        "target": "uplink-s3-fof_badge",
        "firmware_name": "uplink-s3-fof_badge",
        "project": "fof_badge_uplink",
        "app_project": "fof_badge_uplink",
        "hardware_type": "seeed_xiao_esp32s3",
        "version": version,
        "hardware_id": "e0:72:a1:f9:47:fc",
        "running_partition": "ota_0",
        "pending_verify": False,
        "rollback_state": "clear",
        "recovery_mode": "normal",
        "usb_health": {"responses_completed": 20},
    }


def _maintenance_status(
    version: str = VERSION,
    *,
    session: str = UPDATE_SESSION,
    ble_initialized: bool = False,
) -> dict:
    status = _uplink_status(version)
    status.update({
        "recovery_mode": "update_maintenance",
        "update_session": session,
        "ble_initialized": ble_initialized,
        "stack_main_free": 4096,
        "stack_uart_ble_free": 4096,
        "stack_uart_wifi_free": 4096,
        "update_uplink": {
            "phase": "idle",
            "session": session,
            "version": "",
            "sha256": "",
            "size": 0,
            "partition": "",
            "received": 0,
        },
        "update_scanner": {
            "phase": "idle",
            "session": session,
            "target": "",
            "sha256": "",
            "size": 0,
            "slot_mask": 0,
            "received": 0,
            "generation": 0,
        },
    })
    return status


def _uplink_reconcile_expected() -> dict:
    return {
        "session": UPDATE_SESSION,
        "version": VERSION,
        "sha256": "a" * 64,
        "size": 8192,
        "partition": "ota_1",
    }


def _maintenance_uplink_phase(
    phase: str,
    *,
    received: int,
) -> dict:
    status = _maintenance_status()
    expected = _uplink_reconcile_expected()
    status["update_uplink"] = {
        "phase": phase,
        **expected,
        "received": received,
    }
    return status


def _scanner_reconcile_expected() -> dict:
    return {
        "session": UPDATE_SESSION,
        "target": "scanner-s3-combo-fof_badge",
        "sha256": "b" * 64,
        "size": 12288,
        "slot_mask": 3,
    }


def _maintenance_scanner_phase(
    phase: str,
    *,
    received: int,
    generation: int,
) -> dict:
    status = _maintenance_status()
    expected = _scanner_reconcile_expected()
    status["update_scanner"] = {
        "phase": phase,
        **expected,
        "received": received,
        "generation": generation,
    }
    return status


class StrictBadgeSerialIntegrationTests(unittest.TestCase):
    def assert_payload_free_failure(
        self,
        action: Callable[[], object],
        *,
        expected_message: str,
        forbidden: tuple[str, ...],
    ) -> None:
        stdout = io.StringIO()
        stderr = io.StringIO()
        with contextlib.redirect_stdout(stdout), \
                contextlib.redirect_stderr(stderr), \
                self.assertRaises(flash.FlashError) as caught:
            action()

        rendered = str(caught.exception)
        self.assertEqual(rendered, expected_message)
        for sentinel in forbidden:
            self.assertNotIn(sentinel, rendered)
            self.assertNotIn(sentinel, stdout.getvalue())
            self.assertNotIn(sentinel, stderr.getvalue())
        self.assertIsNone(caught.exception.__cause__)
        self.assertIsNone(caught.exception.__context__)
        self.assertEqual(stdout.getvalue(), "")
        self.assertEqual(stderr.getvalue(), "")

    def test_firmware_prefix_requires_schema_selection_before_read(self) -> None:
        clock = SimpleNamespace(now=0.0)
        serial = _TimedByteSerial(clock, [(
            0.0,
            _frame(
                "FOF_UPLINK_OTA:",
                _uplink_receipt("ready", total=1, credit=1),
            ),
        )])
        badge = flash.BadgeSerial(_usb_record(), False)
        badge.ser = serial

        with self.assertRaises(flash.SerialTransportError):
            badge.read_prefixed_json("FOF_UPLINK_OTA:", 1)

        self.assertEqual(serial.read_calls, 0)

    def test_prepare_update_retries_same_session_then_accepts_rebooting(
        self,
    ) -> None:
        clock = SimpleNamespace(now=0.0)
        waiting = {
            "ok": False,
            "phase": "waiting_for_owner",
            "session": UPDATE_SESSION,
            "retryable": True,
            "reboot_required": False,
            "error": "firmware_operation_active",
        }
        rebooting = {
            "ok": True,
            "phase": "rebooting",
            "session": UPDATE_SESSION,
            "retryable": True,
            "reboot_required": True,
        }
        serial = _TimedByteSerial(clock, [(
            0.0,
            _frame("FOF_UPDATE_MODE:", waiting)
            + _frame("FOF_UPDATE_MODE:", rebooting),
        )])
        badge = flash.BadgeSerial(_usb_record(), False)
        badge.ser = serial

        with mock.patch.object(
            flash.time, "monotonic", side_effect=lambda: clock.now
        ), mock.patch.object(
            flash.time, "sleep",
            side_effect=lambda delay: setattr(clock, "now", clock.now + delay),
        ):
            receipt = badge.prepare_update_maintenance(
                UPDATE_SESSION,
                deadline=30.0,
                source_supports_update_maintenance=False,
            )

        self.assertEqual(receipt, rebooting)
        command = (
            b'\nFOF_CTL:{"cmd":"prepare_update",'
            b'"session":"0123456789ABCDEF"}\n'
        )
        self.assertEqual(serial.writes, [command, command])
        self.assertEqual(clock.now, flash.UPDATE_PREPARE_RETRY_S)

    def test_prepare_update_recovers_only_after_same_session_maintenance_proof(
        self,
    ) -> None:
        active = {
            "ok": True,
            "phase": "active",
            "session": UPDATE_SESSION,
            "retryable": False,
            "reboot_required": False,
        }
        command = (
            b'\nFOF_CTL:{"cmd":"prepare_update",'
            b'"session":"0123456789ABCDEF"}\n'
        )
        for label, initial_serial_factory in (
            (
                "silent-dropped-receipt",
                lambda clock: _TimedByteSerial(clock),
            ),
            (
                "terminal-disconnect",
                lambda clock: SimpleNamespace(
                    writes=[],
                    closed=False,
                    write=lambda payload: (
                        initial_serial.writes.append(bytes(payload))
                        or len(payload)
                    ),
                    flush=lambda: None,
                    close=lambda: setattr(initial_serial, "closed", True),
                    read=lambda _size: (_ for _ in ()).throw(
                        OSError("USB rebooted")
                    ),
                ),
            ),
            (
                "deferred-legacy-leading-lf-marker",
                lambda clock: _TimedByteSerial(
                    clock, [(0.0, b"FOF_ERROR:unknown command\n")]
                ),
            ),
        ):
            with self.subTest(label=label):
                clock = SimpleNamespace(now=0.0)
                initial_serial = initial_serial_factory(clock)
                rebound_serial = _TimedByteSerial(
                    clock,
                    [(
                        0.0,
                        f"FOF_PONG:{VERSION}\n".encode("ascii")
                        + _frame("FOF_UPDATE_MODE:", active),
                    )],
                )
                badge = flash.BadgeSerial(_usb_record("/dev/old"), False)
                badge.ser = initial_serial
                session_during_proof: list[str | None] = []

                def open_serial() -> None:
                    self.assertEqual(
                        badge._update_session, UPDATE_SESSION
                    )
                    badge.ser = rebound_serial

                def status(*, timeout_s: float) -> dict:
                    self.assertGreater(timeout_s, 0)
                    session_during_proof.append(badge._update_session)
                    return _maintenance_status()

                badge._open_serial = open_serial  # type: ignore[method-assign]
                badge.status = status  # type: ignore[method-assign]
                with mock.patch.object(
                    flash, "_take_badge_usb_descriptor_census",
                    return_value=(_usb_record("/dev/rebound"),),
                ), mock.patch.object(
                    flash.time, "monotonic",
                    side_effect=lambda: clock.now,
                ), mock.patch.object(
                    flash.time, "sleep",
                    side_effect=lambda delay: setattr(
                        clock, "now", clock.now + delay
                    ),
                ):
                    receipt = badge.prepare_update_maintenance(
                        UPDATE_SESSION,
                        deadline=30.0,
                        source_supports_update_maintenance=True,
                    )

                self.assertEqual(receipt, active)
                self.assertEqual(
                    session_during_proof, [UPDATE_SESSION]
                )
                self.assertEqual(badge._update_session, UPDATE_SESSION)
                self.assertEqual(initial_serial.writes, [command])
                self.assertEqual(
                    rebound_serial.writes,
                    [b"\nFOF_PING\n", command],
                )
                self.assertTrue(initial_serial.closed)
                self.assertLessEqual(clock.now, 3.25)

    def test_prepare_update_recovery_uses_three_second_slices_and_one_deadline(
        self,
    ) -> None:
        clock = SimpleNamespace(now=0.0)
        badge = flash.BadgeSerial(_usb_record(), False)
        badge.ser = _TimedByteSerial(clock)
        active = {
            "ok": True,
            "phase": "active",
            "session": UPDATE_SESSION,
            "retryable": False,
            "reboot_required": False,
        }
        receipt_timeouts: list[float] = []
        reconnect_deadlines: list[float] = []

        def read_receipt(
            timeout_s: float,
            *,
            allowed_schema_ids: tuple[flash.HostJsonSchemaId, ...],
        ) -> dict:
            self.assertTrue(allowed_schema_ids)
            receipt_timeouts.append(timeout_s)
            if len(receipt_timeouts) == 1:
                clock.now += timeout_s
                raise flash.SerialReadTimeout(
                    "receipt dropped",
                    saw_activity=False,
                    partial_frame=False,
                )
            return active

        def rebind(
            *,
            deadline: float,
            maintenance_session: str | None,
        ) -> dict:
            self.assertEqual(badge._update_session, UPDATE_SESSION)
            self.assertEqual(maintenance_session, UPDATE_SESSION)
            reconnect_deadlines.append(deadline)
            clock.now = 29.0
            return _maintenance_status()

        badge._read_update_mode_or_control_error = (  # type: ignore[method-assign]
            read_receipt
        )
        badge._reconnect_same_uplink_mode = rebind  # type: ignore[method-assign]
        with mock.patch.object(
            flash.time, "monotonic", side_effect=lambda: clock.now
        ):
            actual = badge.prepare_update_maintenance(
                UPDATE_SESSION,
                deadline=30.0,
                source_supports_update_maintenance=True,
            )

        self.assertEqual(actual, active)
        self.assertEqual(reconnect_deadlines, [30.0])
        self.assertEqual(receipt_timeouts, [3.0, 1.0])
        self.assertEqual(badge._update_session, UPDATE_SESSION)
        self.assertEqual(len(badge.ser.writes), 2)

    def test_prepare_update_never_recovers_nonambiguous_failures(self) -> None:
        wrong_session = {
            "ok": True,
            "phase": "active",
            "session": "FEDCBA9876543210",
            "retryable": False,
            "reboot_required": False,
        }
        conflict = {
            "ok": False,
            "phase": "busy",
            "session": UPDATE_SESSION,
            "retryable": False,
            "reboot_required": False,
            "error": "session_conflict",
        }
        failures: tuple[tuple[str, object, type[BaseException]], ...] = (
            ("wrong-session", wrong_session, flash.FlashError),
            ("conflict", conflict, flash.FlashError),
            (
                "malformed",
                flash.SerialTransportError("malformed response"),
                flash.SerialTransportError,
            ),
            (
                "explicit-current-schema-rejection",
                flash.UpdateMaintenanceUnsupportedError(
                    "explicit current-schema rejection"
                ),
                flash.UpdateMaintenanceUnsupportedError,
            ),
        )
        for label, result, error_type in failures:
            with self.subTest(label=label):
                badge = flash.BadgeSerial(_usb_record(), False)
                badge.ser = _TimedByteSerial(SimpleNamespace(now=0.0))
                if isinstance(result, BaseException):
                    badge._read_update_mode_or_control_error = mock.Mock(  # type: ignore[method-assign]
                        side_effect=result
                    )
                else:
                    badge._read_update_mode_or_control_error = mock.Mock(  # type: ignore[method-assign]
                        return_value=result
                    )
                badge._reconnect_same_uplink_mode = mock.Mock(  # type: ignore[method-assign]
                    side_effect=AssertionError(
                        "nonambiguous failure reached recovery"
                    )
                )

                with self.assertRaises(error_type):
                    badge.prepare_update_maintenance(
                        UPDATE_SESSION,
                        deadline=time.monotonic() + 30.0,
                        source_supports_update_maintenance=True,
                    )

                badge._reconnect_same_uplink_mode.assert_not_called()

    def test_prepare_update_legacy_source_keeps_exact_unsupported_fallback(
        self,
    ) -> None:
        clock = SimpleNamespace(now=0.0)
        badge = flash.BadgeSerial(_usb_record(), False)
        badge.ser = _TimedByteSerial(
            clock, [(0.0, b"FOF_ERROR:unknown command\n")]
        )
        badge._reconnect_same_uplink_mode = mock.Mock(  # type: ignore[method-assign]
            side_effect=AssertionError("legacy source reached recovery")
        )

        with mock.patch.object(
            flash.time, "monotonic", side_effect=lambda: clock.now
        ), mock.patch.object(
            flash.time, "sleep",
            side_effect=lambda delay: setattr(clock, "now", clock.now + delay),
        ), self.assertRaises(flash.UpdateMaintenanceUnsupportedError):
            badge.prepare_update_maintenance(
                UPDATE_SESSION,
                deadline=0.2,
                source_supports_update_maintenance=False,
            )

        badge._reconnect_same_uplink_mode.assert_not_called()

    def test_supported_prepare_rejection_keeps_bound_recovery_session(
        self,
    ) -> None:
        badge = flash.BadgeSerial(_usb_record(), False)
        badge.ser = _TimedByteSerial(SimpleNamespace(now=0.0))
        badge._read_update_mode_or_control_error = mock.Mock(  # type: ignore[method-assign]
            side_effect=flash.UpdateMaintenanceUnsupportedError(
                "supported prepare response became ambiguous"
            )
        )

        with self.assertRaises(flash.UpdateMaintenanceUnsupportedError):
            badge.prepare_update_maintenance(
                UPDATE_SESSION,
                deadline=time.monotonic() + 30.0,
                source_supports_update_maintenance=True,
            )

        self.assertEqual(badge._update_session, UPDATE_SESSION)

    def test_prepare_update_accepts_idempotent_active_receipt(self) -> None:
        active = {
            "ok": True,
            "phase": "active",
            "session": UPDATE_SESSION,
            "retryable": False,
            "reboot_required": False,
        }
        clock = SimpleNamespace(now=0.0)
        badge = flash.BadgeSerial(_usb_record(), False)
        badge.ser = _TimedByteSerial(
            clock, [(0.0, _frame("FOF_UPDATE_MODE:", active))]
        )

        receipt = badge.prepare_update_maintenance(
            UPDATE_SESSION,
            deadline=time.monotonic() + 30.0,
            source_supports_update_maintenance=False,
        )

        self.assertEqual(receipt, active)

    def test_prepare_update_conflict_and_deadline_fail_closed(self) -> None:
        conflict = {
            "ok": False,
            "phase": "busy",
            "session": UPDATE_SESSION,
            "retryable": False,
            "reboot_required": False,
            "error": "session_conflict",
        }
        clock = SimpleNamespace(now=0.0)
        badge = flash.BadgeSerial(_usb_record(), False)
        badge.ser = _TimedByteSerial(
            clock, [(0.0, _frame("FOF_UPDATE_MODE:", conflict))]
        )
        with self.assertRaisesRegex(flash.FlashError, "conflict"):
            badge.prepare_update_maintenance(
                UPDATE_SESSION,
                deadline=time.monotonic() + 30.0,
                source_supports_update_maintenance=False,
            )

        waiting = {
            **conflict,
            "phase": "waiting_for_owner",
            "retryable": True,
            "error": "firmware_operation_active",
        }
        clock = SimpleNamespace(now=0.0)
        badge = flash.BadgeSerial(_usb_record(), False)
        badge.ser = _TimedByteSerial(
            clock, [(0.0, _frame("FOF_UPDATE_MODE:", waiting))]
        )
        with mock.patch.object(
            flash.time, "monotonic", side_effect=lambda: clock.now
        ), mock.patch.object(
            flash.time, "sleep",
            side_effect=lambda delay: setattr(clock, "now", clock.now + delay),
        ), self.assertRaisesRegex(flash.FlashError, "deadline"):
            badge.prepare_update_maintenance(
                UPDATE_SESSION,
                deadline=0.2,
                source_supports_update_maintenance=False,
            )

    def test_prepare_update_distinguishes_only_exact_unknown_command(
        self,
    ) -> None:
        cases = (
            (
                {"error": "unknown command"},
                flash.UpdateMaintenanceUnsupportedError,
            ),
            ({"error": "invalid update session"}, flash.FlashError),
        )
        for receipt, exception_type in cases:
            clock = SimpleNamespace(now=0.0)
            badge = flash.BadgeSerial(_usb_record(), False)
            badge.ser = _TimedByteSerial(clock, [(
                0.0, _frame("FOF_CTL_ERROR:", receipt)
            )])
            with self.subTest(receipt=receipt), self.assertRaises(
                exception_type
            ):
                badge.prepare_update_maintenance(
                    UPDATE_SESSION,
                    deadline=time.monotonic() + 30.0,
                    source_supports_update_maintenance=False,
                )

        clock = SimpleNamespace(now=0.0)
        badge = flash.BadgeSerial(_usb_record(), False)
        badge.ser = _TimedByteSerial(clock, [(
            0.0,
            _frame(
                "FOF_CTL_ERROR:",
                {"error": "unknown command", "extra": True},
            ),
        )])
        with self.assertRaises(flash.SerialTransportError):
            badge.prepare_update_maintenance(
                UPDATE_SESSION,
                deadline=time.monotonic() + 30.0,
                source_supports_update_maintenance=False,
            )

    def test_prepare_update_timeout_preserves_unrelated_frame_activity(
        self,
    ) -> None:
        clock = SimpleNamespace(now=0.0)
        badge = flash.BadgeSerial(_usb_record(), False)
        badge.ser = _TimedByteSerial(clock, [(0.0, b"boot diagnostic\n")])

        with mock.patch.object(
            flash.time, "monotonic", side_effect=lambda: clock.now
        ), mock.patch.object(
            flash.time, "sleep",
            side_effect=lambda delay: setattr(clock, "now", clock.now + delay),
        ), self.assertRaises(flash.SerialReadTimeout) as caught:
            badge.prepare_update_maintenance(
                UPDATE_SESSION,
                deadline=0.2,
                source_supports_update_maintenance=False,
            )

        self.assertTrue(caught.exception.saw_activity)
        self.assertFalse(caught.exception.partial_frame)

    def test_prepare_update_accepts_only_exact_legacy_unknown_frame(
        self,
    ) -> None:
        clock = SimpleNamespace(now=0.0)
        badge = flash.BadgeSerial(_usb_record(), False)
        badge.ser = _TimedByteSerial(
            clock, [(0.0, b"FOF_ERROR:unknown command\n")]
        )

        with mock.patch.object(
            flash.time, "monotonic", side_effect=lambda: clock.now
        ), mock.patch.object(
            flash.time, "sleep",
            side_effect=lambda delay: setattr(clock, "now", clock.now + delay),
        ), self.assertRaises(flash.UpdateMaintenanceUnsupportedError):
            badge.prepare_update_maintenance(
                UPDATE_SESSION,
                deadline=0.2,
                source_supports_update_maintenance=False,
            )

        near_misses = (
            b"FOF_ERROR:unknown command \n",
            b"FOF_ERROR:Unknown command\n",
            b"FOF_ERROR:unknown command:extra\n",
            b'FOF_ERROR:{"error":"unknown command"}\n',
            b"FOF_ERROR:startup_recovery_only\n",
            b"boot diagnostic\n",
        )
        for frame in near_misses:
            clock = SimpleNamespace(now=0.0)
            badge = flash.BadgeSerial(_usb_record(), False)
            badge.ser = _TimedByteSerial(clock, [(0.0, frame)])
            with self.subTest(frame=frame), mock.patch.object(
                flash.time, "monotonic", side_effect=lambda: clock.now
            ), mock.patch.object(
                flash.time, "sleep",
                side_effect=lambda delay: setattr(
                    clock, "now", clock.now + delay
                ),
            ), self.assertRaises(flash.SerialReadTimeout) as caught:
                badge.prepare_update_maintenance(
                    UPDATE_SESSION,
                    deadline=0.2,
                    source_supports_update_maintenance=False,
                )
            self.assertTrue(caught.exception.saw_activity)
            self.assertFalse(caught.exception.partial_frame)

        clock = SimpleNamespace(now=0.0)
        badge = flash.BadgeSerial(_usb_record(), False)
        badge.ser = _TimedByteSerial(
            clock, [(0.0, b"FOF_ERROR:unknown command")]
        )
        with mock.patch.object(
            flash.time, "monotonic", side_effect=lambda: clock.now
        ), mock.patch.object(
            flash.time, "sleep",
            side_effect=lambda delay: setattr(clock, "now", clock.now + delay),
        ), self.assertRaises(flash.SerialReadTimeout) as caught:
            badge.prepare_update_maintenance(
                UPDATE_SESSION,
                deadline=0.2,
                source_supports_update_maintenance=False,
            )
        self.assertTrue(caught.exception.saw_activity)
        self.assertTrue(caught.exception.partial_frame)

    def test_prepare_update_ignores_leading_lf_legacy_error_before_receipt(
        self,
    ) -> None:
        receipt = {
            "ok": True,
            "phase": "rebooting",
            "session": UPDATE_SESSION,
            "retryable": True,
            "reboot_required": True,
        }
        clock = SimpleNamespace(now=0.0)
        badge = flash.BadgeSerial(_usb_record(), False)
        badge.ser = _TimedByteSerial(clock, [(
            0.0,
            b"FOF_ERROR:unknown command\n"
            + _frame("FOF_UPDATE_MODE:", receipt),
        )])

        with mock.patch.object(
            flash.time, "monotonic", side_effect=lambda: clock.now
        ):
            actual = badge.prepare_update_maintenance(
                UPDATE_SESSION,
                deadline=5.0,
                source_supports_update_maintenance=False,
            )

        self.assertEqual(actual, receipt)

    def test_prepare_update_legacy_rejection_allows_complete_nonfof_logs(
        self,
    ) -> None:
        heartbeat = (
            b"I (12345) uart_rx: [BLE] heartbeat: 42 total bytes "
            b"received stack=1880\n"
        )
        wifi_heartbeat = (
            b"I (12346) uart_rx: [WiFi] heartbeat: 84 total bytes "
            b"received stack=1872\n"
        )
        clock = SimpleNamespace(now=0.0)
        badge = flash.BadgeSerial(_usb_record(), False)
        badge.ser = _TimedByteSerial(clock, [(
            0.0,
            heartbeat
            + b"FOF_ERROR:unknown command\n"
            + b"legacy console note without ESP-IDF framing\n"
            + wifi_heartbeat,
        )])
        with mock.patch.object(
            flash.time, "monotonic", side_effect=lambda: clock.now
        ), mock.patch.object(
            flash.time, "sleep",
            side_effect=lambda delay: setattr(clock, "now", clock.now + delay),
        ), self.assertRaises(flash.UpdateMaintenanceUnsupportedError):
            badge.prepare_update_maintenance(
                UPDATE_SESSION,
                deadline=0.2,
                source_supports_update_maintenance=False,
            )

        for diagnostic_only in (
            heartbeat,
            b"not an ESP-IDF diagnostic\n",
        ):
            clock = SimpleNamespace(now=0.0)
            badge = flash.BadgeSerial(_usb_record(), False)
            badge.ser = _TimedByteSerial(
                clock, [(0.0, diagnostic_only)]
            )
            with self.subTest(
                diagnostic_only=diagnostic_only
            ), mock.patch.object(
                flash.time, "monotonic", side_effect=lambda: clock.now
            ), mock.patch.object(
                flash.time, "sleep",
                side_effect=lambda delay: setattr(
                    clock, "now", clock.now + delay
                ),
            ), self.assertRaises(flash.SerialReadTimeout) as caught:
                badge.prepare_update_maintenance(
                    UPDATE_SESSION,
                    deadline=0.2,
                    source_supports_update_maintenance=False,
                )
            self.assertTrue(caught.exception.saw_activity)

        blocking_cases = (
            (
                b"FOF_ERROR:unknown command\n"
                b"FOF_ERROR:booting\n",
                flash.SerialReadTimeout,
                False,
            ),
            (
                b"FOF_ERROR:unknown command\n"
                b"FOF_STATUS:{",
                flash.SerialReadTimeout,
                True,
            ),
            (
                b"FOF_ERROR:unknown command\n"
                b"FOF_UPDATE_MODE:{bad json}\n",
                flash.SerialTransportError,
                False,
            ),
        )
        for payload, error_type, partial in blocking_cases:
            clock = SimpleNamespace(now=0.0)
            badge = flash.BadgeSerial(_usb_record(), False)
            badge.ser = _TimedByteSerial(clock, [(0.0, payload)])
            with self.subTest(payload=payload), mock.patch.object(
                flash.time, "monotonic", side_effect=lambda: clock.now
            ), mock.patch.object(
                flash.time, "sleep",
                side_effect=lambda delay: setattr(
                    clock, "now", clock.now + delay
                ),
            ), self.assertRaises(error_type) as caught:
                badge.prepare_update_maintenance(
                    UPDATE_SESSION,
                    deadline=0.2,
                    source_supports_update_maintenance=False,
                )
            if isinstance(caught.exception, flash.SerialReadTimeout):
                self.assertEqual(caught.exception.partial_frame, partial)

    def test_prepare_update_legacy_rejection_allows_captured_detections(
        self,
    ) -> None:
        clock = SimpleNamespace(now=0.0)
        badge = flash.BadgeSerial(_usb_record(), False)
        badge.ser = _TimedByteSerial(clock, [(
            0.0,
            b"I (17431) badge: USB observation stream active\n"
            + b"FOF_ERROR:unknown command\n"
            + CAPTURED_FINDMY_DETECTION
            + b"FOF_ERROR:unknown command\n"
            + CAPTURED_BLE_NEARBY_DETECTION,
        )])

        with mock.patch.object(
            flash.time, "monotonic", side_effect=lambda: clock.now
        ), mock.patch.object(
            flash.time, "sleep",
            side_effect=lambda delay: setattr(clock, "now", clock.now + delay),
        ), self.assertRaises(flash.UpdateMaintenanceUnsupportedError):
            badge.prepare_update_maintenance(
                UPDATE_SESSION,
                deadline=0.2,
                source_supports_update_maintenance=False,
            )

    def test_prepare_update_valid_detection_does_not_authorize_fallback(
        self,
    ) -> None:
        clock = SimpleNamespace(now=0.0)
        badge = flash.BadgeSerial(_usb_record(), False)
        badge.ser = _TimedByteSerial(
            clock, [(0.0, CAPTURED_FINDMY_DETECTION)]
        )

        with mock.patch.object(
            flash.time, "monotonic", side_effect=lambda: clock.now
        ), mock.patch.object(
            flash.time, "sleep",
            side_effect=lambda delay: setattr(clock, "now", clock.now + delay),
        ), self.assertRaises(flash.SerialReadTimeout) as caught:
            badge.prepare_update_maintenance(
                UPDATE_SESSION,
                deadline=0.2,
                source_supports_update_maintenance=False,
            )

        self.assertTrue(caught.exception.saw_activity)
        self.assertFalse(caught.exception.partial_frame)

    def test_prepare_update_valid_receipt_wins_after_captured_detection(
        self,
    ) -> None:
        active = {
            "ok": True,
            "phase": "active",
            "session": UPDATE_SESSION,
            "retryable": False,
            "reboot_required": False,
        }
        clock = SimpleNamespace(now=0.0)
        badge = flash.BadgeSerial(_usb_record(), False)
        badge.ser = _TimedByteSerial(clock, [(
            0.0,
            b"FOF_ERROR:unknown command\n"
            + CAPTURED_BLE_NEARBY_DETECTION
            + _frame("FOF_UPDATE_MODE:", active),
        )])

        with mock.patch.object(
            flash.time, "monotonic", side_effect=lambda: clock.now
        ):
            actual = badge.prepare_update_maintenance(
                UPDATE_SESSION,
                deadline=5.0,
                source_supports_update_maintenance=False,
            )

        self.assertEqual(actual, active)

    def test_prepare_update_malformed_detection_frames_fail_closed(
        self,
    ) -> None:
        valid = {
            "id": "BLE:17D27B12:FindMy Accessory",
            "manufacturer": "FindMy Accessory",
            "badge_label": "Find My",
            "badge_class": "tracker",
            "badge_entity_key": "PRIV:TAG:FP:17D27B12",
            "source": 6,
            "confidence": 0.649999976158142,
            "threat_score": 36,
            "rssi": -51,
        }
        encoded = json.dumps(valid, separators=(",", ":"))
        malformed = {
            "bad JSON": b"FOF_DET:{bad json}\n",
            "missing member": _frame(
                "FOF_DET:",
                {key: value for key, value in valid.items() if key != "id"},
            ),
            "extra member": _frame(
                "FOF_DET:", {**valid, "unexpected": True}
            ),
            "duplicate member": (
                b"FOF_DET:"
                + encoded.replace(
                    '"source":6', '"source":6,"source":6'
                ).encode("utf-8")
                + b"\n"
            ),
            "escaped member name": (
                b"FOF_DET:"
                + encoded.replace(
                    '"source":6', '"sour\\u0063e":6'
                ).encode("utf-8")
                + b"\n"
            ),
            "source boolean": _frame(
                "FOF_DET:", {**valid, "source": True}
            ),
            "source negative": _frame(
                "FOF_DET:", {**valid, "source": -1}
            ),
            "source overflow": _frame(
                "FOF_DET:", {**valid, "source": 256}
            ),
            "source outside producer enum": _frame(
                "FOF_DET:", {**valid, "source": 9}
            ),
            "confidence boolean": _frame(
                "FOF_DET:", {**valid, "confidence": True}
            ),
            "confidence string": _frame(
                "FOF_DET:", {**valid, "confidence": "0.65"}
            ),
            "confidence below zero": _frame(
                "FOF_DET:", {**valid, "confidence": -0.01}
            ),
            "confidence above one": _frame(
                "FOF_DET:", {**valid, "confidence": 1.01}
            ),
            "confidence nonfinite": (
                b"FOF_DET:"
                + encoded.replace(
                    '"confidence":0.649999976158142',
                    '"confidence":NaN',
                ).encode("utf-8")
                + b"\n"
            ),
            "threat below zero": _frame(
                "FOF_DET:", {**valid, "threat_score": -0.01}
            ),
            "threat boolean": _frame(
                "FOF_DET:", {**valid, "threat_score": False}
            ),
            "threat above one hundred": _frame(
                "FOF_DET:", {**valid, "threat_score": 100.01}
            ),
            "threat overflow": (
                b"FOF_DET:"
                + encoded.replace(
                    '"threat_score":36', '"threat_score":1e309'
                ).encode("utf-8")
                + b"\n"
            ),
            "rssi boolean": _frame(
                "FOF_DET:", {**valid, "rssi": False}
            ),
            "rssi fraction": _frame(
                "FOF_DET:", {**valid, "rssi": -51.5}
            ),
            "rssi below producer int8": _frame(
                "FOF_DET:", {**valid, "rssi": -129}
            ),
            "rssi above producer int8": _frame(
                "FOF_DET:", {**valid, "rssi": 128}
            ),
            "rssi below int32": _frame(
                "FOF_DET:", {**valid, "rssi": -(1 << 31) - 1}
            ),
            "rssi above int32": _frame(
                "FOF_DET:", {**valid, "rssi": 1 << 31}
            ),
            "invalid UTF-8": (
                CAPTURED_FINDMY_DETECTION[:-2] + b"\xff\n"
            ),
            "id producer buffer overflow": _frame(
                "FOF_DET:", {**valid, "id": "x" * 64}
            ),
            "id UTF-8 byte overflow": _frame(
                "FOF_DET:", {**valid, "id": "\N{LATIN SMALL LETTER E WITH ACUTE}" * 32}
            ),
            "manufacturer producer buffer overflow": _frame(
                "FOF_DET:", {**valid, "manufacturer": "x" * 32}
            ),
            "label producer buffer overflow": _frame(
                "FOF_DET:", {**valid, "badge_label": "x" * 24}
            ),
            "class producer buffer overflow": _frame(
                "FOF_DET:", {**valid, "badge_class": "x" * 16}
            ),
            "entity key producer buffer overflow": _frame(
                "FOF_DET:", {**valid, "badge_entity_key": "x" * 96}
            ),
            "payload producer buffer overflow": _frame(
                "FOF_DET:", {**valid, "id": "x" * 1600}
            ),
        }

        for name, detection in malformed.items():
            clock = SimpleNamespace(now=0.0)
            badge = flash.BadgeSerial(_usb_record(), False)
            badge.ser = _TimedByteSerial(clock, [(
                0.0,
                b"FOF_ERROR:unknown command\n" + detection,
            )])
            with self.subTest(name=name), mock.patch.object(
                flash.time, "monotonic", side_effect=lambda: clock.now
            ), mock.patch.object(
                flash.time, "sleep",
                side_effect=lambda delay: setattr(
                    clock, "now", clock.now + delay
                ),
            ), self.assertRaises(flash.SerialTransportError):
                badge.prepare_update_maintenance(
                    UPDATE_SESSION,
                    deadline=0.2,
                    source_supports_update_maintenance=False,
                )

    def test_prepare_update_detection_near_misses_still_veto_fallback(
        self,
    ) -> None:
        near_misses = (
            b"FOF_DET\n",
            b"FOF_DET_EXTRA:{}\n",
            b"FOF_STATUS:{}\n",
            b"FOF_ERROR:booting\n",
        )
        for frame in near_misses:
            clock = SimpleNamespace(now=0.0)
            badge = flash.BadgeSerial(_usb_record(), False)
            badge.ser = _TimedByteSerial(clock, [(
                0.0,
                b"FOF_ERROR:unknown command\n" + frame,
            )])
            with self.subTest(frame=frame), mock.patch.object(
                flash.time, "monotonic", side_effect=lambda: clock.now
            ), mock.patch.object(
                flash.time, "sleep",
                side_effect=lambda delay: setattr(
                    clock, "now", clock.now + delay
                ),
            ), self.assertRaises(flash.SerialReadTimeout) as caught:
                badge.prepare_update_maintenance(
                    UPDATE_SESSION,
                    deadline=0.2,
                    source_supports_update_maintenance=False,
                )
            self.assertTrue(caught.exception.saw_activity)
            self.assertFalse(caught.exception.partial_frame)

        clock = SimpleNamespace(now=0.0)
        badge = flash.BadgeSerial(_usb_record(), False)
        badge.ser = _TimedByteSerial(clock, [(
            0.0,
            b"FOF_ERROR:unknown command\n"
            + CAPTURED_FINDMY_DETECTION[:-1],
        )])
        with mock.patch.object(
            flash.time, "monotonic", side_effect=lambda: clock.now
        ), mock.patch.object(
            flash.time, "sleep",
            side_effect=lambda delay: setattr(clock, "now", clock.now + delay),
        ), self.assertRaises(flash.SerialReadTimeout) as caught:
            badge.prepare_update_maintenance(
                UPDATE_SESSION,
                deadline=0.2,
                source_supports_update_maintenance=False,
            )
        self.assertTrue(caught.exception.saw_activity)
        self.assertTrue(caught.exception.partial_frame)

    def test_prepare_update_partial_tail_only_allows_proven_nonfof_text(
        self,
    ) -> None:
        safe_tail = b"I (12345) uart_rx: partial heartbeat"
        clock = SimpleNamespace(now=0.0)
        badge = flash.BadgeSerial(_usb_record(), False)
        badge.ser = _TimedByteSerial(clock, [(
            0.0,
            b"FOF_ERROR:unknown command\n" + safe_tail,
        )])
        with mock.patch.object(
            flash.time, "monotonic", side_effect=lambda: clock.now
        ), mock.patch.object(
            flash.time, "sleep",
            side_effect=lambda delay: setattr(clock, "now", clock.now + delay),
        ), self.assertRaises(flash.UpdateMaintenanceUnsupportedError):
            badge.prepare_update_maintenance(
                UPDATE_SESSION,
                deadline=0.2,
                source_supports_update_maintenance=False,
            )
        self.assertEqual(bytes(badge._rx_buffer), safe_tail)

        for reserved_tail in (
            b"F",
            b"FO",
            b"FOF",
            b"FOF_DET:{",
            b"FOF_UPDATE_MODE:{",
        ):
            clock = SimpleNamespace(now=0.0)
            badge = flash.BadgeSerial(_usb_record(), False)
            badge.ser = _TimedByteSerial(clock, [(
                0.0,
                b"FOF_ERROR:unknown command\n" + reserved_tail,
            )])
            with self.subTest(reserved_tail=reserved_tail), mock.patch.object(
                flash.time, "monotonic", side_effect=lambda: clock.now
            ), mock.patch.object(
                flash.time, "sleep",
                side_effect=lambda delay: setattr(
                    clock, "now", clock.now + delay
                ),
            ), self.assertRaises(flash.SerialReadTimeout) as caught:
                badge.prepare_update_maintenance(
                    UPDATE_SESSION,
                    deadline=0.2,
                    source_supports_update_maintenance=False,
                )
            self.assertTrue(caught.exception.partial_frame)
            self.assertEqual(bytes(badge._rx_buffer), reserved_tail)

        clock = SimpleNamespace(now=0.0)
        badge = flash.BadgeSerial(_usb_record(), False)
        badge.ser = _TimedByteSerial(clock, [(0.0, safe_tail)])
        with mock.patch.object(
            flash.time, "monotonic", side_effect=lambda: clock.now
        ), mock.patch.object(
            flash.time, "sleep",
            side_effect=lambda delay: setattr(clock, "now", clock.now + delay),
        ), self.assertRaises(flash.SerialReadTimeout) as caught:
            badge.prepare_update_maintenance(
                UPDATE_SESSION,
                deadline=0.2,
                source_supports_update_maintenance=False,
            )
        self.assertTrue(caught.exception.partial_frame)
        self.assertEqual(bytes(badge._rx_buffer), safe_tail)

    def test_direct_bootstrap_reproof_accepts_only_unchanged_dot_78(
        self,
    ) -> None:
        source = _uplink_status(
            flash.UPDATE_MAINTENANCE_BOOTSTRAP_SOURCE_VERSION
        )
        fresh = {
            **source,
            "usb_health": {"responses_completed": 23},
        }
        badge = flash.BadgeSerial(_usb_record(), False)
        badge._prove_open_application = mock.Mock(  # type: ignore[method-assign]
            return_value=fresh
        )
        with mock.patch.object(
            flash.time, "monotonic", return_value=1.0
        ):
            actual = flash._prove_direct_bootstrap_source_after_rejection(
                badge,
                initial_status=source,
                target_version=(
                    flash.UPDATE_MAINTENANCE_BOOTSTRAP_TARGET_VERSION
                ),
                deadline=5.0,
            )

        self.assertEqual(actual, fresh)
        badge._prove_open_application.assert_called_once_with(4.0)

    def test_direct_bootstrap_reproof_rejects_every_near_miss(
        self,
    ) -> None:
        source = _uplink_status(
            flash.UPDATE_MAINTENANCE_BOOTSTRAP_SOURCE_VERSION
        )
        healthy_fresh = {
            **source,
            "usb_health": {"responses_completed": 23},
        }
        other_id = "14:c1:9f:52:ca:b0"
        cases = (
            (
                "identity drift",
                source,
                {**healthy_fresh, "hardware_id": other_id},
                flash.UPDATE_MAINTENANCE_BOOTSTRAP_TARGET_VERSION,
                True,
            ),
            (
                "unhealthy source",
                {**source, "pending_verify": True,
                 "rollback_state": "pending_verify",
                 "recovery_mode": "startup_dependency"},
                healthy_fresh,
                flash.UPDATE_MAINTENANCE_BOOTSTRAP_TARGET_VERSION,
                False,
            ),
            (
                "unhealthy fresh",
                source,
                {**healthy_fresh, "pending_verify": True,
                 "rollback_state": "pending_verify",
                 "recovery_mode": "startup_dependency"},
                flash.UPDATE_MAINTENANCE_BOOTSTRAP_TARGET_VERSION,
                True,
            ),
            (
                "partition drift",
                source,
                {**healthy_fresh, "running_partition": "ota_1"},
                flash.UPDATE_MAINTENANCE_BOOTSTRAP_TARGET_VERSION,
                True,
            ),
            (
                "response counter reset",
                source,
                {**healthy_fresh,
                 "usb_health": {"responses_completed": 1}},
                flash.UPDATE_MAINTENANCE_BOOTSTRAP_TARGET_VERSION,
                True,
            ),
            (
                "response counter unchanged",
                source,
                {**healthy_fresh,
                 "usb_health": {"responses_completed": 20}},
                flash.UPDATE_MAINTENANCE_BOOTSTRAP_TARGET_VERSION,
                True,
            ),
            (
                "response counter bool",
                source,
                {**healthy_fresh,
                 "usb_health": {"responses_completed": True}},
                flash.UPDATE_MAINTENANCE_BOOTSTRAP_TARGET_VERSION,
                True,
            ),
            (
                "OTA byte counter bool",
                {
                    **source,
                    "uplink_ota": {
                        "state": "idle",
                        "partition": "",
                        "received": 0,
                        "total": 0,
                        "target_version": "",
                        "last_error": "",
                    },
                },
                {
                    **healthy_fresh,
                    "uplink_ota": {
                        "state": "idle",
                        "partition": "",
                        "received": False,
                        "total": 0,
                        "target_version": "",
                        "last_error": "",
                    },
                },
                flash.UPDATE_MAINTENANCE_BOOTSTRAP_TARGET_VERSION,
                True,
            ),
            (
                "newer fresh",
                source,
                {**healthy_fresh, "version":
                 flash.UPDATE_MAINTENANCE_BOOTSTRAP_TARGET_VERSION},
                flash.UPDATE_MAINTENANCE_BOOTSTRAP_TARGET_VERSION,
                True,
            ),
            (
                "wrong status target",
                source,
                {**healthy_fresh, "target": "other"},
                flash.UPDATE_MAINTENANCE_BOOTSTRAP_TARGET_VERSION,
                True,
            ),
            (
                "wrong firmware target",
                source,
                healthy_fresh,
                "0.64.80-badge-defcon34",
                False,
            ),
            (
                "newer source",
                {**source, "version":
                 flash.UPDATE_MAINTENANCE_BOOTSTRAP_TARGET_VERSION},
                healthy_fresh,
                flash.UPDATE_MAINTENANCE_BOOTSTRAP_TARGET_VERSION,
                False,
            ),
        )
        for (
            label, initial, fresh, target, should_reprobe
        ) in cases:
            badge = flash.BadgeSerial(_usb_record(), False)
            badge._prove_open_application = mock.Mock(  # type: ignore[method-assign]
                return_value=fresh
            )
            with self.subTest(label=label), mock.patch.object(
                flash.time, "monotonic", return_value=1.0
            ), self.assertRaises(flash.FlashError):
                flash._prove_direct_bootstrap_source_after_rejection(
                    badge,
                    initial_status=initial,
                    target_version=target,
                    deadline=5.0,
                )
            self.assertEqual(
                badge._prove_open_application.call_count,
                1 if should_reprobe else 0,
            )

    def test_direct_bootstrap_uploader_rejects_second_proof_drift_before_begin(
        self,
    ) -> None:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        source_version = (
            flash.UPDATE_MAINTENANCE_BOOTSTRAP_SOURCE_VERSION
        )
        target_version = (
            flash.UPDATE_MAINTENANCE_BOOTSTRAP_TARGET_VERSION
        )
        source = _uplink_status(source_version)
        data = _firmware_image(
            platform["uplink_project"],
            target_version,
            platform["uplink_name"],
            platform["hardware_type"],
        )
        drift_cases = (
            {
                **source,
                "running_partition": "ota_1",
                "usb_health": {"responses_completed": 23},
            },
            {
                **source,
                "version": target_version,
                "usb_health": {"responses_completed": 23},
            },
        )
        for fresh in drift_cases:
            badge = flash.BadgeSerial(_usb_record(), False)
            badge._prove_open_application = mock.Mock(  # type: ignore[method-assign]
                return_value=fresh
            )
            badge.write_line = mock.Mock(  # type: ignore[method-assign]
                side_effect=AssertionError(
                    "drifted second proof reached uplink_ota_begin"
                )
            )
            with self.subTest(fresh=fresh), self.assertRaises(
                flash.FlashError
            ):
                badge.upload_uplink_firmware(
                    platform,
                    _frozen_firmware_set(data),
                    target_version,
                    expected_pre_status=source,
                )
            badge.write_line.assert_not_called()

    def test_finish_and_abort_require_exact_lifecycle_receipts(self) -> None:
        for command, phase, method_name in (
            ("finish_update", "finishing", "finish_update_maintenance"),
            ("abort_update", "aborting", "abort_update_maintenance"),
        ):
            receipt = {
                "ok": True,
                "phase": phase,
                "session": UPDATE_SESSION,
                "retryable": False,
                "reboot_required": True,
            }
            clock = SimpleNamespace(now=0.0)
            serial = _TimedByteSerial(
                clock, [(0.0, _frame("FOF_UPDATE_MODE:", receipt))]
            )
            badge = flash.BadgeSerial(_usb_record(), False)
            badge._update_session = UPDATE_SESSION
            badge.ser = serial

            actual = getattr(badge, method_name)(
                deadline=time.monotonic() + 30.0
            )

            self.assertEqual(actual, receipt)
            self.assertEqual(
                serial.writes,
                [(
                    "\nFOF_CTL:"
                    + json.dumps(
                        {"cmd": command, "session": UPDATE_SESSION},
                        separators=(",", ":"),
                    )
                    + "\n"
                ).encode("ascii")],
            )

    def test_finish_and_abort_retry_exact_retryable_busy_receipts(
        self,
    ) -> None:
        for command, phase, method_name, error in (
            (
                "finish_update",
                "finishing",
                "finish_update_maintenance",
                "success_gates_pending",
            ),
            (
                "abort_update",
                "aborting",
                "abort_update_maintenance",
                "firmware_operation_active",
            ),
        ):
            busy = {
                "ok": False,
                "phase": "busy",
                "session": UPDATE_SESSION,
                "retryable": True,
                "reboot_required": False,
                "error": error,
            }
            terminal = {
                "ok": True,
                "phase": phase,
                "session": UPDATE_SESSION,
                "retryable": False,
                "reboot_required": True,
            }
            clock = SimpleNamespace(now=0.0)
            serial = _TimedByteSerial(clock, [(
                0.0,
                _frame("FOF_UPDATE_MODE:", busy)
                + _frame("FOF_UPDATE_MODE:", terminal),
            )])
            badge = flash.BadgeSerial(_usb_record(), False)
            badge._update_session = UPDATE_SESSION
            badge.ser = serial

            with self.subTest(command=command), mock.patch.object(
                flash.time, "monotonic", side_effect=lambda: clock.now
            ), mock.patch.object(
                flash.time, "sleep",
                side_effect=lambda delay: setattr(
                    clock, "now", clock.now + delay
                ),
            ):
                actual = getattr(badge, method_name)(deadline=30.0)

            self.assertEqual(actual, terminal)
            expected_command = (
                "\nFOF_CTL:"
                + json.dumps(
                    {"cmd": command, "session": UPDATE_SESSION},
                    separators=(",", ":"),
                )
                + "\n"
            ).encode("ascii")
            self.assertEqual(
                serial.writes, [expected_command, expected_command]
            )
            self.assertEqual(clock.now, flash.UPDATE_PREPARE_RETRY_S)

    def test_direct_bootstrap_is_exactly_dot_78_to_dot_79(self) -> None:
        source = _uplink_status(
            flash.UPDATE_MAINTENANCE_BOOTSTRAP_SOURCE_VERSION
        )
        self.assertTrue(
            flash._allows_direct_update_maintenance_bootstrap(
                source,
                target_version=(
                    flash.UPDATE_MAINTENANCE_BOOTSTRAP_TARGET_VERSION
                ),
            )
        )
        near_misses = (
            ({**source, "version": OLD_VERSION},
             flash.UPDATE_MAINTENANCE_BOOTSTRAP_TARGET_VERSION),
            ({**source, "firmware_name": "other"},
             flash.UPDATE_MAINTENANCE_BOOTSTRAP_TARGET_VERSION),
            ({**source, "app_project": "other"},
             flash.UPDATE_MAINTENANCE_BOOTSTRAP_TARGET_VERSION),
            ({**source, "hardware_type": "other"},
             flash.UPDATE_MAINTENANCE_BOOTSTRAP_TARGET_VERSION),
            (source, source["version"]),
            (source, "0.64.80-badge-defcon34"),
        )
        for status, target in near_misses:
            with self.subTest(status=status, target=target):
                self.assertFalse(
                    flash._allows_direct_update_maintenance_bootstrap(
                        status, target_version=target
                    )
                )

    def test_reconnect_same_uplink_retries_boot_races_and_proves_maintenance(
        self,
    ) -> None:
        clock = SimpleNamespace(now=0.0)
        rebound = _usb_record("/dev/rebound")
        badge = flash.BadgeSerial(_usb_record("/dev/old"), False)
        badge._update_session = UPDATE_SESSION
        opens = 0
        closes = 0
        statuses = 0
        rebound_serials: list[_TimedByteSerial] = []

        def open_serial() -> None:
            nonlocal opens
            opens += 1
            if opens == 1:
                raise flash.SerialTransportError(
                    "startup race", terminal_unavailable=True
                )
            serial = _TimedByteSerial(
                clock,
                [(0.0, f"FOF_PONG:{VERSION}\n".encode("ascii"))],
            )
            rebound_serials.append(serial)
            badge.ser = serial

        def close_serial() -> None:
            nonlocal closes
            closes += 1
            badge.ser = None

        def status(*, timeout_s: float) -> dict:
            nonlocal statuses
            self.assertGreater(timeout_s, 0)
            statuses += 1
            if statuses == 1:
                raise flash.SerialReadTimeout(
                    "startup log race",
                    saw_activity=True,
                    partial_frame=False,
                )
            return _maintenance_status()

        badge._open_serial = open_serial  # type: ignore[method-assign]
        badge._close_serial = close_serial  # type: ignore[method-assign]
        badge.status = status  # type: ignore[method-assign]
        with mock.patch.object(
            flash, "_take_badge_usb_descriptor_census",
            return_value=(rebound,),
        ), mock.patch.object(
            flash.time, "monotonic", side_effect=lambda: clock.now
        ), mock.patch.object(
            flash.time, "sleep",
            side_effect=lambda delay: setattr(clock, "now", clock.now + delay),
        ):
            result = badge.reconnect_same_uplink(deadline=10.0)

        self.assertEqual(result, _maintenance_status())
        self.assertEqual(badge.port, "/dev/rebound")
        self.assertEqual(opens, 3)
        self.assertEqual(statuses, 2)
        self.assertGreaterEqual(closes, 3)
        self.assertEqual(
            [serial.writes for serial in rebound_serials],
            [[b"\nFOF_PING\n"], [b"\nFOF_PING\n"]],
        )

    def test_reconnect_same_uplink_retries_terminal_status_startup_race(
        self,
    ) -> None:
        clock = SimpleNamespace(now=0.0)
        rebound = _usb_record("/dev/rebound")
        badge = flash.BadgeSerial(_usb_record("/dev/old"), False)
        badge._update_session = UPDATE_SESSION
        statuses = 0
        closes = 0
        rebound_serials: list[_TimedByteSerial] = []

        def open_serial() -> None:
            serial = _TimedByteSerial(
                clock,
                [(0.0, f"FOF_PONG:{VERSION}\n".encode("ascii"))],
            )
            rebound_serials.append(serial)
            badge.ser = serial

        def close_serial() -> None:
            nonlocal closes
            closes += 1
            badge.ser = None

        def status(*, timeout_s: float) -> dict:
            nonlocal statuses
            self.assertGreater(timeout_s, 0)
            statuses += 1
            if statuses == 1:
                raise flash.SerialTransportError(
                    "CDC application endpoint is not ready",
                    terminal_unavailable=True,
                )
            return _maintenance_status()

        badge._open_serial = open_serial  # type: ignore[method-assign]
        badge._close_serial = close_serial  # type: ignore[method-assign]
        badge.status = status  # type: ignore[method-assign]
        with mock.patch.object(
            flash, "_take_badge_usb_descriptor_census",
            return_value=(rebound,),
        ), mock.patch.object(
            flash.time, "monotonic", side_effect=lambda: clock.now
        ), mock.patch.object(
            flash.time, "sleep",
            side_effect=lambda delay: setattr(clock, "now", clock.now + delay),
        ):
            result = badge.reconnect_same_uplink(deadline=1.0)

        self.assertEqual(result, _maintenance_status())
        self.assertEqual(statuses, 2)
        self.assertEqual(clock.now, flash.UPDATE_PREPARE_RETRY_S)
        self.assertGreaterEqual(closes, 2)
        self.assertEqual(
            [serial.writes for serial in rebound_serials],
            [[b"\nFOF_PING\n"], [b"\nFOF_PING\n"]],
        )

    def test_reconnect_terminal_status_never_extends_absolute_deadline(
        self,
    ) -> None:
        clock = SimpleNamespace(now=0.0)
        badge = flash.BadgeSerial(_usb_record("/dev/old"), False)
        badge._update_session = UPDATE_SESSION
        status_calls = 0
        rebound_serials: list[_TimedByteSerial] = []

        def open_serial() -> None:
            serial = _TimedByteSerial(
                clock,
                [(0.0, f"FOF_PONG:{VERSION}\n".encode("ascii"))],
            )
            rebound_serials.append(serial)
            badge.ser = serial

        badge._open_serial = open_serial  # type: ignore[method-assign]
        badge._close_serial = lambda: setattr(  # type: ignore[method-assign]
            badge, "ser", None
        )

        def status(*, timeout_s: float) -> dict:
            nonlocal status_calls
            self.assertGreater(timeout_s, 0)
            status_calls += 1
            raise flash.SerialTransportError(
                "CDC application endpoint is not ready",
                terminal_unavailable=True,
            )

        badge.status = status  # type: ignore[method-assign]
        with mock.patch.object(
            flash, "_take_badge_usb_descriptor_census",
            return_value=(_usb_record("/dev/rebound"),),
        ), mock.patch.object(
            flash.time, "monotonic", side_effect=lambda: clock.now
        ), mock.patch.object(
            flash.time, "sleep",
            side_effect=lambda delay: setattr(clock, "now", clock.now + delay),
        ), self.assertRaises(flash.SerialReadTimeout):
            badge.reconnect_same_uplink(deadline=0.5)

        self.assertEqual(clock.now, 0.5)
        self.assertEqual(status_calls, 2)
        self.assertEqual(
            [serial.writes for serial in rebound_serials],
            [[b"\nFOF_PING\n"], [b"\nFOF_PING\n"]],
        )

    def test_reconnect_same_uplink_does_not_retry_nonterminal_status_error(
        self,
    ) -> None:
        badge = flash.BadgeSerial(_usb_record("/dev/old"), False)
        badge._update_session = UPDATE_SESSION
        opens = 0
        clock = SimpleNamespace(now=0.0)
        rebound_serial = _TimedByteSerial(
            clock,
            [(0.0, f"FOF_PONG:{VERSION}\n".encode("ascii"))],
        )

        def open_serial() -> None:
            nonlocal opens
            opens += 1
            badge.ser = rebound_serial

        badge._open_serial = open_serial  # type: ignore[method-assign]
        badge.status = mock.Mock(  # type: ignore[method-assign]
            side_effect=flash.SerialTransportError("malformed status frame")
        )
        with mock.patch.object(
            flash, "_take_badge_usb_descriptor_census",
            return_value=(_usb_record("/dev/rebound"),),
        ), self.assertRaisesRegex(
            flash.SerialTransportError, "malformed status"
        ):
            badge.reconnect_same_uplink(
                deadline=time.monotonic() + 1.0
            )

        self.assertEqual(opens, 1)
        self.assertEqual(badge.status.call_count, 1)
        self.assertEqual(rebound_serial.writes, [b"\nFOF_PING\n"])

    def test_reconnect_same_uplink_rejects_location_before_open(self) -> None:
        badge = flash.BadgeSerial(_usb_record("/dev/old"), False)
        badge._update_session = UPDATE_SESSION
        opened = False

        def open_serial() -> None:
            nonlocal opened
            opened = True

        badge._open_serial = open_serial  # type: ignore[method-assign]
        moved = flash.UsbDescriptorRecord(
            device="/dev/moved",
            vid=flash.ESPRESSIF_USB_SERIAL_JTAG_VID,
            pid=flash.ESPRESSIF_USB_SERIAL_JTAG_PID,
            serial_number=badge.expected_hardware_id,
            location="different-location",
            stat_device=1,
            stat_inode=2,
            stat_rdev=3,
        )
        with mock.patch.object(
            flash, "_take_badge_usb_descriptor_census",
            return_value=(moved,),
        ), self.assertRaisesRegex(flash.FlashError, "location"):
            badge.reconnect_same_uplink(deadline=time.monotonic() + 1.0)

        self.assertFalse(opened)

    def test_reconnect_same_uplink_requires_exact_maintenance_proof(
        self,
    ) -> None:
        badge = flash.BadgeSerial(_usb_record("/dev/old"), False)
        badge._update_session = UPDATE_SESSION
        rebound = _usb_record("/dev/rebound")
        clock = SimpleNamespace(now=0.0)
        rebound_serial = _TimedByteSerial(
            clock,
            [(0.0, f"FOF_PONG:{VERSION}\n".encode("ascii"))],
        )
        badge._open_serial = lambda: setattr(  # type: ignore[method-assign]
            badge, "ser", rebound_serial
        )
        bad_status = _maintenance_status(ble_initialized=True)
        badge.status = lambda **_kwargs: bad_status  # type: ignore[method-assign]

        with mock.patch.object(
            flash, "_take_badge_usb_descriptor_census",
            return_value=(rebound,),
        ), self.assertRaisesRegex(flash.FlashError, "Bluetooth|BLE"):
            badge.reconnect_same_uplink(deadline=time.monotonic() + 1.0)

        self.assertEqual(rebound_serial.writes, [b"\nFOF_PING\n"])

    def test_reconnect_sends_ping_before_status_on_rebound_transport(
        self,
    ) -> None:
        clock = SimpleNamespace(now=0.0)
        serial = _TimedByteSerial(
            clock,
            [(
                0.0,
                f"FOF_PONG:{VERSION}\n".encode("ascii")
                + _frame("FOF_STATUS:", _maintenance_status()),
            )],
        )
        badge = flash.BadgeSerial(_usb_record("/dev/old"), False)
        badge._update_session = UPDATE_SESSION
        badge._open_serial = lambda: setattr(  # type: ignore[method-assign]
            badge, "ser", serial
        )

        with mock.patch.object(
            flash, "_take_badge_usb_descriptor_census",
            return_value=(_usb_record("/dev/rebound"),),
        ):
            result = badge.reconnect_same_uplink(
                deadline=time.monotonic() + 1.0
            )

        self.assertEqual(result, _maintenance_status())
        self.assertEqual(
            serial.writes,
            [b"\nFOF_PING\n", b"\nFOF_STATUS\n"],
        )

    def test_reconnect_waits_for_binary_parser_idle_before_first_command(
        self,
    ) -> None:
        clock = SimpleNamespace(now=0.0)
        badge = flash.BadgeSerial(_usb_record("/dev/old"), False)
        badge._update_session = UPDATE_SESSION
        badge._binary_transport_loss_at = 0.0
        command_times: list[tuple[bytes, float]] = []
        status_times: list[float] = []

        class RecordingSerial(_TimedByteSerial):
            def write(self, payload: bytes) -> int:
                command_times.append((bytes(payload), clock.now))
                return super().write(payload)

        rebound_serial = RecordingSerial(
            clock,
            [(0.0, f"FOF_PONG:{VERSION}\n".encode("ascii"))],
        )
        badge._open_serial = lambda: setattr(  # type: ignore[method-assign]
            badge, "ser", rebound_serial
        )

        def status(*, timeout_s: float) -> dict:
            self.assertGreater(timeout_s, 0)
            status_times.append(clock.now)
            return _maintenance_status()

        badge.status = status  # type: ignore[method-assign]
        with mock.patch.object(
            flash, "_take_badge_usb_descriptor_census",
            return_value=(_usb_record("/dev/rebound"),),
        ), mock.patch.object(
            flash.time, "monotonic", side_effect=lambda: clock.now
        ), mock.patch.object(
            flash.time, "sleep",
            side_effect=lambda delay: setattr(clock, "now", clock.now + delay),
        ):
            badge.reconnect_same_uplink(deadline=10.0)

        self.assertEqual(len(command_times), 1)
        self.assertEqual(command_times[0][0], b"\nFOF_PING\n")
        self.assertEqual(rebound_serial.writes, [b"\nFOF_PING\n"])
        self.assertEqual(len(status_times), 1)
        self.assertGreaterEqual(
            command_times[0][1],
            flash.UPDATE_BINARY_IDLE_TIMEOUT_S
            + flash.UPDATE_BINARY_IDLE_GUARD_S,
        )
        self.assertLessEqual(command_times[0][1], status_times[0])

    def test_reconcile_uplink_accepts_only_exact_durable_commit(self) -> None:
        badge = flash.BadgeSerial(_usb_record(), False)
        badge._update_session = UPDATE_SESSION
        badge.status = lambda **_kwargs: _maintenance_uplink_phase(  # type: ignore[method-assign]
            "committed", received=8192
        )

        self.assertEqual(
            badge.reconcile_uplink_ota(_uplink_reconcile_expected()),
            "committed",
        )

        for field, replacement in (
            ("session", "FEDCBA9876543210"),
            ("version", OLD_VERSION),
            ("sha256", "b" * 64),
            ("size", 8191),
            ("partition", "ota_0"),
            ("received", 8191),
        ):
            status = _maintenance_uplink_phase(
                "committed", received=8192
            )
            status["update_uplink"][field] = replacement
            badge.status = lambda status=status, **_kwargs: status  # type: ignore[method-assign]
            with self.subTest(field=field), self.assertRaises(
                flash.FlashError
            ):
                badge.reconcile_uplink_ota(
                    _uplink_reconcile_expected()
                )

    def test_reconcile_uplink_aborts_exact_receiving_before_zero_restart(
        self,
    ) -> None:
        expected = _uplink_reconcile_expected()
        aborted = _uplink_receipt(
            "aborted",
            total=expected["size"],
            received=4096,
            credit=0,
            ok=True,
            reboot_required=False,
            error="",
        )
        aborted["retryable"] = True
        clock = SimpleNamespace(now=0.0)
        serial = _TimedByteSerial(
            clock, [(0.0, _frame("FOF_UPLINK_OTA:", aborted))]
        )
        badge = flash.BadgeSerial(_usb_record(), False)
        badge._update_session = UPDATE_SESSION
        badge.ser = serial
        badge.status = lambda **_kwargs: _maintenance_uplink_phase(  # type: ignore[method-assign]
            "receiving", received=4096
        )

        self.assertEqual(
            badge.reconcile_uplink_ota(expected),
            "restart_from_zero",
        )
        self.assertEqual(
            serial.writes,
            [
                b'\nFOF_CTL:{"cmd":"uplink_ota_abort",'
                b'"session":"0123456789ABCDEF"}\n'
            ],
        )

    def test_reconcile_uplink_idle_restarts_and_ambiguous_state_fails(
        self,
    ) -> None:
        badge = flash.BadgeSerial(_usb_record(), False)
        badge._update_session = UPDATE_SESSION
        badge.status = lambda **_kwargs: _maintenance_status()  # type: ignore[method-assign]
        self.assertEqual(
            badge.reconcile_uplink_ota(_uplink_reconcile_expected()),
            "restart_from_zero",
        )

        ambiguous = _maintenance_uplink_phase(
            "receiving", received=4096
        )
        ambiguous["update_uplink"]["partition"] = "ota_0"
        badge.status = lambda **_kwargs: ambiguous  # type: ignore[method-assign]
        with self.assertRaises(flash.FlashError):
            badge.reconcile_uplink_ota(_uplink_reconcile_expected())

    def test_reconcile_scanner_accepts_exact_commit_and_restarts_idle(
        self,
    ) -> None:
        badge = flash.BadgeSerial(_usb_record(), False)
        badge._update_session = UPDATE_SESSION
        badge.status = lambda **_kwargs: _maintenance_scanner_phase(  # type: ignore[method-assign]
            "committed", received=12288, generation=7
        )
        self.assertEqual(
            badge.reconcile_scanner_stage(_scanner_reconcile_expected()),
            "committed",
        )

        badge.status = lambda **_kwargs: _maintenance_status()  # type: ignore[method-assign]
        self.assertEqual(
            badge.reconcile_scanner_stage(_scanner_reconcile_expected()),
            "restart_from_zero",
        )

    def test_reconcile_scanner_matching_parser_restarts_from_byte_zero(
        self,
    ) -> None:
        badge = flash.BadgeSerial(_usb_record(), False)
        badge._update_session = UPDATE_SESSION
        badge.status = lambda **_kwargs: _maintenance_scanner_phase(  # type: ignore[method-assign]
            "receiving", received=4096, generation=0
        )

        self.assertEqual(
            badge.reconcile_scanner_stage(_scanner_reconcile_expected()),
            "restart_from_zero",
        )

    def test_reconcile_scanner_mismatch_and_impossible_state_fail_closed(
        self,
    ) -> None:
        badge = flash.BadgeSerial(_usb_record(), False)
        badge._update_session = UPDATE_SESSION
        active_mismatch = _maintenance_scanner_phase(
            "receiving", received=4096, generation=0
        )
        active_mismatch["update_scanner"]["sha256"] = "c" * 64
        badge.status = lambda **_kwargs: active_mismatch  # type: ignore[method-assign]
        with self.assertRaises(flash.FlashError):
            badge.reconcile_scanner_stage(_scanner_reconcile_expected())

        impossible = _maintenance_scanner_phase(
            "committed", received=12287, generation=7
        )
        badge.status = lambda **_kwargs: impossible  # type: ignore[method-assign]
        with self.assertRaises(flash.FlashError):
            badge.reconcile_scanner_stage(_scanner_reconcile_expected())

    def test_reconcile_scanner_other_valid_commit_restarts_from_zero(
        self,
    ) -> None:
        badge = flash.BadgeSerial(_usb_record(), False)
        badge._update_session = UPDATE_SESSION
        other = _maintenance_scanner_phase(
            "committed", received=12288, generation=7
        )
        other["update_scanner"]["sha256"] = "c" * 64
        badge.status = lambda **_kwargs: other  # type: ignore[method-assign]

        self.assertEqual(
            badge.reconcile_scanner_stage(_scanner_reconcile_expected()),
            "restart_from_zero",
        )

    def test_interrupted_uplink_transfer_restarts_at_byte_zero_only(
        self,
    ) -> None:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        data = _firmware_image(
            platform["uplink_project"],
            VERSION,
            platform["uplink_name"],
            platform["hardware_type"],
        ) + b"X" * 5000
        first_credit = min(flash.UPLINK_OTA_CREDIT_BYTES, len(data))
        second_credit = len(data) - first_credit
        ready = _uplink_receipt(
            "ready", total=len(data), credit=first_credit
        )
        credit = _uplink_receipt(
            "credit",
            total=len(data),
            received=first_credit,
            credit=second_credit,
        )
        committed = _uplink_receipt(
            "committed",
            total=len(data),
            received=len(data),
            credit=0,
            reboot_required=True,
        )
        clock = SimpleNamespace(now=0.0)

        class FailSecondBinaryWrite(_TimedByteSerial):
            def __init__(self) -> None:
                super().__init__(
                    clock, [(0.0, _frame("FOF_UPLINK_OTA:", ready))]
                )
                self.binary_writes: list[bytes] = []

            def write(self, payload: bytes) -> int:
                value = bytes(payload)
                self.writes.append(value)
                if value.startswith(b"\n"):
                    return len(value)
                if len(self.binary_writes) == 1:
                    raise OSError("scheduled transport loss")
                self.binary_writes.append(value)
                return len(value)

        first = FailSecondBinaryWrite()
        second = _TimedByteSerial(clock, [(
            0.0,
            _frame("FOF_UPLINK_OTA:", ready)
            + _frame("FOF_UPLINK_OTA:", credit)
            + _frame("FOF_UPLINK_OTA:", committed),
        )])
        badge = flash.BadgeSerial(_usb_record(), False)
        badge._update_session = UPDATE_SESSION
        badge.ser = first
        badge.status = lambda **_kwargs: _maintenance_status(OLD_VERSION)  # type: ignore[method-assign]
        reconnects: list[float] = []
        reconciliations: list[dict] = []

        def reconnect(*, deadline: float) -> dict:
            reconnects.append(deadline)
            badge.ser = second
            return _maintenance_status(OLD_VERSION)

        def reconcile(expected: dict) -> str:
            reconciliations.append(dict(expected))
            return "restart_from_zero"

        badge.reconnect_same_uplink = reconnect  # type: ignore[method-assign]
        badge.reconcile_uplink_ota = reconcile  # type: ignore[method-assign]
        with contextlib.redirect_stdout(io.StringIO()):
            receipt = badge.upload_uplink_firmware(
                platform, _frozen_firmware_set(data), VERSION
            )

        self.assertEqual(receipt, committed)
        self.assertEqual(len(reconnects), 1)
        self.assertEqual(len(reconciliations), 1)
        self.assertEqual(
            reconciliations[0],
            {
                "session": UPDATE_SESSION,
                "version": VERSION,
                "sha256": hashlib.sha256(data).hexdigest(),
                "size": len(data),
                "partition": "ota_1",
            },
        )
        self.assertEqual(first.binary_writes, [data[:1024]])
        second_binary = [
            payload for payload in second.writes
            if not payload.startswith(b"\n")
        ]
        self.assertEqual(b"".join(second_binary), data)
        self.assertEqual(second_binary[0], data[:1024])
        command_writes = [
            payload for payload in first.writes + second.writes
            if payload.startswith(b"\n")
        ]
        self.assertEqual(len(command_writes), 2)
        self.assertTrue(all(
            b'"session":"0123456789ABCDEF"' in payload
            for payload in command_writes
        ))

    def test_interrupted_scanner_stage_restarts_at_byte_zero_only(
        self,
    ) -> None:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        data = _firmware_image(
            platform["scanner_project"],
            VERSION,
            platform["scanner_name"],
            platform["hardware_type"],
        ) + b"Y" * 5000
        first_credit = min(flash.SCANNER_STAGE_CREDIT_BYTES, len(data))
        second_credit = len(data) - first_credit
        ready = _stage_receipt(
            platform, VERSION, data, 3,
            phase="ready", received=0, credit=first_credit,
        )
        credit = _stage_receipt(
            platform, VERSION, data, 3,
            phase="credit", received=first_credit, credit=second_credit,
        )
        final = _stage_receipt(
            platform, VERSION, data, 3,
            phase="final", received=len(data), credit=0, generation=9,
        )
        clock = SimpleNamespace(now=0.0)

        class FailSecondBinaryWrite(_TimedByteSerial):
            def __init__(self) -> None:
                super().__init__(
                    clock, [(0.0, _frame("FOF_FW_UPLOAD:", ready))]
                )
                self.binary_writes: list[bytes] = []

            def write(self, payload: bytes) -> int:
                value = bytes(payload)
                self.writes.append(value)
                if value.startswith(b"\n"):
                    return len(value)
                if len(self.binary_writes) == 1:
                    raise OSError("scheduled scanner transport loss")
                self.binary_writes.append(value)
                return len(value)

        first = FailSecondBinaryWrite()
        second = _TimedByteSerial(clock, [(
            0.0,
            _frame("FOF_FW_UPLOAD:", ready)
            + _frame("FOF_FW_UPLOAD:", credit)
            + _frame("FOF_FW_UPLOAD:", final),
        )])
        badge = flash.BadgeSerial(_usb_record(), False)
        badge._update_session = UPDATE_SESSION
        badge.ser = first
        badge.status = lambda **_kwargs: _maintenance_status()  # type: ignore[method-assign]
        reconciliations: list[dict] = []

        def reconnect(*, deadline: float) -> dict:
            self.assertGreater(deadline, time.monotonic())
            badge.ser = second
            return _maintenance_status()

        def reconcile(expected: dict) -> str:
            reconciliations.append(dict(expected))
            return "restart_from_zero"

        badge.reconnect_same_uplink = reconnect  # type: ignore[method-assign]
        badge.reconcile_scanner_stage = reconcile  # type: ignore[method-assign]
        with contextlib.redirect_stdout(io.StringIO()):
            receipt = badge.stage_scanner_firmware(
                platform, _frozen_firmware_set(data), VERSION,
                ["ble", "wifi"],
            )

        self.assertEqual(receipt, final)
        self.assertEqual(
            reconciliations,
            [{
                "session": UPDATE_SESSION,
                "target": platform["scanner_name"],
                "sha256": hashlib.sha256(data).hexdigest(),
                "size": len(data),
                "slot_mask": 3,
            }],
        )
        self.assertEqual(first.binary_writes, [data[:1024]])
        second_binary = [
            payload for payload in second.writes
            if not payload.startswith(b"\n")
        ]
        self.assertEqual(b"".join(second_binary), data)
        self.assertEqual(second_binary[0], data[:1024])
        command_writes = [
            payload for payload in first.writes + second.writes
            if payload.startswith(b"\n")
        ]
        self.assertEqual(len(command_writes), 2)
        self.assertTrue(all(
            b'"session":"0123456789ABCDEF"' in payload
            for payload in command_writes
        ))

    def test_missing_scanner_credit_receipt_restarts_before_stage_deadline(
        self,
    ) -> None:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        data = _firmware_image(
            platform["scanner_project"],
            VERSION,
            platform["scanner_name"],
            platform["hardware_type"],
        ) + b"Y" * 5000
        first_credit = min(flash.SCANNER_STAGE_CREDIT_BYTES, len(data))
        second_credit = len(data) - first_credit
        ready = _stage_receipt(
            platform, VERSION, data, 3,
            phase="ready", received=0, credit=first_credit,
        )
        credit = _stage_receipt(
            platform, VERSION, data, 3,
            phase="credit", received=first_credit, credit=second_credit,
        )
        final = _stage_receipt(
            platform, VERSION, data, 3,
            phase="final", received=len(data), credit=0, generation=10,
        )
        clock = SimpleNamespace(now=0.0)
        first = _TimedByteSerial(
            clock, [(0.0, _frame("FOF_FW_UPLOAD:", ready))]
        )
        second = _TimedByteSerial(clock, [(
            0.0,
            _frame("FOF_FW_UPLOAD:", ready)
            + _frame("FOF_FW_UPLOAD:", credit)
            + _frame("FOF_FW_UPLOAD:", final),
        )])
        badge = flash.BadgeSerial(_usb_record(), False)
        badge._update_session = UPDATE_SESSION
        badge.ser = first
        badge.status = lambda **_kwargs: _maintenance_status()  # type: ignore[method-assign]
        reconnect_times: list[float] = []

        def reconnect(*, deadline: float) -> dict:
            reconnect_times.append(clock.now)
            self.assertGreater(deadline, clock.now)
            badge.ser = second
            return _maintenance_status()

        badge.reconnect_same_uplink = reconnect  # type: ignore[method-assign]
        badge.reconcile_scanner_stage = (  # type: ignore[method-assign]
            lambda _expected: "restart_from_zero"
        )
        with contextlib.redirect_stdout(io.StringIO()), \
             mock.patch.object(
                 flash.time, "monotonic", side_effect=lambda: clock.now
             ), mock.patch.object(
                 flash.time, "sleep",
                 side_effect=lambda delay: setattr(
                     clock, "now", clock.now + delay
                 ),
             ):
            receipt = badge.stage_scanner_firmware(
                platform, _frozen_firmware_set(data), VERSION,
                ["ble", "wifi"],
            )

        self.assertEqual(receipt, final)
        self.assertEqual(len(reconnect_times), 1)
        self.assertLessEqual(
            reconnect_times[0],
            flash.UPDATE_KEEPALIVE_MAX_S + 0.03,
        )
        self.assertEqual(
            b"".join(
                payload for payload in second.writes
                if not payload.startswith(b"\n")
            ),
            data,
        )

    def test_invalid_utf8_target_is_rejected_without_replacement_or_chain(
        self,
    ) -> None:
        clock = SimpleNamespace(now=0.0)
        raw = (
            b'FOF_FW_UPLOAD:{"ok":false,'
            b'"error":"PRIVATE_SENTINEL_\xff"}\n'
        )
        badge = flash.BadgeSerial(_usb_record(), False)
        badge.ser = _TimedByteSerial(clock, [(0.0, raw)])
        stdout = io.StringIO()

        with contextlib.redirect_stdout(stdout), self.assertRaises(
            flash.SerialTransportError
        ) as caught:
            badge.read_prefixed_json(
                "FOF_FW_UPLOAD:",
                1,
                allowed_schema_ids=(
                    flash.HostJsonSchemaId.SCANNER_STAGE_FAILURE,
                ),
            )

        rendered = str(caught.exception)
        self.assertNotIn("PRIVATE_SENTINEL", rendered)
        self.assertNotIn("\ufffd", rendered)
        self.assertEqual(stdout.getvalue(), "")
        self.assertIsNone(caught.exception.__cause__)
        self.assertIsNone(caught.exception.__context__)

    def test_generic_status_json_remains_compatible_without_schema_id(
        self,
    ) -> None:
        clock = SimpleNamespace(now=0.0)
        badge = flash.BadgeSerial(_usb_record(), False)
        badge.ser = _TimedByteSerial(clock, [(
            0.0,
            b'FOF_STATUS:{"version":"test","ready":true}\r\n',
        )])

        self.assertEqual(
            badge.read_prefixed_json("FOF_STATUS:", 1),
            {"version": "test", "ready": True},
        )

    def test_coalesced_uplink_ready_and_credit_remain_separate(self) -> None:
        ready = _uplink_receipt("ready", total=5000, credit=4096)
        credit = _uplink_receipt(
            "credit", total=5000, received=4096, credit=904
        )
        clock = SimpleNamespace(now=0.0)
        serial = _TimedByteSerial(clock, [(
            0.0,
            _frame("FOF_UPLINK_OTA:", ready)
            + _frame("FOF_UPLINK_OTA:", credit, crlf=True),
        )])
        badge = flash.BadgeSerial(_usb_record(), False)
        badge.ser = serial
        schemas = (flash.HostJsonSchemaId.UPLINK_OTA,)

        self.assertEqual(
            badge.read_prefixed_json(
                "FOF_UPLINK_OTA:", 1, allowed_schema_ids=schemas
            ),
            ready,
        )
        self.assertEqual(
            badge.read_prefixed_json(
                "FOF_UPLINK_OTA:", 1, allowed_schema_ids=schemas
            ),
            credit,
        )
        self.assertEqual(serial.read_calls, 1)

    def test_malformed_progress_never_logs_or_extends_activity_deadline(
        self,
    ) -> None:
        size = 2048
        terminal = _frame("FOF_FW_RELAY:", _relay_terminal(size=size))
        missing_error = _relay_progress(size=size)
        missing_error.pop("error")
        wrong_slot = {
            **_relay_progress(size=size),
            "uart": "wifi",
            "error": "PRIVATE_PROGRESS_SENTINEL",
        }
        invalid_utf8 = _frame(
            "FOF_FW_RELAY_PROGRESS:",
            {
                **_relay_progress(size=size),
                "error": "PRIVATE_PROGRESS_SENTINEL",
            },
        ).replace(
            b"PRIVATE_PROGRESS_SENTINEL",
            b"PRIVATE_PROGRESS_SENTINEL_\xff",
        )
        cases = {
            "schema": _frame("FOF_FW_RELAY_PROGRESS:", missing_error),
            "bound state": _frame("FOF_FW_RELAY_PROGRESS:", wrong_slot),
            "utf8": invalid_utf8,
        }

        for name, malformed in cases.items():
            clock = SimpleNamespace(now=0.0)
            badge = flash.BadgeSerial(_usb_record(), False)
            badge.ser = _TimedByteSerial(
                clock,
                [(0.75, malformed), (0.30, terminal)],
            )
            badge.write_line = lambda *_args, **_kwargs: None  # type: ignore[method-assign]
            stdout = io.StringIO()

            with self.subTest(name=name), contextlib.redirect_stdout(
                stdout
            ), mock.patch.object(
                flash.time, "monotonic", side_effect=lambda: clock.now
            ), mock.patch.object(
                flash, "scanner_relay_timeout_s", return_value=1
            ), self.assertRaises(flash.SerialReadTimeout):
                badge.relay_scanner(
                    "ble",
                    False,
                    True,
                    size,
                    expected_generation=42,
                    expected_hardware_id=HARDWARE_ID,
                )

            self.assertNotIn("[relay]", stdout.getvalue())
            self.assertNotIn("PRIVATE_PROGRESS_SENTINEL", stdout.getvalue())

    def test_authorized_progress_logs_and_extends_activity_deadline(
        self,
    ) -> None:
        size = 2048
        clock = SimpleNamespace(now=0.0)
        badge = flash.BadgeSerial(_usb_record(), False)
        badge.ser = _TimedByteSerial(clock, [
            (0.75, _frame(
                "FOF_FW_RELAY_PROGRESS:", _relay_progress(size=size)
            )),
            (0.75, _frame(
                "FOF_FW_RELAY:", _relay_terminal(size=size)
            )),
        ])
        badge.write_line = lambda *_args, **_kwargs: None  # type: ignore[method-assign]
        stdout = io.StringIO()

        with contextlib.redirect_stdout(stdout), mock.patch.object(
            flash.time, "monotonic", side_effect=lambda: clock.now
        ), mock.patch.object(
            flash, "scanner_relay_timeout_s", return_value=1
        ):
            result = badge.relay_scanner(
                "ble",
                False,
                True,
                size,
                expected_generation=42,
                expected_hardware_id=HARDWARE_ID,
            )

        self.assertEqual(result, _relay_terminal(size=size))
        self.assertEqual(stdout.getvalue().count("[relay] ble chunks 50%"), 1)

    def test_duplicate_terminal_cannot_authorize_relay_success(self) -> None:
        size = 2048
        terminal = json.dumps(
            _relay_terminal(size=size), separators=(",", ":")
        )
        duplicate = (
            "FOF_FW_RELAY:"
            + terminal.replace('"ok":true', '"ok":false,"ok":true', 1)
            + "\n"
        ).encode("ascii")
        clock = SimpleNamespace(now=0.0)
        badge = flash.BadgeSerial(_usb_record(), False)
        badge.ser = _TimedByteSerial(clock, [(0.0, duplicate)])
        badge.write_line = lambda *_args, **_kwargs: None  # type: ignore[method-assign]

        with self.assertRaises(flash.SerialTransportError):
            badge.relay_scanner(
                "ble",
                False,
                True,
                size,
                expected_generation=42,
                expected_hardware_id=HARDWARE_ID,
            )

    def test_relay_generation_above_uint32_is_rejected_before_command(
        self,
    ) -> None:
        badge = flash.BadgeSerial(_usb_record(), False)
        writes: list[str] = []
        badge.write_line = (  # type: ignore[method-assign]
            lambda line, **_kwargs: writes.append(line)
        )
        badge.read_prefixed_json = (  # type: ignore[method-assign]
            lambda *_args, **_kwargs: _relay_terminal()
        )

        with self.assertRaises(flash.FlashError):
            badge.relay_scanner(
                "ble",
                False,
                True,
                2048,
                expected_generation=UINT32_MAX + 1,
                expected_hardware_id=HARDWARE_ID,
            )

        self.assertEqual(writes, [])

    def test_scanner_stage_rejects_uncredited_ready_before_binary(self) -> None:
        platform = dict(flash.PLATFORMS["badge-trio-xiao-s3"])
        with tempfile.TemporaryDirectory() as temp_dir:
            data = _firmware_image(
                platform["scanner_project"],
                VERSION,
                platform["scanner_name"],
                platform["hardware_type"],
            ) + b"X" * 5000
            image = Path(temp_dir) / "scanner.bin"
            image.write_bytes(data)
            platform["scanner_bin"] = image
            begin = _stage_receipt(platform, VERSION, data, 1)
            final = _stage_receipt(
                platform, VERSION, data, 1, generation=7
            )
            clock = SimpleNamespace(now=0.0)
            serial = _TimedByteSerial(clock, [(
                0.0,
                _frame("FOF_FW_UPLOAD:", begin)
                + _frame("FOF_FW_UPLOAD:", final),
            )])
            badge = flash.BadgeSerial(_usb_record(), False)
            badge.ser = serial
            badge.write_line = lambda *_args, **_kwargs: None  # type: ignore[method-assign]

            with contextlib.redirect_stdout(io.StringIO()), self.assertRaises(
                flash.FlashError
            ):
                badge.stage_scanner_firmware(
                    platform, _frozen_firmware_set(data), VERSION, ["ble"]
                )

            self.assertEqual(serial.writes, [])

    def test_scanner_stage_rejects_final_schema_as_ready_before_binary(
        self,
    ) -> None:
        platform = dict(flash.PLATFORMS["badge-trio-xiao-s3"])
        with tempfile.TemporaryDirectory() as temp_dir:
            data = _firmware_image(
                platform["scanner_project"],
                VERSION,
                platform["scanner_name"],
                platform["hardware_type"],
            ) + b"X" * 5000
            image = Path(temp_dir) / "scanner.bin"
            image.write_bytes(data)
            platform["scanner_bin"] = image
            wrong_ready = _stage_receipt(
                platform,
                VERSION,
                data,
                1,
                phase="ready",
                received=0,
                credit=min(flash.SCANNER_STAGE_CREDIT_BYTES, len(data)),
                generation=9,
            )
            failure = {"ok": False, "error": "stop after wrong ready"}
            clock = SimpleNamespace(now=0.0)
            serial = _TimedByteSerial(clock, [(
                0.0,
                _frame("FOF_FW_UPLOAD:", wrong_ready)
                + _frame("FOF_FW_UPLOAD:", failure),
            )])
            badge = flash.BadgeSerial(_usb_record(), False)
            badge.ser = serial
            badge.write_line = lambda *_args, **_kwargs: None  # type: ignore[method-assign]

            with contextlib.redirect_stdout(io.StringIO()), self.assertRaises(
                flash.FlashError
            ):
                badge.stage_scanner_firmware(
                    platform, _frozen_firmware_set(data), VERSION, ["ble"]
                )

            self.assertEqual(serial.writes, [])

    def test_scanner_stage_coalesced_credit_v1_windows_and_final(self) -> None:
        platform = dict(flash.PLATFORMS["badge-trio-xiao-s3"])
        with tempfile.TemporaryDirectory() as temp_dir:
            data = _firmware_image(
                platform["scanner_project"],
                VERSION,
                platform["scanner_name"],
                platform["hardware_type"],
            ) + b"X" * 5000
            image = Path(temp_dir) / "scanner.bin"
            image.write_bytes(data)
            platform["scanner_bin"] = image
            first_credit = min(flash.SCANNER_STAGE_CREDIT_BYTES, len(data))
            ready = _stage_receipt(
                platform,
                VERSION,
                data,
                3,
                phase="ready",
                received=0,
                credit=first_credit,
            )
            next_credit = len(data) - first_credit
            credit = _stage_receipt(
                platform,
                VERSION,
                data,
                3,
                phase="credit",
                received=first_credit,
                credit=next_credit,
            )
            final = _stage_receipt(
                platform,
                VERSION,
                data,
                3,
                phase="final",
                received=len(data),
                credit=0,
                generation=11,
            )
            clock = SimpleNamespace(now=0.0)
            serial = _TimedByteSerial(clock, [(
                0.0,
                _frame("FOF_FW_UPLOAD:", ready)
                + _frame("FOF_FW_UPLOAD:", credit)
                + _frame("FOF_FW_UPLOAD:", final, crlf=True),
            )])
            badge = flash.BadgeSerial(_usb_record(), False)
            badge.ser = serial
            badge.write_line = lambda *_args, **_kwargs: None  # type: ignore[method-assign]

            with contextlib.redirect_stdout(io.StringIO()):
                result = badge.stage_scanner_firmware(
                    platform, _frozen_firmware_set(data), VERSION,
                    ["ble", "wifi"],
                )

            self.assertEqual(result, final)
            self.assertEqual(b"".join(serial.writes), data)
            self.assertEqual(
                sum(map(len, serial.writes[:4])),
                first_credit,
            )
            self.assertEqual(serial.read_calls, 1)

    def test_uplink_duplicate_ready_causes_zero_binary_writes(self) -> None:
        platform = dict(flash.PLATFORMS["badge-trio-xiao-s3"])
        with tempfile.TemporaryDirectory() as temp_dir:
            data = _firmware_image(
                platform["uplink_project"],
                VERSION,
                platform["uplink_name"],
                platform["hardware_type"],
            )
            image = Path(temp_dir) / "uplink.bin"
            image.write_bytes(data)
            platform["uplink_bin"] = image
            ready = json.dumps(
                _uplink_receipt(
                    "ready", total=len(data), credit=len(data)
                ),
                separators=(",", ":"),
            )
            duplicate_ready = (
                "FOF_UPLINK_OTA:"
                + ready.replace(
                    '"partition":"ota_1"',
                    '"partition":"ota_0","partition":"ota_1"',
                    1,
                )
                + "\n"
            ).encode("ascii")
            abort = _frame(
                "FOF_UPLINK_OTA:",
                _uplink_receipt(
                    "aborted",
                    total=len(data),
                    received=len(data),
                    ok=False,
                    error="duplicate should have stopped before this",
                ),
            )
            clock = SimpleNamespace(now=0.0)
            serial = _TimedByteSerial(
                clock, [(0.0, duplicate_ready + abort)]
            )
            badge = flash.BadgeSerial(_usb_record(), False)
            badge.ser = serial
            badge._prove_open_application = (  # type: ignore[method-assign]
                lambda _timeout: _uplink_status(OLD_VERSION)
            )
            badge.write_line = lambda *_args, **_kwargs: None  # type: ignore[method-assign]

            with self.assertRaises(flash.FlashError):
                badge.upload_uplink_firmware(
                    platform, _frozen_firmware_set(data), VERSION
                )

            self.assertEqual(serial.writes, [])

    def test_scanner_stage_errors_are_payload_free_and_context_free(
        self,
    ) -> None:
        platform = dict(flash.PLATFORMS["badge-trio-xiao-s3"])
        with tempfile.TemporaryDirectory() as temp_dir:
            data = _firmware_image(
                platform["scanner_project"],
                VERSION,
                platform["scanner_name"],
                platform["hardware_type"],
            ) + b"X" * 5000
            image = Path(temp_dir) / "scanner.bin"
            image.write_bytes(data)
            platform["scanner_bin"] = image
            first_credit = min(flash.SCANNER_STAGE_CREDIT_BYTES, len(data))
            valid_ready = _stage_receipt(
                platform,
                VERSION,
                data,
                1,
                phase="ready",
                received=0,
                credit=first_credit,
            )
            cases = (
                (
                    "failure",
                    {
                        "ok": False,
                        "error": "PRIVATE_SCANNER_FAILURE_SENTINEL",
                    },
                    "USB scanner firmware stage ready failed",
                    "PRIVATE_SCANNER_FAILURE_SENTINEL",
                ),
                (
                    "semantic",
                    {
                        **valid_ready,
                        "project": "PRIVATE_SCANNER_PROJECT_SENTINEL",
                    },
                    "USB scanner firmware stage ready project mismatch",
                    "PRIVATE_SCANNER_PROJECT_SENTINEL",
                ),
            )

            for name, receipt, expected_message, sentinel in cases:
                with self.subTest(name=name):
                    clock = SimpleNamespace(now=0.0)
                    serial = _TimedByteSerial(clock, [(
                        0.0,
                        _frame("FOF_FW_UPLOAD:", receipt),
                    )])
                    badge = flash.BadgeSerial(_usb_record(), False)
                    badge.ser = serial
                    badge.write_line = (  # type: ignore[method-assign]
                        lambda *_args, **_kwargs: None
                    )

                    self.assert_payload_free_failure(
                        lambda: badge.stage_scanner_firmware(
                            platform, _frozen_firmware_set(data), VERSION,
                            ["ble"],
                        ),
                        expected_message=expected_message,
                        forbidden=(
                            sentinel,
                            HARDWARE_ID,
                            repr(receipt),
                        ),
                    )
                    self.assertEqual(serial.writes, [])

    def test_uplink_errors_are_payload_free_and_context_free(self) -> None:
        platform = dict(flash.PLATFORMS["badge-trio-xiao-s3"])
        with tempfile.TemporaryDirectory() as temp_dir:
            data = _firmware_image(
                platform["uplink_project"],
                VERSION,
                platform["uplink_name"],
                platform["hardware_type"],
            )
            image = Path(temp_dir) / "uplink.bin"
            image.write_bytes(data)
            platform["uplink_bin"] = image
            ready = _uplink_receipt(
                "ready",
                total=len(data),
                credit=len(data),
            )
            wrong_partition = {
                **ready,
                "partition": "PRIVATE_UPLINK_PARTITION_SENTINEL",
            }
            aborted = _uplink_receipt(
                "aborted",
                total=len(data),
                received=len(data),
                ok=False,
                error="PRIVATE_UPLINK_ABORT_SENTINEL",
            )
            cases = (
                (
                    "semantic",
                    _frame("FOF_UPLINK_OTA:", wrong_partition),
                    "uplink OTA ready partition mismatch",
                    "PRIVATE_UPLINK_PARTITION_SENTINEL",
                    0,
                    wrong_partition,
                ),
                (
                    "abort",
                    _frame("FOF_UPLINK_OTA:", ready)
                    + _frame("FOF_UPLINK_OTA:", aborted),
                    "uplink OTA aborted definitively",
                    "PRIVATE_UPLINK_ABORT_SENTINEL",
                    len(data),
                    aborted,
                ),
            )

            for name, frames, expected_message, sentinel, sent, receipt in cases:
                with self.subTest(name=name):
                    clock = SimpleNamespace(now=0.0)
                    serial = _TimedByteSerial(clock, [(0.0, frames)])
                    badge = flash.BadgeSerial(_usb_record(), False)
                    badge.ser = serial
                    badge._prove_open_application = (  # type: ignore[method-assign]
                        lambda _timeout: _uplink_status(OLD_VERSION)
                    )
                    badge.write_line = (  # type: ignore[method-assign]
                        lambda *_args, **_kwargs: None
                    )

                    self.assert_payload_free_failure(
                        lambda: badge.upload_uplink_firmware(
                            platform, _frozen_firmware_set(data), VERSION
                        ),
                        expected_message=expected_message,
                        forbidden=(
                            sentinel,
                            HARDWARE_ID,
                            repr(receipt),
                        ),
                    )
                    self.assertEqual(
                        sum(map(len, serial.writes)),
                        sent,
                    )

    def test_relay_semantic_error_is_payload_free_and_context_free(
        self,
    ) -> None:
        size = 2048
        terminal = {
            **_relay_terminal(size=size),
            "error": "PRIVATE_RELAY_ERROR_SENTINEL",
        }
        clock = SimpleNamespace(now=0.0)
        serial = _TimedByteSerial(clock, [(
            0.0,
            _frame("FOF_FW_RELAY:", terminal),
        )])
        badge = flash.BadgeSerial(_usb_record(), False)
        badge.ser = serial
        badge.write_line = (  # type: ignore[method-assign]
            lambda *_args, **_kwargs: None
        )

        self.assert_payload_free_failure(
            lambda: badge.relay_scanner(
                "ble",
                False,
                True,
                size,
                expected_generation=42,
                expected_hardware_id=HARDWARE_ID,
            ),
            expected_message="USB scanner relay terminal error mismatch",
            forbidden=(
                "PRIVATE_RELAY_ERROR_SENTINEL",
                HARDWARE_ID,
                repr(terminal),
            ),
        )
        self.assertEqual(serial.writes, [])

    def test_post_upload_receipt_identity_errors_are_payload_free(
        self,
    ) -> None:
        pre_status = _uplink_status(OLD_VERSION)
        cases = (
            (
                "current",
                {
                    "ok": True,
                    "skipped": True,
                    "phase": "current",
                    "hardware_id": "PRIVATE_CURRENT_ID_SENTINEL",
                    "version": VERSION,
                    "partition": "ota_0",
                },
                "uplink current receipt hardware_id is invalid",
                "PRIVATE_CURRENT_ID_SENTINEL",
            ),
            (
                "terminal",
                {
                    "ok": False,
                    "uncertain": True,
                    "phase": "terminal_unavailable",
                    "expected_partition": "ota_1",
                    "hardware_id": "PRIVATE_TERMINAL_ID_SENTINEL",
                    "version": VERSION,
                    "received": 1,
                    "total": 1,
                    "error": "transport unavailable",
                },
                "uplink terminal receipt hardware_id is invalid",
                "PRIVATE_TERMINAL_ID_SENTINEL",
            ),
        )

        for name, receipt, expected_message, sentinel in cases:
            with self.subTest(name=name):
                self.assert_payload_free_failure(
                    lambda receipt=receipt: (
                        flash._classify_uplink_update_receipt(
                            receipt,
                            pre_status=pre_status,
                            target_version=VERSION,
                            expected_sha256="a" * 64,
                            expected_size=1,
                            update_session=UPDATE_SESSION,
                        )
                    ),
                    expected_message=expected_message,
                    forbidden=(sentinel, HARDWARE_ID, repr(receipt)),
                )


if __name__ == "__main__":
    unittest.main()
