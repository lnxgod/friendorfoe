#!/usr/bin/env python3
"""One-handle, descriptor-bound ESP32-S3 ROM mutation capability.

The public session accepts only an immutable USB descriptor, a canonical
expected base MAC, and Phase-B frozen artifact bytes.  It never gives
esptool a pathname it can reopen and never resumes a mutation after transport
continuity is lost.
"""

from __future__ import annotations

from dataclasses import dataclass
import hashlib
import hmac
from pathlib import Path
import re
import struct
import sys
from types import SimpleNamespace
from typing import Any


SCRIPTS_DIR = Path(__file__).resolve().parent
ESP32_SCRIPTS_DIR = SCRIPTS_DIR.parent / "esp32" / "scripts"
if str(SCRIPTS_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPTS_DIR))
if str(ESP32_SCRIPTS_DIR) not in sys.path:
    sys.path.insert(0, str(ESP32_SCRIPTS_DIR))

if __package__:
    from .esptool_provenance import (  # noqa: E402
        EsptoolProvenanceError,
        VerifiedEsptoolRuntime,
        load_verified_platformio_esptool,
    )
    from .usb_descriptor_binding import (  # noqa: E402
        BoundTransportReopenForbidden,
        UsbDescriptorBindingError,
        UsbDescriptorRecord,
        canonical_usb_serial,
        require_fd_matches_record,
        reset_neutral_clear_flow_control,
        revalidate_usb_descriptor_record,
    )
else:
    from esptool_provenance import (  # noqa: E402
        EsptoolProvenanceError,
        VerifiedEsptoolRuntime,
        load_verified_platformio_esptool,
    )
    from usb_descriptor_binding import (  # noqa: E402
        BoundTransportReopenForbidden,
        UsbDescriptorBindingError,
        UsbDescriptorRecord,
        canonical_usb_serial,
        require_fd_matches_record,
        reset_neutral_clear_flow_control,
        revalidate_usb_descriptor_record,
    )
from secure_artifact_tree import (  # noqa: E402
    FrozenArtifactMember,
    FrozenArtifactSet,
    FrozenBytesView,
    SecureArtifactError,
    _aggregate_sha256,
)
from verified_badge_artifacts import (  # noqa: E402
    UPLINK_ROLE,
    _validate_frozen_inputs,
)


_ROM_BAUD = 115200
_FLASH_BAUD = 460800
_SERIAL_TIMEOUT_S = 0.1
_SERIAL_WRITE_TIMEOUT_S = 10.0
_RTC_CNTL_OPTION1_REG = 0x6000812C
_RTC_CNTL_FORCE_DOWNLOAD_BOOT_MASK = 0x00000001
_OWNER_CLOSE_TOKEN = object()
_FLASH_SECTOR_BYTES = 0x1000
_UPLINK_FLASH_BYTES = 8 * 1024 * 1024
_UPLINK_OTA0_BYTES = 0x200000
_PARTITION_ENTRY = struct.Struct("<HBBII16sI")
_UPLINK_REGION_CAPACITIES = {
    "artifact.bootloader": 0x8000,
    "artifact.partitions": 0x1000,
    "artifact.ota_data_initial": 0x2000,
    "artifact.firmware": _UPLINK_OTA0_BYTES,
}

_UPLINK_LOGICAL_BY_FILENAME = {
    "bootloader/bootloader.bin": "artifact.bootloader",
    "partition_table/partition-table.bin": "artifact.partitions",
    "ota_data_initial.bin": "artifact.ota_data_initial",
    UPLINK_ROLE.app_filename: "artifact.firmware",
}


class BoundRomError(RuntimeError):
    """A continuous ROM session failed closed."""


class BoundRomIdentityError(BoundRomError):
    """The USB descriptor and ROM eFuse identity did not agree."""


class BoundRomArtifactError(BoundRomError):
    """Frozen artifact bytes did not prove the exact uplink layout."""


class BoundRomStateError(BoundRomError):
    """A ROM operation was attempted outside the one-way state machine."""


class BoundRomOperationError(BoundRomError):
    """A non-transport ROM command failed."""


class BoundRomUnavailableError(BoundRomError):
    """The reset-neutral ROM sync received no serial data before mutation."""


class BoundRomMutationUncertainError(BoundRomError):
    """A mutation lost continuity and must never be resumed."""


@dataclass(frozen=True, slots=True)
class RomIdentityEvidence:
    descriptor_serial: str
    base_mac: str
    chip_name: str
    revision: str
    flash_size: str
    psram_size: str


@dataclass(frozen=True, slots=True)
class RomOperationEvidence:
    operation: str
    base_mac: str
    aggregate_sha256: str | None
    member_sha256: tuple[tuple[int, str, str], ...]


