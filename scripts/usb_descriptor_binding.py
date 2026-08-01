"""Immutable USB descriptor binding for badge application serial I/O.

This module deliberately stops at the running-application transport.  ROM
attachment and esptool ownership belong to the later continuous-ROM phase.
"""

from __future__ import annotations

import os
import re
import stat
import termios
from dataclasses import dataclass
from typing import Any, Literal


ESPRESSIF_USB_SERIAL_JTAG_VID = 0x303A
ESPRESSIF_USB_SERIAL_JTAG_PID = 0x1001
_SERIAL_MAC_RE = re.compile(
    r"^(?:[0-9A-Fa-f]{12}|"
    r"[0-9A-Fa-f]{2}(?P<sep>[:-])"
    r"[0-9A-Fa-f]{2}(?P=sep)[0-9A-Fa-f]{2}(?P=sep)"
    r"[0-9A-Fa-f]{2}(?P=sep)[0-9A-Fa-f]{2}(?P=sep)"
    r"[0-9A-Fa-f]{2})$"
)


class UsbDescriptorBindingError(RuntimeError):
    """A descriptor census, role binding, or bound open failed closed."""


class BoundTransportReopenForbidden(UsbDescriptorBindingError):
    """The retained application transport cannot change paths or reopen."""


@dataclass(frozen=True, slots=True)
class UsbDescriptorRecord:
    device: str
    vid: int
    pid: int
    serial_number: str
    location: str
    stat_device: int
    stat_inode: int
    stat_rdev: int


@dataclass(frozen=True, slots=True)
class TrustedUplinkBinding:
    serial_number: str
    location: str | None
    source: Literal[
        "retained-session", "factory-ledger", "operator-selection"
    ]


def canonical_usb_serial(value: Any) -> str:
    """Return the canonical lower-case colon-delimited ESP32 base MAC."""
    if type(value) is not str or value != value.strip():
        raise UsbDescriptorBindingError(
            "supported USB descriptor has a malformed serial number"
        )
    match = _SERIAL_MAC_RE.fullmatch(value)
    if match is None:
        raise UsbDescriptorBindingError(
            "supported USB descriptor has a malformed serial number"
        )
    compact = value.replace(":", "").replace("-", "").lower()
    return ":".join(compact[index:index + 2] for index in range(0, 12, 2))


def _normalized_device_path(value: Any) -> str:
    if type(value) is not str or value != value.strip() or not value or \
            "\x00" in value or "\r" in value or "\n" in value or \
            not os.path.isabs(value) or os.path.normpath(value) != value or \
            not value.startswith("/dev/"):
        raise UsbDescriptorBindingError(
            "supported USB descriptor has a malformed device path"
        )
    return value


def _normalized_location(value: Any) -> str:
    if type(value) is not str or value != value.strip() or not value or \
            "\x00" in value or "\r" in value or "\n" in value or \
            any(ord(character) < 0x20 or ord(character) == 0x7F
                for character in value):
        raise UsbDescriptorBindingError(
            "supported USB descriptor has a malformed USB location"
        )
    return value


def _descriptor_record(descriptor: Any) -> UsbDescriptorRecord:
    device = _normalized_device_path(getattr(descriptor, "device", None))
    vid = getattr(descriptor, "vid", None)
    pid = getattr(descriptor, "pid", None)
    if type(vid) is not int or type(pid) is not int or \
            vid != ESPRESSIF_USB_SERIAL_JTAG_VID or \
            pid != ESPRESSIF_USB_SERIAL_JTAG_PID:
        raise UsbDescriptorBindingError(
            "supported USB descriptor has a malformed VID/PID"
        )
    serial_number = canonical_usb_serial(
        getattr(descriptor, "serial_number", None)
    )
    location = _normalized_location(getattr(descriptor, "location", None))
    try:
        info = os.lstat(device)
    except OSError as exc:
        raise UsbDescriptorBindingError(
            "supported USB descriptor path could not be inspected"
        ) from exc
    if not stat.S_ISCHR(info.st_mode):
        raise UsbDescriptorBindingError(
            "supported USB descriptor path is not a no-follow character "
            "device"
        )
    return UsbDescriptorRecord(
        device=device,
        vid=vid,
        pid=pid,
        serial_number=serial_number,
        location=location,
        stat_device=info.st_dev,
        stat_inode=info.st_ino,
        stat_rdev=info.st_rdev,
    )


