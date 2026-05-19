#!/usr/bin/env python3
"""Debug HTTP bridge for testing Android badge flows from an emulator.

The Android emulator can sometimes see the ESP32-S3 USB device at the kernel
level but not expose it through UsbManager. This bridge keeps the real badge on
the Mac side, then presents badge-compatible HTTP endpoints that the emulator
can reach at http://10.0.2.2:8765.
"""

from __future__ import annotations

import argparse
import glob
import http.server
import json
import os
import queue
import re
import socketserver
import sys
import threading
import time
import urllib.parse
import zlib
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

try:
    import serial
except Exception as exc:  # pragma: no cover - operator-facing failure path
    fallback_python = Path.home() / ".platformio" / "penv" / "bin" / "python"
    if (fallback_python.exists() and
            os.environ.get("FOF_DEBUG_BRIDGE_NO_PENV") != "1" and
            Path(sys.executable).resolve() != fallback_python.resolve()):
        env = os.environ.copy()
        env["FOF_DEBUG_BRIDGE_NO_PENV"] = "1"
        os.execve(str(fallback_python), [str(fallback_python), *sys.argv], env)
    print(f"pyserial is required: {exc}", file=sys.stderr)
    sys.exit(2)


DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 8765
DEFAULT_BAUD = 115200
STATUS_RE = re.compile(r"^FOF_STATUS:(\{.*\})$")
PREFIXES = (
    "FOF_CTL_OK:",
    "FOF_CTL_ERROR:",
    "FOF_FW_UPLOAD:",
    "FOF_FW_RELAY:",
    "FOF_FW_RELAY_PROGRESS:",
)


def detect_usb_port() -> str:
    cu_ports = sorted(set(glob.glob("/dev/cu.usbmodem*")))
    if len(cu_ports) == 1:
        return cu_ports[0]
    if len(cu_ports) > 1:
        raise RuntimeError(
            "multiple USB modem ports found; pass --port: " + ", ".join(cu_ports)
        )
    tty_ports = sorted(set(glob.glob("/dev/tty.usbmodem*")))
    if len(tty_ports) == 1:
        return tty_ports[0]
    if len(tty_ports) > 1:
        raise RuntimeError(
            "multiple USB modem ports found; pass --port: " + ", ".join(tty_ports)
        )
    raise RuntimeError("no /dev/cu.usbmodem* badge port found")


def parse_prefixed_json(line: str, prefix: str) -> dict[str, Any] | None:
    if not line.startswith(prefix):
        return None
    try:
        return json.loads(line[len(prefix):])
    except json.JSONDecodeError:
        return None


