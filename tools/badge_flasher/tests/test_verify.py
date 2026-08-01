from __future__ import annotations

import json
import sys
import unittest
from types import SimpleNamespace
from unittest import mock

from scripts import usb_descriptor_binding
from tools.badge_flasher import verify
from tools.badge_flasher.models import (
    SeedRebootProof,
    TopologyAssignment,
    UsbDevice,
)
from tools.badge_flasher.verify import (
    VerificationError,
    runtime_evidence,
    verify_status,
)


VERSION = "0.64.76-badge-defcon34"
TARGET = "uplink-s3-fof_badge"
ASSIGNMENT = TopologyAssignment(
    "E0:72:A1:F9:47:FC",
    "E0:72:A1:F9:49:84",
    "E0:72:A1:F8:4C:58",
)
UPLINK = UsbDevice(
    ASSIGNMENT.uplink_mac,
    "/dev/cu.uplink",
    "ESP32-S3",
    "v0.2",
    "8MB",
    "8MB",
)


def valid_status(*, generation: int = 8):
    return {"version":"0.64.76-badge-defcon34","target":"uplink-s3-fof_badge","firmware_name":"uplink-s3-fof_badge","app_project":"fof_badge_uplink","hardware_type":"seeed_xiao_esp32s3","hardware_id":"E0:72:A1:F9:47:FC","safe_mode":False,"recovery_mode":"normal","last_expected_reboot_reason":"usb_reboot","last_expected_reboot_generation":generation,"usb_health":{"responses_completed":1},"usb_control_alive":True,"scanner_uart_alive":True,"display_alive":True,"power_converged":True,"scanner_power_converged":True,"psram_total":8388608,"game_seed":"normal","game_state":"normal","game_active":False,"game_shield":0,"scanners":[
        {"uart":"ble","connected":True,"firmware_name":"scanner-s3-combo-fof_badge","app_project":"fof_badge_scanner","hardware_type":"seeed_xiao_esp32s3","ver":"0.64.76-badge-defcon34","hardware_id":"E0:72:A1:F9:49:84","role_acked":True,"scan_profile":"ble_primary","ble_scanning":True,"recovery_mode":"normal","rollback_pending":False,"health":"ok"},
        {"uart":"wifi","connected":True,"firmware_name":"scanner-s3-combo-fof_badge","app_project":"fof_badge_scanner","hardware_type":"seeed_xiao_esp32s3","ver":"0.64.76-badge-defcon34","hardware_id":"E0:72:A1:F8:4C:58","role_acked":True,"scan_profile":"wifi_primary","wifi_active":True,"recovery_mode":"normal","rollback_pending":False,"health":"ok"},
    ]}


def status_reply(status: dict, *, stale_first: dict | None = None) -> bytes:
    result = bytearray()
    if stale_first is not None:
        result.extend(b"FOF_STATUS:")
        result.extend(
            json.dumps(stale_first, separators=(",", ":")).encode("utf-8")
        )
        result.extend(b"\r\n")
    result.extend(f"FOF_PONG:{VERSION}\r\n".encode("ascii"))
    result.extend(b"FOF_STATUS:")
    result.extend(json.dumps(status, separators=(",", ":")).encode("utf-8"))
    result.extend(b"\r\n")
    return bytes(result)


class ScriptedSerial:
    def __init__(
        self,
        replies: dict[bytes, bytes | list[bytes]],
    ) -> None:
        self.replies = replies
        self.writes: list[bytes] = []
        self.events: list[str] = []
        self.buffer = bytearray()
        self.closed = False
        self.dtr = True
        self.rts = True

    def write(self, data: bytes) -> int:
        self.writes.append(data)
        self.events.append(f"write:{data.decode('ascii').strip()}")
        reply = self.replies.get(data, b"")
        if isinstance(reply, list):
            reply = reply.pop(0) if reply else b""
        self.buffer.extend(reply)
        return len(data)

    def read(self, size: int) -> bytes:
        chunk = bytes(self.buffer[:size])
        del self.buffer[:size]
        return chunk

    def reset_input_buffer(self) -> None:
        self.events.append("reset")
        self.buffer.clear()

    def close(self) -> None:
        self.events.append("close")
        self.closed = True


class PortSnapshots:
    def __init__(self, *snapshots: list[str]) -> None:
        self.snapshots = snapshots
        self.index = 0

    def __call__(self) -> list[str]:
        index = min(self.index, len(self.snapshots) - 1)
        self.index += 1
        return list(self.snapshots[index])


def reboot_proof(
    *,
    generation: int = 7,
    old_port: str = "/dev/cu.uplink",
) -> SeedRebootProof:
    return SeedRebootProof(
        hardware_id=ASSIGNMENT.uplink_mac,
        pre_reboot_generation=generation,
        pre_reboot_responses_completed=1,
        old_port=old_port,
    )


