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
DEFAULT_BACKEND = os.environ.get("FOF_BACKEND", "http://localhost:8000")
SCANNER_RELAY_TIMEOUT_MIN_S = 240
SCANNER_RELAY_TIMEOUT_MAX_S = 900
SCANNER_RELAY_TIMEOUT_PER_KB_S = 0.30

PLATFORMS: dict[str, dict[str, Any]] = {
    "badge-trio-xiao-s3": {
        "hardware": "FoF Badge trio on Seeed XIAO ESP32-S3",
        "uplink_env": "uplink-s3-fof_badge",
        "uplink_name": "uplink-s3-fof_badge",
        "uplink_bin": UPLINK_DIR / ".pio/build/uplink-s3-fof_badge/firmware.bin",
        "scanner_env": "scanner-s3-combo-fof_badge",
        "scanner_name": "scanner-s3-combo-fof_badge",
        "scanner_bin": SCANNER_DIR / ".pio/build/scanner-s3-combo-fof_badge/firmware.bin",
        "slots": ("ble", "wifi"),
    },
}


class FlashError(RuntimeError):
    pass


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
    if got_norm == wanted_norm:
        return True
    if len(got_norm) >= 12 and wanted_norm.startswith(got_norm):
        return True
    if len(wanted_norm) >= 12 and got_norm.startswith(wanted_norm):
        return True
    return False


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
                               version: str) -> None:
        data = platform["scanner_bin"].read_bytes() if platform["scanner_bin"].exists() else b""
        crc = binascii.crc32(data) & 0xFFFFFFFF
        self.write_line("FOF_CTL:" + json.dumps({
            "cmd": "fw_upload_begin",
            "name": platform["scanner_name"],
            "version": version,
            "size": len(data),
            "crc32": crc,
        }, separators=(",", ":")))
        begin = self.read_prefixed_json("FOF_FW_UPLOAD:", 20)
        if not begin.get("ok"):
            raise FlashError(f"USB scanner firmware stage begin failed: {begin}")
        log(f"[usb] staging scanner firmware ({len(data)} bytes crc={crc:08x})")
        if not self.dry_run:
            for offset in range(0, len(data), 1024):
                self.ser.write(data[offset:offset + 1024])
                self.ser.flush()
        done = self.read_prefixed_json("FOF_FW_UPLOAD:", 120)
        if not done.get("ok"):
            raise FlashError(f"USB scanner firmware stage failed: {done}")
        if self.dry_run:
            log("[usb] staged scanner firmware verify skipped for dry-run")
            return
        expected = {
            "name": platform["scanner_name"],
            "version": version,
            "size": len(data),
            "crc32": crc,
        }
        for key, wanted in expected.items():
            got = done.get(key)
            if key == "version":
                if not versions_match(str(got), str(wanted)):
                    raise FlashError(f"USB scanner firmware stage version mismatch: got {got}, wanted {wanted}")
            elif got != wanted:
                raise FlashError(f"USB scanner firmware stage {key} mismatch: got {got}, wanted {wanted}")
        log(f"[usb] staged scanner firmware verified ({done.get('name')} {done.get('version')})")

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
    if not versions_match(got, version):
        raise FlashError(f"uplink version mismatch: got {got}, wanted {version}")


def verify_scanners(status: dict[str, Any], platform: dict[str, Any],
                    slots: list[str], version: str) -> None:
    by_uart = {
        item.get("uart"): item
        for item in status.get("scanners", [])
        if isinstance(item, dict)
    }
    for slot in slots:
        info = by_uart.get(slot)
        if not info or not info.get("connected"):
            raise FlashError(f"{slot} scanner is not connected")
        board = info.get("board")
        got = info.get("ver") or info.get("version")
        if board != platform["scanner_name"]:
            raise FlashError(f"{slot} scanner board mismatch: got {board}, wanted {platform['scanner_name']}")
        if not versions_match(got, version):
            raise FlashError(f"{slot} scanner version mismatch: got {got}, wanted {version}")


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


def scanner_status_ready(status: dict[str, Any], slots: list[str]) -> bool:
    by_uart = {
        item.get("uart"): item
        for item in status.get("scanners", [])
        if isinstance(item, dict)
    }
    return all(by_uart.get(slot, {}).get("connected") for slot in slots)


