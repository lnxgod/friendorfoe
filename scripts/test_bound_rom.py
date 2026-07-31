#!/usr/bin/env python3
"""Hardware-free adversarial tests for the continuous bound ROM session."""

from __future__ import annotations

import builtins
import hashlib
import os
from pathlib import Path
import pty
import struct
import termios
from types import SimpleNamespace
import unittest
from unittest import mock

import bound_rom
import secure_artifact_tree as secure
import usb_descriptor_binding as usb


MAC = "e0:72:a1:f9:47:fc"


def _partition_entry(
    entry_type: int,
    subtype: int,
    offset: int,
    size: int,
    label: str,
) -> bytes:
    return struct.pack(
        "<HBBII16sI",
        0x50AA,
        entry_type,
        subtype,
        offset,
        size,
        label.encode("ascii").ljust(16, b"\0"),
        0,
    )


def _valid_partition_table(*, total_size: int | None = None) -> bytes:
    content = b"".join((
        _partition_entry(1, 2, 0x9000, 0x6000, "nvs"),
        _partition_entry(1, 0, 0xF000, 0x2000, "otadata"),
        _partition_entry(0, 0x10, 0x20000, 0x200000, "ota_0"),
        b"\xff" * 32,
    ))
    if total_size is not None:
        if total_size < len(content):
            raise ValueError("requested partition table size is too small")
        content += b"\xff" * (total_size - len(content))
    return content


def _fake_bootloader(payload: bytes = b"BOOT") -> bytes:
    return b"\xe9\x03\x02\x3f" + (b"\0" * 20) + payload


def _frozen_uplink_artifacts(
    *,
    bootloader: bytes,
    partitions: bytes,
    ota_data: bytes,
    firmware: bytes,
) -> secure.FrozenArtifactSet:
    contents = {
        "artifact.bootloader": bootloader,
        "artifact.firmware": firmware,
        "artifact.ota_data_initial": ota_data,
        "artifact.partitions": partitions,
    }
    members = tuple(
        secure.FrozenArtifactMember(
            logical_name=name,
            size=len(content),
            sha256=hashlib.sha256(content).hexdigest(),
            content=content,
        )
        for name, content in sorted(contents.items())
    )
    receipt = "1" * 64
    return secure.FrozenArtifactSet(
        receipt_sha256=receipt,
        members=members,
        aggregate_sha256=secure._aggregate_sha256(receipt, members),
    )


class _SerialFailure(OSError):
    pass


class _FakeFatalError(RuntimeError):
    pass


class _FakeProvenanceError(RuntimeError):
    pass


class _FakeBootloaderImage:
    def __init__(self, source: object) -> None:
        content = source.read()
        source.seek(0)
        if len(content) < 24 or content[:4] != b"\xe9\x03\x02\x3f":
            raise _FakeFatalError("invalid ESP32-S3 image")
        self.chip_id = 9
        self.flash_mode = 0x02
        self.flash_size_freq = 0x3F

    def verify(self) -> None:
        return None


class _FakeRawSerial:
    def __init__(self, source_fd: int, events: list[object]) -> None:
        object.__setattr__(self, "_source_fd", source_fd)
        object.__setattr__(self, "_fd", -1)
        object.__setattr__(self, "_events", events)
        object.__setattr__(self, "_reset_input_buffer", self._private_reset)
        object.__setattr__(self, "close_calls", 0)
        object.__setattr__(self, "open_calls", 0)
        object.__setattr__(self, "reset_line_writes", [])

    def __setattr__(self, name: str, value: object) -> None:
        if name in ("dtr", "rts"):
            self.reset_line_writes.append((name, value))
        object.__setattr__(self, name, value)

    def _private_reset(self) -> None:
        self._events.append("private-reset")

    def open(self) -> None:
        self.open_calls += 1
        self._events.append("serial-open")
        if self._fd >= 0:
            raise AssertionError("raw serial reopened")
        self._fd = os.dup(self._source_fd)
        self._reset_input_buffer()

    def close(self) -> None:
        self.close_calls += 1
        self._events.append("raw-close")
        if self._fd >= 0:
            os.close(self._fd)
            self._fd = -1

    def fileno(self) -> int:
        if self._fd < 0:
            raise OSError("closed")
        return self._fd

    @property
    def is_open(self) -> bool:
        return self._fd >= 0

    def reset_input_buffer(self) -> None:
        self._events.append("reset-input")

    def flushOutput(self) -> None:
        self._events.append("flush-output")

    def read(self, _size: int = 1) -> bytes:
        return b""

    def write(self, data: bytes) -> int:
        return len(data)


class _FakeSerialModule:
    def __init__(self, raw: _FakeRawSerial, events: list[object]) -> None:
        self.raw = raw
        self.events = events
        self.serialutil = SimpleNamespace(SerialException=_SerialFailure)

    def Serial(self, *, port: object) -> _FakeRawSerial:
        self.events.append(("serial-construct", port))
        if port is not None:
            raise AssertionError("serial must be constructed reset-neutral")
        return self.raw