class _RetainedSerialTransport:
    """Esptool-facing view whose path, FD, close, and reopen are fixed."""

    __slots__ = (
        "__raw",
        "__record",
        "__fd",
        "__violated",
        "__owner_closed",
        "__connect_active",
        "__connect_close_attempted",
    )

    def __init__(
        self,
        raw: Any,
        record: UsbDescriptorRecord,
        fd: int,
    ) -> None:
        object.__setattr__(self, "_RetainedSerialTransport__raw", raw)
        object.__setattr__(self, "_RetainedSerialTransport__record", record)
        object.__setattr__(self, "_RetainedSerialTransport__fd", fd)
        object.__setattr__(
            self, "_RetainedSerialTransport__violated", False
        )
        object.__setattr__(
            self, "_RetainedSerialTransport__owner_closed", False
        )
        object.__setattr__(
            self, "_RetainedSerialTransport__connect_active", False
        )
        object.__setattr__(
            self,
            "_RetainedSerialTransport__connect_close_attempted",
            False,
        )

    def __setattr__(self, name: str, value: object) -> None:
        if name.startswith("_RetainedSerialTransport__"):
            object.__setattr__(self, name, value)
            return
        if name in ("baudrate", "timeout", "write_timeout"):
            setattr(self.__raw, name, value)
            return
        self._forbid(
            f"bound ROM transport attribute {name!r} cannot change"
        )

    @property
    def port(self) -> str:
        return self.__record.device

    @port.setter
    def port(self, _value: object) -> None:
        self._forbid("bound ROM transport path cannot change")

    @property
    def portstr(self) -> str:
        return self.__record.device

    @portstr.setter
    def portstr(self, _value: object) -> None:
        self._forbid("bound ROM transport path cannot change")

    @property
    def name(self) -> str:
        return self.__record.device

    @name.setter
    def name(self, _value: object) -> None:
        self._forbid("bound ROM transport path cannot change")

    def _forbid(self, message: str) -> None:
        object.__setattr__(
            self, "_RetainedSerialTransport__violated", True
        )
        raise BoundTransportReopenForbidden(message)

    def open(self) -> None:
        self._forbid("bound ROM transport cannot reopen")

    def close(self) -> None:
        if self.__connect_active:
            object.__setattr__(
                self,
                "_RetainedSerialTransport__connect_close_attempted",
                True,
            )
            return
        self._forbid("only the bound ROM session owner may close the transport")

    def begin_connect(self) -> None:
        if self.__connect_active or self.__connect_close_attempted:
            self._forbid("bound ROM connect lifecycle cannot restart")
        object.__setattr__(
            self, "_RetainedSerialTransport__connect_active", True
        )

    def finish_connect(self) -> bool:
        if not self.__connect_active:
            self._forbid("bound ROM connect lifecycle is not active")
        object.__setattr__(
            self, "_RetainedSerialTransport__connect_active", False
        )
        return self.__connect_close_attempted

    def fileno(self) -> int:
        self.assert_intact()
        return self.__fd

    def assert_intact(self) -> None:
        if self.__violated or self.__connect_close_attempted:
            raise BoundTransportReopenForbidden(
                "bound ROM transport previously attempted to close, reopen, "
                "or change paths"
            )
        if self.__owner_closed:
            raise BoundTransportReopenForbidden(
                "bound ROM transport owner has closed the retained handle"
            )
        try:
            current_fd = self.__raw.fileno()
        except Exception as exc:
            raise BoundTransportReopenForbidden(
                "bound ROM transport lost its retained descriptor"
            ) from exc
        if current_fd != self.__fd:
            raise BoundTransportReopenForbidden(
                "bound ROM transport descriptor changed"
            )
        raw_port = getattr(self.__raw, "port", self.__record.device)
        if raw_port != self.__record.device:
            raise BoundTransportReopenForbidden(
                "bound ROM raw transport path changed"
            )

    def owner_close(self, token: object) -> None:
        if token is not _OWNER_CLOSE_TOKEN:
            self._forbid(
                "only the bound ROM session owner may close the transport"
            )
        if self.__owner_closed:
            return
        object.__setattr__(
            self, "_RetainedSerialTransport__owner_closed", True
        )
        self.__raw.close()

    def __getattr__(self, name: str) -> Any:
        if name in ("dtr", "rts", "setDTR", "setRTS"):
            raise AttributeError(name)
        return getattr(self.__raw, name)


def _serial_module() -> Any:
    try:
        import serial  # type: ignore

        return serial
    except Exception as exc:
        raise BoundRomError(
            "pyserial is required for a bound ROM session"
        ) from exc


def _canonical_rom_mac(value: object) -> str:
    if isinstance(value, str):
        try:
            canonical = canonical_usb_serial(value)
        except UsbDescriptorBindingError as exc:
            raise BoundRomIdentityError(
                "ROM returned a malformed eFuse base MAC"
            ) from exc
        if canonical != value:
            raise BoundRomIdentityError(
                "ROM returned a noncanonical eFuse base MAC"
            )
        return canonical
    if isinstance(value, (bytes, bytearray, tuple, list)):
        values = tuple(value)
        if len(values) != 6 or any(
            type(part) is not int or part < 0 or part > 0xFF
            for part in values
        ):
            raise BoundRomIdentityError(
                "ROM returned a malformed eFuse base MAC"
            )
        return ":".join(f"{part:02x}" for part in values)
    raise BoundRomIdentityError("ROM returned a malformed eFuse base MAC")