def scanner_slot_has_relay_path(info: dict[str, Any] | None) -> bool:
    if not info:
        return False
    if info.get("connected"):
        return True
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
                       skip_current: bool,
                       label: str) -> list[str]:
    current = current_scanner_slots(status, platform, slots, version)
    relay_slots: list[str] = []
    for slot in slots:
        if slot not in current:
            relay_slots.append(slot)
            continue
        if skip_current:
            log(f"[{label}] {slot} scanner already current; skipping because --skip-current was set")
            continue
        relay_slots.append(slot)
        log(f"[{label}] {slot} scanner is current; rewriting because badge scanner flashing is recovery-first")
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
        except FlashError as exc:
            last_error = exc
            last = {}
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
                          timeout_s: int = 120) -> None:
    deadline = time.time() + timeout_s
    last_error: Exception | None = None
    next_log = 0.0
    while time.time() < deadline:
        try:
            status = serial_link.status()
            verify_scanners(status, platform, slots, version)
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

    if need_uplink:
        flash_uplink_usb(platform, port, args.dry_run)
        if not args.dry_run:
            port = wait_for_port(port)

    if not slots:
        if args.dry_run:
            log("[verify] uplink version")
            return
        with BadgeSerial(port, args.dry_run) as badge:
            badge.wait_ping()
            verify_uplink_status(badge.status(), version)
        return

    with BadgeSerial(port, args.dry_run) as badge:
        badge.wait_ping()
        if need_uplink and not args.dry_run:
            verify_uplink_status(badge.status(), version)
        relay_slots = list(slots)
        if not args.dry_run:
            status = wait_for_scanner_status_usb(badge, slots)
            if not scanner_status_ready(status, slots):
                if scanner_status_has_relay_path(status, slots):
                    log("[usb] scanner status is not clean, but UART evidence exists; proceeding with recovery relay through uplink")
                    relay_slots = list(slots)
                    args.skip_command_probe = True
                else:
                    raise FlashError(
                        "scanner status has no UART evidence for requested slot(s); "
                        "the scanner app is physically offline, unpowered, unplugged, "
                        "or in ROM bootloader"
                    )
            else:
                relay_slots = choose_relay_slots(
                    status, platform, slots, version,
                    args.skip_current, "usb"
                )
        if relay_slots:
            badge.stage_scanner_firmware(platform, version)
        for slot in relay_slots:
            badge.relay_scanner(slot, args.skip_command_probe,
                                True,
                                scanner_firmware_size(platform))
        if args.dry_run:
            log("[verify] scanner versions: " + ", ".join(slots))
        else:
            wait_for_scanners_usb(badge, platform, slots, version)


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
    base_url = network_base_url(args)
    log(f"[platform] {args.platform}: {platform['hardware']}")
    log(f"[network] using {base_url}")

    if args.port:
        mode = "local_ap" if args.transport == "ap" else "backend"
        enable_network_from_usb(args.port, mode, args.network_ttl_s, args.dry_run)

    if not args.dry_run:
        wait_http_status(base_url, timeout_s=90)

    if slots:
        relay_slots = list(slots)
        if not args.dry_run:
            status = wait_for_scanner_status_network(base_url, slots)
            if not scanner_status_ready(status, slots):
                if scanner_status_has_relay_path(status, slots):
                    log("[network] scanner status is not clean, but UART evidence exists; proceeding with recovery relay")
                    relay_slots = list(slots)
                    args.skip_command_probe = True
                else:
                    raise FlashError(
                        "scanner status has no UART evidence for requested slot(s); "
                        "the scanner app is physically offline, unpowered, unplugged, "
                        "or in ROM bootloader"
                    )
            else:
                relay_slots = choose_relay_slots(
                    status, platform, slots, version,
                    args.skip_current, "network"
                )
        if relay_slots:
            upload_scanner_network(platform, base_url, version, args.dry_run)
        for slot in relay_slots:
            relay_scanner_network(base_url, slot, args.dry_run,
                                  args.skip_command_probe,
                                  True,
                                  scanner_firmware_size(platform))
        if args.dry_run:
            log("[verify] scanner versions: " + ", ".join(slots))
        else:
            wait_for_scanners_network(base_url, platform, slots, version)

    if need_uplink:
        flash_uplink_network(platform, base_url, args.dry_run)
        if args.dry_run:
            log("[verify] uplink version after reboot")
        else:
            status = wait_http_status(base_url, timeout_s=180)
            verify_uplink_status(status, version)


def manual_scanner_flow(args: argparse.Namespace, platform: dict[str, Any],
                        version: str) -> None:
    slot = args.manual_scanner
    port = args.port or detect_usb_port()
    log(f"[platform] {args.platform}: {platform['hardware']}")
    log(f"[manual] direct USB scanner flash for {slot} scanner on {port}")
    flash_scanner_usb(platform, port, args.dry_run, slot)
    if args.verify_port:
        log(f"[manual] reconnect badge trio and verifying {slot} through uplink {args.verify_port}")
        if args.dry_run:
            log(f"[verify] {slot} scanner via uplink status")
            return
        with BadgeSerial(args.verify_port, args.dry_run) as badge:
            badge.wait_ping()
            wait_for_scanners_usb(badge, platform, [slot], version)
    else:
        log("[manual] scanner flashed directly; reconnect the trio and run "
            "`--transport usb --only scanners --skip-build` to verify both slots")


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
                        help="Optional badge uplink USB port to verify after a direct scanner flash")
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument("--force", action="store_true",
                        help="Accepted for compatibility; badge scanner flashing rewrites requested slots by default")
    parser.add_argument("--allow-same-version", action="store_true",
                        help="Accepted for compatibility; same-version badge scanner rewrites are enabled by default")
    parser.add_argument("--skip-current", action="store_true",
                        help="Do not relay to scanners that already report the target badge scanner version")
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
