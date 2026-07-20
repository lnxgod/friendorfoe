#!/usr/bin/env python3
"""Badge-only one-command flasher for a FoF badge trio.

Targets one physical badge assembly:
  - uplink: XIAO ESP32-S3 running uplink-s3-fof_badge
  - ble scanner: XIAO ESP32-S3 running scanner-s3-combo-fof_badge
  - wifi scanner: XIAO ESP32-S3 running scanner-s3-combo-fof_badge
"""

from __future__ import annotations

import argparse
import binascii
import glob
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path
from typing import Any
from urllib.error import HTTPError, URLError
from urllib.parse import urlencode
from urllib.request import Request, urlopen


REPO_ROOT = Path(__file__).resolve().parent.parent
ESP32_DIR = REPO_ROOT / "esp32"
SCANNER_DIR = ESP32_DIR / "scanner"
UPLINK_DIR = ESP32_DIR / "uplink"
ESP32_SCRIPTS_DIR = ESP32_DIR / "scripts"
if str(ESP32_SCRIPTS_DIR) not in sys.path:
    sys.path.insert(0, str(ESP32_SCRIPTS_DIR))

from firmware_version import parse_firmware_identity

DEFAULT_BACKEND = os.environ.get("FOF_BACKEND", "http://localhost:8000")
SCANNER_RELAY_TIMEOUT_MIN_S = 240
SCANNER_RELAY_TIMEOUT_MAX_S = 900
SCANNER_RELAY_TIMEOUT_PER_KB_S = 0.30

PLATFORMS: dict[str, dict[str, Any]] = {
    "badge-trio-xiao-s3": {
        "hardware": "FoF Badge trio on Seeed XIAO ESP32-S3",
        "uplink_env": "uplink-s3-fof_badge",
        "uplink_name": "uplink-s3-fof_badge",
        "uplink_project": "fof_badge_uplink",
        "uplink_bin": UPLINK_DIR / ".pio/build/uplink-s3-fof_badge/firmware.bin",
        "scanner_env": "scanner-s3-combo-fof_badge",
        "scanner_name": "scanner-s3-combo-fof_badge",
        "scanner_project": "fof_badge_scanner",
        "scanner_bin": SCANNER_DIR / ".pio/build/scanner-s3-combo-fof_badge/firmware.bin",
        "hardware_type": "seeed_xiao_esp32s3",
        "slots": ("ble", "wifi"),
    },
}


class FlashError(RuntimeError):
    pass


def require_usb_firmware_transport(transport: str) -> None:
    if transport != "usb":
        raise FlashError(
            "badge firmware transport is USB to the uplink plus UART to the "
            "scanners; HTTP/AP/LAN firmware mutation is disabled"
        )


def log(msg: str) -> None:
    print(msg, flush=True)


def scanner_relay_timeout_s(size: int) -> int:
    """Conservative wall-clock timeout for one scanner UART relay."""
    estimate = int((max(size, 0) / 1024.0) * SCANNER_RELAY_TIMEOUT_PER_KB_S)
    return max(SCANNER_RELAY_TIMEOUT_MIN_S,
               min(SCANNER_RELAY_TIMEOUT_MAX_S, estimate))


def format_relay_progress(progress: dict[str, Any]) -> str:
    slot = progress.get("uart") or progress.get("slot") or "scanner"
    stage = progress.get("stage") or "relay"
    pct = progress.get("percent")
    if pct is None:
        size = progress.get("size") or progress.get("total") or 0
        got = progress.get("bytes") or progress.get("received") or 0
        pct = int((int(got) * 100) / int(size)) if size else 0
    details = [
        f"[relay] {slot} {stage} {pct}%",
        f"{progress.get('bytes', progress.get('received', 0))}/{progress.get('size', progress.get('total', 0))}",
        f"chunks={progress.get('chunks', 0)}",
        f"nacks={progress.get('nacks', 0)}",
        f"retries={progress.get('retries', 0)}",
        f"elapsed={progress.get('elapsed_s', 0)}s",
    ]
    error = progress.get("error")
    if error:
        details.append(f"error={error}")
    return " ".join(str(part) for part in details)


def find_pio() -> str:
    candidates = [
        os.environ.get("PIO"),
        shutil.which("pio"),
        str(ESP32_DIR / ".venv312/bin/pio"),
        str(Path.home() / ".platformio/penv/bin/pio"),
    ]
    for candidate in candidates:
        if candidate and Path(candidate).exists():
            return candidate
    raise FlashError("PlatformIO not found; set PIO or install PlatformIO")


def find_platformio_python() -> str:
    candidates = [
        os.environ.get("PIO_PYTHON"),
        str(Path.home() / ".platformio/penv/bin/python"),
        sys.executable,
    ]
    for candidate in candidates:
        if candidate and Path(candidate).exists():
            return candidate
    return sys.executable


def repo_version() -> str:
    version_h = ESP32_DIR / "shared/version.h"
    text = version_h.read_text(encoding="utf-8")
    match = re.search(r'#define\s+FOF_VERSION_BADGE\s+"([^"]+)"', text)
    if match:
        return match.group(1)
    match = re.search(r'#define\s+FOF_VERSION\s+"([^"]+)"',
                      text)
    return match.group(1) if match else "unknown"


def norm_version(value: str | None) -> str:
    value = (value or "").strip()
    return value[1:] if value[:1].lower() == "v" else value


def versions_match(got: str | None, wanted: str | None) -> bool:
    got_norm = norm_version(got)
    wanted_norm = norm_version(wanted)
    return bool(got_norm) and got_norm == wanted_norm


_ORDERED_VERSION_RE = re.compile(
    r"^(\d+)\.(\d+)\.(\d+)(?:-([0-9A-Za-z][0-9A-Za-z._-]*))?$"
)
_HARDWARE_ID_RE = re.compile(r"^(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}$")


def firmware_version_relation(candidate: str | None,
                              current: str | None) -> str:
    """Order numeric cores and fail closed on ambiguous named variants."""
    candidate_norm = norm_version(candidate)
    current_norm = norm_version(current)
    candidate_match = _ORDERED_VERSION_RE.fullmatch(candidate_norm)
    current_match = _ORDERED_VERSION_RE.fullmatch(current_norm)
    if not candidate_match or not current_match:
        return "invalid"
    candidate_core = tuple(int(candidate_match.group(i)) for i in range(1, 4))
    current_core = tuple(int(current_match.group(i)) for i in range(1, 4))
    if candidate_core < current_core:
        return "older"
    if candidate_core > current_core:
        return "newer"
    if candidate_norm == current_norm:
        return "equal"
    return "unordered"


def scanner_status_by_uart(status: dict[str, Any]) -> dict[str, dict[str, Any]]:
    return {
        str(item.get("uart")): item
        for item in status.get("scanners", [])
        if isinstance(item, dict) and item.get("uart")
    }


def normalized_hardware_id(value: Any) -> str:
    rendered = str(value or "").strip().lower()
    if not _HARDWARE_ID_RE.fullmatch(rendered):
        raise FlashError(f"invalid scanner hardware id: {value!r}")
    return rendered


def verify_scanner_identity_fields(info: dict[str, Any],
                                   platform: dict[str, Any],
                                   slot: str) -> None:
    expected = {
        "firmware_name": platform["scanner_name"],
        "app_project": platform["scanner_project"],
        "hardware_type": platform["hardware_type"],
    }
    labels = {
        "firmware_name": "target",
        "app_project": "project",
        "hardware_type": "hardware type",
    }
    for key, wanted in expected.items():
        got = info.get(key)
        if got != wanted:
            raise FlashError(
                f"{slot} scanner {labels[key]} mismatch: got {got}, wanted {wanted}"
            )
    board = info.get("board")
    if board not in (None, "", platform["scanner_name"]):
        raise FlashError(
            f"{slot} scanner board mismatch: got {board}, "
            f"wanted {platform['scanner_name']}"
        )