class VerifyTests(unittest.TestCase):
    def test_preseed_runtime_ignores_old_role_but_requires_graph_health(
        self,
    ) -> None:
        for prior_seed in ("normal", "infected", "immune", "unexpected"):
            status = valid_status(generation=0)
            status["game_seed"] = prior_seed
            status["game_state"] = prior_seed
            status["last_expected_reboot_reason"] = "power_on"
            with self.subTest(prior_seed=prior_seed):
                checked = verify.verify_preseed_runtime(
                    status,
                    ASSIGNMENT,
                    VERSION,
                    expected_target=verify.UPLINK_TARGET,
                )
                self.assertEqual(
                    checked["hardware_id"],
                    ASSIGNMENT.uplink_mac,
                )

        mutations = (
            ("hardware_id", ASSIGNMENT.ble_leaf_mac),
            ("version", "0.67.1-badge-defcon34"),
            ("target", "wrong-target"),
            ("usb_control_alive", False),
            ("scanner_uart_alive", False),
            ("psram_total", 1024),
            ("safe_mode", True),
            ("recovery_mode", "recovery"),
        )
        for field, value in mutations:
            status = valid_status()
            status[field] = value
            with (
                self.subTest(field=field),
                self.assertRaises(VerificationError),
            ):
                verify.verify_preseed_runtime(
                    status,
                    ASSIGNMENT,
                    VERSION,
                    expected_target=verify.UPLINK_TARGET,
                )

        for scanner_index, field, value in (
            (0, "hardware_id", ASSIGNMENT.wifi_leaf_mac),
            (0, "scan_profile", "wifi_primary"),
            (0, "ble_scanning", False),
            (1, "wifi_active", False),
            (1, "rollback_pending", True),
        ):
            status = valid_status()
            status["scanners"][scanner_index][field] = value
            with (
                self.subTest(scanner=scanner_index, field=field),
                self.assertRaises(VerificationError),
            ):
                verify.verify_preseed_runtime(
                    status,
                    ASSIGNMENT,
                    VERSION,
                    expected_target=verify.UPLINK_TARGET,
                )

    def test_seed_transaction_sends_exact_order_for_every_role(self):
        for role in ("normal", "infected", "immune"):
            before = valid_status(generation=7)
            before["game_seed"] = role
            before["game_state"] = role
            handle = ScriptedSerial({
                b"FOF_PING\nFOF_STATUS\n": [
                    status_reply(valid_status(generation=7)),
                    status_reply(before),
                ],
                f"FOF_SET:game_seed={role}\n".encode("ascii"):
                    b"FOF_OK:game_seed\r\n",
                b"FOF_REBOOT\n": b"FOF_REBOOT:OK\r\n",
            })
            with (
                self.subTest(role=role),
                mock.patch.object(verify.time, "sleep", return_value=None),
            ):
                proof = verify.provision_game_seed(
                    UPLINK,
                    role,
                    VERSION,
                    serial_factory=lambda _port: handle,
                    candidate_ports=lambda: [UPLINK.port],
                )
            self.assertEqual(proof, reboot_proof())
            self.assertEqual(
                handle.writes,
                [
                    b"FOF_PING\nFOF_STATUS\n",
                    f"FOF_SET:game_seed={role}\n".encode("ascii"),
                    b"FOF_PING\nFOF_STATUS\n",
                    b"FOF_REBOOT\n",
                ],
            )
            self.assertEqual(handle.events[-1], "close")

    def test_seed_transaction_fails_on_error_or_wrong_ack_key(self):
        for reply, message in (
            (b"FOF_ERROR:game_seed update failed\n", "FOF_ERROR"),
            (b"FOF_OK:game_state\n", "acknowledgment"),
            (b" FOF_OK:game_seed \r\n", "timed out"),
        ):
            handle = ScriptedSerial({
                b"FOF_PING\nFOF_STATUS\n":
                    status_reply(valid_status(generation=7)),
                b"FOF_SET:game_seed=infected\n": reply,
            })
            with (
                self.subTest(reply=reply),
                mock.patch.object(verify.time, "sleep", return_value=None),
                self.assertRaisesRegex(VerificationError, message),
            ):
                verify.provision_game_seed(
                    UPLINK,
                    "infected",
                    VERSION,
                    timeout_s=0.01,
                    serial_factory=lambda _port: handle,
                    candidate_ports=lambda: [UPLINK.port],
                )
            self.assertNotIn(b"FOF_REBOOT\n", handle.writes)
            if not reply.startswith(b" "):
                self.assertEqual(
                    handle.writes.count(
                        b"FOF_SET:game_seed=infected\n"
                    ),
                    1,
                )
            self.assertTrue(handle.closed)

    def test_seed_retries_exact_booting_preseed_rejection(self):
        booting = ScriptedSerial({
            b"FOF_PING\nFOF_STATUS\n": b"FOF_ERROR:booting\r\n",
        })
        seeded_status = valid_status(generation=9)
        seeded_status["game_seed"] = "infected"
        seeded_status["game_state"] = "infected"
        recovered = ScriptedSerial({
            b"FOF_PING\nFOF_STATUS\n": [
                status_reply(valid_status(generation=9)),
                status_reply(seeded_status),
            ],
            b"FOF_SET:game_seed=infected\n": b"FOF_OK:game_seed\r\n",
            b"FOF_REBOOT\n": b"FOF_REBOOT:OK\r\n",
        })
        handles = [booting, recovered]

        with (
            mock.patch.object(verify.time, "sleep", return_value=None),
            mock.patch.object(
                verify,
                "_descriptor_bound_serial_factory",
                return_value=lambda _port: handles.pop(0),
            ),
        ):
            proof = verify.provision_game_seed(
                UPLINK,
                "infected",
                VERSION,
                timeout_s=0.1,
                candidate_ports=lambda: [UPLINK.port],
            )

        self.assertEqual(proof, reboot_proof(generation=9))
        self.assertEqual(booting.writes, [b"FOF_PING\nFOF_STATUS\n"])
        self.assertTrue(booting.closed)
        self.assertTrue(recovered.closed)

    def test_seed_preseed_nonbooting_firmware_rejection_fails_immediately(self):
        for firmware_error in (
            b"FOF_ERROR:unknown command\r\n",
            b"FOF_ERROR:booting \r\n",
            b"FOF_ERROR:Booting\r\n",
        ):
            handle = ScriptedSerial({
                b"FOF_PING\nFOF_STATUS\n": firmware_error,
            })
            open_candidate = mock.Mock(return_value=handle)
            with (
                self.subTest(firmware_error=firmware_error),
                mock.patch.object(verify.time, "sleep", return_value=None),
                mock.patch.object(
                    verify,
                    "_descriptor_bound_serial_factory",
                    return_value=open_candidate,
                ),
                self.assertRaisesRegex(
                    VerificationError,
                    "uplink pre-seed identity rejected by firmware",
                ),
            ):
                verify.provision_game_seed(
                    UPLINK,
                    "infected",
                    VERSION,
                    timeout_s=0.1,
                    candidate_ports=lambda: [UPLINK.port],
                )
            open_candidate.assert_called_once_with(UPLINK.port)
            self.assertNotIn(b"FOF_SET:game_seed=infected\n", handle.writes)
            self.assertTrue(handle.closed)

    def test_seed_discovers_exact_uplink_identity_before_mutating(self):
        wrong_status = valid_status(generation=7)
        wrong_status["hardware_id"] = ASSIGNMENT.ble_leaf_mac
        wrong = ScriptedSerial({
            b"FOF_PING\nFOF_STATUS\n": status_reply(wrong_status),
        })
        seeded_status = valid_status(generation=7)
        seeded_status["game_seed"] = "immune"
        seeded_status["game_state"] = "immune"
        correct = ScriptedSerial({
            b"FOF_PING\nFOF_STATUS\n": [
                status_reply(valid_status(generation=7)),
                status_reply(seeded_status),
            ],
            b"FOF_SET:game_seed=immune\n": b"FOF_OK:game_seed\r\n",
            b"FOF_REBOOT\n": b"FOF_REBOOT:OK\r\n",
        })
        handles = {
            "/dev/cu.scanner": wrong,
            "/dev/cu.uplink": correct,
        }
        with mock.patch.object(verify.time, "sleep", return_value=None):
            proof = verify.provision_game_seed(
                UPLINK,
                "immune",
                VERSION,
                timeout_s=0.1,
                serial_factory=lambda port: handles[port],
                candidate_ports=lambda: list(handles),
            )
        self.assertEqual(proof, reboot_proof())
        self.assertEqual(wrong.writes, [b"FOF_PING\nFOF_STATUS\n"])
        self.assertNotIn(b"FOF_SET:game_seed=immune\n", wrong.writes)
        self.assertTrue(wrong.closed)
        self.assertTrue(correct.closed)

    def test_seed_retries_reenumeration_after_bound_transport_disconnect(self):
        class DisconnectOnPhase(ScriptedSerial):
            def __init__(
                self,
                replies,
                *,
                command: bytes,
                occurrence: int = 1,
            ):
                super().__init__(replies)
                self.command = command
                self.occurrence = occurrence
                self.command_count = 0

            def write(self, data: bytes) -> int:
                if data == self.command:
                    self.command_count += 1
                if (
                    data == self.command
                    and self.command_count == self.occurrence
                ):
                    self.writes.append(data)
                    raise OSError("uplink re-enumerated")
                return super().write(data)

        for phase, command, occurrence in (
            ("seed acknowledgment", b"FOF_SET:game_seed=infected\n", 1),
            ("post-ack status", b"FOF_PING\nFOF_STATUS\n", 2),
            ("reboot receipt", b"FOF_REBOOT\n", 1),
        ):
            first_seeded = valid_status(generation=7)
            first_seeded["game_seed"] = "infected"
            first_seeded["game_state"] = "infected"
            disconnected = DisconnectOnPhase(
                {
                    b"FOF_PING\nFOF_STATUS\n": [
                        status_reply(valid_status(generation=7)),
                        status_reply(first_seeded),
                    ],
                    b"FOF_SET:game_seed=infected\n":
                        b"FOF_OK:game_seed\r\n",
                    b"FOF_REBOOT\n": b"FOF_REBOOT:OK\r\n",
                },
                command=command,
                occurrence=occurrence,
            )
            seeded_status = valid_status(generation=9)
            seeded_status["game_seed"] = "infected"
            seeded_status["game_state"] = "infected"
            recovered = ScriptedSerial({
                b"FOF_PING\nFOF_STATUS\n": [
                    status_reply(valid_status(generation=9)),
                    status_reply(seeded_status),
                ],
                b"FOF_SET:game_seed=infected\n":
                    b"FOF_OK:game_seed\r\n",
                b"FOF_REBOOT\n": b"FOF_REBOOT:OK\r\n",
            })
            handles = {
                "/dev/cu.before": disconnected,
                "/dev/cu.after": recovered,
            }
            ports = PortSnapshots(
                ["/dev/cu.before"],
                ["/dev/cu.after"],
            )
            with (
                self.subTest(phase=phase),
                mock.patch.object(verify.time, "sleep", return_value=None),
            ):
                proof = verify.provision_game_seed(
                    UPLINK,
                    "infected",
                    VERSION,
                    timeout_s=0.1,
                    serial_factory=lambda port: handles[port],
                    candidate_ports=ports,
                )
                self.assertEqual(
                    proof,
                    reboot_proof(
                        generation=9,
                        old_port="/dev/cu.after",
                    ),
                )
                self.assertIn(
                    b"FOF_SET:game_seed=infected\n",
                    disconnected.writes,
                )
                self.assertIn(
                    b"FOF_SET:game_seed=infected\n",
                    recovered.writes,
                )
                self.assertTrue(disconnected.closed)
                self.assertTrue(recovered.closed)

    def test_seed_retries_silent_transport_timeout_within_global_bound(self):
        silent = ScriptedSerial({
            b"FOF_PING\nFOF_STATUS\n":
                status_reply(valid_status(generation=7)),
            b"FOF_SET:game_seed=infected\n": b"",
        })
        seeded_status = valid_status(generation=9)
        seeded_status["game_seed"] = "infected"
        seeded_status["game_state"] = "infected"
        recovered = ScriptedSerial({
            b"FOF_PING\nFOF_STATUS\n": [
                status_reply(valid_status(generation=9)),
                status_reply(seeded_status),
            ],
            b"FOF_SET:game_seed=infected\n": b"FOF_OK:game_seed\r\n",
            b"FOF_REBOOT\n": b"FOF_REBOOT:OK\r\n",
        })
        handles = {
            "/dev/cu.silent": silent,
            "/dev/cu.recovered": recovered,
        }
        with (
            mock.patch.object(verify.time, "sleep", return_value=None),
            mock.patch.object(
                verify,
                "APPLICATION_ATTEMPT_TIMEOUT_S",
                0.005,
            ),
        ):
            proof = verify.provision_game_seed(
                UPLINK,
                "infected",
                VERSION,
                timeout_s=0.1,
                serial_factory=lambda port: handles[port],
                candidate_ports=PortSnapshots(
                    ["/dev/cu.silent"],
                    ["/dev/cu.recovered"],
                ),
            )
        self.assertEqual(
            proof,
            reboot_proof(
                generation=9,
                old_port="/dev/cu.recovered",
            ),
        )
        self.assertTrue(silent.closed)
        self.assertTrue(recovered.closed)

    def test_seed_timeout_preserves_matching_uplink_error(self):
        matching = ScriptedSerial({
            b"FOF_PING\nFOF_STATUS\n": b"",
        })

        def open_candidate(port: str):
            if port == UPLINK.port:
                return matching
            raise verify._RetryableApplicationPort(
                "candidate is not the descriptor-bound uplink"
            )

        candidates = [
            UPLINK.port,
            "/dev/cu.scanner-a",
            "/dev/cu.scanner-b",
        ]
        with (
            mock.patch.object(verify.time, "sleep", return_value=None),
            mock.patch.object(
                verify,
                "APPLICATION_ATTEMPT_TIMEOUT_S",
                0.005,
            ),
            mock.patch.object(
                verify,
                "_candidate_snapshot",
                return_value=candidates,
            ),
            self.assertRaisesRegex(
                VerificationError,
                "uplink pre-seed identity timed out waiting for fresh status",
            ),
        ):
            verify.provision_game_seed(
                UPLINK,
                "immune",
                VERSION,
                timeout_s=0.1,
                serial_factory=open_candidate,
                candidate_ports=lambda: candidates,
            )

    def test_seed_descriptor_bound_identity_mismatch_fails_immediately(self):
        mismatched_status = valid_status(generation=7)
        mismatched_status["hardware_id"] = ASSIGNMENT.ble_leaf_mac
        matching = ScriptedSerial({
            b"FOF_PING\nFOF_STATUS\n": status_reply(mismatched_status),
        })
        open_candidate = mock.Mock(return_value=matching)

        with (
            mock.patch.object(verify.time, "sleep", return_value=None),
            mock.patch.object(
                verify,
                "_descriptor_bound_serial_factory",
                return_value=open_candidate,
            ) as descriptor_factory,
            self.assertRaises(VerificationError) as raised,
        ):
            verify.provision_game_seed(
                UPLINK,
                "immune",
                VERSION,
                timeout_s=0.01,
                candidate_ports=lambda: [UPLINK.port],
            )

        self.assertEqual(
            str(raised.exception),
            "uplink pre-seed identity hardware ID mismatch",
        )
        descriptor_factory.assert_called_once_with(UPLINK.mac)
        open_candidate.assert_called_once_with(UPLINK.port)
        self.assertEqual(matching.writes, [b"FOF_PING\nFOF_STATUS\n"])
        self.assertTrue(matching.closed)

    def test_native_open_disables_control_lines_before_open(self):
        events: list[str] = []

        class NativeHandle:
            def __init__(self, *, port, baudrate, timeout, write_timeout):
                self._port = port
                events.append(
                    f"construct:{port}:{baudrate}:{timeout}:{write_timeout}"
                )

            @property
            def dtr(self):
                return False

            @dtr.setter
            def dtr(self, value):
                events.append(f"dtr:{value}")

            @property
            def rts(self):
                return False

            @rts.setter
            def rts(self, value):
                events.append(f"rts:{value}")

            @property
            def port(self):
                return self._port

            @port.setter
            def port(self, value):
                self._port = value
                events.append(f"port:{value}")

            def open(self):
                events.append("open")

        serial_module = SimpleNamespace(Serial=NativeHandle)
        with mock.patch.dict(sys.modules, {"serial": serial_module}):
            verify._default_serial_factory("/dev/cu.application")
        self.assertEqual(
            events,
            [
                "construct:None:115200:0.1:1",
                "dtr:False",
                "rts:False",
                "port:/dev/cu.application",
                "open",
            ],
        )

    def test_native_open_failure_closes_the_partial_handle(self):
        events: list[str] = []

        class FailingNativeHandle:
            def __init__(self, *, port, baudrate, timeout, write_timeout):
                self.port = port
                self.dtr = True
                self.rts = True

            def open(self):
                events.append("open")
                raise OSError("stale application port")

            def close(self):
                events.append("close")

        serial_module = SimpleNamespace(Serial=FailingNativeHandle)
        with (
            mock.patch.dict(sys.modules, {"serial": serial_module}),
            self.assertRaisesRegex(OSError, "stale application port"),
        ):
            verify._default_serial_factory("/dev/cu.stale")
        self.assertEqual(events, ["open", "close"])

    def test_seed_default_open_uses_descriptor_bound_uplink_factory(self):
        before = valid_status(generation=7)
        handle = ScriptedSerial({
            b"FOF_PING\nFOF_STATUS\n": [
                status_reply(valid_status(generation=7)),
                status_reply(before),
            ],
            b"FOF_SET:game_seed=normal\n": b"FOF_OK:game_seed\r\n",
            b"FOF_REBOOT\n": b"FOF_REBOOT:OK\r\n",
        })
        bound_factory = mock.Mock(return_value=handle)

        with (
            mock.patch.object(verify.time, "sleep", return_value=None),
            mock.patch.object(
                verify,
                "_descriptor_bound_serial_factory",
                return_value=bound_factory,
            ) as build_factory,
            mock.patch.object(
                verify,
                "_default_serial_factory",
                side_effect=AssertionError("raw serial open is forbidden"),
            ),
        ):
            proof = verify.provision_game_seed(
                UPLINK,
                "normal",
                VERSION,
                candidate_ports=lambda: [UPLINK.port],
            )

        self.assertEqual(proof, reboot_proof())
        build_factory.assert_called_once_with(UPLINK.mac)
        bound_factory.assert_called_with(UPLINK.port)

    def test_descriptor_bound_factory_selects_exact_usb_serial(self):
        wanted = usb_descriptor_binding.UsbDescriptorRecord(
            device=UPLINK.port,
            vid=0x303A,
            pid=0x1001,
            serial_number="e0:72:a1:f9:47:fc",
            location="1-1",
            stat_device=1,
            stat_inode=2,
            stat_rdev=3,
        )
        wrong = usb_descriptor_binding.UsbDescriptorRecord(
            device="/dev/cu.other",
            vid=0x303A,
            pid=0x1001,
            serial_number="e0:72:a1:f9:49:84",
            location="1-2",
            stat_device=4,
            stat_inode=5,
            stat_rdev=6,
        )
        handle = object()

        with (
            mock.patch.object(
                usb_descriptor_binding,
                "take_usb_descriptor_census",
                return_value=(wrong, wanted),
            ),
            mock.patch.object(
                usb_descriptor_binding,
                "open_bound_application_serial",
                return_value=handle,
            ) as open_bound,
        ):
            factory = verify._descriptor_bound_serial_factory(UPLINK.mac)
            self.assertIs(factory(UPLINK.port), handle)

        open_bound.assert_called_once_with(
            wanted,
            expected_uplink_serial="e0:72:a1:f9:47:fc",
            baudrate=115200,
            timeout=0.1,
            write_timeout=1,
        )

    def test_runtime_default_open_uses_descriptor_bound_uplink_factory(self):
        handle = ScriptedSerial({
            b"FOF_PING\nFOF_STATUS\n":
                status_reply(valid_status(generation=8)),
        })
        bound_factory = mock.Mock(return_value=handle)

        with (
            mock.patch.object(verify.time, "sleep", return_value=None),
            mock.patch.object(
                verify,
                "_descriptor_bound_serial_factory",
                return_value=bound_factory,
            ) as build_factory,
            mock.patch.object(
                verify,
                "_default_serial_factory",
                side_effect=AssertionError("raw serial open is forbidden"),
            ),
        ):
            status = verify.wait_for_runtime(
                UPLINK,
                ASSIGNMENT,
                VERSION,
                "normal",
                reboot_proof=reboot_proof(),
                candidate_ports=lambda: ["/dev/cu.reenumerated"],
            )

        self.assertEqual(status["hardware_id"], ASSIGNMENT.uplink_mac)
        build_factory.assert_called_once_with(UPLINK.mac)
        bound_factory.assert_called_with("/dev/cu.reenumerated")

    def test_seed_requires_integer_pre_reboot_generation(self):
        for generation in (None, False, -1):
            before = valid_status(generation=7)
            before["game_seed"] = "infected"
            before["game_state"] = "infected"
            if generation is None:
                before.pop("last_expected_reboot_generation")
            else:
                before["last_expected_reboot_generation"] = generation
            handle = ScriptedSerial({
                b"FOF_PING\nFOF_STATUS\n": [
                    status_reply(valid_status(generation=7)),
                    status_reply(before),
                ],
                b"FOF_SET:game_seed=infected\n":
                    b"FOF_OK:game_seed\r\n",
            })
            with (
                self.subTest(generation=generation),
                mock.patch.object(verify.time, "sleep", return_value=None),
                self.assertRaisesRegex(
                    VerificationError,
                    "pre-reboot status reboot generation is invalid",
                ),
            ):
                verify.provision_game_seed(
                    UPLINK,
                    "infected",
                    VERSION,
                    timeout_s=0.1,
                    serial_factory=lambda _port: handle,
                    candidate_ports=lambda: [UPLINK.port],
                )
            self.assertNotIn(b"FOF_REBOOT\n", handle.writes)
            self.assertTrue(handle.closed)

    def test_seed_captures_exact_pre_reboot_response_counter(self):
        before = valid_status(generation=7)
        before["game_seed"] = "infected"
        before["game_state"] = "infected"
        before["usb_health"]["responses_completed"] = 23
        handle = ScriptedSerial({
            b"FOF_PING\nFOF_STATUS\n": [
                status_reply(valid_status(generation=7)),
                status_reply(before),
            ],
            b"FOF_SET:game_seed=infected\n": b"FOF_OK:game_seed\r\n",
            b"FOF_REBOOT\n": b"FOF_REBOOT:OK\r\n",
        })

        with mock.patch.object(verify.time, "sleep", return_value=None):
            proof = verify.provision_game_seed(
                UPLINK,
                "infected",
                VERSION,
                serial_factory=lambda _port: handle,
                candidate_ports=lambda: [UPLINK.port],
            )

        self.assertTrue(
            hasattr(proof, "pre_reboot_responses_completed"),
            "seed proof omitted its comparable pre-reboot response counter",
        )
        self.assertEqual(proof.pre_reboot_responses_completed, 23)

    def test_seed_rejects_malformed_pre_reboot_response_counter(self):
        for value in (None, False, -1, 0x1_0000_0000):
            before = valid_status(generation=7)
            before["game_seed"] = "infected"
            before["game_state"] = "infected"
            if value is None:
                del before["usb_health"]["responses_completed"]
            else:
                before["usb_health"]["responses_completed"] = value
            handle = ScriptedSerial({
                b"FOF_PING\nFOF_STATUS\n": [
                    status_reply(valid_status(generation=7)),
                    status_reply(before),
                ],
                b"FOF_SET:game_seed=infected\n":
                    b"FOF_OK:game_seed\r\n",
                b"FOF_REBOOT\n": b"FOF_REBOOT:OK\r\n",
            })

            with (
                self.subTest(value=value),
                mock.patch.object(verify.time, "sleep", return_value=None),
                self.assertRaisesRegex(
                    VerificationError,
                    "pre-reboot status response counter is invalid",
                ),
            ):
                verify.provision_game_seed(
                    UPLINK,
                    "infected",
                    VERSION,
                    timeout_s=0.1,
                    serial_factory=lambda _port: handle,
                    candidate_ports=lambda: [UPLINK.port],
                )
            self.assertNotIn(b"FOF_REBOOT\n", handle.writes)

    def test_runtime_closes_stale_buffered_status_then_retries_fresh_pong(
        self,
    ):
        stale = valid_status(generation=7)
        stale["fresh_marker"] = "pre-reboot"
        contaminated = valid_status(generation=8)
        contaminated["fresh_marker"] = "must-not-pass"
        fresh = valid_status(generation=8)
        fresh["fresh_marker"] = "post-reboot"
        stale_handle = ScriptedSerial({
            b"FOF_PING\nFOF_STATUS\n":
                status_reply(contaminated, stale_first=stale),
        })
        fresh_handle = ScriptedSerial({
            b"FOF_PING\nFOF_STATUS\n": status_reply(fresh),
        })
        handles = [stale_handle, fresh_handle]
        opens: list[str] = []

        def open_candidate(port: str):
            opens.append(port)
            return handles.pop(0)

        ports = lambda: ["/dev/cu.reenumerated"]
        with mock.patch.object(verify.time, "sleep", return_value=None):
            status = verify.wait_for_runtime(
                UPLINK,
                ASSIGNMENT,
                VERSION,
                "normal",
                reboot_proof=reboot_proof(),
                timeout_s=0.1,
                serial_factory=open_candidate,
                candidate_ports=ports,
            )
        self.assertEqual(status["fresh_marker"], "post-reboot")
        self.assertEqual(
            opens,
            ["/dev/cu.reenumerated", "/dev/cu.reenumerated"],
        )
        self.assertTrue(stale_handle.closed)
        self.assertTrue(fresh_handle.closed)

    def test_runtime_can_reopen_persistent_native_usb_path(self):
        opens: list[str] = []
        handle = ScriptedSerial({
            b"FOF_PING\nFOF_STATUS\n":
                status_reply(valid_status(generation=8)),
        })
        with mock.patch.object(verify.time, "sleep", return_value=None):
            verify.wait_for_runtime(
                UPLINK,
                ASSIGNMENT,
                VERSION,
                "normal",
                reboot_proof=reboot_proof(),
                timeout_s=0.1,
                serial_factory=lambda port: opens.append(port) or handle,
                candidate_ports=lambda: ["/dev/cu.uplink"],
            )
        self.assertEqual(opens, ["/dev/cu.uplink"])

    def test_runtime_retries_disconnect_and_wrong_identity_then_binds_mac(self):
        wrong_status = valid_status(generation=8)
        wrong_status["hardware_id"] = ASSIGNMENT.wifi_leaf_mac
        wrong = ScriptedSerial({
            b"FOF_PING\nFOF_STATUS\n": status_reply(wrong_status),
        })
        correct = ScriptedSerial({
            b"FOF_PING\nFOF_STATUS\n":
                status_reply(valid_status(generation=8)),
        })
        attempts: list[str] = []

        def open_candidate(port: str):
            attempts.append(port)
            if port == "/dev/cu.1-disconnected":
                raise OSError("transport disappeared")
            if port == "/dev/cu.2-wrong":
                return wrong
            return correct

        ports = lambda: [
            "/dev/cu.1-disconnected",
            "/dev/cu.2-wrong",
            "/dev/cu.3-correct",
        ]
        with mock.patch.object(verify.time, "sleep", return_value=None):
            status = verify.wait_for_runtime(
                UPLINK,
                ASSIGNMENT,
                VERSION,
                "normal",
                reboot_proof=reboot_proof(),
                timeout_s=0.1,
                serial_factory=open_candidate,
                candidate_ports=ports,
            )
        self.assertEqual(status["hardware_id"], ASSIGNMENT.uplink_mac)
        self.assertEqual(
            attempts,
            [
                "/dev/cu.1-disconnected",
                "/dev/cu.2-wrong",
                "/dev/cu.3-correct",
            ],
        )
        self.assertTrue(wrong.closed)
        self.assertTrue(correct.closed)

    def test_runtime_accepts_persistent_native_usb_path_with_successor(self):
        opens: list[str] = []
        handle = ScriptedSerial({
            b"FOF_PING\nFOF_STATUS\n":
                status_reply(valid_status(generation=8)),
        })
        with mock.patch.object(verify.time, "sleep", return_value=None):
            status = verify.wait_for_runtime(
                UPLINK,
                ASSIGNMENT,
                VERSION,
                "normal",
                reboot_proof=reboot_proof(),
                timeout_s=0.1,
                serial_factory=lambda port: opens.append(port) or handle,
                candidate_ports=lambda: [UPLINK.port],
            )
        self.assertEqual(status["last_expected_reboot_generation"], 8)
        self.assertEqual(opens, [UPLINK.port])
        self.assertTrue(handle.closed)

    def test_runtime_requires_fresh_reboot_reason_and_response_counter(self):
        for field, value in (
            ("last_expected_reboot_reason", "cold_boot"),
            ("responses_completed", 0),
            ("responses_completed", False),
            ("last_expected_reboot_generation", 7),
            ("last_expected_reboot_generation", 0),
            ("last_expected_reboot_generation", False),
        ):
            status = valid_status()
            if field == "responses_completed":
                status["usb_health"][field] = value
            else:
                status[field] = value
            handles: list[ScriptedSerial] = []

            def open_candidate(_port: str):
                handle = ScriptedSerial({
                    b"FOF_PING\nFOF_STATUS\n": status_reply(status),
                })
                handles.append(handle)
                return handle

            with (
                self.subTest(field=field, value=value),
                mock.patch.object(verify.time, "sleep", return_value=None),
                self.assertRaises(VerificationError),
            ):
                verify.wait_for_runtime(
                    UPLINK,
                    ASSIGNMENT,
                    VERSION,
                    "normal",
                    reboot_proof=reboot_proof(),
                    timeout_s=0.01,
                    serial_factory=open_candidate,
                    candidate_ports=PortSnapshots(
                        [UPLINK.port],
                        [],
                        ["/dev/cu.new"],
                    ),
                )
            self.assertTrue(all(handle.closed for handle in handles))

    def test_runtime_requires_exact_wrap_aware_reboot_successor(self):
        for pre_generation, post_generation in (
            (7, 6),
            (7, 7),
            (7, 9),
            (0xF0F0B006, 0xF0F0B007),
        ):
            with (
                self.subTest(
                    pre_generation=pre_generation,
                    post_generation=post_generation,
                ),
                self.assertRaisesRegex(
                    VerificationError,
                    "exact successor",
                ),
            ):
                verify.verify_status(
                    valid_status(generation=post_generation),
                    ASSIGNMENT,
                    VERSION,
                    "normal",
                    reboot_proof=reboot_proof(
                        generation=pre_generation
                    ),
                )

        for pre_generation, post_generation in (
            (7, 8),
            (0xFFFFFFFF, 1),
            (0xF0F0B006, 0xF0F0B008),
        ):
            with self.subTest(
                pre_generation=pre_generation,
                post_generation=post_generation,
            ):
                result = verify.verify_status(
                    valid_status(generation=post_generation),
                    ASSIGNMENT,
                    VERSION,
                    "normal",
                    reboot_proof=reboot_proof(
                        generation=pre_generation
                    ),
                )
                self.assertEqual(
                    result["last_expected_reboot_generation"],
                    post_generation,
                )

    def test_runtime_host_path_never_calls_blocking_serial_flush(self):
        source = __import__("inspect").getsource(
            __import__("tools.badge_flasher.verify", fromlist=["*"])
        )
        self.assertNotIn("handle.flush()", source)

    def test_accepts_exact_runtime_graph(self):
        assignment=TopologyAssignment("E0:72:A1:F9:47:FC","E0:72:A1:F9:49:84","E0:72:A1:F8:4C:58")
        self.assertEqual(verify_status(valid_status(), assignment, "0.64.76-badge-defcon34", "normal")["safe_mode"], False)

    def test_requires_exact_selected_seed_current_inactive_and_zero_shield(
        self,
    ):
        assignment = TopologyAssignment(
            "E0:72:A1:F9:47:FC",
            "E0:72:A1:F9:49:84",
            "E0:72:A1:F8:4C:58",
        )
        for role in ("normal", "infected", "immune"):
            status = valid_status()
            status["game_seed"] = role
            status["game_state"] = role
            with self.subTest(role=role):
                self.assertEqual(
                    verify_status(
                        status,
                        assignment,
                        "0.64.76-badge-defcon34",
                        role,
                    )["game_seed"],
                    role,
                )

        invalid_values = (
            ("game_seed", "infected"),
            ("game_state", "immune"),
            ("game_active", True),
            ("game_shield", 1),
            ("game_shield", False),
        )
        for field, value in invalid_values:
            status = valid_status()
            status[field] = value
            with (
                self.subTest(field=field, value=value),
                self.assertRaises(VerificationError),
            ):
                verify_status(
                    status,
                    assignment,
                    "0.64.76-badge-defcon34",
                    "normal",
                )

        for field in (
            "game_seed",
            "game_state",
            "game_active",
            "game_shield",
        ):
            status = valid_status()
            status.pop(field)
            with (
                self.subTest(missing=field),
                self.assertRaises(VerificationError),
            ):
                verify_status(
                    status,
                    assignment,
                    "0.64.76-badge-defcon34",
                    "normal",
                )

    def test_rejects_wrong_scanner_identity(self):
        status=valid_status(); status["scanners"][0]["hardware_id"]="E0:72:A1:00:00:01"
        with self.assertRaisesRegex(VerificationError,"MAC mismatch"):
            verify_status(status,TopologyAssignment("E0:72:A1:F9:47:FC","E0:72:A1:F9:49:84","E0:72:A1:F8:4C:58"),"0.64.76-badge-defcon34","normal")

    def test_manufacturing_evidence_excludes_nearby_detections(self):
        status = valid_status()
        status["entities"] = [{"bssid": "AA:BB:CC:DD:EE:FF"}]
        status["game_peer"] = "not-safe-for-ledger"
        evidence = runtime_evidence(status)
        self.assertNotIn("entities", evidence)
        self.assertNotIn("bssid", str(evidence))
        self.assertNotIn("game_peer", evidence)
        self.assertEqual(
            {
                field: evidence[field]
                for field in (
                    "game_seed",
                    "game_state",
                    "game_active",
                    "game_shield",
                )
            },
            {
                "game_seed": "normal",
                "game_state": "normal",
                "game_active": False,
                "game_shield": 0,
            },
        )

    def test_missing_fail_closed_fields_never_pass(self):
        assignment = TopologyAssignment("E0:72:A1:F9:47:FC","E0:72:A1:F9:49:84","E0:72:A1:F8:4C:58")
        for field in ("safe_mode", "recovery_mode", "usb_control_alive", "scanner_uart_alive"):
            status = valid_status()
            status.pop(field)
            with self.subTest(field=field), self.assertRaises(VerificationError):
                verify_status(status, assignment, "0.64.76-badge-defcon34", "normal")
        for field in ("rollback_pending", "recovery_mode"):
            status = valid_status()
            status["scanners"][0].pop(field)
            with self.subTest(scanner_field=field), self.assertRaises(VerificationError):
                verify_status(status, assignment, "0.64.76-badge-defcon34", "normal")


if __name__ == "__main__": unittest.main()
