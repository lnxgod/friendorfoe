"""Manifest-driven esptool production writer and readback verifier."""

from __future__ import annotations

import re
import subprocess
import sys
from collections.abc import Callable
from pathlib import Path
from typing import Any

from .bundles import FactoryBundle
from .models import FlashEvidence, UsbDevice
from .topology import normalize_mac


class FlashError(RuntimeError):
    """Production write/readback evidence did not meet the release gate."""


_FORCE_DOWNLOAD_CLEAR = (
    "Wrote 00000000, mask 00000001 to 6000812c"
)
_WATCHDOG_RESET = "Hard resetting with a watchdog..."


def _role_alias(role: str) -> str:
    return "UPLINK" if role == "uplink" else "BADGE"


def _default_runner(command: list[str]) -> str:
    completed = subprocess.run(
        command,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        timeout=180,
    )
    if completed.returncode:
        raise FlashError(
            f"esptool failed ({completed.returncode}): {completed.stdout.strip()}"
        )
    return completed.stdout


def _python() -> str:
    pio_python = Path.home() / ".platformio/penv/bin/python"
    return str(pio_python) if pio_python.exists() else sys.executable


def _reported_mac(output: str) -> str:
    matches = re.findall(r"^MAC:\s*([0-9A-Fa-f:.-]+)", output, re.MULTILINE)
    if not matches:
        raise FlashError("esptool output did not prove the target MAC")
    return normalize_mac(matches[-1])


def _verify_handoff_receipt(output: str, expected_mac: str) -> None:
    if _reported_mac(output) != normalize_mac(expected_mac):
        raise FlashError("BADGE application handoff reached a different ESP32")
    lines = [line.strip() for line in output.splitlines() if line.strip()]
    if lines.count(_FORCE_DOWNLOAD_CLEAR) != 1:
        raise FlashError(
            "BADGE application handoff did not prove force-download clear"
        )
    if lines.count(_WATCHDOG_RESET) != 1 or lines[-1] != _WATCHDOG_RESET:
        raise FlashError(
            "BADGE application handoff did not prove watchdog reset"
        )
    if lines.index(_FORCE_DOWNLOAD_CLEAR) >= lines.index(_WATCHDOG_RESET):
        raise FlashError("BADGE application handoff receipts are out of order")


class FlashEngine:
    def __init__(self, runner: Callable[[list[str]], str] = _default_runner) -> None:
        self._run = runner

    def _parts(self, bundle: FactoryBundle, role: str) -> list[str]:
        result: list[str] = []
        for part in sorted(bundle.layout(role)["parts"], key=lambda item: item["offset"]):
            result.extend([hex(part["offset"]), str(bundle.root / part["path"])])
        return result

    def handoff_to_application(self, device: UsbDevice) -> None:
        output = self._run([
            _python(), "-m", "esptool", "--chip", "esp32s3",
            "--port", device.port, "--baud", "115200",
            "--before", "no_reset", "--after", "watchdog_reset",
            "--no-stub", "--connect-attempts", "1",
            "write_mem", "0x6000812c", "0x0", "0x1",
        ])
        _verify_handoff_receipt(output, device.mac)

    def reset_device(self, device: UsbDevice) -> None:
        output = self._run([
            _python(), "-m", "esptool", "--chip", "esp32s3",
            "--port", device.port, "--before", "no_reset",
            "--after", "hard_reset", "run",
        ])
        if _reported_mac(output) != device.mac:
            raise FlashError("BADGE final reset reached a different ESP32")

    def prove_device(self, device: UsbDevice) -> None:
        """Prove the port's immutable MAC immediately before any erase/write."""
        output = self._run([
            _python(), "-m", "esptool", "--chip", "esp32s3",
            "--port", device.port, "--before", "usb_reset",
            "--after", "no_reset", "flash_id",
        ])
        if _reported_mac(output) != device.mac:
            raise FlashError(
                "BADGE pre-write identity proof reached a different ESP32"
            )

    def flash_and_verify(
        self, device: UsbDevice, bundle: FactoryBundle, role: str
    ) -> FlashEvidence:
        layout = bundle.layout(role)
        self.prove_device(device)
        common = [
            _python(), "-m", "esptool", "--chip", "esp32s3",
            "--port", device.port,
        ]
        parts = self._parts(bundle, role)
        write = common + [
            # prove_device left this exact MAC in ROM; do not reset or allow
            # the macOS port binding to change between proof and first erase.
            "--before", "no_reset", "--after", "no_reset",
            "write_flash", "--erase-all", "--flash_size", layout["flash_size"],
            "--flash_mode", "dio", "--flash_freq", "80m", "--verify",
        ] + parts
        write_output = self._run(write)
        if _reported_mac(write_output) != device.mac:
            raise FlashError(
                f"{_role_alias(role)}: esptool wrote a different ESP32"
            )
        if "Hash of data verified" not in write_output:
            raise FlashError(
                f"{_role_alias(role)}: write verification evidence missing"
            )

        readback = common + [
            "--before", "no_reset", "--after", "no_reset",
            "verify_flash", "--flash_size", layout["flash_size"],
        ] + parts
        readback_output = self._run(readback)
        if _reported_mac(readback_output) != device.mac:
            raise FlashError(
                f"{_role_alias(role)}: readback came from a different ESP32"
            )
        verified_regions = len(re.findall(
            r"--\s+verify OK\s+\(digest matched\)",
            readback_output,
            re.IGNORECASE,
        ))
        if verified_regions != len(layout["parts"]):
            raise FlashError(
                f"{_role_alias(role)}: explicit flash readback evidence missing "
                f"({verified_regions}/{len(layout['parts'])} regions)"
            )

        self.reset_device(device)
        return FlashEvidence(
            mac=device.mac,
            role=role,
            port=device.port,
            version=bundle.version if role != "probe" else str(layout["identity"]["version"]),
            write_verified=True,
            readback_verified=True,
        )