class _FakeRom:
    CHIP_NAME = "ESP32-S3"
    WRITE_FLASH_ATTEMPTS = 1
    mac = bytes.fromhex(MAC.replace(":", ""))
    connect_action = None
    watchdog_action = None
    flash_setup_action = None
    description = "ESP32-S3 (QFN56) (revision v0.2)"
    features = ["WiFi", "BLE", "Embedded PSRAM 8MB (AP_3v3)"]
    flash_id_value = 0x1740EF
    BOOTLOADER_IMAGE = _FakeBootloaderImage

    def __init__(
        self,
        port: object,
        baud: int = 115200,
        trace_enabled: bool = False,
    ) -> None:
        self._port = port
        self.baud = baud
        self.trace_enabled = trace_enabled
        self.secure_download_mode = False
        self.events: list[object] = []

    def connect(
        self,
        mode: str = "default_reset",
        attempts: int = 7,
        detecting: bool = False,
        warnings: bool = True,
    ) -> None:
        self.events.append(("connect", mode, attempts, detecting, warnings))
        action = type(self).connect_action
        if action is not None:
            action(self)

    def read_mac(self, mac_type: str = "BASE_MAC") -> tuple[int, ...]:
        self.events.append(("read-mac", mac_type))
        return tuple(self.mac)

    def run_stub(self, stub: object = None) -> "_FakeStub":
        self.events.append(("run-stub", stub))
        return _FakeStub(self)

    def get_chip_description(self) -> str:
        self.events.append("chip-description")
        return type(self).description

    def get_chip_features(self) -> list[str]:
        self.events.append("chip-features")
        return list(type(self).features)

    def flash_id(self) -> int:
        self.events.append("flash-id")
        return type(self).flash_id_value

    def change_baud(self, baud: int) -> None:
        self.events.append(("change-baud", baud))
        self._port.baudrate = baud

    def flash_set_parameters(self, size: int) -> None:
        self.events.append(("flash-set-parameters", size))
        action = type(self).flash_setup_action
        if action is not None:
            action(self, size)

    def write_reg(
        self,
        address: int,
        value: int,
        mask: int,
        delay_us: int,
    ) -> None:
        self.events.append(("write-reg", address, value, mask, delay_us))

    def watchdog_reset(self) -> None:
        self.events.append("watchdog-reset")
        action = type(self).watchdog_action
        if action is not None:
            action(self)


class _FakeStub(_FakeRom):
    IS_STUB = True

    def __init__(self, rom: _FakeRom) -> None:
        self._port = rom._port
        self.baud = rom.baud
        self.trace_enabled = rom.trace_enabled
        self.secure_download_mode = False
        self.events = rom.events


class _FakeRuntime:
    def __init__(self, events: list[object]) -> None:
        self.events = events
        self.closed = False
        self.command_loaders: list[tuple[str, object]] = []
        self.write_views: tuple[object, ...] = ()
        self.verify_views: tuple[object, ...] = ()
        self.write_action = None
        self.audit_action = None

        def flash_id(loader: object, _args: object) -> None:
            self.command_loaders.append(("probe", loader))

        def write_flash(loader: object, args: object) -> None:
            self.command_loaders.append(("write", loader))
            loader.events.append("write-command")
            self.write_views = tuple(view for _, view in args.addr_filename)
            self.events.append((
                "write-layout",
                tuple((offset, view.name, view.read()) for offset, view
                      in args.addr_filename),
                vars(args).copy(),
            ))
            if self.write_action is not None:
                self.write_action(loader)

        def verify_flash(loader: object, args: object) -> None:
            self.command_loaders.append(("verify", loader))
            self.verify_views = tuple(view for _, view in args.addr_filename)
            self.events.append((
                "verify-layout",
                tuple((offset, view.name, view.read()) for offset, view
                      in args.addr_filename),
                vars(args).copy(),
            ))

        def run(loader: object, _args: object) -> None:
            self.command_loaders.append(("run", loader))

        self.esptool = SimpleNamespace(
            cmds=SimpleNamespace(
                DETECTED_FLASH_SIZES={0x16: "4MB", 0x17: "8MB"},
                flash_id=flash_id,
                write_flash=write_flash,
                verify_flash=verify_flash,
                run=run,
            ),
            targets=SimpleNamespace(
                esp32s3=SimpleNamespace(
                    ESP32S3ROM=_FakeRom,
                    ESP32S3StubLoader=_FakeStub,
                )
            ),
            FatalError=_FakeFatalError,
        )

    def audit_after_command(self, stub_loader: object | None = None) -> None:
        self.events.append(("audit", stub_loader))
        if self.audit_action is not None:
            self.audit_action(stub_loader)

    def close(self) -> None:
        self.events.append("runtime-close")
        self.closed = True