def capture_scanner_hardware_ids(status: dict[str, Any],
                                 platform: dict[str, Any],
                                 slots: list[str], *,
                                 require_connected: bool = True) -> dict[str, str]:
    by_uart = scanner_status_by_uart(status)
    captured: dict[str, str] = {}
    for slot in slots:
        info = by_uart.get(slot)
        if not info or not info.get("connected"):
            if require_connected:
                raise FlashError(f"{slot} scanner is not connected")
            continue
        verify_scanner_identity_fields(info, platform, slot)
        captured[slot] = normalized_hardware_id(info.get("hardware_id"))
    if len(set(captured.values())) != len(captured):
        raise FlashError("scanner hardware ids are not unique across requested slots")
    return captured


def scanner_update_newer_slots(status: dict[str, Any], slots: list[str],
                               target_version: str, *,
                               require_connected: bool = True) -> set[str]:
    """Classify safe per-slot skips without weakening manual-flash guards."""
    by_uart = scanner_status_by_uart(status)
    newer: set[str] = set()
    for slot in slots:
        info = by_uart.get(slot)
        if not info or not info.get("connected"):
            if require_connected:
                raise FlashError(
                    f"cannot prove downgrade safety: {slot} scanner is not connected"
                )
            continue
        current = info.get("ver") or info.get("version")
        relation = firmware_version_relation(target_version, str(current or ""))
        if relation == "older":
            newer.add(slot)
        elif relation == "unordered":
            raise FlashError(
                f"unordered firmware variants refused for {slot}: current "
                f"{current}, candidate {target_version}"
            )
        elif relation == "invalid":
            raise FlashError(
                f"cannot prove downgrade safety for {slot}: current {current!r}, "
                f"candidate {target_version!r}"
            )
    return newer


def reject_scanner_downgrades(status: dict[str, Any], slots: list[str],
                              target_version: str) -> None:
    newer = scanner_update_newer_slots(
        status, slots, target_version, require_connected=True
    )
    if newer:
        slot = sorted(newer)[0]
        info = scanner_status_by_uart(status).get(slot, {})
        current = info.get("ver") or info.get("version")
        raise FlashError(
            f"downgrade refused for {slot}: current {current}, "
            f"candidate {target_version}"
        )


def validate_firmware_artifact(path: Path, *, target: str, project: str,
                               hardware: str, version: str) -> None:
    """Reject stale or cross-target images before any flash path can run."""
    try:
        image = path.read_bytes()
    except OSError as exc:
        raise FlashError(f"cannot read firmware artifact {path}: {exc}") from exc

    identity = parse_firmware_identity(image)
    if identity is None:
        raise FlashError(f"invalid firmware project/version descriptor: {path}")
    if identity.project != project:
        raise FlashError(
            f"firmware project mismatch for {target}: "
            f"embedded {identity.project}, expected {project}"
        )
    if identity.version != version:
        raise FlashError(
            f"firmware version mismatch for {target}: "
            f"embedded {identity.version}, expected {version}"
        )
    if target.encode("ascii") not in image:
        raise FlashError(f"firmware target marker missing for {target}: {path}")
    if hardware.encode("ascii") not in image:
        raise FlashError(
            f"firmware hardware marker missing for {target}: "
            f"expected {hardware} in {path}"
        )


def run(cmd: list[str], cwd: Path, dry_run: bool) -> None:
    log("$ " + " ".join(cmd))
    if dry_run:
        return
    proc = subprocess.run(cmd, cwd=str(cwd), text=True)
    if proc.returncode != 0:
        raise FlashError(f"command failed with exit {proc.returncode}: {' '.join(cmd)}")


def build_firmware(platform: dict[str, Any], dry_run: bool) -> None:
    pio = find_pio()
    run([pio, "run", "-e", platform["scanner_env"]], SCANNER_DIR, dry_run)
    run([pio, "run", "-e", platform["uplink_env"]], UPLINK_DIR, dry_run)


def build_scanner_firmware(platform: dict[str, Any], dry_run: bool) -> None:
    pio = find_pio()
    run([pio, "run", "-e", platform["scanner_env"]], SCANNER_DIR, dry_run)


def selected_targets(only: str) -> tuple[bool, list[str]]:
    if only == "all":
        return True, ["ble", "wifi"]
    if only == "uplink":
        return True, []
    if only == "scanners":
        return False, ["ble", "wifi"]
    if only in ("ble", "wifi"):
        return False, [only]
    raise FlashError(f"unsupported --only value: {only}")


def scanner_slot_mask(slots: list[str]) -> int:
    mask = 0
    for slot in slots:
        if slot == "ble":
            mask |= 0x1
        elif slot == "wifi":
            mask |= 0x2
        else:
            raise FlashError(f"unsupported scanner slot: {slot}")
    if mask == 0:
        raise FlashError("scanner firmware staging requires at least one slot")
    return mask


