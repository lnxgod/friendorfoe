#!/usr/bin/env python3
"""Focused C1-C3 tests for trusted descriptor-bound application I/O."""

from __future__ import annotations

import dataclasses
import fcntl
import os
import pty
import stat
import termios
import unittest
from types import SimpleNamespace
from unittest import mock

import usb_descriptor_binding as usb


def _descriptor(
    device: str,
    serial_number: str,
    location: str,
) -> SimpleNamespace:
    return SimpleNamespace(
        device=device,
        vid=usb.ESPRESSIF_USB_SERIAL_JTAG_VID,
        pid=usb.ESPRESSIF_USB_SERIAL_JTAG_PID,
        serial_number=serial_number,
        location=location,
    )


def _record(
    device: str,
    serial_number: str,
    location: str,
) -> usb.UsbDescriptorRecord:
    info = os.lstat(device)
    return usb.UsbDescriptorRecord(
        device=device,
        vid=usb.ESPRESSIF_USB_SERIAL_JTAG_VID,
        pid=usb.ESPRESSIF_USB_SERIAL_JTAG_PID,
        serial_number=usb.canonical_usb_serial(serial_number),
        location=location,
        stat_device=info.st_dev,
        stat_inode=info.st_ino,
        stat_rdev=info.st_rdev,
    )