def _is_exact_no_data_connect_failure(exc: BaseException) -> bool:
    expected = (
        "Failed to connect to ESP32-S3: No serial data received.\n"
        "For troubleshooting steps visit: "
        "https://docs.espressif.com/projects/esptool/en/latest/"
        "troubleshooting.html"
    )
    return type(exc.args) is tuple and exc.args == (expected,)


def _uplink_layout() -> tuple[tuple[int, str], ...]:
    mapping = UPLINK_ROLE.full_mapping
    try:
        layout = tuple(
            sorted(
                (
                    offset,
                    _UPLINK_LOGICAL_BY_FILENAME[filename],
                )
                for offset, filename in mapping.items()
            )
        )
    except (KeyError, TypeError) as exc:
        raise BoundRomArtifactError(
            "uplink role layout is outside the reviewed ROM mapping"
        ) from exc
    expected = (
        (0x00000, "artifact.bootloader"),
        (0x08000, "artifact.partitions"),
        (0x0F000, "artifact.ota_data_initial"),
        (0x20000, "artifact.firmware"),
    )
    if layout != expected:
        raise BoundRomArtifactError(
            "uplink role layout differs from the reviewed ESP32-S3 layout"
        )
    return layout


def _validate_artifact_set(
    artifacts: FrozenArtifactSet,
) -> tuple[tuple[int, str], ...]:
    if type(artifacts) is not FrozenArtifactSet:
        raise BoundRomArtifactError(
            "ROM mutation requires an exact FrozenArtifactSet"
        )
    try:
        if type(artifacts.members) is not tuple:
            raise BoundRomArtifactError(
                "frozen artifact members are not an immutable tuple"
            )
        for member in artifacts.members:
            if type(member) is not FrozenArtifactMember or \
                    type(member.content) is not bytes or \
                    member.size != len(member.content) or \
                    not hmac.compare_digest(
                        member.sha256,
                        hashlib.sha256(member.content).hexdigest(),
                    ):
                raise BoundRomArtifactError(
                    "frozen artifact member bytes or digest changed"
                )
        # Re-run the set's structural, ordering, total-size, receipt, and
        # aggregate invariants in addition to the independent member hashes.
        FrozenArtifactSet(
            receipt_sha256=artifacts.receipt_sha256,
            members=artifacts.members,
            aggregate_sha256=artifacts.aggregate_sha256,
        )
        aggregate = _aggregate_sha256(
            artifacts.receipt_sha256, artifacts.members
        )
        if not hmac.compare_digest(
            artifacts.aggregate_sha256, aggregate
        ):
            raise BoundRomArtifactError(
                "frozen artifact aggregate changed"
            )
        _validate_frozen_inputs(artifacts, UPLINK_ROLE)
        layout = _uplink_layout()
        members_by_name = {
            member.logical_name: member for member in artifacts.members
        }
        if any(name not in members_by_name for _, name in layout):
            raise BoundRomArtifactError(
                "frozen artifacts do not contain the complete uplink layout"
            )
        if any(
            members_by_name[name].size <= 0
            for _, name in layout
        ):
            raise BoundRomArtifactError(
                "frozen uplink flash regions must all be non-empty"
            )
        prior_sector_end = 0
        for offset, name in layout:
            member = members_by_name[name]
            padded_size = (member.size + 3) & ~3
            if padded_size > _UPLINK_REGION_CAPACITIES[name]:
                raise BoundRomArtifactError(
                    f"frozen {name} exceeds its exact badge region"
                )
            sector_start = offset & ~(_FLASH_SECTOR_BYTES - 1)
            sector_end = (
                offset + member.size + _FLASH_SECTOR_BYTES - 1
            ) & ~(_FLASH_SECTOR_BYTES - 1)
            if sector_start < prior_sector_end:
                raise BoundRomArtifactError(
                    "frozen uplink flash regions overlap after sector "
                    "rounding"
                )
            if sector_end > _UPLINK_FLASH_BYTES:
                raise BoundRomArtifactError(
                    "frozen uplink flash region exceeds 8MB flash"
                )
            prior_sector_end = sector_end

        partition_bytes = members_by_name[
            "artifact.partitions"
        ].content
        ota0_entries: list[tuple[int, int, str]] = []
        otadata_entries: list[tuple[int, int, str]] = []
        for index in range(
            0,
            len(partition_bytes) - _PARTITION_ENTRY.size + 1,
            _PARTITION_ENTRY.size,
        ):
            (
                magic,
                entry_type,
                subtype,
                offset,
                size,
                raw_label,
                _flags,
            ) = _PARTITION_ENTRY.unpack_from(partition_bytes, index)
            if magic in (0xFFFF, 0xEBEB):
                break
            if magic != 0x50AA:
                raise BoundRomArtifactError(
                    "compiled uplink partition table is malformed"
                )
            if entry_type == 0 and subtype == 0x10:
                try:
                    label = raw_label.split(b"\0", 1)[0].decode(
                        "ascii", errors="strict"
                    )
                except UnicodeDecodeError as exc:
                    raise BoundRomArtifactError(
                        "compiled ota_0 label is malformed"
                    ) from exc
                ota0_entries.append((offset, size, label))
            if entry_type == 1 and subtype == 0x00:
                try:
                    label = raw_label.split(b"\0", 1)[0].decode(
                        "ascii", errors="strict"
                    )
                except UnicodeDecodeError as exc:
                    raise BoundRomArtifactError(
                        "compiled otadata label is malformed"
                    ) from exc
                otadata_entries.append((offset, size, label))
        if otadata_entries != [(0x0F000, 0x2000, "otadata")]:
            raise BoundRomArtifactError(
                "compiled uplink otadata partition differs from the exact "
                "badge slot"
            )
        if ota0_entries != [
            (
                UPLINK_ROLE.app_offset,
                _UPLINK_OTA0_BYTES,
                "ota_0",
            )
        ]:
            raise BoundRomArtifactError(
                "compiled uplink ota_0 partition differs from the exact "
                "2MB badge slot"
            )
        firmware_size = members_by_name["artifact.firmware"].size
        padded_firmware_size = (firmware_size + 3) & ~3
        if padded_firmware_size > _UPLINK_OTA0_BYTES:
            raise BoundRomArtifactError(
                "frozen uplink firmware does not fit the ota_0 partition"
            )
        return layout
    except BoundRomArtifactError:
        raise
    except (SecureArtifactError, ValueError, TypeError) as exc:
        raise BoundRomArtifactError(
            "frozen artifacts failed the exact uplink layout checks"
        ) from exc