def _list_port_descriptors() -> tuple[Any, ...]:
    try:
        import serial.tools.list_ports  # type: ignore

        return tuple(serial.tools.list_ports.comports())
    except Exception as exc:
        raise UsbDescriptorBindingError(
            "could not complete the USB descriptor census"
        ) from exc


def take_usb_descriptor_census() -> tuple[UsbDescriptorRecord, ...]:
    """Freeze one complete, validated supported-device descriptor census.

    Enumeration and no-follow metadata inspection perform no serial open.
    One malformed supported descriptor rejects the entire round.
    """
    raw_descriptors = _list_port_descriptors()
    records: list[UsbDescriptorRecord] = []
    for descriptor in raw_descriptors:
        vid = getattr(descriptor, "vid", None)
        pid = getattr(descriptor, "pid", None)
        if (vid, pid) != (
            ESPRESSIF_USB_SERIAL_JTAG_VID,
            ESPRESSIF_USB_SERIAL_JTAG_PID,
        ):
            continue
        records.append(_descriptor_record(descriptor))

    records.sort(key=lambda record: record.device)
    unique_fields = (
        ("path", [record.device for record in records]),
        ("serial", [record.serial_number for record in records]),
        ("location", [record.location for record in records]),
    )
    for label, values in unique_fields:
        if len(values) != len(set(values)):
            raise UsbDescriptorBindingError(
                f"USB descriptor census contains a duplicate {label}"
            )
    return tuple(records)


def _validated_census(
    census: tuple[UsbDescriptorRecord, ...],
) -> tuple[UsbDescriptorRecord, ...]:
    if type(census) is not tuple or any(
        type(record) is not UsbDescriptorRecord for record in census
    ):
        raise UsbDescriptorBindingError(
            "USB descriptor census is not an immutable record tuple"
        )
    if tuple(sorted(census, key=lambda record: record.device)) != census:
        raise UsbDescriptorBindingError(
            "USB descriptor census is not canonically ordered"
        )
    for field_name in ("device", "serial_number", "location"):
        values = [getattr(record, field_name) for record in census]
        if len(values) != len(set(values)):
            raise UsbDescriptorBindingError(
                f"USB descriptor census contains duplicate {field_name}"
            )
    for record in census:
        if record.device != _normalized_device_path(record.device) or \
                record.serial_number != canonical_usb_serial(
                    record.serial_number
                ) or record.location != _normalized_location(record.location) \
                or record.vid != ESPRESSIF_USB_SERIAL_JTAG_VID or \
                record.pid != ESPRESSIF_USB_SERIAL_JTAG_PID or any(
                    type(value) is not int or value < 0 for value in (
                        record.stat_device,
                        record.stat_inode,
                        record.stat_rdev,
                    )
                ):
            raise UsbDescriptorBindingError(
                "USB descriptor census contains a malformed record"
            )
    return census