class DescriptorCensusTests(unittest.TestCase):
    def setUp(self) -> None:
        self.master_fds: list[int] = []
        self.slave_fds: list[int] = []
        self.paths: list[str] = []
        for _index in range(3):
            master, slave = pty.openpty()
            self.master_fds.append(master)
            self.slave_fds.append(slave)
            self.paths.append(os.ttyname(slave))

    def tearDown(self) -> None:
        for fd in self.slave_fds + self.master_fds:
            try:
                os.close(fd)
            except OSError:
                pass

    def test_records_and_bindings_are_frozen(self) -> None:
        self.assertTrue(dataclasses.is_dataclass(usb.UsbDescriptorRecord))
        self.assertTrue(
            usb.UsbDescriptorRecord.__dataclass_params__.frozen
        )
        self.assertTrue(usb.TrustedUplinkBinding.__dataclass_params__.frozen)

    def test_complete_census_normalizes_and_opens_nothing(self) -> None:
        descriptors = (
            _descriptor(self.paths[2], "E072A1F94859", "3-3"),
            SimpleNamespace(
                device="/dev/unrelated",
                vid=0x1234,
                pid=0x5678,
                serial_number=None,
                location=None,
            ),
            _descriptor(self.paths[0], "E0:72:A1:F9:47:FC", "3-1"),
            _descriptor(self.paths[1], "e0-72-a1-f9-48-58", "3-2"),
        )
        with mock.patch.object(
            usb, "_list_port_descriptors", return_value=descriptors
        ), mock.patch.object(
            usb, "_serial_module",
            side_effect=AssertionError("census constructed serial"),
        ):
            census = usb.take_usb_descriptor_census()
        self.assertEqual(
            [record.device for record in census], sorted(self.paths)
        )
        self.assertEqual(
            {record.serial_number for record in census},
            {
                "e0:72:a1:f9:47:fc",
                "e0:72:a1:f9:48:58",
                "e0:72:a1:f9:48:59",
            },
        )
        self.assertTrue(all(
            stat.S_ISCHR(os.lstat(record.device).st_mode)
            for record in census
        ))

    def test_any_malformed_supported_descriptor_rejects_round(self) -> None:
        valid = _descriptor(
            self.paths[0], "e0:72:a1:f9:47:fc", "3-1"
        )
        cases = (
            _descriptor(self.paths[1], "not-a-serial", "3-2"),
            _descriptor(self.paths[1], "e0:72:a1:f9:48:58", ""),
            _descriptor("/dev/../tmp/not-normal", "e0:72:a1:f9:48:58", "3-2"),
        )
        for malformed in cases:
            with self.subTest(malformed=malformed), mock.patch.object(
                usb, "_list_port_descriptors",
                return_value=(valid, malformed),
            ), self.assertRaises(usb.UsbDescriptorBindingError):
                usb.take_usb_descriptor_census()

    def test_duplicate_path_serial_or_location_rejects_whole_round(self) -> None:
        base = (
            _descriptor(
                self.paths[0], "e0:72:a1:f9:47:fc", "3-1"
            ),
            _descriptor(
                self.paths[1], "e0:72:a1:f9:48:58", "3-2"
            ),
        )
        cases = (
            _descriptor(
                self.paths[0], "e0:72:a1:f9:48:59", "3-3"
            ),
            _descriptor(
                self.paths[2], "e0:72:a1:f9:47:fc", "3-3"
            ),
            _descriptor(
                self.paths[2], "e0:72:a1:f9:48:59", "3-2"
            ),
        )
        for duplicate in cases:
            with self.subTest(duplicate=duplicate), mock.patch.object(
                usb, "_list_port_descriptors",
                return_value=base + (duplicate,),
            ), self.assertRaisesRegex(
                usb.UsbDescriptorBindingError, "duplicate"
            ):
                usb.take_usb_descriptor_census()

    def test_three_cables_plus_path_without_role_ack_has_zero_opens(self) -> None:
        census = tuple(sorted((
            _record(self.paths[0], "e0:72:a1:f9:47:fc", "3-1"),
            _record(self.paths[1], "e0:72:a1:f9:48:58", "3-2"),
            _record(self.paths[2], "e0:72:a1:f9:48:59", "3-3"),
        ), key=lambda record: record.device))
        with mock.patch.object(
            usb, "_serial_module",
            side_effect=AssertionError("untrusted role opened serial"),
        ) as serial_module, self.assertRaisesRegex(
            usb.UsbDescriptorBindingError, "not trusted"
        ):
            usb.bind_selected_uplink(
                census, selected_port=self.paths[0]
            )
        serial_module.assert_not_called()

    def test_operator_or_retained_binding_selects_only_exact_identity(
        self,
    ) -> None:
        census = tuple(sorted((
            _record(self.paths[0], "e0:72:a1:f9:47:fc", "3-1"),
            _record(self.paths[1], "e0:72:a1:f9:48:58", "3-2"),
            _record(self.paths[2], "e0:72:a1:f9:48:59", "3-3"),
        ), key=lambda record: record.device))
        selected, binding = usb.bind_selected_uplink(
            census,
            selected_port=self.paths[0],
            operator_acknowledged=True,
        )
        self.assertEqual(selected, census[
            [record.device for record in census].index(self.paths[0])
        ])
        self.assertEqual(binding.source, "operator-selection")

        moved = dataclasses.replace(selected, device=self.paths[2])
        rebound_census = tuple(sorted(
            (moved,) + tuple(
                record for record in census
                if record.serial_number != selected.serial_number and
                record.device != self.paths[2]
            ),
            key=lambda record: record.device,
        ))
        rebound, same_binding = usb.bind_selected_uplink(
            rebound_census,
            selected_port=None,
            trusted_binding=dataclasses.replace(
                binding, source="retained-session"
            ),
        )
        self.assertEqual(rebound.serial_number, selected.serial_number)
        self.assertEqual(rebound.location, selected.location)
        self.assertEqual(same_binding.source, "retained-session")

    def test_operator_binding_opens_only_the_selected_three_cable_record(
        self,
    ) -> None:
        census = tuple(sorted((
            _record(self.paths[0], "e0:72:a1:f9:47:fc", "3-1"),
            _record(self.paths[1], "e0:72:a1:f9:48:58", "3-2"),
            _record(self.paths[2], "e0:72:a1:f9:48:59", "3-3"),
        ), key=lambda record: record.device))
        selected, binding = usb.bind_selected_uplink(
            census,
            selected_port=self.paths[1],
            operator_acknowledged=True,
        )
        selected_fd = self.slave_fds[
            self.paths.index(selected.device)
        ]
        opened_paths: list[str] = []

        class FakeSerial:
            def __init__(self, *, port) -> None:
                self.port = port

            def open(self) -> None:
                opened_paths.append(self.port)

            def fileno(self) -> int:
                return selected_fd

            def reset_input_buffer(self) -> None:
                return None

            def close(self) -> None:
                return None

        with mock.patch.object(
            usb, "take_usb_descriptor_census", return_value=census
        ), mock.patch.object(
            usb, "_serial_module",
            return_value=SimpleNamespace(Serial=FakeSerial),
        ), mock.patch.object(
            usb, "reset_neutral_clear_flow_control",
        ):
            transport = usb.open_bound_application_serial(
                selected,
                expected_uplink_serial=binding.serial_number,
            )
            transport.close()
        self.assertEqual(opened_paths, [selected.device])

    def test_operator_acknowledgement_must_be_exact_boolean(self) -> None:
        census = (_record(
            self.paths[0], "e0:72:a1:f9:47:fc", "3-1"
        ),)
        for invalid in (1, "yes", None):
            with self.subTest(invalid=invalid), self.assertRaises(
                usb.UsbDescriptorBindingError
            ):
                usb.bind_selected_uplink(
                    census,
                    selected_port=self.paths[0],
                    operator_acknowledged=invalid,  # type: ignore[arg-type]
                )