def _open_reset_neutral_raw(
    expected: UsbDescriptorRecord,
) -> tuple[Any, int]:
    serial_module = _serial_module()
    try:
        raw = serial_module.Serial(port=None)
    except Exception as exc:
        raise BoundRomError(
            "could not construct the bound ROM serial transport"
        ) from exc

    opened = False
    original_private_reset: Any = None
    private_reset_suppressed = False
    try:
        raw.dsrdtr = True
        raw.rtscts = True
        raw.baudrate = _ROM_BAUD
        raw.timeout = _SERIAL_TIMEOUT_S
        raw.write_timeout = _SERIAL_WRITE_TIMEOUT_S
        raw.exclusive = True
        raw.port = expected.device

        original_private_reset = getattr(raw, "_reset_input_buffer", None)
        if not callable(original_private_reset):
            raise BoundRomError(
                "pyserial cannot defer its pre-authorization input drain"
            )
        raw._reset_input_buffer = lambda: None
        private_reset_suppressed = True
        raw.open()
        opened = True
        fd = raw.fileno()
        require_fd_matches_record(fd, expected)
        revalidate_usb_descriptor_record(expected)
        # rtscts=True prevents pyserial's initial open from asserting modem
        # lines.  Once the FD is authorized, make its future reconfiguration
        # policy flow-control-free as well; otherwise a later baud change
        # silently restores CRTSCTS/CCTS_OFLOW.
        raw.rtscts = False
        if raw.rtscts is not False:
            raise BoundRomError(
                "pyserial retained hardware flow-control policy"
            )
        reset_neutral_clear_flow_control(fd)
        require_fd_matches_record(fd, expected)
        revalidate_usb_descriptor_record(expected)
        raw._reset_input_buffer = original_private_reset
        private_reset_suppressed = False
        return raw, fd
    except BaseException:
        if private_reset_suppressed:
            try:
                raw._reset_input_buffer = original_private_reset
            except Exception:
                pass
        if opened:
            try:
                raw.close()
            except Exception:
                pass
        raise