def scanner_stage_receipt_fields(platform: dict[str, Any], version: str,
                                 data: bytes, slot_mask: int) -> dict[str, Any]:
    return {
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


def validate_scanner_stage_receipt(receipt: dict[str, Any],
                                   expected: dict[str, Any], *,
                                   phase: str,
                                   require_generation: bool) -> None:
    if receipt.get("ok") is not True:
        raise FlashError(f"USB scanner firmware stage {phase} failed: {receipt}")
    for key, wanted in expected.items():
        got = receipt.get(key)
        if type(got) is not type(wanted) or got != wanted:
            raise FlashError(
                f"USB scanner firmware stage {phase} {key} mismatch: "
                f"got {got!r}, wanted {wanted!r}"
            )
    if require_generation:
        generation = receipt.get("generation")
        if (
            not isinstance(generation, int) or isinstance(generation, bool) or
            generation <= 0
        ):
            raise FlashError(
                "USB scanner firmware stage final generation is invalid: "
                f"{generation!r}"
            )


def require_artifacts(platform: dict[str, Any], need_uplink: bool,
                      slots: list[str]) -> None:
    missing: list[Path] = []
    if need_uplink and not platform["uplink_bin"].exists():
        missing.append(platform["uplink_bin"])
    if slots and not platform["scanner_bin"].exists():
        missing.append(platform["scanner_bin"])
    if missing:
        rendered = "\n".join(f"  {p}" for p in missing)
        raise FlashError(f"missing firmware artifact(s):\n{rendered}")
    version = repo_version()
    if need_uplink:
        validate_firmware_artifact(
            platform["uplink_bin"],
            target=platform["uplink_name"],
            project=platform["uplink_project"],
            hardware=platform["hardware_type"],
            version=version,
        )
    if slots:
        validate_firmware_artifact(
            platform["scanner_bin"],
            target=platform["scanner_name"],
            project=platform["scanner_project"],
            hardware=platform["hardware_type"],
            version=version,
        )


def scanner_firmware_size(platform: dict[str, Any]) -> int:
    return platform["scanner_bin"].stat().st_size if platform["scanner_bin"].exists() else 0


def detect_usb_port() -> str:
    ports: list[str] = []
    for pattern in (
        "/dev/cu.usbmodem*",
        "/dev/cu.usbserial*",
        "/dev/cu.wchusbserial*",
        "/dev/cu.SLAB*",
    ):
        ports.extend(glob.glob(pattern))
    ports = sorted(dict.fromkeys(ports))
    if not ports:
        raise FlashError("no badge USB serial port found; pass --port")
    if len(ports) > 1:
        raise FlashError("multiple USB serial ports found; pass --port:\n" +
                         "\n".join(f"  {p}" for p in ports))
    return ports[0]


def import_pyserial() -> Any:
    try:
        import serial  # type: ignore
        return serial
    except Exception:
        for site in glob.glob(str(Path.home() / ".platformio/penv/lib/python*/site-packages")):
            if site not in sys.path:
                sys.path.insert(0, site)
        try:
            import serial  # type: ignore
            return serial
        except Exception as exc:
            raise FlashError(
                "pyserial is required for USB badge flashing; install it for "
                "python3 or run with /Users/billh/.platformio/penv/bin/python"
            ) from exc


def wait_for_port(port: str, timeout_s: int = 30) -> str:
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        if Path(port).exists():
            return port
        time.sleep(0.25)
    return detect_usb_port()


def reset_uplink_from_bootloader(port: str, dry_run: bool) -> None:
    python = find_platformio_python()
    run([python, "-m", "esptool", "--port", port, "--before", "no-reset",
         "--after", "watchdog-reset", "run"], REPO_ROOT, dry_run)
    if not dry_run:
        time.sleep(1.5)


def flash_uplink_usb(platform: dict[str, Any], port: str, dry_run: bool) -> None:
    pio = find_pio()
    run([pio, "run", "-e", platform["uplink_env"], "-t", "upload",
         "--upload-port", port], UPLINK_DIR, dry_run)


def flash_scanner_usb(platform: dict[str, Any], port: str, dry_run: bool,
                      slot: str | None = None) -> None:
    pio = find_pio()
    label = f" ({slot})" if slot else ""
    log(f"[scanner-usb] flashing scanner firmware{label} on {port}")
    run([pio, "run", "-e", platform["scanner_env"], "-t", "upload",
         "--upload-port", port], SCANNER_DIR, dry_run)


def http_json(url: str, method: str = "GET", data: bytes | None = None,
              timeout: int = 30) -> dict[str, Any]:
    req = Request(url, data=data, method=method)
    if data is not None:
        req.add_header("Content-Type", "application/octet-stream")
    with urlopen(req, timeout=timeout) as resp:
        body = resp.read().decode("utf-8", "replace")
    parsed = json.loads(body)
    if not isinstance(parsed, dict):
        raise FlashError(f"unexpected JSON response from {url}: {parsed!r}")
    return parsed


def post_json(url: str, payload: dict[str, Any], timeout: int = 30) -> dict[str, Any]:
    data = json.dumps(payload, separators=(",", ":")).encode("utf-8")
    req = Request(url, data=data, method="POST")
    req.add_header("Content-Type", "application/json")
    with urlopen(req, timeout=timeout) as resp:
        body = resp.read().decode("utf-8", "replace")
    parsed = json.loads(body)
    if not isinstance(parsed, dict):
        raise FlashError(f"unexpected JSON response from {url}: {parsed!r}")
    return parsed


def wait_http_status(base_url: str, timeout_s: int = 90) -> dict[str, Any]:
    deadline = time.time() + timeout_s
    last_error: Exception | None = None
    while time.time() < deadline:
        try:
            return http_json(f"{base_url}/api/badge/status", timeout=10)
        except (HTTPError, URLError, TimeoutError, json.JSONDecodeError) as exc:
            last_error = exc
            time.sleep(2)
    raise FlashError(f"badge status did not become reachable at {base_url}: {last_error}")


def upload_scanner_network(platform: dict[str, Any], base_url: str,
                           version: str, dry_run: bool) -> None:
    query = urlencode({"name": platform["scanner_name"], "version": version})
    url = f"{base_url}/api/fw/upload?{query}"
    log(f"[stage] POST {url} ({platform['scanner_bin'].stat().st_size if platform['scanner_bin'].exists() else '?'} bytes)")
    if dry_run:
        return
    data = platform["scanner_bin"].read_bytes()
    body = http_json(url, method="POST", data=data, timeout=600)
    if not body.get("ok"):
        raise FlashError(f"scanner firmware upload failed: {body}")


def relay_scanner_network(base_url: str, slot: str, dry_run: bool,
                          force_probe: bool,
                          allow_same_version: bool,
                          firmware_size: int = 0) -> None:
    query = urlencode({
        "uart": slot,
        "force": "1" if force_probe else "0",
        "allow_same_version": "1" if allow_same_version else "0",
    })
    url = f"{base_url}/api/fw/relay?{query}"
    log(f"[relay] POST {url}")
    if dry_run:
        return
    body = http_json(url, method="POST", data=b"",
                     timeout=scanner_relay_timeout_s(firmware_size))
    if not body.get("ok"):
        raise FlashError(f"{slot} scanner relay failed: {body}")


def flash_uplink_network(platform: dict[str, Any], base_url: str,
                         dry_run: bool) -> None:
    url = f"{base_url}/api/ota"
    log(f"[uplink] POST {url} ({platform['uplink_bin'].stat().st_size if platform['uplink_bin'].exists() else '?'} bytes)")
    if dry_run:
        return
    data = platform["uplink_bin"].read_bytes()
    body = http_json(url, method="POST", data=data, timeout=600)
    if not body.get("ok"):
        raise FlashError(f"uplink OTA failed: {body}")


def resolve_node(backend: str, node: str) -> str:
    if re.match(r"^\d+\.\d+\.\d+\.\d+$", node):
        return node
    for path in ("/detections/nodes/status", "/nodes"):
        try:
            data = http_json(f"{backend}{path}", timeout=10)
        except Exception:
            continue
        nodes = data.get("nodes", data.get("items", data if isinstance(data, list) else []))
        for item in nodes:
            if not isinstance(item, dict):
                continue
            if item.get("device_id") == node or item.get("name") == node:
                ip = item.get("ip") or item.get("last_ip") or item.get("static_ip")
                if ip:
                    return ip
    fallback = REPO_ROOT / "scripts/fof_flash.local.json"
    if fallback.exists():
        data = json.loads(fallback.read_text(encoding="utf-8"))
        ip = data.get("device_ip", {}).get(node)
        if ip:
            return ip
    raise FlashError(f"could not resolve badge node {node!r}")


class BadgeSerial:
    def __init__(self, port: str, dry_run: bool) -> None:
        self.port = port
        self.dry_run = dry_run
        self.ser: Any = None

    def __enter__(self) -> "BadgeSerial":
        if self.dry_run:
            return self
        self._open_serial()
        return self

    def _open_serial(self) -> None:
        serial = import_pyserial()
        ser = serial.Serial()
        ser.port = self.port
        ser.baudrate = 115200
        ser.timeout = 0.15
        ser.write_timeout = 3
        ser.dtr = False
        ser.rts = False
        ser.open()
        try:
            ser.setDTR(False)
            ser.setRTS(False)
        except Exception:
            pass
        self.ser = ser
        self.ser.reset_input_buffer()

    def _close_serial(self) -> None:
        if self.ser:
            self.ser.close()
            self.ser = None

    def reconnect(self, timeout_s: int = 15) -> None:
        if self.dry_run:
            return
        previous_port = self.port
        self._close_serial()
        self.port = wait_for_port(previous_port, timeout_s=timeout_s)
        try:
            self._open_serial()
            self._wait_ping_once(timeout_s)
        except Exception:
            self._close_serial()
            raise

    def __exit__(self, *_exc: object) -> None:
        self._close_serial()

    def write_line(self, line: str) -> None:
        log(f"[usb] > {line}")
        if self.dry_run:
            return
        self.ser.write(("\n" + line + "\n").encode("utf-8"))
        self.ser.flush()

    def read_prefixed_json(self, prefix: str, timeout_s: int,
                           progress_prefix: str | None = None) -> dict[str, Any]:
        if self.dry_run:
            return {"ok": True, "dry_run": True}
        deadline = time.time() + timeout_s
        buf = bytearray()
        last_bad_json: str | None = None
        while time.time() < deadline:
            chunk = self.ser.read(512)
            if chunk:
                buf.extend(chunk)
                while b"\n" in buf:
                    raw, _, buf = buf.partition(b"\n")
                    line = raw.decode("utf-8", "replace").strip()
                    if progress_prefix and line.startswith(progress_prefix):
                        payload = line[len(progress_prefix):]
                        try:
                            progress = json.loads(payload)
                            if isinstance(progress, dict):
                                log(format_relay_progress(progress))
                        except json.JSONDecodeError:
                            log(f"[relay] malformed progress: {line[:160]}")
                        continue
                    diagnostic_markers = (
                        "Auto scanner relay[",
                        "Relay FAILED @",
                        "Relay complete:",
                        "Firmware update offered:",
                        "firmware ready durably accepted:",
                    )
                    for marker in diagnostic_markers:
                        marker_at = line.find(marker)
                        if marker_at >= 0:
                            log(f"[device] {line[marker_at:][:320]}")
                            break
                    if not line.startswith(prefix):
                        continue
                    payload = line[len(prefix):]
                    try:
                        return json.loads(payload)
                    except json.JSONDecodeError as exc:
                        last_bad_json = f"{exc} in {line[:160]!r}"
                        continue
            else:
                time.sleep(0.03)
        if last_bad_json:
            raise FlashError(
                f"timed out waiting for valid {prefix} on {self.port}; "
                f"last malformed frame: {last_bad_json}"
            )
        raise FlashError(f"timed out waiting for {prefix} on {self.port}")

    def _wait_ping_once(self, timeout_s: int) -> None:
        if self.dry_run:
            log("[usb] wait for FOF_PONG")
            return
        deadline = time.time() + timeout_s
        bootloader_seen = False
        while time.time() < deadline:
            self.write_line("FOF_PING")
            line_deadline = time.time() + 2
            data = bytearray()
            while time.time() < line_deadline:
                chunk = self.ser.read(256)
                if chunk:
                    data.extend(chunk)
                    if b"FOF_PONG:" in data:
                        log("[usb] badge uplink responded")
                        return
                    if b"waiting for download" in data or b"DOWNLOAD" in data:
                        bootloader_seen = True
                time.sleep(0.03)
            if bootloader_seen and time.time() + 4 < deadline:
                break
        if bootloader_seen:
            raise FlashError("badge uplink is in ESP ROM download mode")
        raise FlashError("badge uplink did not answer FOF_PING")

    def wait_ping(self, timeout_s: int = 45) -> None:
        try:
            self._wait_ping_once(timeout_s)
            return
        except FlashError as exc:
            if self.dry_run:
                raise
            log(f"[usb] {exc}; trying watchdog app reset")
            self._close_serial()
            try:
                reset_uplink_from_bootloader(self.port, self.dry_run)
            except FlashError as reset_exc:
                self._open_serial()
                try:
                    self._wait_ping_once(timeout_s)
                    return
                except FlashError:
                    raise FlashError(f"{exc}; watchdog reset failed: {reset_exc}") from reset_exc
            self._open_serial()
            self._wait_ping_once(timeout_s)

    def status(self) -> dict[str, Any]:
        self.write_line("FOF_STATUS")
        return self.read_prefixed_json("FOF_STATUS:", 5)

    def ctl(self, payload: dict[str, Any], prefix: str = "FOF_CTL_OK:",
            timeout_s: int = 30) -> dict[str, Any]:
        self.write_line("FOF_CTL:" + json.dumps(payload, separators=(",", ":")))
        return self.read_prefixed_json(prefix, timeout_s)

    def stage_scanner_firmware(self, platform: dict[str, Any],
                               version: str,
                               slots: list[str]) -> dict[str, Any]:
        image_path = platform["scanner_bin"]
        if image_path.exists():
            validate_firmware_artifact(
                image_path,
                target=platform["scanner_name"],
                project=platform["scanner_project"],
                hardware=platform["hardware_type"],
                version=version,
            )
        elif not self.dry_run:
            raise FlashError(f"missing scanner firmware artifact: {image_path}")
        data = image_path.read_bytes() if image_path.exists() else b""
        crc = binascii.crc32(data) & 0xFFFFFFFF
        sha256 = hashlib.sha256(data).hexdigest()
        slot_mask = scanner_slot_mask(slots)
        expected = scanner_stage_receipt_fields(
            platform, version, data, slot_mask
        )
        self.write_line("FOF_CTL:" + json.dumps({
            "cmd": "fw_upload_begin",
            "name": platform["scanner_name"],
            "target": platform["scanner_name"],
            "project": platform["scanner_project"],
            "hardware_type": platform["hardware_type"],
            "version": version,
            "size": len(data),
            "crc32": crc,
            "sha256": sha256,
            "slot_mask": slot_mask,
        }, separators=(",", ":")))
        begin = self.read_prefixed_json("FOF_FW_UPLOAD:", 20)
        if self.dry_run:
            log("[usb] staged scanner firmware verify skipped for dry-run")
            return {"ok": True, "dry_run": True, **expected}
        validate_scanner_stage_receipt(
            begin, expected, phase="begin", require_generation=False
        )
        log(
            f"[usb] staging scanner firmware ({len(data)} bytes "
            f"crc={crc:08x} sha256={sha256})"
        )
        if not self.dry_run:
            for offset in range(0, len(data), 1024):
                self.ser.write(data[offset:offset + 1024])
                self.ser.flush()
        done = self.read_prefixed_json("FOF_FW_UPLOAD:", 120)
        validate_scanner_stage_receipt(
            done, expected, phase="final", require_generation=True
        )
        log(f"[usb] staged scanner firmware verified ({done.get('name')} {done.get('version')})")
        return dict(done)

    def relay_scanner(self, slot: str, force_probe: bool,
                      allow_same_version: bool,
                      firmware_size: int = 0) -> None:
        payload = {
            "cmd": "fw_relay",
            "uart": slot,
            "allow_same_version": allow_same_version,
        }
        if force_probe:
            payload["force"] = True
            payload["skip_command_probe"] = True
        self.write_line("FOF_CTL:" + json.dumps(payload, separators=(",", ":")))
        body = self.read_prefixed_json(
            "FOF_FW_RELAY:",
            scanner_relay_timeout_s(firmware_size),
            progress_prefix="FOF_FW_RELAY_PROGRESS:",
        )
        if not body.get("ok"):
            raise FlashError(f"{slot} scanner relay failed: {body}")


def verify_uplink_status(status: dict[str, Any], version: str) -> None:
    got = status.get("version")
    if norm_version(str(got or "")) != norm_version(version):
        raise FlashError(f"uplink version mismatch: got {got}, wanted {version}")


def verify_uplink_identity_fields(status: dict[str, Any],
                                  platform: dict[str, Any]) -> None:
    expected = {
        "firmware_name": platform["uplink_name"],
        "app_project": platform["uplink_project"],
        "hardware_type": platform["hardware_type"],
    }
    labels = {
        "firmware_name": "target",
        "app_project": "project",
        "hardware_type": "hardware type",
    }
    for key, wanted in expected.items():
        got = status.get(key)
        if got != wanted:
            raise FlashError(
                f"uplink {labels[key]} mismatch: got {got}, wanted {wanted}"
            )


def uplink_flash_needed(status: dict[str, Any], platform: dict[str, Any],
                        target_version: str,
                        recovery_rewrite_same_version: bool) -> bool:
    verify_uplink_identity_fields(status, platform)
    current = str(status.get("version") or "")
    relation = firmware_version_relation(target_version, current)
    if relation == "older":
        raise FlashError(
            f"uplink downgrade refused: current {current}, "
            f"candidate {target_version}"
        )
    if relation == "unordered":
        raise FlashError(
            f"unordered uplink firmware variants refused: current {current}, "
            f"candidate {target_version}"
        )
    if relation == "invalid":
        raise FlashError(
            f"cannot prove uplink downgrade safety: current {current!r}, "
            f"candidate {target_version!r}"
        )
    if relation == "equal" and not recovery_rewrite_same_version:
        log(
            f"[usb] uplink already has {target_version}; same-version UART "
            "rewrite is disabled"
        )
        return False
    return True


def verify_scanners(status: dict[str, Any], platform: dict[str, Any],
                    slots: list[str], version: str, *,
                    expected_hardware_ids: dict[str, str] | None = None,
                    allowed_newer_slots: set[str] | None = None,
                    require_radio_health: bool = True) -> None:
    if status.get("recovery_mode") not in (None, "", "normal"):
        raise FlashError(
            f"uplink recovery mode is not normal: {status.get('recovery_mode')}"
        )
    if status.get("safe_mode") is True:
        raise FlashError("uplink safe mode is active")
    for key in ("usb_control_alive", "scanner_uart_alive"):
        if key in status and status.get(key) is not True:
            raise FlashError(f"uplink health check failed: {key}=false")

    by_uart = scanner_status_by_uart(status)
    allowed_newer = set(allowed_newer_slots or ())
    seen_hardware_ids: dict[str, str] = {}
    for slot in slots:
        info = by_uart.get(slot)
        if not info or not info.get("connected"):
            raise FlashError(f"{slot} scanner is not connected")
        verify_scanner_identity_fields(info, platform, slot)
        got = info.get("ver") or info.get("version")
        if slot in allowed_newer:
            relation = firmware_version_relation(version, str(got or ""))
            if relation != "older":
                raise FlashError(
                    f"{slot} scanner newer-skip proof is invalid: got {got}, "
                    f"candidate {version}; scanner must remain newer"
                )
        elif norm_version(str(got or "")) != norm_version(version):
            raise FlashError(f"{slot} scanner version mismatch: got {got}, wanted {version}")
        hardware_id = normalized_hardware_id(info.get("hardware_id"))
        prior_slot = seen_hardware_ids.get(hardware_id)
        if prior_slot is not None:
            raise FlashError(
                f"requested scanner hardware ids are not unique: "
                f"{prior_slot} and {slot} both report {hardware_id}"
            )
        seen_hardware_ids[hardware_id] = slot
        if expected_hardware_ids is not None and slot in expected_hardware_ids:
            wanted_id = normalized_hardware_id(expected_hardware_ids.get(slot))
            if hardware_id != wanted_id:
                raise FlashError(
                    f"{slot} scanner hardware id mismatch: got {hardware_id}, "
                    f"wanted {wanted_id}"
                )
        if info.get("rollback_pending") is not False:
            raise FlashError(
                f"{slot} scanner rollback is not proven clear: "
                f"rollback_pending={info.get('rollback_pending')!r}"
            )
        recovery_mode = info.get("recovery_mode")
        if recovery_mode not in (None, "", "normal"):
            raise FlashError(
                f"{slot} scanner recovery mode is not normal: {recovery_mode}"
            )
        health = info.get("health")
        if require_radio_health and health not in (None, "", "ok"):
            raise FlashError(f"{slot} scanner health is not normal: {health}")
        ota_state = info.get("ota_state")
        if ota_state not in (None, "", "idle"):
            raise FlashError(
                f"{slot} scanner OTA state is not idle: {ota_state}"
            )

        slot_profile = "ble_primary" if slot == "ble" else "wifi_primary"
        slot_role = info.get("slot_role")
        reported_expected = info.get("expected_scan_profile")
        scan_profile = info.get("scan_profile")
        if (slot_role != slot_profile or
                reported_expected != slot_profile or
                scan_profile != slot_profile):
            raise FlashError(
                f"{slot} scanner role mismatch: slot_role={slot_role!r}, "
                f"expected_scan_profile={reported_expected!r}, "
                f"scan_profile={scan_profile!r}, "
                f"wanted all role fields={slot_profile!r}"
            )
        if info.get("role_acked") is not True:
            raise FlashError(
                f"{slot} scanner role convergence missing: "
                f"role_acked={info.get('role_acked')!r}, "
                f"scan_profile={scan_profile!r}"
            )

        ble_ok = (
            info.get("ble_initialized") is True and
            info.get("ble_scanning") is True and
            info.get("ble_host_active") is True and
            info.get("ble_host_synced") is True
        )
        if "full_scan_ok" in info:
            full_scan_ok = info.get("full_scan_ok")
            full_scan_alias_ok = (
                "wifi_full_scan_ok" not in info or
                info.get("wifi_full_scan_ok") == full_scan_ok
            )
        else:
            full_scan_ok = info.get("wifi_full_scan_ok")
            full_scan_alias_ok = "wifi_full_scan_ok" in info
        wifi_init_rc = info.get("wifi_init_rc")
        wifi_ok = (
            info.get("wifi_initialized") is True and
            isinstance(wifi_init_rc, int) and
            not isinstance(wifi_init_rc, bool) and
            wifi_init_rc == 0 and
            info.get("wifi_active") is True and
            isinstance(full_scan_ok, int) and
            not isinstance(full_scan_ok, bool) and
            full_scan_ok > 0 and
            full_scan_alias_ok
        )
        if scan_profile == "ble_primary":
            radio_ok = ble_ok and info.get("wifi_paused") is True
        elif scan_profile == "wifi_primary":
            radio_ok = (
                wifi_ok and
                info.get("ble_scanning") is False and
                info.get("ble_host_active") is False and
                info.get("wifi_paused") is False
            )
        else:
            radio_ok = False
        if require_radio_health and not radio_ok:
            raise FlashError(
                f"{slot} scanner physical radio health is not proven for "
                f"profile {scan_profile!r}: "
                f"ble_initialized={info.get('ble_initialized')!r}, "
                f"ble_scanning={info.get('ble_scanning')!r}, "
                f"ble_host_active={info.get('ble_host_active')!r}, "
                f"ble_host_synced={info.get('ble_host_synced')!r}, "
                f"wifi_paused={info.get('wifi_paused')!r}, "
                f"wifi_initialized={info.get('wifi_initialized')!r}, "
                f"wifi_init_rc={wifi_init_rc!r}, "
                f"wifi_active={info.get('wifi_active')!r}, "
                f"full_scan_ok={full_scan_ok!r}"
            )


def verify_auto_update_convergence(status: dict[str, Any],
                                   slots: list[str], *,
                                   expected_stage_receipt: dict[str, Any] | None = None) -> None:
    store = status.get("firmware_store")
    if not isinstance(store, dict) or store.get("stored") is not True:
        raise FlashError("automatic scanner update has no staged firmware manifest")
    generation = store.get("generation")
    if not isinstance(generation, int) or isinstance(generation, bool) or generation <= 0:
        raise FlashError(f"invalid staged firmware generation: {generation!r}")

    expected_mask = scanner_slot_mask(slots)
    if expected_stage_receipt is not None:
        receipt_aliases = {
            "name": "target",
            "project": "app_project",
            "hardware": "hardware_type",
        }
        if expected_stage_receipt.get("ok") is not True:
            raise FlashError("automatic scanner update stage receipt is not successful")
        for alias, canonical in receipt_aliases.items():
            if expected_stage_receipt.get(alias) != expected_stage_receipt.get(canonical):
                raise FlashError(
                    f"automatic scanner update stage receipt {alias} identity mismatch"
                )
        store_fields = {
            "target": "target",
            "app_project": "app_project",
            "hardware_type": "hardware_type",
            "version": "version",
            "size": "size",
            "crc32": "crc32",
            "sha256": "sha256",
            "generation": "generation",
        }
        for receipt_key, store_key in store_fields.items():
            wanted = expected_stage_receipt.get(receipt_key)
            got = store.get(store_key)
            if type(got) is not type(wanted) or got != wanted:
                raise FlashError(
                    "automatic scanner update stage receipt "
                    f"{receipt_key} mismatch: store={got!r}, receipt={wanted!r}"
                )
        receipt_mask = expected_stage_receipt.get("slot_mask")
        if (
            not isinstance(receipt_mask, int) or isinstance(receipt_mask, bool) or
            receipt_mask != expected_mask
        ):
            raise FlashError(
                "automatic scanner update stage receipt slot_mask mismatch: "
                f"receipt={receipt_mask!r}, requested={expected_mask}"
            )

    auto_update = store.get("auto_update")
    if not isinstance(auto_update, dict):
        raise FlashError("automatic scanner update status is missing")
    auto_generation = auto_update.get("generation")
    if auto_generation != generation:
        raise FlashError(
            "automatic scanner update generation mismatch: "
            f"coordinator={auto_generation!r}, manifest={generation}"
        )

    target_mask = auto_update.get("target_slot_mask")
    if target_mask != expected_mask:
        raise FlashError(
            "automatic scanner update slot mask mismatch: "
            f"got={target_mask!r}, wanted={expected_mask}"
        )
    pending_mask = auto_update.get("pending_mask")
    if pending_mask != 0:
        raise FlashError(
            f"automatic scanner update still has pending mask {pending_mask!r}"
        )
    if auto_update.get("worker_running") is not False:
        raise FlashError(
            "automatic scanner update worker has not reached a stopped state"
        )

    probes = auto_update.get("readiness_probes")
    if (
        not isinstance(probes, list) or len(probes) != 2 or
        any(
            not isinstance(value, int) or isinstance(value, bool) or
            value < 0 or value > 3
            for value in probes
        )
    ):
        raise FlashError(
            f"automatic scanner update readiness probe proof is invalid: {probes!r}"
        )

    scanner_entries = auto_update.get("scanners")
    if not isinstance(scanner_entries, list):
        raise FlashError("automatic scanner update per-slot status is missing")
    by_slot = {
        entry.get("slot"): entry
        for entry in scanner_entries
        if isinstance(entry, dict) and entry.get("slot") in (0, 1)
    }
    if set(by_slot) != {0, 1}:
        raise FlashError("automatic scanner update must report both scanner slots")

    for scanner_id in (0, 1):
        entry = by_slot[scanner_id]
        state = entry.get("state")
        attempts = entry.get("attempts")
        if (
            not isinstance(attempts, int) or isinstance(attempts, bool) or
            attempts < 0 or attempts > 3
        ):
            raise FlashError(
                f"automatic scanner update slot {scanner_id} attempt proof is invalid: "
                f"{attempts!r}"
            )
        requested = (expected_mask & (1 << scanner_id)) != 0
        if requested and state not in {
            "converged", "current", "newer_skipped"
        }:
            raise FlashError(
                f"automatic scanner update slot {scanner_id} is not in a "
                f"successful terminal state: {state!r}"
            )
        if not requested and state != "excluded":
            raise FlashError(
                f"automatic scanner update slot {scanner_id} was not excluded: "
                f"{state!r}"
            )


def coordinator_newer_skipped_slots(status: dict[str, Any],
                                     slots: list[str]) -> set[str]:
    store = status.get("firmware_store")
    auto_update = store.get("auto_update") if isinstance(store, dict) else None
    entries = auto_update.get("scanners") if isinstance(auto_update, dict) else None
    if not isinstance(entries, list):
        return set()
    requested = set(slots)
    slot_names = {0: "ble", 1: "wifi"}
    return {
        slot_names[entry["slot"]]
        for entry in entries
        if isinstance(entry, dict) and entry.get("slot") in slot_names and
        entry.get("state") == "newer_skipped" and
        slot_names[entry["slot"]] in requested
    }


def current_scanner_slots(status: dict[str, Any], platform: dict[str, Any],
                          slots: list[str], version: str) -> set[str]:
    by_uart = {
        item.get("uart"): item
        for item in status.get("scanners", [])
        if isinstance(item, dict)
    }
    current: set[str] = set()
    for slot in slots:
        info = by_uart.get(slot)
        if not info or not info.get("connected"):
            continue
        board = info.get("board")
        got = info.get("ver") or info.get("version")
        if board == platform["scanner_name"] and versions_match(got, version):
            current.add(slot)
    return current


def scanner_slot_identity_ready(info: dict[str, Any] | None) -> bool:
    if not info or not info.get("connected"):
        return False
    return all(
        info.get(key) not in (None, "")
        for key in (
            "firmware_name",
            "app_project",
            "hardware_type",
            "hardware_id",
        )
    ) and (info.get("ver") or info.get("version")) not in (None, "")


def scanner_status_ready(status: dict[str, Any], slots: list[str]) -> bool:
    by_uart = {
        item.get("uart"): item
        for item in status.get("scanners", [])
        if isinstance(item, dict)
    }
    return all(scanner_slot_identity_ready(by_uart.get(slot)) for slot in slots)


def scanner_slot_has_relay_path(info: dict[str, Any] | None) -> bool:
    if not info:
        return False
    if info.get("connected"):
        # During uplink boot a scanner UART can be marked connected before its
        # immutable target, version, and MAC status fields have arrived. Do not
        # let that transient placeholder end the condition-based preflight.
        return scanner_slot_identity_ready(info)
    if info.get("uart_raw_seen") or int(info.get("uart_raw_bytes") or 0) > 0:
        return True
    for key in ("board", "ver", "version", "cmd_rx", "fw_check_count",
                "ota_state", "recovery_mode"):
        if info.get(key) not in (None, "", False):
            return True
    return False


def scanner_status_has_relay_path(status: dict[str, Any],
                                  slots: list[str]) -> bool:
    by_uart = {
        item.get("uart"): item
        for item in status.get("scanners", [])
        if isinstance(item, dict)
    }
    return all(scanner_slot_has_relay_path(by_uart.get(slot)) for slot in slots)


def scanner_debug_summary(status: dict[str, Any], slots: list[str]) -> str:
    by_uart = {
        item.get("uart"): item
        for item in status.get("scanners", [])
        if isinstance(item, dict)
    }
    parts: list[str] = []
    for slot in slots:
        info = by_uart.get(slot)
        if not info:
            parts.append(f"{slot}:missing")
            continue

        fields: list[str] = ["up" if info.get("connected") else "down"]
        board = info.get("board")
        ver = info.get("ver") or info.get("version")
        if board or ver:
            fields.append(f"{board or '?'}@{ver or '?'}")
        role = info.get("slot_role")
        if role:
            fields.append(f"role={role}")
        if "role_acked" in info:
            fields.append(f"role_ack={1 if info.get('role_acked') else 0}")
        profile = info.get("scan_profile")
        if profile:
            fields.append(f"profile={profile}")
        if info.get("uart_raw_seen") and not info.get("connected"):
            raw = info.get("uart_raw_bytes", 0)
            ovf = info.get("uart_line_overflow", 0)
            json_err = info.get("uart_json_err", 0)
            fields.append(f"raw={raw} ovf={ovf} json_err={json_err}")

        for key, label in (
            ("ota_state", "ota"),
            ("recovery_mode", "recovery"),
            ("safe_reason", "safe"),
            ("cmd_age_ms", "cmd_age_ms"),
            ("last_relay_error", "relay_error"),
            ("radio_restart_count", "radio_restarts"),
            ("crc", "crc"),
        ):
            value = info.get(key)
            if value not in (None, "", False):
                fields.append(f"{label}={value}")
        if info.get("rollback_pending"):
            fields.append("rollback_pending=1")
        if info.get("crash_count"):
            fields.append(f"crashes={info.get('crash_count')}")

        parts.append(f"{slot}:(" + " ".join(fields) + ")")
    return " ".join(parts) if parts else "no scanner slots requested"


def choose_relay_slots(status: dict[str, Any], platform: dict[str, Any],
                       slots: list[str], version: str,
                       recovery_rewrite_same_version: bool,
                       label: str) -> list[str]:
    current = current_scanner_slots(status, platform, slots, version)
    relay_slots: list[str] = []
    for slot in slots:
        if slot not in current:
            log(
                f"[{label}] {slot} scanner is not current; automatic uplink "
                "convergence owns the upgrade"
            )
            continue
        if not recovery_rewrite_same_version:
            log(
                f"[{label}] {slot} scanner already current; same-version "
                "rewrite is disabled"
            )
            continue
        relay_slots.append(slot)
        log(
            f"[{label}] {slot} scanner is current; explicit recovery "
            "same-version rewrite requested"
        )
    return relay_slots


def wait_for_scanner_status_usb(serial_link: BadgeSerial, slots: list[str],
                                timeout_s: int = 45) -> dict[str, Any]:
    deadline = time.time() + timeout_s
    started = time.time()
    last: dict[str, Any] = {}
    last_error: Exception | None = None
    next_log = 0.0
    while time.time() < deadline:
        try:
            last = serial_link.status()
            last_error = None
        except Exception as exc:
            last_error = exc
            last = {}
            try:
                now = time.time()
                remaining_s = max(1, min(15, int(deadline - now)))
                serial_link.reconnect(timeout_s=remaining_s)
            except Exception as reconnect_exc:
                last_error = FlashError(
                    f"{exc}; USB reconnect failed: {reconnect_exc}"
                )
        now = time.time()
        if now >= next_log:
            if last:
                log(f"[usb] scanner status: {scanner_debug_summary(last, slots)}")
            elif last_error:
                log(f"[usb] scanner status unavailable: {last_error}")
            next_log = now + 6
        if scanner_status_ready(last, slots):
            return last
        if last and scanner_status_has_relay_path(last, slots) and time.time() - started > 6:
            return last
        time.sleep(2)
    if last:
        log(f"[usb] scanner status timeout: {scanner_debug_summary(last, slots)}")
    elif last_error:
        log(f"[usb] scanner status timeout: {last_error}")
    return last


def wait_for_scanner_status_network(base_url: str, slots: list[str],
                                    timeout_s: int = 45) -> dict[str, Any]:
    deadline = time.time() + timeout_s
    started = time.time()
    last: dict[str, Any] = {}
    next_log = 0.0
    while time.time() < deadline:
        last = http_json(f"{base_url}/api/badge/status", timeout=10)
        now = time.time()
        if now >= next_log:
            log(f"[network] scanner status: {scanner_debug_summary(last, slots)}")
            next_log = now + 6
        if scanner_status_ready(last, slots):
            return last
        if last and scanner_status_has_relay_path(last, slots) and time.time() - started > 6:
            return last
        time.sleep(2)
    if last:
        log(f"[network] scanner status timeout: {scanner_debug_summary(last, slots)}")
    return last


def wait_for_scanners_usb(serial_link: BadgeSerial, platform: dict[str, Any],
                          slots: list[str], version: str,
                          timeout_s: int = 120, *,
                          expected_hardware_ids: dict[str, str] | None = None,
                          expected_stage_receipt: dict[str, Any] | None = None,
                          allowed_newer_slots: set[str] | None = None,
                          require_auto_update: bool = True,
                          require_radio_health: bool = True) -> None:
    if require_auto_update and expected_stage_receipt is None:
        raise FlashError(
            "automatic scanner verification requires the exact stage receipt"
        )
    deadline = time.time() + timeout_s
    last_error: Exception | None = None
    next_log = 0.0
    status: dict[str, Any] = {}
    while time.time() < deadline:
        try:
            status = serial_link.status()
        except Exception as exc:
            last_error = exc
            now = time.time()
            if now >= next_log:
                log(f"[usb] scanner verify transport unavailable: {exc}")
                next_log = now + 6
            try:
                remaining_s = max(1, min(15, int(deadline - now)))
                serial_link.reconnect(timeout_s=remaining_s)
            except Exception as reconnect_exc:
                last_error = FlashError(
                    f"{exc}; USB reconnect failed: {reconnect_exc}"
                )
            time.sleep(3)
            continue
        try:
            if require_auto_update:
                # Automatic newer-skip authority comes only from the exact
                # receipt-bound coordinator checked below, never from a stale
                # preflight observation.
                proven_newer = coordinator_newer_skipped_slots(status, slots)
            else:
                proven_newer = set(allowed_newer_slots or ())
            verify_scanners(
                status,
                platform,
                slots,
                version,
                expected_hardware_ids=expected_hardware_ids,
                allowed_newer_slots=proven_newer,
                require_radio_health=require_radio_health,
            )
            if require_auto_update:
                verify_auto_update_convergence(
                    status,
                    slots,
                    expected_stage_receipt=expected_stage_receipt,
                )
            return
        except Exception as exc:
            last_error = exc
            now = time.time()
            if now >= next_log:
                try:
                    log(f"[usb] scanner verify waiting: {scanner_debug_summary(status, slots)} ({exc})")
                except Exception:
                    log(f"[usb] scanner verify waiting: {exc}")
                next_log = now + 6
            time.sleep(3)
    raise FlashError(f"scanner verification failed: {last_error}")


def wait_for_scanners_network(base_url: str, platform: dict[str, Any],
                              slots: list[str], version: str,
                              timeout_s: int = 120) -> None:
    deadline = time.time() + timeout_s
    last_error: Exception | None = None
    next_log = 0.0
    while time.time() < deadline:
        try:
            status = http_json(f"{base_url}/api/badge/status", timeout=10)
            verify_scanners(status, platform, slots, version)
            return
        except Exception as exc:
            last_error = exc
            now = time.time()
            if now >= next_log:
                try:
                    log(f"[network] scanner verify waiting: {scanner_debug_summary(status, slots)} ({exc})")
                except Exception:
                    log(f"[network] scanner verify waiting: {exc}")
                next_log = now + 6
            time.sleep(3)
    raise FlashError(f"scanner verification failed: {last_error}")


def usb_flow(args: argparse.Namespace, platform: dict[str, Any],
             need_uplink: bool, slots: list[str], version: str) -> None:
    port = args.port or detect_usb_port()
    log(f"[platform] {args.platform}: {platform['hardware']}")
    log(f"[usb] using {port}")

    flash_uplink = need_uplink
    if need_uplink and not args.dry_run:
        with BadgeSerial(port, False) as badge:
            badge.wait_ping()
            flash_uplink = uplink_flash_needed(
                badge.status(),
                platform,
                version,
                getattr(args, "recovery_rewrite_same_version", False),
            )

    if flash_uplink:
        flash_uplink_usb(platform, port, args.dry_run)
        if not args.dry_run:
            port = wait_for_port(port)

    if not slots:
        if args.dry_run:
            log("[verify] uplink version")
            return
        with BadgeSerial(port, args.dry_run) as badge:
            badge.wait_ping()
            status = badge.status()
            verify_uplink_identity_fields(status, platform)
            verify_uplink_status(status, version)
        return

    with BadgeSerial(port, args.dry_run) as badge:
        badge.wait_ping()
        if not args.dry_run:
            post_flash_status = badge.status()
            verify_uplink_identity_fields(post_flash_status, platform)
            verify_uplink_status(post_flash_status, version)
        if args.dry_run:
            badge.stage_scanner_firmware(platform, version, slots)
            log("[usb] automatic uplink convergence will upgrade strictly older scanners")
            log("[verify] scanner versions: " + ", ".join(slots))
            return

        status = wait_for_scanner_status_usb(badge, slots)
        expected_hardware_ids = capture_scanner_hardware_ids(
            status, platform, slots, require_connected=False
        )
        missing_preflight_ids = sorted(set(slots) - set(expected_hardware_ids))
        if missing_preflight_ids:
            log(
                "[usb] preflight MAC continuity unavailable for: " +
                ", ".join(missing_preflight_ids) +
                "; relay-time firmware will enforce immutable same-MAC "
                "continuity and final verification will enforce unique IDs"
            )
        newer_slots = scanner_update_newer_slots(
            status, slots, version, require_connected=False
        )
        if newer_slots:
            log(
                "[usb] scanner(s) already newer than the staged image will be "
                "durably skipped: " + ", ".join(sorted(newer_slots))
            )
        recovery_slots = choose_relay_slots(
            status,
            platform,
            slots,
            version,
            getattr(args, "recovery_rewrite_same_version", False),
            "usb",
        )

        stage_receipt = badge.stage_scanner_firmware(platform, version, slots)
        if recovery_slots:
            log(
                "[usb] explicit recovery mode selected; manually relaying only "
                "the exact same-version scanner slot(s)"
            )
        else:
            log(
                "[usb] scanner image staged; waiting for automatic strict-newer "
                "uplink convergence"
            )
        for slot in recovery_slots:
            # Same-version recovery must prove the immutable identity, UART
            # path, rollback/OTA state, and exact physical-slot role before
            # relay.  It intentionally does not require the old image's radio
            # to be healthy: repairing a failed radio is the recovery use case.
            # The post-reboot verification below restores the full radio gate.
            wait_for_scanners_usb(
                badge,
                platform,
                [slot],
                version,
                expected_hardware_ids=expected_hardware_ids,
                require_auto_update=False,
                require_radio_health=False,
            )
            badge.relay_scanner(
                slot,
                getattr(args, "skip_command_probe", False),
                True,
                scanner_firmware_size(platform),
            )

        timeout_s = (
            scanner_relay_timeout_s(scanner_firmware_size(platform)) * len(slots)
            + 180
        )
        wait_for_scanners_usb(
            badge,
            platform,
            slots,
            version,
            timeout_s=timeout_s,
            expected_hardware_ids=expected_hardware_ids,
            expected_stage_receipt=stage_receipt,
        )


def enable_network_from_usb(port: str, mode: str, ttl_s: int,
                            dry_run: bool) -> None:
    with BadgeSerial(port, dry_run) as badge:
        badge.wait_ping()
        badge.ctl({"cmd": "network", "mode": mode, "ttl_s": ttl_s})


def network_base_url(args: argparse.Namespace) -> str:
    if args.transport == "ap":
        return "http://192.168.4.1"
    if args.host:
        host = args.host
    elif args.node:
        host = resolve_node(args.backend, args.node)
    else:
        raise FlashError("LAN transport requires --host or --node")
    if host.startswith("http://") or host.startswith("https://"):
        return host.rstrip("/")
    return f"http://{host}"


def network_flow(args: argparse.Namespace, platform: dict[str, Any],
                 need_uplink: bool, slots: list[str], version: str) -> None:
    require_usb_firmware_transport(args.transport)
    base_url = network_base_url(args)
    log(f"[platform] {args.platform}: {platform['hardware']}")
    log(f"[network] using {base_url}")

    if args.port:
        mode = "local_ap" if args.transport == "ap" else "backend"
        enable_network_from_usb(args.port, mode, args.network_ttl_s, args.dry_run)

    initial_status: dict[str, Any] = {}
    if not args.dry_run:
        initial_status = wait_http_status(base_url, timeout_s=90)

    flash_uplink = need_uplink
    if need_uplink and not args.dry_run:
        flash_uplink = uplink_flash_needed(
            initial_status,
            platform,
            version,
            getattr(args, "recovery_rewrite_same_version", False),
        )

    if slots:
        relay_slots = list(slots)
        same_version_recovery_slots: set[str] = set()
        if not args.dry_run:
            status = wait_for_scanner_status_network(base_url, slots)
            if not scanner_status_ready(status, slots):
                reject_scanner_downgrades(status, slots, version)
                raise FlashError(
                    "cannot prove downgrade safety: every requested scanner "
                    "must be connected and report its current version"
                )
            else:
                reject_scanner_downgrades(status, slots, version)
                current = current_scanner_slots(
                    status, platform, slots, version
                )
                relay_slots = [slot for slot in slots if slot not in current]
                same_version_recovery_slots = set(choose_relay_slots(
                    status,
                    platform,
                    slots,
                    version,
                    getattr(args, "recovery_rewrite_same_version", False),
                    "network",
                ))
                relay_slots.extend(same_version_recovery_slots)
        if relay_slots:
            upload_scanner_network(platform, base_url, version, args.dry_run)
        for slot in relay_slots:
            relay_scanner_network(base_url, slot, args.dry_run,
                                  args.skip_command_probe,
                                  slot in same_version_recovery_slots,
                                  scanner_firmware_size(platform))
        if args.dry_run:
            log("[verify] scanner versions: " + ", ".join(slots))
        else:
            wait_for_scanners_network(base_url, platform, slots, version)

    if flash_uplink:
        flash_uplink_network(platform, base_url, args.dry_run)
        if args.dry_run:
            log("[verify] uplink version after reboot")
        else:
            status = wait_http_status(base_url, timeout_s=180)
            verify_uplink_identity_fields(status, platform)
            verify_uplink_status(status, version)


def manual_scanner_flow(args: argparse.Namespace, platform: dict[str, Any],
                        version: str) -> None:
    slot = args.manual_scanner
    port = args.port or detect_usb_port()
    log(f"[platform] {args.platform}: {platform['hardware']}")
    log(f"[manual] direct USB scanner flash for {slot} scanner on {port}")
    if args.dry_run:
        flash_scanner_usb(platform, port, True, slot)
        log(f"[verify] {slot} scanner identity/version via uplink status")
        return
    if not args.verify_port:
        raise FlashError(
            "cannot prove downgrade safety for a direct scanner flash without "
            "--verify-port pointing to the connected badge uplink"
        )

    log(
        f"[manual] preflighting {slot} through uplink {args.verify_port} "
        "before direct flash"
    )
    with BadgeSerial(args.verify_port, False) as badge:
        badge.wait_ping()
        status = wait_for_scanner_status_usb(badge, [slot])
        expected_hardware_ids = capture_scanner_hardware_ids(
            status, platform, [slot]
        )
        reject_scanner_downgrades(status, [slot], version)
        current = current_scanner_slots(status, platform, [slot], version)
        if slot in current and not getattr(
            args, "recovery_rewrite_same_version", False
        ):
            log(
                f"[manual] {slot} scanner already has {version}; direct "
                "same-version rewrite is disabled"
            )
            return

        flash_scanner_usb(platform, port, False, slot)
        wait_for_scanners_usb(
            badge,
            platform,
            [slot],
            version,
            timeout_s=240,
            expected_hardware_ids=expected_hardware_ids,
            require_auto_update=False,
        )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--transport", choices=("usb", "ap", "lan"), default="usb")
    parser.add_argument("--platform", choices=sorted(PLATFORMS),
                        default="badge-trio-xiao-s3")
    parser.add_argument("--port", help="USB serial port for USB flashing or enabling AP/LAN")
    parser.add_argument("--host", help="LAN badge host/IP for --transport lan")
    parser.add_argument("--backend", default=DEFAULT_BACKEND)
    parser.add_argument("--node", help="Backend node/device id for --transport lan")
    parser.add_argument("--only", choices=("uplink", "scanners", "ble", "wifi", "all"),
                        default="all")
    parser.add_argument("--manual-scanner", choices=("ble", "wifi"),
                        help="Directly flash one scanner MCU over its own USB port")
    parser.add_argument("--verify-port",
                        help="Required badge uplink USB port for safe direct scanner preflight and verification")
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument("--force", action="store_true",
                        help="Accepted for compatibility; does not bypass version safety")
    parser.add_argument("--allow-same-version", action="store_true",
                        help=argparse.SUPPRESS)
    parser.add_argument(
        "--recovery-rewrite-same-version",
        action="store_true",
        help=(
            "Recovery-only: manually relay an exact same-version scanner image; "
            "never permits a downgrade"
        ),
    )
    parser.add_argument("--skip-current", action="store_true",
                        help="Deprecated compatibility option; same-version scanners are skipped by default")
    parser.add_argument("--skip-command-probe", action="store_true",
                        help="Recovery-only: skip scanner command-ingress probe during relay")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--network-ttl-s", type=int, default=900)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    platform = PLATFORMS[args.platform]
    version = repo_version()
    need_uplink, slots = selected_targets(args.only)

    try:
        require_usb_firmware_transport(args.transport)
        if args.allow_same_version:
            raise FlashError(
                "--allow-same-version is disabled; use the explicit "
                "--recovery-rewrite-same-version flag for an exact "
                "same-version recovery only"
            )
        if args.manual_scanner:
            if not args.skip_build:
                build_scanner_firmware(platform, args.dry_run)
            if not args.dry_run or args.skip_build:
                require_artifacts(platform, False, [args.manual_scanner])
            manual_scanner_flow(args, platform, version)
            log("[done] badge scanner manual flash flow complete")
            return 0

        if not args.skip_build:
            build_firmware(platform, args.dry_run)
        if not args.dry_run or args.skip_build:
            require_artifacts(platform, need_uplink, slots)

        if args.transport == "usb":
            usb_flow(args, platform, need_uplink, slots, version)
        else:
            network_flow(args, platform, need_uplink, slots, version)
    except FlashError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("Interrupted", file=sys.stderr)
        return 130
    log("[done] badge flash flow complete")
    return 0


if __name__ == "__main__":
    sys.exit(main())