class BoundApplicationOpenTests(unittest.TestCase):
    SERIAL = "e0:72:a1:f9:47:fc"

    def setUp(self) -> None:
        self.master_fd, self.slave_fd = pty.openpty()
        self.path = os.ttyname(self.slave_fd)
        self.record = _record(self.path, self.SERIAL, "4-1")

    def tearDown(self) -> None:
        for fd in (self.slave_fd, self.master_fd):
            try:
                os.close(fd)
            except OSError:
                pass

    def test_preopen_drift_constructs_no_serial_handle(self) -> None:
        with mock.patch.object(
            usb, "take_usb_descriptor_census", return_value=()
        ), mock.patch.object(
            usb, "_serial_module",
            side_effect=AssertionError("drift constructed serial"),
        ) as serial_module, self.assertRaisesRegex(
            usb.UsbDescriptorBindingError, "changed"
        ):
            usb.open_bound_application_serial(
                self.record, expected_uplink_serial=self.SERIAL
            )
        serial_module.assert_not_called()

    def test_open_sequence_is_reset_neutral_and_authorizes_before_drain(
        self,
    ) -> None:
        events: list[str] = []

        class FakeSerial:
            def __init__(self, *, port) -> None:
                events.append(f"construct:{port}")
                self.closed = False
                self.line_changes: list[tuple[str, object]] = []

            def __setattr__(self, name, value):
                if name in ("dtr", "rts"):
                    self.line_changes.append((name, value))
                object.__setattr__(self, name, value)

            def open(self) -> None:
                self.assert_configuration()
                events.append("open")

            def assert_configuration(self) -> None:
                if self.dsrdtr is not True or self.rtscts is not True or \
                        self.exclusive is not True or \
                        self.port != outer.path:
                    raise AssertionError("unsafe serial configuration")

            def fileno(self) -> int:
                return outer.slave_fd

            def reset_input_buffer(self) -> None:
                events.append("drain")

            def close(self) -> None:
                self.closed = True
                events.append("close")

            def write(self, payload: bytes) -> int:
                events.append("write")
                return len(payload)

        outer = self
        raw_holder: list[FakeSerial] = []

        def construct(*, port):
            raw = FakeSerial(port=port)
            raw_holder.append(raw)
            return raw

        revalidations = 0

        def revalidate(record):
            nonlocal revalidations
            revalidations += 1
            events.append(f"census:{revalidations}")
            return record

        with mock.patch.object(
            usb, "revalidate_usb_descriptor_record",
            side_effect=revalidate,
        ), mock.patch.object(
            usb, "_serial_module",
            return_value=SimpleNamespace(Serial=construct),
        ), mock.patch.object(
            usb, "_require_fd_matches_record",
            side_effect=lambda *_args: events.append("fd"),
        ), mock.patch.object(
            usb, "reset_neutral_clear_flow_control",
            side_effect=lambda *_args: events.append("termios"),
        ):
            transport = usb.open_bound_application_serial(
                self.record, expected_uplink_serial=self.SERIAL
            )

        self.assertEqual(raw_holder[0].line_changes, [])
        self.assertEqual(events[0:3], [
            "census:1", "construct:None", "open",
        ])
        self.assertGreater(events.index("drain"), events.index("census:3"))
        self.assertGreater(events.index("drain"), events.index("fd"))
        with self.assertRaises(usb.BoundTransportReopenForbidden):
            transport.open()
        with self.assertRaises(usb.BoundTransportReopenForbidden):
            transport.port = "/dev/replacement"
        transport.close()

    def test_post_open_descriptor_drift_closes_before_drain_or_write(
        self,
    ) -> None:
        events: list[str] = []

        class FakeSerial:
            def __init__(self, *, port) -> None:
                self.port = port
                self.dsrdtr = False
                self.rtscts = False

            def open(self) -> None:
                events.append("open")

            def fileno(self) -> int:
                return outer.slave_fd

            def reset_input_buffer(self) -> None:
                events.append("drain")

            def close(self) -> None:
                events.append("close")

            def write(self, _payload: bytes) -> int:
                events.append("write")
                return 0

        outer = self
        calls = 0

        def revalidate(record):
            nonlocal calls
            calls += 1
            if calls == 2:
                raise usb.UsbDescriptorBindingError("injected drift")
            return record

        with mock.patch.object(
            usb, "revalidate_usb_descriptor_record",
            side_effect=revalidate,
        ), mock.patch.object(
            usb, "_serial_module",
            return_value=SimpleNamespace(Serial=FakeSerial),
        ), mock.patch.object(
            usb, "_require_fd_matches_record",
        ), self.assertRaisesRegex(
            usb.UsbDescriptorBindingError, "drift"
        ):
            usb.open_bound_application_serial(
                self.record, expected_uplink_serial=self.SERIAL
            )
        self.assertEqual(events, ["open", "close"])

    def test_real_serialposix_pty_issues_no_modem_control_ioctl(self) -> None:
        import serial.serialposix  # type: ignore

        modem_requests = {
            value for name in ("TIOCMBIS", "TIOCMBIC", "TIOCMSET")
            if type(value := getattr(termios, name, None)) is int
        }
        observed_modem_requests: list[int] = []
        original_ioctl = serial.serialposix.fcntl.ioctl

        def watched_ioctl(fd, request, *args):
            if request in modem_requests:
                observed_modem_requests.append(request)
            return original_ioctl(fd, request, *args)

        with mock.patch.object(
            usb, "take_usb_descriptor_census",
            return_value=(self.record,),
        ), mock.patch.object(
            serial.serialposix.fcntl, "ioctl", side_effect=watched_ioctl,
        ):
            transport = usb.open_bound_application_serial(
                self.record, expected_uplink_serial=self.SERIAL
            )
            self.assertEqual(transport.fileno(), transport.fileno())
            transport.close()
        self.assertEqual(observed_modem_requests, [])

    def test_real_serialposix_post_open_drift_closes_without_modem_ioctl(
        self,
    ) -> None:
        import serial.serialposix  # type: ignore

        modem_requests = {
            value for name in ("TIOCMBIS", "TIOCMBIC", "TIOCMSET")
            if type(value := getattr(termios, name, None)) is int
        }
        observed_modem_requests: list[int] = []
        original_ioctl = serial.serialposix.fcntl.ioctl

        def watched_ioctl(fd, request, *args):
            if request in modem_requests:
                observed_modem_requests.append(request)
            return original_ioctl(fd, request, *args)

        with mock.patch.object(
            usb, "take_usb_descriptor_census",
            side_effect=((self.record,), ()),
        ), mock.patch.object(
            serial.serialposix.fcntl, "ioctl", side_effect=watched_ioctl,
        ), self.assertRaisesRegex(
            usb.UsbDescriptorBindingError, "changed"
        ):
            usb.open_bound_application_serial(
                self.record, expected_uplink_serial=self.SERIAL
            )
        self.assertEqual(observed_modem_requests, [])


if __name__ == "__main__":
    unittest.main(verbosity=2)