def bind_selected_uplink(
    census: tuple[UsbDescriptorRecord, ...],
    *,
    selected_port: str | None,
    trusted_binding: TrustedUplinkBinding | None = None,
    operator_acknowledged: bool = False,
) -> tuple[UsbDescriptorRecord, TrustedUplinkBinding]:
    """Resolve exactly one trusted uplink without probing applications."""
    records = _validated_census(census)
    if type(operator_acknowledged) is not bool:
        raise UsbDescriptorBindingError(
            "operator uplink acknowledgement must be an exact boolean"
        )
    normalized_port = (
        _normalized_device_path(selected_port)
        if selected_port is not None else None
    )

    if trusted_binding is not None:
        if type(trusted_binding) is not TrustedUplinkBinding:
            raise UsbDescriptorBindingError("trusted uplink binding is invalid")
        serial_number = canonical_usb_serial(trusted_binding.serial_number)
        if trusted_binding.serial_number != serial_number or \
                type(trusted_binding.source) is not str or \
                trusted_binding.source not in (
                    "retained-session",
                    "factory-ledger",
                    "operator-selection",
                ):
            raise UsbDescriptorBindingError("trusted uplink binding is invalid")
        if trusted_binding.location is not None:
            if type(trusted_binding.location) is not str:
                raise UsbDescriptorBindingError(
                    "trusted uplink location is not canonical"
                )
            location = _normalized_location(trusted_binding.location)
            if location != trusted_binding.location:
                raise UsbDescriptorBindingError(
                    "trusted uplink location is not canonical"
                )
        matches = [
            record for record in records
            if record.serial_number == serial_number and
            (
                trusted_binding.location is None or
                record.location == trusted_binding.location
            ) and
            (normalized_port is None or record.device == normalized_port)
        ]
        if len(matches) != 1:
            raise UsbDescriptorBindingError(
                "trusted uplink is absent, duplicated, moved outside its "
                "location policy, or excluded by the path filter"
            )
        return matches[0], trusted_binding

    if operator_acknowledged:
        if normalized_port is None:
            raise UsbDescriptorBindingError(
                "operator uplink binding requires an explicit selected port"
            )
        matches = [
            record for record in records if record.device == normalized_port
        ]
        if len(matches) != 1:
            raise UsbDescriptorBindingError(
                "selected uplink path is absent or duplicated"
            )
        selected = matches[0]
        return selected, TrustedUplinkBinding(
            serial_number=selected.serial_number,
            location=selected.location,
            source="operator-selection",
        )

    if len(records) == 1 and (
        normalized_port is None or records[0].device == normalized_port
    ):
        # Preserve the existing one-cable development flow.  This does not
        # weaken the three-cable rule: there is only one supported identity in
        # the complete census, and the resulting binding is frozen before I/O.
        selected = records[0]
        return selected, TrustedUplinkBinding(
            serial_number=selected.serial_number,
            location=selected.location,
            source="operator-selection",
        )

    raise UsbDescriptorBindingError(
        "uplink role is not trusted; select --port and acknowledge it with "
        "--bind-selected-uplink before opening any of the three badge cables"
    )


def revalidate_usb_descriptor_record(
    expected: UsbDescriptorRecord,
) -> UsbDescriptorRecord:
    """Require a fresh complete census containing the exact bound record."""
    if type(expected) is not UsbDescriptorRecord:
        raise UsbDescriptorBindingError(
            "bound application open requires a USB descriptor record"
        )
    census = take_usb_descriptor_census()
    exact = [record for record in census if record == expected]
    if len(exact) != 1:
        raise UsbDescriptorBindingError(
            "bound USB descriptor changed before or during application open"
        )
    return exact[0]


def _require_fd_matches_record(
    fd: int,
    expected: UsbDescriptorRecord,
) -> None:
    if type(fd) is not int or fd < 0:
        raise UsbDescriptorBindingError(
            "bound application transport returned an invalid descriptor"
        )
    try:
        descriptor_info = os.fstat(fd)
        path_info = os.lstat(expected.device)
    except OSError as exc:
        raise UsbDescriptorBindingError(
            "bound application descriptor/path could not be revalidated"
        ) from exc
    if not stat.S_ISCHR(path_info.st_mode) or (
        path_info.st_dev,
        path_info.st_ino,
        path_info.st_rdev,
    ) != (
        expected.stat_device,
        expected.stat_inode,
        expected.stat_rdev,
    ) or (
        descriptor_info.st_dev,
        descriptor_info.st_ino,
        descriptor_info.st_rdev,
    ) != (
        path_info.st_dev,
        path_info.st_ino,
        path_info.st_rdev,
    ):
        raise UsbDescriptorBindingError(
            "opened application descriptor does not match the bound "
            "no-follow device"
        )


def require_fd_matches_record(
    fd: int,
    expected: UsbDescriptorRecord,
) -> None:
    """Public shared FD/path identity check for retained USB transports."""
    _require_fd_matches_record(fd, expected)


def reset_neutral_clear_flow_control(fd: int) -> None:
    """Clear hardware flow control using termios, never modem-line ioctls."""
    try:
        attributes = termios.tcgetattr(fd)
        cflag = attributes[2]
        for name in ("CRTSCTS", "CCTS_OFLOW", "CRTS_IFLOW"):
            flag = getattr(termios, name, 0)
            if type(flag) is int:
                cflag &= ~flag
        attributes[2] = cflag
        termios.tcsetattr(fd, termios.TCSANOW, attributes)
    except (OSError, termios.error) as exc:
        raise UsbDescriptorBindingError(
            "could not clear flow control on the bound application "
            "descriptor"
        ) from exc