class BoundRomSessionTests(unittest.TestCase):
    def setUp(self) -> None:
        self.master_fd, self.slave_fd = pty.openpty()
        self.device = os.ttyname(self.slave_fd)
        info = os.lstat(self.device)
        self.record = usb.UsbDescriptorRecord(
            device=self.device,
            vid=usb.ESPRESSIF_USB_SERIAL_JTAG_VID,
            pid=usb.ESPRESSIF_USB_SERIAL_JTAG_PID,
            serial_number=MAC,
            location="3-1",
            stat_device=info.st_dev,
            stat_inode=info.st_ino,
            stat_rdev=info.st_rdev,
        )
        self.events: list[object] = []
        self.drift = False
        self.raw = _FakeRawSerial(self.slave_fd, self.events)
        self.serial_module = _FakeSerialModule(self.raw, self.events)
        self.runtime = _FakeRuntime(self.events)
        self.patches = (
            mock.patch.object(
                bound_rom,
                "load_verified_platformio_esptool",
                side_effect=self._load_runtime,
            ),
            mock.patch.object(
                bound_rom,
                "_serial_module",
                return_value=self.serial_module,
            ),
            mock.patch.object(
                bound_rom,
                "revalidate_usb_descriptor_record",
                side_effect=self._revalidate,
            ),
            mock.patch.object(
                bound_rom,
                "reset_neutral_clear_flow_control",
                side_effect=lambda fd: self.events.append(("clear-flow", fd)),
            ),
        )
        for patcher in self.patches:
            patcher.start()
            self.addCleanup(patcher.stop)
        self.addCleanup(os.close, self.slave_fd)
        self.addCleanup(os.close, self.master_fd)
        _FakeRom.mac = bytes.fromhex(MAC.replace(":", ""))
        _FakeRom.connect_action = None
        _FakeRom.watchdog_action = None
        _FakeRom.flash_setup_action = None
        _FakeRom.CHIP_NAME = "ESP32-S3"
        _FakeRom.description = "ESP32-S3 (QFN56) (revision v0.2)"
        _FakeRom.features = [
            "WiFi", "BLE", "Embedded PSRAM 8MB (AP_3v3)"
        ]
        _FakeRom.flash_id_value = 0x1740EF

    def _load_runtime(self) -> _FakeRuntime:
        self.events.append("provenance")
        return self.runtime

    def _revalidate(
        self, expected: usb.UsbDescriptorRecord
    ) -> usb.UsbDescriptorRecord:
        self.events.append("descriptor")
        if self.drift or expected != self.record:
            raise usb.UsbDescriptorBindingError("descriptor drift")
        return self.record

    def open_session(self) -> bound_rom.BoundRomSession:
        return bound_rom.BoundRomSession.open(self.record, MAC)

    def test_open_is_provenance_first_reset_neutral_and_single_handle(
        self,
    ) -> None:
        session = self.open_session()
        try:
            self.assertLess(
                self.events.index("provenance"),
                self.events.index(("serial-construct", None)),
            )
            self.assertEqual(self.raw.open_calls, 1)
            self.assertEqual(self.raw.close_calls, 0)
            self.assertEqual(self.raw.reset_line_writes, [])
            self.assertTrue(self.raw.dsrdtr)
            self.assertFalse(self.raw.rtscts)
            self.assertTrue(self.raw.exclusive)
            self.assertNotIn("private-reset", self.events)
            self.assertEqual(
                session._loader.events[0],
                ("connect", "no_reset", 1, False, True),
            )
        finally:
            session.close()
        self.assertEqual(self.raw.close_calls, 1)
        self.assertTrue(self.runtime.closed)

    def test_real_serialposix_pty_open_uses_no_modem_control_ioctl(
        self,
    ) -> None:
        import serial  # type: ignore
        import serial.serialposix  # type: ignore

        modem_requests = {
            value for name in ("TIOCMBIS", "TIOCMBIC", "TIOCMSET")
            if type(value := getattr(termios, name, None)) is int
        }
        observed: list[int] = []
        original_ioctl = serial.serialposix.fcntl.ioctl

        def watched_ioctl(fd: int, request: int, *args: object) -> object:
            if request in modem_requests:
                observed.append(request)
            return original_ioctl(fd, request, *args)

        with mock.patch.object(
            bound_rom, "_serial_module", return_value=serial
        ), mock.patch.object(
            bound_rom,
            "reset_neutral_clear_flow_control",
            side_effect=usb.reset_neutral_clear_flow_control,
        ), mock.patch.object(
            serial.serialposix.fcntl, "ioctl", side_effect=watched_ioctl
        ):
            session = self.open_session()
            for baudrate in (9600, 115200):
                session._transport.baudrate = baudrate
                cflag = termios.tcgetattr(
                    session._transport.fileno()
                )[2]
                for name in ("CRTSCTS", "CCTS_OFLOW", "CRTS_IFLOW"):
                    flag = getattr(termios, name, 0)
                    if type(flag) is int:
                        self.assertEqual(cflag & flag, 0)
            session.close()
        self.assertEqual(observed, [])

    def test_preopen_descriptor_drift_opens_nothing(self) -> None:
        with mock.patch.object(
            bound_rom,
            "revalidate_usb_descriptor_record",
            side_effect=usb.UsbDescriptorBindingError("drift"),
        ), self.assertRaises(usb.UsbDescriptorBindingError):
            self.open_session()
        self.assertEqual(self.raw.open_calls, 0)
        self.assertEqual(self.raw.close_calls, 0)
        self.assertTrue(self.runtime.closed)

    def test_provenance_failure_opens_no_serial_transport(self) -> None:
        with mock.patch.object(
            bound_rom,
            "load_verified_platformio_esptool",
            side_effect=bound_rom.EsptoolProvenanceError("drift"),
        ), self.assertRaises(bound_rom.BoundRomError):
            self.open_session()
        self.assertEqual(self.raw.open_calls, 0)
        self.assertEqual(self.raw.close_calls, 0)

    def test_post_open_fd_mismatch_closes_without_connecting(self) -> None:
        wrong_raw = _FakeRawSerial(self.master_fd, self.events)
        self.serial_module.raw = wrong_raw
        with self.assertRaises(usb.UsbDescriptorBindingError):
            self.open_session()
        self.assertEqual(wrong_raw.open_calls, 1)
        self.assertEqual(wrong_raw.close_calls, 1)
        self.assertTrue(self.runtime.closed)

    def test_baseexception_during_connect_closes_owner_and_runtime(self) -> None:
        def interrupt(_rom: object) -> None:
            raise KeyboardInterrupt()

        _FakeRom.connect_action = interrupt
        with self.assertRaises(KeyboardInterrupt):
            self.open_session()
        self.assertEqual(self.raw.open_calls, 1)
        self.assertEqual(self.raw.close_calls, 1)
        self.assertTrue(self.runtime.closed)

    def test_exact_reset_neutral_no_data_is_explicitly_unavailable(
        self,
    ) -> None:
        def no_data(rom: object) -> None:
            rom._port.close()
            raise _FakeFatalError(
                "Failed to connect to ESP32-S3: No serial data received.\n"
                "For troubleshooting steps visit: "
                "https://docs.espressif.com/projects/esptool/en/latest/"
                "troubleshooting.html"
            )

        _FakeRom.connect_action = no_data
        with self.assertRaises(bound_rom.BoundRomUnavailableError):
            self.open_session()
        self.assertEqual(self.raw.open_calls, 1)
        self.assertEqual(self.raw.close_calls, 1)
        self.assertTrue(self.runtime.closed)

    def test_nonempty_or_malformed_rom_reply_is_not_normal_absence(
        self,
    ) -> None:
        def corrupt(rom: object) -> None:
            rom._port.close()
            raise _FakeFatalError(
                "Failed to connect to ESP32-S3: Serial data stream stopped: "
                "Possible serial noise or corruption.\n"
                "For troubleshooting steps visit: "
                "https://docs.espressif.com/projects/esptool/en/latest/"
                "troubleshooting.html"
            )

        _FakeRom.connect_action = corrupt
        with self.assertRaises(bound_rom.BoundRomOperationError):
            self.open_session()
        self.assertEqual(self.raw.open_calls, 1)
        self.assertEqual(self.raw.close_calls, 1)
        self.assertTrue(self.runtime.closed)

    def test_mac_must_match_descriptor_and_expected_base_mac(self) -> None:
        _FakeRom.mac = b"\x00\x01\x02\x03\x04\x05"
        with self.assertRaises(bound_rom.BoundRomIdentityError):
            self.open_session()
        self.assertEqual(self.raw.open_calls, 1)
        self.assertEqual(self.raw.close_calls, 1)
        self.assertTrue(self.runtime.closed)

        with self.assertRaises(bound_rom.BoundRomIdentityError):
            bound_rom.BoundRomSession.open(
                self.record, "00:01:02:03:04:05"
            )

    def test_connect_internal_close_is_forbidden_and_owner_closes_once(
        self,
    ) -> None:
        _FakeRom.connect_action = lambda rom: rom._port.close()
        with self.assertRaises(usb.BoundTransportReopenForbidden):
            self.open_session()
        self.assertEqual(self.raw.open_calls, 1)
        self.assertEqual(self.raw.close_calls, 1)
        self.assertTrue(self.runtime.closed)

    def test_retained_transport_forbids_reopen_and_path_mutation(self) -> None:
        for action in (
            lambda loader: loader._port.open(),
            lambda loader: setattr(loader._port, "port", "/dev/other"),
            lambda loader: setattr(loader._port, "_port", "/dev/other"),
            lambda loader: setattr(loader._port, "fd", 999),
        ):
            with self.subTest(action=action):
                session = self.open_session()
                with self.assertRaises(usb.BoundTransportReopenForbidden):
                    action(session._loader)
                self.assertEqual(self.raw.open_calls, 1)
                self.assertEqual(self.raw.close_calls, 0)
                session.close()
                self.assertEqual(self.raw.close_calls, 1)
                # Recreate the isolated fakes for the second subtest.
                self.raw = _FakeRawSerial(self.slave_fd, self.events)
                self.serial_module.raw = self.raw
                self.runtime = _FakeRuntime(self.events)

    def test_operation_descriptor_drift_fails_before_command(self) -> None:
        session = self.open_session()
        with mock.patch.object(
            bound_rom,
            "revalidate_usb_descriptor_record",
            side_effect=usb.UsbDescriptorBindingError("drift"),
        ), self.assertRaises(usb.UsbDescriptorBindingError):
            session.probe()
        self.assertEqual(self.runtime.command_loaders, [])
        session.close()

    def test_probe_uses_the_connected_loader_and_retained_handle(self) -> None:
        session = self.open_session()
        evidence = session.probe()
        self.assertEqual(evidence.base_mac, MAC)
        self.assertEqual(evidence.descriptor_serial, MAC)
        self.assertEqual(evidence.chip_name, "ESP32-S3")
        self.assertEqual(evidence.revision, "v0.2")
        self.assertEqual(evidence.flash_size, "8MB")
        self.assertEqual(evidence.psram_size, "8MB")
        self.assertIs(self.runtime.command_loaders[0][1], session._loader)
        self.assertIs(session._loader._port, session._transport)
        self.assertEqual(self.raw.open_calls, 1)
        session.close()

    def test_probe_rejects_non_s3_target(self) -> None:
        _FakeRom.CHIP_NAME = "ESP32-C3"
        session = self.open_session()
        with self.assertRaises(bound_rom.BoundRomIdentityError):
            session.probe()
        with self.assertRaises(bound_rom.BoundRomStateError):
            session.probe()
        session.close()

    def test_probe_rejects_non_8mb_flash(self) -> None:
        _FakeRom.flash_id_value = 0x1640EF
        session = self.open_session()
        with self.assertRaises(bound_rom.BoundRomIdentityError):
            session.probe()
        session.close()

    def test_probe_rejects_missing_or_wrong_embedded_psram(self) -> None:
        _FakeRom.features = [
            "WiFi", "BLE", "Embedded PSRAM 4MB (AP_3v3)"
        ]
        session = self.open_session()
        with self.assertRaises(bound_rom.BoundRomIdentityError):
            session.probe()
        session.close()

    def test_probe_rejects_malformed_revision_evidence(self) -> None:
        _FakeRom.description = "ESP32-S3 (QFN56)"
        session = self.open_session()
        with self.assertRaises(bound_rom.BoundRomIdentityError):
            session.probe()
        session.close()

    def test_write_verify_run_use_exact_layout_fresh_views_and_one_handle(
        self,
    ) -> None:
        partition_table = _valid_partition_table()
        bootloader = _fake_bootloader()
        frozen = _frozen_uplink_artifacts(
            bootloader=bootloader,
            partitions=partition_table,
            ota_data=b"OTA",
            firmware=b"APP",
        )
        session = self.open_session()
        session.probe()
        with mock.patch.object(
            bound_rom, "_validate_frozen_inputs", return_value=None
        ):
            written = session.write_layout(frozen)
            verified = session.verify_layout(frozen)
        ran = session.run_application()

        expected = (
            (0x00000, "<frozen:artifact.bootloader>", bootloader),
            (
                0x08000,
                "<frozen:artifact.partitions>",
                partition_table,
            ),
            (0x0F000, "<frozen:artifact.ota_data_initial>", b"OTA"),
            (0x20000, "<frozen:artifact.firmware>", b"APP"),
        )
        write_event = next(item for item in self.events
                           if isinstance(item, tuple)
                           and item[0] == "write-layout")
        verify_event = next(item for item in self.events
                            if isinstance(item, tuple)
                            and item[0] == "verify-layout")
        self.assertEqual(write_event[1], expected)
        self.assertEqual(verify_event[1], expected)
        self.assertEqual(
            write_event[2],
            {
                "addr_filename": mock.ANY,
                "compress": True,
                "no_compress": False,
                "no_stub": False,
                "force": False,
                "chip": "esp32s3",
                "flash_size": "keep",
                "flash_mode": "keep",
                "flash_freq": "keep",
                "erase_all": False,
                "encrypt": False,
                "encrypt_files": None,
                "ignore_flash_encryption_efuse_setting": False,
            },
        )
        self.assertEqual(
            verify_event[2],
            {
                "addr_filename": mock.ANY,
                "diff": "no",
                "chip": "esp32s3",
                "flash_size": "keep",
                "flash_mode": "keep",
                "flash_freq": "keep",
            },
        )
        self.assertTrue(all(
            first is not second
            for first, second in zip(
                self.runtime.write_views, self.runtime.verify_views
            )
        ))
        loaders = [loader for _, loader in self.runtime.command_loaders]
        self.assertTrue(all(loader._port is session._transport
                            for loader in loaders))
        self.assertEqual(self.raw.open_calls, 1)
        self.assertEqual(written.base_mac, MAC)
        self.assertEqual(verified.aggregate_sha256, frozen.aggregate_sha256)
        self.assertEqual(ran.operation, "run")
        self.assertLess(
            session._loader.events.index(
                ("flash-set-parameters", 8 * 1024 * 1024)
            ),
            session._loader.events.index("write-command"),
        )
        clear = (
            "write-reg",
            0x6000812C,
            0,
            1,
            0,
        )
        self.assertLess(
            session._loader.events.index(clear),
            session._loader.events.index("watchdog-reset"),
        )
        session.close()

    def test_write_rejects_wrong_type_layout_and_tampered_aggregate(self) -> None:
        session = self.open_session()
        session.probe()
        with self.assertRaises(bound_rom.BoundRomArtifactError):
            session.write_layout(object())  # type: ignore[arg-type]

        frozen = _frozen_uplink_artifacts(
            bootloader=_fake_bootloader(),
            partitions=_valid_partition_table(),
            ota_data=b"O",
            firmware=b"F",
        )
        object.__setattr__(frozen, "aggregate_sha256", "0" * 64)
        with self.assertRaises(bound_rom.BoundRomArtifactError):
            session.write_layout(frozen)

        missing = _frozen_uplink_artifacts(
            bootloader=_fake_bootloader(),
            partitions=_valid_partition_table(),
            ota_data=b"O",
            firmware=b"F",
        )
        object.__setattr__(
            missing,
            "members",
            tuple(
                member for member in missing.members
                if member.logical_name != "artifact.ota_data_initial"
            ),
        )
        object.__setattr__(
            missing,
            "aggregate_sha256",
            secure._aggregate_sha256(
                missing.receipt_sha256, missing.members
            ),
        )
        with mock.patch.object(
            bound_rom, "_validate_frozen_inputs", return_value=None
        ), self.assertRaises(bound_rom.BoundRomArtifactError):
            session.write_layout(missing)
        self.assertFalse(any(name == "write" for name, _loader
                             in self.runtime.command_loaders))
        session.close()

    def test_each_required_flash_region_must_be_nonempty(self) -> None:
        names = (
            "bootloader",
            "partitions",
            "ota_data",
            "firmware",
        )
        for empty_name in names:
            values = {
                "bootloader": _fake_bootloader(),
                "partitions": _valid_partition_table(),
                "ota_data": b"O",
                "firmware": b"F",
            }
            values[empty_name] = b""
            frozen = _frozen_uplink_artifacts(**values)
            with self.subTest(empty_name=empty_name), mock.patch.object(
                bound_rom, "_validate_frozen_inputs", return_value=None
            ), self.assertRaises(bound_rom.BoundRomArtifactError):
                bound_rom._validate_artifact_set(frozen)

    def test_required_flash_regions_cannot_overwrite_live_partitions(
        self,
    ) -> None:
        valid = {
            "bootloader": _fake_bootloader(),
            "partitions": _valid_partition_table(),
            "ota_data": b"O",
            "firmware": b"F",
        }
        oversized = {
            "bootloader": _fake_bootloader()
            + b"B" * (0x8001 - len(_fake_bootloader())),
            "partitions": _valid_partition_table(total_size=0x1001),
            "ota_data": b"O" * 0x2001,
            "firmware": b"F" * 0x200001,
        }
        for region, content in oversized.items():
            values = dict(valid)
            values[region] = content
            frozen = _frozen_uplink_artifacts(**values)
            with self.subTest(region=region), mock.patch.object(
                bound_rom, "_validate_frozen_inputs", return_value=None
            ), self.assertRaises(bound_rom.BoundRomArtifactError):
                bound_rom._validate_artifact_set(frozen)

    def test_bootloader_encoding_is_validated_before_stub_upload(self) -> None:
        session = self.open_session()
        session.probe()
        cases = (
            b"\x00\x03\x02\x3f" + b"\0" * 24,
            b"\xe9\x03\x00\x3f" + b"\0" * 24,
            b"\xe9\x03\x02\x20" + b"\0" * 24,
        )
        for bootloader in cases:
            frozen = _frozen_uplink_artifacts(
                bootloader=bootloader,
                partitions=_valid_partition_table(),
                ota_data=b"O",
                firmware=b"F",
            )
            with self.subTest(
                header=bootloader[:4]
            ), mock.patch.object(
                bound_rom, "_validate_frozen_inputs", return_value=None
            ), self.assertRaises(bound_rom.BoundRomArtifactError):
                session.write_layout(frozen)
        self.assertNotIn(("run-stub", None), session._loader.events)
        session.close()

    def test_artifact_commands_do_not_read_paths_after_freeze(self) -> None:
        frozen = _frozen_uplink_artifacts(
            bootloader=_fake_bootloader(),
            partitions=_valid_partition_table(),
            ota_data=b"O",
            firmware=b"F",
        )
        session = self.open_session()
        session.probe()
        with mock.patch.object(
            bound_rom, "_validate_frozen_inputs", return_value=None
        ), mock.patch.object(
            Path, "read_bytes", side_effect=AssertionError("filesystem read")
        ), mock.patch.object(
            builtins, "open", side_effect=AssertionError("filesystem open")
        ), mock.patch.object(
            os, "open", side_effect=AssertionError("filesystem os.open")
        ):
            session.write_layout(frozen)
            session.verify_layout(frozen)
        session.close()

    def test_transport_loss_after_mutation_is_uncertain_and_never_reopens(
        self,
    ) -> None:
        frozen = _frozen_uplink_artifacts(
            bootloader=_fake_bootloader(),
            partitions=_valid_partition_table(),
            ota_data=b"O",
            firmware=b"F",
        )
        session = self.open_session()
        session.probe()
        self.runtime.write_action = lambda loader: loader._port.close()
        with mock.patch.object(
            bound_rom, "_validate_frozen_inputs", return_value=None
        ), self.assertRaises(bound_rom.BoundRomMutationUncertainError):
            session.write_layout(frozen)
        self.assertEqual(self.raw.open_calls, 1)
        self.assertEqual(self.raw.close_calls, 0)
        with self.assertRaises(bound_rom.BoundRomMutationUncertainError):
            session.probe()
        session.close()
        self.assertEqual(self.raw.close_calls, 1)

    def test_post_write_descriptor_drift_is_explicitly_uncertain(self) -> None:
        frozen = _frozen_uplink_artifacts(
            bootloader=_fake_bootloader(),
            partitions=_valid_partition_table(),
            ota_data=b"O",
            firmware=b"F",
        )
        session = self.open_session()
        session.probe()
        self.runtime.write_action = lambda _loader: setattr(
            self, "drift", True
        )
        with mock.patch.object(
            bound_rom, "_validate_frozen_inputs", return_value=None
        ), self.assertRaises(bound_rom.BoundRomMutationUncertainError):
            session.write_layout(frozen)
        self.assertEqual(self.raw.open_calls, 1)
        session.close()
        self.assertEqual(self.raw.close_calls, 1)

    def test_post_mutation_provenance_audit_failure_is_uncertain(self) -> None:
        frozen = _frozen_uplink_artifacts(
            bootloader=_fake_bootloader(),
            partitions=_valid_partition_table(),
            ota_data=b"O",
            firmware=b"F",
        )
        session = self.open_session()
        session.probe()

        def arm_failed_audit(_loader: object) -> None:
            def fail_audit(_subject: object) -> None:
                raise _FakeProvenanceError("runtime origin changed")

            self.runtime.audit_action = fail_audit

        self.runtime.write_action = arm_failed_audit
        with mock.patch.object(
            bound_rom, "_validate_frozen_inputs", return_value=None
        ), self.assertRaises(bound_rom.BoundRomMutationUncertainError):
            session.write_layout(frozen)
        with self.assertRaises(
            bound_rom.BoundRomMutationUncertainError
        ):
            session.verify_layout(frozen)
        self.assertEqual(self.raw.open_calls, 1)
        session.close()

    def test_post_write_evidence_baseexceptions_are_always_uncertain(
        self,
    ) -> None:
        frozen = _frozen_uplink_artifacts(
            bootloader=_fake_bootloader(),
            partitions=_valid_partition_table(),
            ota_data=b"O",
            firmware=b"F",
        )
        for failure in (
            KeyboardInterrupt("evidence interrupt"),
            SystemExit("evidence exit"),
            MemoryError("evidence allocation"),
        ):
            with self.subTest(failure=type(failure).__name__):
                session = self.open_session()
                session.probe()
                with mock.patch.object(
                    bound_rom, "_validate_frozen_inputs", return_value=None
                ), mock.patch.object(
                    bound_rom.BoundRomSession,
                    "_operation_evidence",
                    side_effect=failure,
                ), self.assertRaises(
                    bound_rom.BoundRomMutationUncertainError
                ):
                    session.write_layout(frozen)
                self.assertTrue(session._mutation_started)
                self.assertIsNotNone(session._uncertain)
                self.assertIn(
                    ("write", session._loader),
                    self.runtime.command_loaders,
                )
                with self.assertRaises(
                    bound_rom.BoundRomMutationUncertainError
                ):
                    session.verify_layout(frozen)
                session.close()

    def test_partial_stub_activation_poisons_session_without_reopen(
        self,
    ) -> None:
        frozen = _frozen_uplink_artifacts(
            bootloader=_fake_bootloader(),
            partitions=_valid_partition_table(),
            ota_data=b"O",
            firmware=b"F",
        )
        session = self.open_session()
        session.probe()

        def fail_stub_audit(subject: object) -> None:
            if type(subject) is _FakeStub:
                raise _FakeProvenanceError("stub audit failed")

        self.runtime.audit_action = fail_stub_audit
        with mock.patch.object(
            bound_rom, "_validate_frozen_inputs", return_value=None
        ), self.assertRaises(bound_rom.BoundRomOperationError):
            session.write_layout(frozen)
        with self.assertRaises(bound_rom.BoundRomStateError):
            session.write_layout(frozen)
        self.assertFalse(any(
            name == "write" for name, _loader
            in self.runtime.command_loaders
        ))
        self.assertEqual(self.raw.open_calls, 1)
        session.close()

    def test_flash_parameter_setup_failure_poisons_without_write(
        self,
    ) -> None:
        frozen = _frozen_uplink_artifacts(
            bootloader=_fake_bootloader(),
            partitions=_valid_partition_table(),
            ota_data=b"O",
            firmware=b"F",
        )
        session = self.open_session()
        session.probe()

        def fail_setup(_loader: object, size: int) -> None:
            self.assertEqual(size, 8 * 1024 * 1024)
            raise _FakeFatalError("flash parameter setup failed")

        _FakeRom.flash_setup_action = fail_setup
        with mock.patch.object(
            bound_rom, "_validate_frozen_inputs", return_value=None
        ), self.assertRaises(bound_rom.BoundRomOperationError):
            session.write_layout(frozen)
        with self.assertRaises(bound_rom.BoundRomStateError):
            session.write_layout(frozen)
        self.assertFalse(any(
            name == "write" for name, _loader
            in self.runtime.command_loaders
        ))
        session.close()

    def test_watchdog_exception_is_uncertain_and_never_reported_as_run(
        self,
    ) -> None:
        frozen = _frozen_uplink_artifacts(
            bootloader=_fake_bootloader(),
            partitions=_valid_partition_table(),
            ota_data=b"O",
            firmware=b"F",
        )
        session = self.open_session()
        session.probe()
        with mock.patch.object(
            bound_rom, "_validate_frozen_inputs", return_value=None
        ):
            session.write_layout(frozen)
            session.verify_layout(frozen)

        def lose_transport(_loader: object) -> None:
            raise _SerialFailure("watchdog reset detached USB")

        _FakeRom.watchdog_action = lose_transport
        with self.assertRaises(
            bound_rom.BoundRomMutationUncertainError
        ):
            session.run_application()
        self.assertFalse(any(
            evidence.operation == "run" for evidence in session.transcript
        ))
        self.assertEqual(self.raw.open_calls, 1)
        self.assertEqual(self.raw.close_calls, 0)
        with self.assertRaises(
            bound_rom.BoundRomMutationUncertainError
        ):
            session.probe()
        session.close()
        self.assertEqual(self.raw.close_calls, 1)

    def test_every_verify_boundary_baseexception_is_uncertain(self) -> None:
        frozen = _frozen_uplink_artifacts(
            bootloader=_fake_bootloader(),
            partitions=_valid_partition_table(),
            ota_data=b"O",
            firmware=b"F",
        )
        phases = (
            ("authorization", "_authorize"),
            ("post-verify evidence", "_operation_evidence"),
            ("frozen-view cleanup", "_close_views"),
        )
        failures = (
            KeyboardInterrupt("verify interrupt"),
            SystemExit("verify exit"),
            MemoryError("verify allocation"),
        )
        for phase, method_name in phases:
            for failure in failures:
                with self.subTest(
                    phase=phase,
                    failure=type(failure).__name__,
                ):
                    session = self.open_session()
                    try:
                        session.probe()
                        with mock.patch.object(
                            bound_rom,
                            "_validate_frozen_inputs",
                            return_value=None,
                        ):
                            session.write_layout(frozen)
                            observed: BaseException | None = None
                            try:
                                with mock.patch.object(
                                    bound_rom.BoundRomSession,
                                    method_name,
                                    side_effect=failure,
                                ):
                                    session.verify_layout(frozen)
                            except BaseException as exc:
                                observed = exc
                        self.assertIsInstance(
                            observed,
                            bound_rom.BoundRomMutationUncertainError,
                        )
                        self.assertIsNotNone(session._uncertain)
                    finally:
                        session.close()

    def test_every_run_boundary_baseexception_is_uncertain(self) -> None:
        frozen = _frozen_uplink_artifacts(
            bootloader=_fake_bootloader(),
            partitions=_valid_partition_table(),
            ota_data=b"O",
            firmware=b"F",
        )
        phases = (
            ("authorization", "_authorize"),
            ("run evidence", "_operation_evidence"),
        )
        failures = (
            KeyboardInterrupt("run interrupt"),
            SystemExit("run exit"),
            MemoryError("run allocation"),
        )
        for phase, method_name in phases:
            for failure in failures:
                with self.subTest(
                    phase=phase,
                    failure=type(failure).__name__,
                ):
                    session = self.open_session()
                    try:
                        session.probe()
                        with mock.patch.object(
                            bound_rom,
                            "_validate_frozen_inputs",
                            return_value=None,
                        ):
                            session.write_layout(frozen)
                            session.verify_layout(frozen)
                        observed: BaseException | None = None
                        try:
                            with mock.patch.object(
                                bound_rom.BoundRomSession,
                                method_name,
                                side_effect=failure,
                            ):
                                session.run_application()
                        except BaseException as exc:
                            observed = exc
                        self.assertIsInstance(
                            observed,
                            bound_rom.BoundRomMutationUncertainError,
                        )
                        self.assertIsNotNone(session._uncertain)
                    finally:
                        session.close()

    def test_run_requires_successful_same_artifact_verify(self) -> None:
        session = self.open_session()
        with self.assertRaises(bound_rom.BoundRomStateError):
            session.run_application()
        session.probe()
        with self.assertRaises(bound_rom.BoundRomStateError):
            session.run_application()
        session.close()


if __name__ == "__main__":
    unittest.main()
