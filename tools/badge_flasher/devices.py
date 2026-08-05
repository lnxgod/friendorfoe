"""Fail-closed ESP32-S3 discovery with MAC-stable USB rebinding."""

from __future__ import annotations

import glob
import re
import subprocess
import sys
import time
from collections.abc import Callable, Iterable
from pathlib import Path

from .models import UsbDevice
from .topology import normalize_mac


PORT_PATTERNS = (
    "/dev/cu.usbmodem*",
    "/dev/cu.usbserial*",
    "/dev/cu.wchusbserial*",
    "/dev/cu.SLAB*",
)


class DeviceError(RuntimeError):
    """Connected hardware cannot be proven safe for factory flashing."""


class ProbeUnavailable(DeviceError):
    """A candidate serial port did not answer as an ESP32 ROM."""


def usb_jtag_app_reset(port: str, settle_s: float = 1.5) -> None:
    """Issue the native USB-Serial/JTAG chip reset proven on XIAO S3.

    ``esptool run`` can leave a USB-JTAG target in the ROM/stub after a
    ``flash_id`` rebind. Opening the native console and releasing DTR/RTS
    produces ``USB_UART_CHIP_RESET`` and boots the selected OTA app.
    """
    try:
        import serial  # type: ignore
    except ImportError as exc:
        raise DeviceError("pyserial is required for USB-JTAG reset") from exc
    handle = serial.Serial(port, 115200, timeout=0.05, write_timeout=1)
    try:
        handle.dtr = False
        handle.rts = False
        time.sleep(settle_s)
    finally:
        handle.close()


def _match(pattern: str, text: str, label: str) -> str:
    found = re.search(pattern, text, re.IGNORECASE | re.MULTILINE)
    if not found:
        raise DeviceError(f"esptool output is missing {label}")
    return found.group(1)


def parse_esptool_probe(
    output: str, port: str, location_id: str | None = None
) -> UsbDevice:
    """Parse the bounded identity emitted by ``esptool flash_id``."""
    chip = _match(
        r"^\s*Chip (?:is|type:)\s+(ESP32-[A-Za-z0-9-]+)(?=\s|\()",
        output,
        "chip identity",
    ).upper()
    if chip != "ESP32-S3":
        raise DeviceError(f"unsupported chip on {port}: {chip}; ESP32-S3 required")
    revision = _match(r"revision\s+(v[0-9.]+)", output, "chip revision")
    mac = normalize_mac(_match(r"^MAC:\s*([0-9a-f:.-]+)", output, "base MAC"))
    flash_size = _match(r"Detected flash size:\s*([0-9]+(?:MB|KB))", output, "flash size").upper()
    psram = re.search(r"(?:Embedded\s+)?PSRAM\s+([0-9]+(?:MB|KB))", output, re.IGNORECASE)
    if not psram:
        raise DeviceError(f"ESP32-S3 on {port} does not report required PSRAM")
    return UsbDevice(
        mac=mac,
        port=port,
        chip=chip,
        revision=revision,
        flash_size=flash_size,
        psram_size=psram.group(1).upper(),
        location_id=location_id,
    )


def _default_runner(command: list[str]) -> str:
    completed = subprocess.run(
        command,
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=30,
    )
    if completed.returncode:
        raise ProbeUnavailable(
            f"esptool failed ({completed.returncode}): {completed.stdout.strip()}"
        )
    return completed.stdout


def _platformio_python() -> str:
    candidate = Path.home() / ".platformio/penv/bin/python"
    return str(candidate) if candidate.exists() else sys.executable


class DeviceBackend:
    """Injected hardware boundary used by discovery and post-reset rebinding."""

    def __init__(
        self,
        *,
        globber: Callable[[str], Iterable[str]] = glob.glob,
        command_runner: Callable[[list[str]], str] = _default_runner,
        location_resolver: Callable[[str], str | None] | None = None,
        clock: Callable[[], float] = time.monotonic,
        sleeper: Callable[[float], None] = time.sleep,
    ) -> None:
        self._globber = globber
        self._run = command_runner
        self._location = location_resolver or (lambda _port: None)
        self._clock = clock
        self._sleep = sleeper

    def list_candidate_ports(self) -> list[str]:
        return sorted({port for pattern in PORT_PATTERNS for port in self._globber(pattern)})

    def probe_rom(self, port: str) -> UsbDevice:
        command = [
            _platformio_python(),
            "-m",
            "esptool",
            "--chip",
            "esp32s3",
            "--port",
            port,
            "--before",
            "usb_reset",
            "--after",
            "no_reset",
            "flash_id",
        ]
        return parse_esptool_probe(self._run(command), port, self._location(port))

    def scan(self) -> dict[str, UsbDevice]:
        devices: dict[str, UsbDevice] = {}
        for port in self.list_candidate_ports():
            try:
                device = self.probe_rom(port)
            except (ProbeUnavailable, OSError, subprocess.SubprocessError):
                continue
            if device.mac in devices:
                raise DeviceError(f"duplicate ESP32 MAC reported: {device.mac}")
            devices[device.mac] = device
        return devices

    def rebind(self, expected_macs: set[str], timeout_s: float = 30) -> dict[str, UsbDevice]:
        expected = {normalize_mac(mac) for mac in expected_macs}
        deadline = self._clock() + max(timeout_s, 0)
        while True:
            found = self.scan()
            extras = set(found) - expected
            if extras:
                raise DeviceError(
                    "unexpected ESP32-S3 connected: " + ", ".join(sorted(extras))
                )
            if set(found) == expected:
                return found
            if self._clock() >= deadline:
                missing = expected - set(found)
                raise DeviceError(
                    "timed out rebinding ESP32 MACs: " + ", ".join(sorted(missing))
                )
            self._sleep(0.25)