class BoundApplicationSerialTransport:
    """Non-reopenable view of one already-authorized pyserial handle."""

    __slots__ = ("__raw", "__record", "__closed")

    def __init__(self, raw: Any, record: UsbDescriptorRecord) -> None:
        object.__setattr__(self, "_BoundApplicationSerialTransport__raw", raw)
        object.__setattr__(
            self, "_BoundApplicationSerialTransport__record", record
        )
        object.__setattr__(
            self, "_BoundApplicationSerialTransport__closed", False
        )

    @property
    def port(self) -> str:
        return self.__record.device

    @port.setter
    def port(self, _value: str) -> None:
        raise BoundTransportReopenForbidden(
            "bound application transport path cannot change"
        )

    def open(self) -> None:
        raise BoundTransportReopenForbidden(
            "bound application transport cannot reopen"
        )

    def close(self) -> None:
        if not self.__closed:
            self.__raw.close()
            object.__setattr__(
                self, "_BoundApplicationSerialTransport__closed", True
            )

    def fileno(self) -> int:
        return self.__raw.fileno()

    def __getattr__(self, name: str) -> Any:
        if name in ("dtr", "rts", "setDTR", "setRTS"):
            raise AttributeError(name)
        return getattr(self.__raw, name)


def _serial_module() -> Any:
    try:
        import serial  # type: ignore

        return serial
    except Exception as exc:
        raise UsbDescriptorBindingError(
            "pyserial is required for bound badge application I/O"
        ) from exc


def open_bound_application_serial(
    expected: UsbDescriptorRecord,
    *,
    expected_uplink_serial: str,
    baudrate: int = 115200,
    timeout: float = 0.15,
    write_timeout: float = 3.0,
) -> BoundApplicationSerialTransport:
    """Open and immediately revalidate one reset-neutral application FD."""
    if type(expected) is not UsbDescriptorRecord:
        raise UsbDescriptorBindingError(
            "bound application open requires a USB descriptor record"
        )
    trusted_serial = canonical_usb_serial(expected_uplink_serial)
    if trusted_serial != expected_uplink_serial or \
            expected.serial_number != trusted_serial:
        raise UsbDescriptorBindingError(
            "bound descriptor does not match the trusted uplink serial"
        )
    revalidate_usb_descriptor_record(expected)

    serial_module = _serial_module()
    try:
        raw = serial_module.Serial(port=None)
    except Exception as exc:
        raise UsbDescriptorBindingError(
            "could not construct the bound application serial transport"
        ) from exc

    opened = False
    original_private_reset: Any = None
    private_reset_suppressed = False
    try:
        raw.dsrdtr = True
        raw.rtscts = True
        raw.baudrate = baudrate
        raw.timeout = timeout
        raw.write_timeout = write_timeout
        raw.exclusive = True
        raw.port = expected.device

        # serialposix.open() normally drains input before returning.  Defer
        # that drain until the opened FD and a fresh complete census are both
        # proven to still name the frozen descriptor record.
        original_private_reset = getattr(raw, "_reset_input_buffer", None)
        if callable(original_private_reset):
            raw._reset_input_buffer = lambda: None
            private_reset_suppressed = True

        raw.open()
        opened = True
        fd = raw.fileno()
        _require_fd_matches_record(fd, expected)
        revalidate_usb_descriptor_record(expected)
        reset_neutral_clear_flow_control(fd)
        _require_fd_matches_record(fd, expected)
        revalidate_usb_descriptor_record(expected)

        if private_reset_suppressed:
            raw._reset_input_buffer = original_private_reset
        raw.reset_input_buffer()
        return BoundApplicationSerialTransport(raw, expected)
    except Exception as exc:
        try:
            if private_reset_suppressed:
                raw._reset_input_buffer = original_private_reset
        except Exception:
            pass
        try:
            raw.close()
        except Exception:
            pass
        if isinstance(exc, UsbDescriptorBindingError):
            raise
        stage = "open" if not opened else "post-open descriptor authorization"
        raise UsbDescriptorBindingError(
            f"bound application serial {stage} failed"
        ) from exc