@dataclass
class BridgeState:
    serial_port: str
    baud: int = DEFAULT_BAUD
    poll_seconds: float = 1.0
    serial_timeout: float = 0.1
    ser: serial.Serial | None = None
    lock: threading.Lock = field(default_factory=threading.Lock)
    stop_event: threading.Event = field(default_factory=threading.Event)
    lines: queue.Queue[str] = field(default_factory=queue.Queue)
    cached_status: dict[str, Any] | None = None
    cached_status_at: float = 0.0
    last_error: str = ""
    control_rx: int = 0
    control_tx: int = 0

    def open(self) -> None:
        self.ser = serial.Serial(self.serial_port, self.baud, timeout=self.serial_timeout)
        self.reader_thread = threading.Thread(target=self._reader_loop, daemon=True)
        self.poll_thread = threading.Thread(target=self._poll_loop, daemon=True)
        self.reader_thread.start()
        self.poll_thread.start()

    def close(self) -> None:
        self.stop_event.set()
        if self.ser:
            self.ser.close()

    def _reader_loop(self) -> None:
        assert self.ser is not None
        while not self.stop_event.is_set():
            try:
                raw = self.ser.readline()
            except Exception as exc:  # pragma: no cover - serial unplug path
                self.last_error = str(exc)
                time.sleep(0.5)
                continue
            if not raw:
                continue
            line = raw.decode("utf-8", "replace").strip()
            if not line:
                continue
            match = STATUS_RE.match(line)
            if match:
                try:
                    self.cached_status = json.loads(match.group(1))
                    self.cached_status_at = time.time()
                except json.JSONDecodeError:
                    self.last_error = "invalid FOF_STATUS json"
            if line.startswith(PREFIXES):
                self.lines.put(line)

    def _poll_loop(self) -> None:
        while not self.stop_event.is_set():
            self.send_line("FOF_STATUS")
            self.stop_event.wait(self.poll_seconds)

    def send_line(self, line: str) -> None:
        assert self.ser is not None
        payload = (line.rstrip("\r\n") + "\n").encode("utf-8")
        with self.lock:
            self.ser.write(payload)
            self.ser.flush()
            self.control_tx += 1

    def write_bytes(self, data: bytes, chunk_size: int = 1024) -> None:
        assert self.ser is not None
        offset = 0
        with self.lock:
            while offset < len(data):
                chunk = data[offset:offset + chunk_size]
                self.ser.write(chunk)
                self.ser.flush()
                offset += len(chunk)

    def wait_for_prefix(
        self,
        prefixes: tuple[str, ...],
        timeout_s: float,
    ) -> tuple[str, dict[str, Any] | None] | None:
        deadline = time.time() + timeout_s
        while time.time() < deadline:
            try:
                line = self.lines.get(timeout=0.1)
            except queue.Empty:
                continue
            for prefix in prefixes:
                parsed = parse_prefixed_json(line, prefix)
                if parsed is not None:
                    self.control_rx += 1
                    return prefix, parsed
        return None

    def request_status(self, timeout_s: float = 2.0) -> dict[str, Any]:
        if self.cached_status and time.time() - self.cached_status_at < 2.0:
            return self.with_bridge_meta(self.cached_status)
        self.send_line("FOF_STATUS")
        deadline = time.time() + timeout_s
        while time.time() < deadline:
            if self.cached_status and time.time() - self.cached_status_at < timeout_s:
                return self.with_bridge_meta(self.cached_status)
            time.sleep(0.05)
        raise TimeoutError("badge did not return FOF_STATUS")

    def send_control(self, payload: dict[str, Any]) -> dict[str, Any]:
        self.send_line("FOF_CTL:" + json.dumps(payload, separators=(",", ":")))
        result = self.wait_for_prefix(("FOF_CTL_OK:", "FOF_CTL_ERROR:"), 5.0)
        if result is None:
            return {"ok": False, "error": "control timeout"}
        prefix, body = result
        body = body or {}
        body.setdefault("ok", prefix == "FOF_CTL_OK:")
        return body

    def upload_firmware(self, name: str, version: str, data: bytes) -> dict[str, Any]:
        crc = zlib.crc32(data) & 0xFFFFFFFF
        self.send_line("FOF_CTL:" + json.dumps({
            "cmd": "fw_upload_begin",
            "name": name,
            "version": version,
            "size": len(data),
            "crc32": crc,
        }, separators=(",", ":")))
        begin = self.wait_for_prefix(("FOF_FW_UPLOAD:",), 8.0)
        if begin is None or not (begin[1] or {}).get("ok", False):
            return begin[1] if begin and begin[1] else {"ok": False, "error": "upload_begin_timeout"}
        self.write_bytes(data)
        done = self.wait_for_prefix(("FOF_FW_UPLOAD:",), max(20.0, len(data) / 30000.0))
        if done is None:
            return {"ok": False, "error": "upload_done_timeout"}
        return done[1] or {"ok": False, "error": "bad_upload_response"}

    def relay_firmware(self, uart: str, force: bool) -> dict[str, Any]:
        self.send_line("FOF_CTL:" + json.dumps({
            "cmd": "fw_relay",
            "uart": uart,
            "force": force,
        }, separators=(",", ":")))
        deadline = time.time() + 360.0
        last_progress: dict[str, Any] | None = None
        while time.time() < deadline:
            result = self.wait_for_prefix(("FOF_FW_RELAY:", "FOF_FW_RELAY_PROGRESS:"), 1.0)
            if result is None:
                continue
            prefix, body = result
            if prefix == "FOF_FW_RELAY_PROGRESS:":
                last_progress = body
                continue
            return body or {"ok": False, "error": "bad_relay_response"}
        return {"ok": False, "error": "relay_timeout", "last_progress": last_progress}

    def with_bridge_meta(self, status: dict[str, Any]) -> dict[str, Any]:
        out = dict(status)
        out["debug_bridge"] = {
            "transport": "Debug Bridge",
            "serial_port": self.serial_port,
            "status_age_s": round(max(0.0, time.time() - self.cached_status_at), 2),
            "last_error": self.last_error,
            "rx": self.control_rx,
            "tx": self.control_tx,
        }
        return out


