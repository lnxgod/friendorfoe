"""USB orchestration for a nonce-bound three-board topology probe."""

from __future__ import annotations

import secrets
import time
import re
from collections.abc import Callable, Iterable
from typing import Any

from .models import TopologyAssignment, UsbDevice
from .topology import classify_topology, parse_probe_report


class ProbeSessionError(RuntimeError):
    pass


_READY_RE = re.compile(r"^FOF_FACTORY_READY:([0-9A-Fa-f:.-]+):1$")


def rebind_probe_ports(
    ports: Iterable[str], expected_macs: set[str], *, timeout_s: float = 8,
    serial_factory: Callable[[str], Any] | None = None,
) -> dict[str, UsbDevice]:
    """Map post-reset probe ports without entering ROM download mode."""
    if serial_factory is None:
        serial_factory = _serial_factory
    expected = {mac.upper() for mac in expected_macs}
    handles: list[tuple[str, Any]] = []
    found: dict[str, UsbDevice] = {}
    try:
        for port in ports:
            handle = serial_factory(port)
            handle.reset_input_buffer()
            handles.append((port, handle))
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            for port, handle in handles:
                handle.write(b"FOF_PROBE_ID\n")
                line = handle.readline().decode("utf-8", "replace").strip()
                match = _READY_RE.fullmatch(line)
                if not match:
                    continue
                mac = match.group(1).upper()
                if mac not in expected:
                    raise ProbeSessionError(f"unexpected probe MAC on {port}: {mac}")
                found[mac] = UsbDevice(mac, port, "ESP32-S3", "probe", "8MB", "8MB")
            if set(found) == expected:
                return found
            time.sleep(0.05)
        raise ProbeSessionError(
            "could not rebind all probe MACs: " + ", ".join(sorted(expected - set(found)))
        )
    finally:
        for _, handle in handles:
            handle.close()


def _serial_factory(port: str) -> Any:
    try:
        import serial  # type: ignore
    except ImportError as exc:
        raise ProbeSessionError("pyserial is required; run with PlatformIO Python") from exc
    handle = serial.Serial(port, 115200, timeout=0.05, write_timeout=1)
    handle.dtr = False
    handle.rts = False
    return handle


def discover_topology(
    devices: Iterable[UsbDevice], *, timeout_s: float = 8,
    serial_factory: Callable[[str], Any] = _serial_factory,
) -> TopologyAssignment:
    items = list(devices)
    if len(items) != 3:
        raise ProbeSessionError(f"exactly three probe devices required; got {len(items)}")
    session = secrets.token_hex(16)
    handles: list[tuple[UsbDevice, Any]] = []
    try:
        for device in items:
            handle = serial_factory(device.port)
            handle.reset_input_buffer()
            handles.append((device, handle))
        time.sleep(0.4)
        command = f"FOF_PROBE_SESSION:{session}\n".encode("ascii")
        for _, handle in handles:
            handle.write(command)
        reports = {}
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            for _, handle in handles:
                line = handle.readline().decode("utf-8", "replace").strip()
                if line.startswith("FOF_FACTORY_PROBE:"):
                    report = parse_probe_report(line, expected_session=session)
                    reports[report.mac] = report
            if len(reports) == 3:
                try:
                    return classify_topology(reports.values())
                except ValueError:
                    pass
            time.sleep(0.01)
        raise ProbeSessionError(
            f"topology proof timed out with {len(reports)}/3 valid reports"
        )
    finally:
        for _, handle in handles:
            handle.close()