class BoundRomSession:
    """One-way probe/write/verify/run session over one retained USB handle."""

    __slots__ = (
        "_expected",
        "_expected_base_mac",
        "_runtime",
        "_transport",
        "_loader",
        "_serial_exception",
        "_esptool_fatal_error",
        "_closed",
        "_probed",
        "_written_aggregate",
        "_verified",
        "_flash_configured",
        "_ran",
        "_mutation_started",
        "_uncertain",
        "_failed",
        "_transcript",
    )

    def __init__(
        self,
        *,
        expected: UsbDescriptorRecord,
        expected_base_mac: str,
        runtime: VerifiedEsptoolRuntime,
        transport: _RetainedSerialTransport,
        loader: object,
        serial_exception: type[Exception],
        esptool_fatal_error: type[Exception],
    ) -> None:
        self._expected = expected
        self._expected_base_mac = expected_base_mac
        self._runtime = runtime
        self._transport = transport
        self._loader = loader
        self._serial_exception = serial_exception
        self._esptool_fatal_error = esptool_fatal_error
        self._closed = False
        self._probed = False
        self._written_aggregate: str | None = None
        self._verified = False
        self._flash_configured = False
        self._ran = False
        self._mutation_started = False
        self._uncertain: str | None = None
        self._failed: str | None = None
        self._transcript: list[RomOperationEvidence] = []

    @classmethod
    def open(
        cls,
        expected: UsbDescriptorRecord,
        expected_base_mac: str,
    ) -> "BoundRomSession":
        if type(expected) is not UsbDescriptorRecord:
            raise BoundRomIdentityError(
                "bound ROM open requires an immutable USB descriptor record"
            )
        try:
            canonical_mac = canonical_usb_serial(expected_base_mac)
        except UsbDescriptorBindingError as exc:
            raise BoundRomIdentityError(
                "expected ROM base MAC is malformed"
            ) from exc
        if canonical_mac != expected_base_mac or \
                expected.serial_number != canonical_mac:
            raise BoundRomIdentityError(
                "expected ROM base MAC does not match the descriptor serial"
            )

        try:
            runtime = load_verified_platformio_esptool()
        except EsptoolProvenanceError as exc:
            raise BoundRomError(
                "verified esptool runtime provenance is unavailable"
            ) from exc
        raw: Any | None = None
        transport: _RetainedSerialTransport | None = None
        try:
            revalidate_usb_descriptor_record(expected)
            raw, fd = _open_reset_neutral_raw(expected)
            transport = _RetainedSerialTransport(raw, expected, fd)
            esptool = runtime.esptool
            rom_class = esptool.targets.esp32s3.ESP32S3ROM
            loader = rom_class(
                transport, baud=_ROM_BAUD, trace_enabled=False
            )
            if type(loader) is not rom_class or \
                    getattr(loader, "_port", None) is not transport:
                raise BoundRomIdentityError(
                    "esptool did not retain the already-open transport"
                )
            esptool_fatal_error = getattr(
                esptool, "FatalError", None
            )
            if not isinstance(esptool_fatal_error, type) or \
                    not issubclass(esptool_fatal_error, Exception):
                raise BoundRomError(
                    "verified esptool FatalError contract is unavailable"
                )
            transport.begin_connect()
            try:
                loader.connect(mode="no_reset", attempts=1)
            except esptool_fatal_error as exc:
                close_attempted = transport.finish_connect()
                if close_attempted and _is_exact_no_data_connect_failure(exc):
                    raise BoundRomUnavailableError(
                        "bound descriptor is not answering reset-neutral ROM "
                        "sync"
                    ) from exc
                raise BoundRomOperationError(
                    "reset-neutral ROM sync returned a nonempty, malformed, "
                    "or incompatible response"
                ) from exc
            else:
                close_attempted = transport.finish_connect()
                if close_attempted:
                    raise BoundTransportReopenForbidden(
                        "esptool attempted to close the retained transport "
                        "during a successful ROM connect"
                    )
            runtime.audit_after_command(loader)
            transport.assert_intact()
            require_fd_matches_record(fd, expected)
            revalidate_usb_descriptor_record(expected)
            actual_mac = _canonical_rom_mac(loader.read_mac("BASE_MAC"))
            runtime.audit_after_command(loader)
            if actual_mac != canonical_mac or \
                    actual_mac != expected.serial_number:
                raise BoundRomIdentityError(
                    "same-handle ROM eFuse MAC differs from the bound USB "
                    "descriptor"
                )

            serial_module = _serial_module()
            serial_exception = getattr(
                getattr(serial_module, "serialutil", None),
                "SerialException",
                OSError,
            )
            if not isinstance(serial_exception, type) or \
                    not issubclass(serial_exception, Exception):
                raise BoundRomError(
                    "pyserial SerialException contract is unavailable"
                )
            return cls(
                expected=expected,
                expected_base_mac=canonical_mac,
                runtime=runtime,
                transport=transport,
                loader=loader,
                serial_exception=serial_exception,
                esptool_fatal_error=esptool_fatal_error,
            )
        except BaseException:
            if transport is not None:
                try:
                    transport.owner_close(_OWNER_CLOSE_TOKEN)
                except BaseException:
                    pass
            elif raw is not None:
                try:
                    raw.close()
                except BaseException:
                    pass
            runtime.close()
            raise

    @property
    def transcript(self) -> tuple[RomOperationEvidence, ...]:
        return tuple(self._transcript)

    def _require_usable(self) -> None:
        if self._closed:
            raise BoundRomStateError("bound ROM session is closed")
        if self._uncertain is not None:
            raise BoundRomMutationUncertainError(self._uncertain)
        if self._failed is not None:
            raise BoundRomStateError(self._failed)
        if self._ran:
            raise BoundRomStateError(
                "bound ROM session already crossed the final run boundary"
            )

    def _is_transport_failure(self, exc: BaseException) -> bool:
        return isinstance(
            exc,
            (
                BoundTransportReopenForbidden,
                UsbDescriptorBindingError,
                OSError,
                self._serial_exception,
                self._esptool_fatal_error,
            ),
        )

    def _mark_uncertain(
        self,
        operation: str,
        exc: BaseException,
    ) -> BoundRomMutationUncertainError:
        self._uncertain = (
            f"bound ROM mutation continuity was lost during {operation}; "
            "the retained path will not be reopened"
        )
        return BoundRomMutationUncertainError(self._uncertain)

    def _poison(self, operation: str) -> None:
        self._failed = (
            f"bound ROM session failed during {operation}; the retained "
            "session cannot resume"
        )

    def _read_and_require_mac(self) -> str:
        actual = _canonical_rom_mac(
            self._loader.read_mac("BASE_MAC")
        )
        self._runtime.audit_after_command(self._loader)
        if actual != self._expected_base_mac or \
                actual != self._expected.serial_number:
            raise BoundRomIdentityError(
                "same-handle ROM eFuse MAC changed"
            )
        return actual

    def _authorize(self, operation: str) -> str:
        self._require_usable()
        try:
            self._transport.assert_intact()
            fd = self._transport.fileno()
            require_fd_matches_record(fd, self._expected)
            revalidate_usb_descriptor_record(self._expected)
            mac = self._read_and_require_mac()
            self._transport.assert_intact()
            require_fd_matches_record(fd, self._expected)
            revalidate_usb_descriptor_record(self._expected)
            return mac
        except BaseException as exc:
            if self._mutation_started:
                raise self._mark_uncertain(operation, exc) from exc
            raise

    def _operation_evidence(
        self,
        operation: str,
        base_mac: str,
        artifacts: FrozenArtifactSet | None,
    ) -> RomOperationEvidence:
        if artifacts is None:
            aggregate = self._written_aggregate
            hashes: tuple[tuple[int, str, str], ...] = ()
        else:
            layout = _uplink_layout()
            aggregate = artifacts.aggregate_sha256
            hashes = tuple(
                (
                    offset,
                    logical_name,
                    next(
                        member.sha256 for member in artifacts.members
                        if member.logical_name == logical_name
                    ),
                )
                for offset, logical_name in layout
            )
        evidence = RomOperationEvidence(
            operation=operation,
            base_mac=base_mac,
            aggregate_sha256=aggregate,
            member_sha256=hashes,
        )
        self._transcript.append(evidence)
        return evidence

    def probe(self) -> RomIdentityEvidence:
        self._require_usable()
        if self._probed:
            raise BoundRomStateError("bound ROM probe already completed")
        mac = self._authorize("probe")
        try:
            self._runtime.esptool.cmds.flash_id(
                self._loader, SimpleNamespace()
            )
            self._runtime.audit_after_command(self._loader)
            rom_class = (
                self._runtime.esptool.targets.esp32s3.ESP32S3ROM
            )
            if type(self._loader) is not rom_class or \
                    getattr(self._loader, "CHIP_NAME", None) != "ESP32-S3":
                raise BoundRomIdentityError(
                    "same-handle ROM target is not the reviewed ESP32-S3"
                )

            description = self._loader.get_chip_description()
            self._runtime.audit_after_command(self._loader)
            if type(description) is not str:
                raise BoundRomIdentityError(
                    "ESP32-S3 revision description is malformed"
                )
            description_match = re.fullmatch(
                r"ESP32-S3(?:-PICO-1)? \([^()\r\n]+\) "
                r"\(revision (v[0-9]{1,2}(?:\.[0-9]{1,2}){1,2})\)",
                description,
            )
            if description_match is None:
                raise BoundRomIdentityError(
                    "ESP32-S3 revision description is malformed"
                )
            revision = description_match.group(1)

            flash_id = self._loader.flash_id()
            self._runtime.audit_after_command(self._loader)
            detected_sizes = getattr(
                self._runtime.esptool.cmds,
                "DETECTED_FLASH_SIZES",
                None,
            )
            if type(flash_id) is not int or isinstance(flash_id, bool) or \
                    type(detected_sizes) is not dict:
                raise BoundRomIdentityError(
                    "ESP32-S3 flash identity is malformed"
                )
            flash_size = detected_sizes.get((flash_id >> 16) & 0xFF)
            if flash_size != "8MB":
                raise BoundRomIdentityError(
                    "bound ESP32-S3 does not have 8MB flash"
                )

            features = self._loader.get_chip_features()
            self._runtime.audit_after_command(self._loader)
            if type(features) is not list or any(
                type(feature) is not str for feature in features
            ):
                raise BoundRomIdentityError(
                    "ESP32-S3 feature evidence is malformed"
                )
            psram = [
                feature for feature in features if "PSRAM" in feature
            ]
            if len(psram) != 1 or re.fullmatch(
                r"Embedded PSRAM 8MB(?: \([^()\r\n]+\))?",
                psram[0],
            ) is None:
                raise BoundRomIdentityError(
                    "bound ESP32-S3 does not have embedded 8MB PSRAM"
                )
        except BaseException as exc:
            self._poison("probe")
            if isinstance(exc, BoundRomError):
                raise
            if self._is_transport_failure(exc):
                raise BoundRomOperationError(
                    "bound ROM probe lost its retained transport"
                ) from exc
            raise BoundRomOperationError("bound ROM probe failed") from exc
        self._probed = True
        return RomIdentityEvidence(
            descriptor_serial=self._expected.serial_number,
            base_mac=mac,
            chip_name="ESP32-S3",
            revision=revision,
            flash_size=flash_size,
            psram_size="8MB",
        )

    def _ensure_stub(self) -> None:
        if bool(getattr(self._loader, "IS_STUB", False)):
            return
        self._authorize("stub upload")
        try:
            stub = self._loader.run_stub()
            expected_class = (
                self._runtime.esptool.targets.esp32s3.ESP32S3StubLoader
            )
            if type(stub) is not expected_class or \
                    getattr(stub, "_port", None) is not self._transport:
                raise BoundRomOperationError(
                    "esptool stub did not retain the same transport"
                )
            self._runtime.audit_after_command(stub)
            change_baud = getattr(stub, "change_baud", None)
            if not callable(change_baud):
                raise BoundRomOperationError(
                    "verified ESP32-S3 stub lacks same-handle baud change"
                )
            change_baud(_FLASH_BAUD)
            self._runtime.audit_after_command(stub)
            self._loader = stub
            self._authorize("stub activation")
        except BaseException as exc:
            self._poison("stub activation")
            if isinstance(exc, BoundRomError):
                raise
            raise BoundRomOperationError(
                "same-handle ESP32-S3 stub activation failed"
            ) from exc

    def _validate_frozen_bootloader(
        self,
        artifacts: FrozenArtifactSet,
    ) -> None:
        content = artifacts.member_bytes("artifact.bootloader")
        if len(content) < 24 or content[0] != 0xE9 or \
                content[2] != 0x02 or content[3] != 0x3F:
            raise BoundRomArtifactError(
                "frozen bootloader is not encoded for ESP32-S3 "
                "dio/80m/8MB"
            )
        view = artifacts.open_readonly("artifact.bootloader")
        try:
            image_class = (
                self._runtime.esptool.targets.esp32s3
                .ESP32S3ROM.BOOTLOADER_IMAGE
            )
            image = image_class(view)
            image.verify()
            if getattr(image, "chip_id", None) != 9 or \
                    getattr(image, "flash_mode", None) != 0x02 or \
                    getattr(image, "flash_size_freq", None) != 0x3F:
                raise BoundRomArtifactError(
                    "frozen bootloader image identity or flash encoding "
                    "differs from the badge contract"
                )
        except EsptoolProvenanceError as exc:
            self._poison("frozen bootloader provenance audit")
            raise BoundRomOperationError(
                "verified esptool provenance failed during bootloader "
                "validation"
            ) from exc
        except BoundRomArtifactError:
            raise
        except BaseException as exc:
            raise BoundRomArtifactError(
                "frozen bootloader is not a valid ESP32-S3 image"
            ) from exc
        finally:
            view.close()
        try:
            self._runtime.audit_after_command(self._loader)
        except BaseException as exc:
            self._poison("frozen bootloader provenance audit")
            raise BoundRomOperationError(
                "verified esptool provenance failed after bootloader "
                "validation"
            ) from exc

    def _configure_flash_parameters(self) -> None:
        if self._flash_configured:
            raise BoundRomStateError(
                "ESP32-S3 flash parameters were already configured"
            )
        try:
            self._loader.flash_set_parameters(_UPLINK_FLASH_BYTES)
            self._runtime.audit_after_command(self._loader)
            self._flash_configured = True
            self._authorize("flash parameter setup")
        except BaseException as exc:
            self._poison("flash parameter setup")
            if isinstance(exc, BoundRomError):
                raise
            raise BoundRomOperationError(
                "same-handle ESP32-S3 flash parameter setup failed"
            ) from exc

    @staticmethod
    def _close_views(views: tuple[FrozenBytesView, ...]) -> None:
        for view in views:
            view.close()

    def write_layout(
        self,
        artifacts: FrozenArtifactSet,
    ) -> RomOperationEvidence:
        self._require_usable()
        if not self._probed or self._written_aggregate is not None:
            raise BoundRomStateError(
                "write requires one successful probe and no prior write"
            )
        layout = _validate_artifact_set(artifacts)
        views = tuple(
            artifacts.open_readonly(logical_name)
            for _, logical_name in layout
        )
        try:
            self._validate_frozen_bootloader(artifacts)
            self._ensure_stub()
            self._configure_flash_parameters()
            self._authorize("write")
            args = SimpleNamespace(
                addr_filename=[
                    (offset, view)
                    for (offset, _), view in zip(layout, views)
                ],
                compress=True,
                no_compress=False,
                no_stub=False,
                force=False,
                chip="esp32s3",
                flash_size="keep",
                flash_mode="keep",
                flash_freq="keep",
                erase_all=False,
                encrypt=False,
                encrypt_files=None,
                ignore_flash_encryption_efuse_setting=False,
            )
            self._mutation_started = True
            try:
                self._runtime.esptool.cmds.write_flash(
                    self._loader, args
                )
                self._runtime.audit_after_command(self._loader)
                self._written_aggregate = artifacts.aggregate_sha256
                post_mac = self._authorize("post-write evidence")
                return self._operation_evidence(
                    "write", post_mac, artifacts
                )
            except BaseException as exc:
                if isinstance(exc, BoundRomMutationUncertainError):
                    if self._uncertain is None:
                        raise self._mark_uncertain("write", exc) from exc
                    raise
                raise self._mark_uncertain("write", exc) from exc
        finally:
            try:
                self._close_views(views)
            except BaseException as exc:
                if self._mutation_started:
                    raise self._mark_uncertain(
                        "write view cleanup", exc
                    ) from exc
                raise

    def verify_layout(
        self,
        artifacts: FrozenArtifactSet,
    ) -> RomOperationEvidence:
        self._require_usable()
        if self._written_aggregate is None or self._verified:
            raise BoundRomStateError(
                "verify requires one completed write and no prior verify"
            )
        layout = _validate_artifact_set(artifacts)
        if not hmac.compare_digest(
            artifacts.aggregate_sha256, self._written_aggregate
        ):
            raise BoundRomArtifactError(
                "verify artifacts differ from the exact written byte set"
            )
        views = tuple(
            artifacts.open_readonly(logical_name)
            for _, logical_name in layout
        )
        try:
            try:
                self._authorize("verify")
                args = SimpleNamespace(
                    addr_filename=[
                        (offset, view)
                        for (offset, _), view in zip(layout, views)
                    ],
                    diff="no",
                    chip="esp32s3",
                    flash_size="keep",
                    flash_mode="keep",
                    flash_freq="keep",
                )
                try:
                    self._runtime.esptool.cmds.verify_flash(
                        self._loader, args
                    )
                    self._runtime.audit_after_command(self._loader)
                except BaseException as exc:
                    raise self._mark_uncertain("verify", exc) from exc
                self._verified = True
                post_mac = self._authorize("post-verify evidence")
                return self._operation_evidence(
                    "verify", post_mac, artifacts
                )
            except BaseException as exc:
                if isinstance(
                    exc, BoundRomMutationUncertainError
                ) and self._uncertain is not None:
                    raise
                raise self._mark_uncertain("verify", exc) from exc
        finally:
            try:
                self._close_views(views)
            except BaseException as exc:
                raise self._mark_uncertain(
                    "verify view cleanup", exc
                ) from exc

    def run_application(self) -> RomOperationEvidence:
        self._require_usable()
        if not self._verified or self._written_aggregate is None:
            raise BoundRomStateError(
                "run requires a successfully written and verified layout"
            )
        try:
            mac = self._authorize("run")
            loader = self._loader
            try:
                loader.write_reg(
                    _RTC_CNTL_OPTION1_REG,
                    0,
                    _RTC_CNTL_FORCE_DOWNLOAD_BOOT_MASK,
                    0,
                )
                self._runtime.audit_after_command(loader)
            except BaseException as exc:
                raise self._mark_uncertain(
                    "force-download clear", exc
                ) from exc

            try:
                loader.watchdog_reset()
                self._runtime.audit_after_command(loader)
            except BaseException as exc:
                raise self._mark_uncertain(
                    "watchdog application reset", exc
                ) from exc
            self._ran = True
            return self._operation_evidence(
                "run",
                mac,
                None,
            )
        except BaseException as exc:
            if isinstance(
                exc, BoundRomMutationUncertainError
            ) and self._uncertain is not None:
                raise
            raise self._mark_uncertain("run", exc) from exc

    def close(self) -> None:
        if self._closed:
            return
        self._closed = True
        try:
            self._transport.owner_close(_OWNER_CLOSE_TOKEN)
        finally:
            self._runtime.close()

    def __enter__(self) -> "BoundRomSession":
        self._require_usable()
        return self

    def __exit__(self, *_args: object) -> None:
        self.close()


__all__ = (
    "BoundRomArtifactError",
    "BoundRomError",
    "BoundRomIdentityError",
    "BoundRomMutationUncertainError",
    "BoundRomOperationError",
    "BoundRomSession",
    "BoundRomStateError",
    "BoundRomUnavailableError",
    "RomIdentityEvidence",
    "RomOperationEvidence",
)