class BadgeBridgeHandler(http.server.BaseHTTPRequestHandler):
    server: "BadgeBridgeServer"

    def end_headers(self) -> None:
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Headers", "content-type")
        self.send_header("Access-Control-Allow-Methods", "GET,POST,OPTIONS")
        super().end_headers()

    def do_OPTIONS(self) -> None:
        self.send_response(204)
        self.end_headers()

    def do_GET(self) -> None:
        if self.path.split("?", 1)[0] != "/api/badge/status":
            self.send_json({"ok": False, "error": "not found"}, code=404)
            return
        try:
            self.send_json(self.server.bridge.request_status())
        except Exception as exc:
            self.send_json({"ok": False, "error": str(exc)}, code=504)

    def do_POST(self) -> None:
        path = self.path.split("?", 1)[0]
        if path == "/api/badge/control":
            self.handle_control()
        elif path in ("/api/fw/upload", "/api/badge/fw_upload", "/api/badge/fw_upload_begin"):
            self.handle_fw_upload()
        elif path in ("/api/fw/relay", "/api/badge/fw_relay"):
            self.handle_fw_relay()
        else:
            self.send_json({"ok": False, "error": "not found"}, code=404)

    def handle_control(self) -> None:
        payload = self.read_json_body()
        if payload is None:
            return
        self.send_json(self.server.bridge.send_control(payload))

    def handle_fw_upload(self) -> None:
        query = urllib.parse.parse_qs(urllib.parse.urlsplit(self.path).query)
        name = query.get("name", ["scanner-s3-combo-fof_badge"])[0]
        version = query.get("version", ["debug-bridge"])[0]
        length = int(self.headers.get("content-length", "0"))
        if length <= 0:
            self.send_json({"ok": False, "error": "empty firmware body"}, code=400)
            return
        data = self.rfile.read(length)
        self.send_json(self.server.bridge.upload_firmware(name, version, data))

    def handle_fw_relay(self) -> None:
        payload = self.read_json_body()
        if payload is None:
            return
        uart = str(payload.get("uart", "ble"))
        if uart not in ("ble", "wifi"):
            self.send_json({"ok": False, "error": "uart must be ble or wifi"}, code=400)
            return
        self.send_json(self.server.bridge.relay_firmware(uart, bool(payload.get("force", False))))

    def read_json_body(self) -> dict[str, Any] | None:
        length = int(self.headers.get("content-length", "0"))
        try:
            body = self.rfile.read(length).decode("utf-8") if length else "{}"
            payload = json.loads(body)
            if not isinstance(payload, dict):
                raise ValueError("json body must be an object")
            return payload
        except Exception as exc:
            self.send_json({"ok": False, "error": str(exc)}, code=400)
            return None

    def send_json(self, payload: dict[str, Any], code: int = 200) -> None:
        body = json.dumps(payload, separators=(",", ":")).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt: str, *args: Any) -> None:
        print(f"[bridge] {self.address_string()} {fmt % args}", file=sys.stderr)


class BadgeBridgeServer(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True

    def __init__(self, address: tuple[str, int], bridge: BridgeState):
        self.bridge = bridge
        super().__init__(address, BadgeBridgeHandler)


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", help="Badge serial port; defaults to one /dev/cu.usbmodem*")
    parser.add_argument("--baud", type=int, default=DEFAULT_BAUD)
    parser.add_argument("--host", default=DEFAULT_HOST)
    parser.add_argument("--http-port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--poll-seconds", type=float, default=1.0)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_arg_parser().parse_args(argv)
    serial_port = args.port or detect_usb_port()
    bridge = BridgeState(serial_port=serial_port, baud=args.baud, poll_seconds=args.poll_seconds)
    bridge.open()
    server = BadgeBridgeServer((args.host, args.http_port), bridge)
    print(
        f"FoF badge debug bridge on http://{args.host}:{args.http_port} "
        f"(emulator: http://10.0.2.2:{args.http_port}) via {serial_port}",
        flush=True,
    )
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nStopping bridge", flush=True)
    finally:
        server.server_close()
        bridge.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
