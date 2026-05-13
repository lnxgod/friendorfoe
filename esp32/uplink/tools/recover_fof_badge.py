#!/usr/bin/env python3
"""Recover and verify the FoF Badge uplink firmware on Seeed XIAO ESP32-S3.

This script is intentionally more stubborn than PlatformIO upload. It first
tries the normal path, then repeatedly looks for the bad app's early USB
command window and ROM bootloader sync opportunities.
"""

from __future__ import annotations

import argparse
import datetime as _dt
import glob
import json
import os
import re
import select
import signal
import subprocess
import sys
import time
from pathlib import Path
from typing import Iterable

try:
    import serial
except Exception as exc:  # pragma: no cover - operator-facing failure path
    fallback_python = Path.home() / ".platformio" / "penv" / "bin" / "python"
    if (fallback_python.exists() and
            os.environ.get("FOF_RECOVERY_NO_PENV") != "1" and
            Path(sys.executable).resolve() != fallback_python.resolve()):
        env = os.environ.copy()
        env["FOF_RECOVERY_NO_PENV"] = "1"
        os.execve(str(fallback_python), [str(fallback_python), *sys.argv], env)
    print(f"pyserial is required: {exc}", file=sys.stderr)
    sys.exit(2)

try:
    import usb.core
except Exception:  # pragma: no cover - optional recovery acceleration
    usb = None


DEFAULT_PORT = None
DEFAULT_ENV = "uplink-s3-fof_badge"
DEFAULT_VERIFY_SECONDS = 60
BAUDS = (115200, 921600)
FLASH_BAUDS = (115200, 460800, 921600)
HARD_RESET_SYNC_WINDOW_S = 180
APP_RECOVERY_WINDOW_S = 25
BOOT_MARKERS = (
    "FOF_READY",
    "FOF_TIMEOUT",
    "Runtime USB serial control listener ready",
    "ESP-ROM:",
    "Guru Meditation",
    "stack overflow",
)
BAD_LOG_MARKERS = (
    "Guru Meditation",
    "stack overflow",
    "panic",
    "abort()",
    "Cannot send polling transaction",
    "ESP_ERR_INVALID_STATE",
)
PASS_LOG_MARKERS = (
    "ST7735 initialized",
    "Badge display-only boot",
    "Badge build: RGB LED task disabled",
    "Badge build: backend upload task disabled",
    "UART RX task started: BLE",
    "UART RX task started: WiFi",
    "Display task started",
    "HTTP status server disabled by badge mode",
    "Sent ready signal to all scanners",
)
STATUS_RE = re.compile(r"^FOF_STATUS:(\{[^\r\n]*\})\r?$", re.MULTILINE)


def repo_paths() -> tuple[Path, Path, Path]:
    tool_path = Path(__file__).resolve()
    uplink_dir = tool_path.parents[1]
    esp32_dir = uplink_dir.parent
    repo_dir = esp32_dir.parent
    return repo_dir, esp32_dir, uplink_dir


REPO_DIR, ESP32_DIR, UPLINK_DIR = repo_paths()
PIO = ESP32_DIR / ".venv312" / "bin" / "pio"
PENV_PYTHON = Path.home() / ".platformio" / "penv" / "bin" / "python"
ESPTOOL = Path.home() / ".platformio" / "packages" / "tool-esptoolpy" / "esptool.py"
LOG_FH = None
SERIAL_FH = None


def log(msg: str) -> None:
    line = f"[{_dt.datetime.now().isoformat(timespec='seconds')}] {msg}"
    print(line, flush=True)
    if LOG_FH:
        LOG_FH.write(line + "\n")
        LOG_FH.flush()


def setup_logging(log_dir: str | None) -> Path:
    global LOG_FH, SERIAL_FH
    stamp = _dt.datetime.now().strftime("%Y%m%d-%H%M%S")
    root = Path(log_dir) if log_dir else (REPO_DIR / "images" / "logs" / "badge_recovery" / stamp)
    root.mkdir(parents=True, exist_ok=True)
    LOG_FH = (root / "recovery.log").open("a", encoding="utf-8")
    SERIAL_FH = (root / "serial-bytes.log").open("ab")
    log(f"Logging to {root}")
    return root


def log_process_line(text: str) -> None:
    print(text, end="", flush=True)
    if LOG_FH:
        LOG_FH.write(text)
        LOG_FH.flush()


def log_serial_bytes(chunk: bytes) -> None:
    if not SERIAL_FH:
        return
    prefix = f"[{_dt.datetime.now().isoformat(timespec='milliseconds')}] ".encode()
    SERIAL_FH.write(prefix + chunk.hex(" ").encode() + b"\n")
    SERIAL_FH.flush()


def run_stream(cmd: list[str], cwd: Path, timeout: float | None = None) -> int:
    log("$ " + " ".join(cmd))
    proc = subprocess.Popen(
        cmd,
        cwd=str(cwd),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        bufsize=0,
    )
    start = time.time()
    assert proc.stdout is not None
    os.set_blocking(proc.stdout.fileno(), False)
    try:
        while True:
            ready, _, _ = select.select([proc.stdout], [], [], 0.2)
            if ready:
                try:
                    chunk = proc.stdout.read()
                except BlockingIOError:
                    chunk = b""
                if chunk:
                    log_process_line(chunk.decode("utf-8", "replace"))
            if proc.poll() is not None:
                try:
                    rest = proc.stdout.read()
                except BlockingIOError:
                    rest = b""
                if rest:
                    log_process_line(rest.decode("utf-8", "replace"))
                return int(proc.returncode)
            if timeout and time.time() - start > timeout:
                proc.terminate()
                try:
                    proc.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    proc.kill()
                log(f"command timed out after {timeout:.0f}s")
                return 124
    except KeyboardInterrupt:
        proc.terminate()
        raise


def run_capture(cmd: list[str], cwd: Path, timeout: float = 15) -> tuple[int, str]:
    try:
        proc = subprocess.run(
            cmd,
            cwd=str(cwd),
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=timeout,
        )
        return proc.returncode, proc.stdout
    except subprocess.TimeoutExpired as exc:
        out = exc.stdout or ""
        if isinstance(out, bytes):
            out = out.decode("utf-8", "replace")
        return 124, out


def kill_pid(pid: int, label: str) -> None:
    if pid in (os.getpid(), os.getppid()):
        return
    try:
        os.kill(pid, signal.SIGTERM)
        log(f"killed stale {label} pid {pid}")
        deadline = time.time() + 1.0
        while time.time() < deadline:
            try:
                os.kill(pid, 0)
            except ProcessLookupError:
                return
            time.sleep(0.05)
        os.kill(pid, signal.SIGKILL)
        log(f"force-killed stale {label} pid {pid}")
    except ProcessLookupError:
        return
    except PermissionError:
        log(f"could not kill {label} pid {pid}")


def pgrep(pattern: str) -> list[int]:
    rc, out = run_capture(["pgrep", "-f", pattern], REPO_DIR, timeout=3)
    if rc != 0:
        return []
    pids: list[int] = []
    for line in out.splitlines():
        try:
            pids.append(int(line.strip()))
        except ValueError:
            pass
    return pids


def lsof_pids(paths: Iterable[str]) -> list[int]:
    existing = [p for p in paths if Path(p).exists()]
    if not existing:
        return []
    rc, out = run_capture(["lsof", "-t", *existing], REPO_DIR, timeout=5)
    if rc != 0:
        return []
    pids: list[int] = []
    for line in out.splitlines():
        try:
            pids.append(int(line.strip()))
        except ValueError:
            pass
    return pids


def discover_usbmodem_ports() -> list[str]:
    ports: list[str] = []
    for pattern in ("/dev/cu.usbmodem*", "/dev/tty.usbmodem*"):
        ports.extend(glob.glob(pattern))
    return sorted(dict.fromkeys(ports), key=lambda p: (0 if "/dev/cu." in p else 1, p))


def detect_usb_port(required: bool = True) -> str | None:
    ports = [p for p in discover_usbmodem_ports() if "/dev/cu." in p]
    if not ports:
        if required:
            raise RuntimeError("no badge USB serial port found; pass --port")
        return None
    if len(ports) > 1:
        raise RuntimeError(
            "multiple USB serial ports found; pass --port:\n" +
            "\n".join(f"  {p}" for p in ports)
        )
    return ports[0]


def paired_serial_ports(port: str | None) -> list[str]:
    if not port:
        return []
    ports = [port]
    if "/dev/cu." in port:
        ports.append(port.replace("/dev/cu.", "/dev/tty."))
    elif "/dev/tty." in port:
        ports.append(port.replace("/dev/tty.", "/dev/cu."))
    return list(dict.fromkeys(ports))


def candidate_serial_ports(preferred_port: str | None) -> list[str]:
    ports = paired_serial_ports(preferred_port)
    ports.extend(discover_usbmodem_ports())
    return list(dict.fromkeys(ports))


def usb_reset_esp32s3_jtag() -> bool:
    if usb is None:
        log("PyUSB unavailable; skipping USB-JTAG device reset")
        return False

    found = False
    for dev in usb.core.find(find_all=True, idVendor=0x303A, idProduct=0x1001):
        found = True
        try:
            log("Resetting ESP32-S3 USB JTAG/serial device via PyUSB")
            dev.reset()
            time.sleep(2.0)
            return True
        except Exception as exc:
            log(f"USB device reset failed: {exc}")

    if not found:
        log("No ESP32-S3 USB JTAG/serial device found for PyUSB reset")
    return False


def kill_stale_processes(port: str | None) -> None:
    log("Cleaning stale local monitor/camera/serial holders...")
    patterns = (
        r"ffmpeg.*avfoundation",
        r"pio .*device monitor",
        r"pio device monitor",
        r"esptool.*usbmodem",
    )
    for pattern in patterns:
        for pid in pgrep(pattern):
            kill_pid(pid, pattern)
    if port:
        for pid in lsof_pids(paired_serial_ports(port)):
            kill_pid(pid, port)
    time.sleep(0.5)


def build_firmware(env: str) -> bool:
    return run_stream([str(PIO), "run", "-e", env], UPLINK_DIR) == 0


def normal_upload(env: str, port: str) -> bool:
    log("Trying normal PlatformIO upload first...")
    return (
        run_stream(
            [str(PIO), "run", "-e", env, "-t", "upload", "--upload-port", port],
            UPLINK_DIR,
            timeout=45,
        )
        == 0
    )


def esptool_write_flash(port: str, env: str, before: str, attempts: int,
                        baud: int = 460800,
                        timeout_s: float | None = None) -> bool:
    build_dir = UPLINK_DIR / ".pio" / "build" / env
    files = {
        "bootloader": build_dir / "bootloader.bin",
        "partitions": build_dir / "partitions.bin",
        "ota": build_dir / "ota_data_initial.bin",
        "firmware": build_dir / "firmware.bin",
    }
    missing = [str(path) for path in files.values() if not path.exists()]
    if missing:
        log("Missing build artifacts: " + ", ".join(missing))
        return False

    cmd = [
        str(PENV_PYTHON),
        str(ESPTOOL),
        "--chip",
        "esp32s3",
        "--port",
        port,
        "--baud",
        str(baud),
        "--connect-attempts",
        str(attempts),
        "--before",
        before,
        "--after",
        "hard-reset",
        "write-flash",
        "-z",
        "--flash-mode",
        "dio",
        "--flash-freq",
        "80m",
        "--flash-size",
        "8MB",
        "0x0",
        str(files["bootloader"]),
        "0x8000",
        str(files["partitions"]),
        "0xf000",
        str(files["ota"]),
        "0x20000",
        str(files["firmware"]),
    ]
    log(f"Trying esptool write-flash before={before} baud={baud} attempts={attempts}...")
    if timeout_s is None:
        timeout_s = max(45, attempts * 2.5) if attempts > 0 else HARD_RESET_SYNC_WINDOW_S
    return run_stream(cmd, UPLINK_DIR, timeout=timeout_s) == 0


def set_lines(ser: serial.Serial, dtr: bool, rts: bool) -> None:
    ser.dtr = dtr
    ser.rts = rts
    time.sleep(0.08)


def read_text(ser: serial.Serial, seconds: float) -> str:
    end = time.time() + seconds
    data = bytearray()
    while time.time() < end:
        chunk = ser.read(2048)
        if chunk:
            log_serial_bytes(chunk)
            data.extend(chunk)
    return bytes(data).decode("utf-8", "replace")


def send_bootloader_commands(ser: serial.Serial) -> None:
    payload = (
        b"\nFOF_STATUS\n"
        b"FOF_CTL:{\"cmd\":\"status\"}\n"
        b"FOF_BOOTLOADER\n"
        b"FOF_CTL:{\"cmd\":\"bootloader\"}\n"
    )
    try:
        ser.write(payload)
        ser.flush()
    except Exception as exc:
        log(f"serial write failed: {exc}")


def serial_recovery_pass(port: str) -> bool:
    log("Sweeping serial baud/control-line states for firmware bootloader window...")
    found_window = False
    for baud in BAUDS:
        for dtr, rts in ((False, False), (False, True), (True, False), (True, True)):
            try:
                with serial.Serial(port, baud, timeout=0.05, write_timeout=0.3) as ser:
                    set_lines(ser, dtr, rts)
                    text = read_text(ser, 1.2)
                    if text:
                        log_process_line(text)
                    if any(marker in text for marker in BOOT_MARKERS):
                        found_window = True
                        log(f"Found boot/config window at baud={baud} DTR={dtr} RTS={rts}")
                    if found_window:
                        send_bootloader_commands(ser)
                        text += read_text(ser, 1.5)
                        if "FOF_BOOTLOADER:OK" in text or "ESP-ROM:" in text:
                            log("Bootloader command appears accepted.")
                            return True
            except Exception as exc:
                log(f"serial open/read failed at baud={baud} DTR={dtr} RTS={rts}: {exc}")

    log("Trying active reset pulses while sending bootloader commands...")
    for baud in BAUDS:
        for boot_asserted in (False, True):
            try:
                with serial.Serial(port, baud, timeout=0.05, write_timeout=0.3) as ser:
                    # On Espressif auto-reset circuits DTR often maps BOOT and RTS maps EN.
                    ser.dtr = boot_asserted
                    ser.rts = True
                    time.sleep(0.25)
                    ser.rts = False
                    start = time.time()
                    collected = ""
                    while time.time() - start < 4:
                        send_bootloader_commands(ser)
                        collected += read_text(ser, 0.35)
                        if any(marker in collected for marker in BOOT_MARKERS):
                            log_process_line(collected)
                            return True
            except Exception as exc:
                log(f"reset pulse failed at baud={baud}: {exc}")

    return False


def touch_1200(port: str) -> None:
    log("Trying 1200-baud touch/open-close...")
    try:
        with serial.Serial(port, 1200, timeout=0.1, write_timeout=0.1) as ser:
            ser.dtr = False
            ser.rts = False
            time.sleep(0.25)
    except Exception as exc:
        log(f"1200-baud touch failed: {exc}")
    time.sleep(2.0)


def latest_status_from_text(text: str) -> dict | None:
    latest = None
    for match in STATUS_RE.finditer(text):
        try:
            latest = json.loads(match.group(1))
        except json.JSONDecodeError as exc:
            log(f"Invalid FOF_STATUS JSON: {exc}")
    return latest


def request_status(ser: serial.Serial) -> None:
    ser.write(b"\nFOF_STATUS\n")
    ser.flush()


def clear_forced_safe_mode(ser: serial.Serial) -> None:
    ser.write(
        b'\nFOF_CTL:{"cmd":"safe_mode","enabled":false,'
        b'"reason":"post_flash_clear"}\n'
    )
    ser.flush()


def request_reboot(ser: serial.Serial) -> None:
    ser.write(b"\nFOF_REBOOT\n")
    ser.flush()


def wait_for_serial_port(port: str, timeout_s: float) -> bool:
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        if Path(port).exists():
            return True
        time.sleep(0.25)
    return False


def any_candidate_port_present(port: str | None) -> bool:
    return any(Path(candidate).exists() for candidate in candidate_serial_ports(port))


def try_flash_matrix(port: str | None, env: str, attempts: int) -> str | None:
    if not any_candidate_port_present(port):
        return None
    usb_reset_esp32s3_jtag()
    for candidate in candidate_serial_ports(port):
        if not Path(candidate).exists():
            continue
        for baud in FLASH_BAUDS:
            for before in ("default-reset", "usb-reset", "no-reset"):
                if not Path(candidate).exists():
                    log(f"Port disappeared during flash matrix: {candidate}")
                    break
                if esptool_write_flash(candidate, env, before=before,
                                       attempts=attempts, baud=baud):
                    return candidate
    return None


def long_rom_catcher_pass(port: str | None, env: str,
                          window_s: int) -> str | None:
    for candidate in candidate_serial_ports(port):
        if not Path(candidate).exists():
            continue
        log(f"Hard-reset catcher waiting for ROM sync on {candidate} for {window_s}s")
        if esptool_write_flash(candidate, env, before="no-reset",
                               attempts=0, baud=460800,
                               timeout_s=window_s):
            return candidate
    return None


def no_button_catcher_upload(port: str | None, env: str,
                             timeout_s: int | None) -> str | None:
    log("")
    log("Entering hard-reset USB catcher. AP recovery is disabled.")
    log("Leave the script running; power-cycle or hard-reset the badge whenever ready.")
    log("The catcher keeps esptool in no-reset ROM sync for long windows,")
    log("then briefly tries app USB bootloader commands before waiting again.")
    start = time.time()
    attempt = 0
    last_present = any_candidate_port_present(port)
    log(f"Initial port state: {'present' if last_present else 'missing'} ({', '.join(candidate_serial_ports(port))})")

    while timeout_s is None or time.time() - start < timeout_s:
        present = any_candidate_port_present(port)
        if present != last_present:
            log(f"USB port {'appeared' if present else 'disappeared'}: {', '.join(candidate_serial_ports(port))}")
            last_present = present
            if present:
                time.sleep(0.35)

        if not present:
            time.sleep(0.2)
            continue

        attempt += 1
        if attempt == 1 or (attempt % 5) == 0:
            kill_stale_processes(port)

        log(f"Hard-reset catcher attempt {attempt}")
        flashed_port = long_rom_catcher_pass(port, env, HARD_RESET_SYNC_WINDOW_S)
        if flashed_port:
            return flashed_port

        log(f"Trying app-command recovery for {APP_RECOVERY_WINDOW_S}s before next ROM wait")
        app_deadline = time.time() + APP_RECOVERY_WINDOW_S
        usb_reset_esp32s3_jtag()
        while time.time() < app_deadline:
            for candidate in candidate_serial_ports(port):
                if Path(candidate).exists():
                    serial_recovery_pass(candidate)
            flashed_port = try_flash_matrix(port, env, attempts=2)
            if flashed_port:
                return flashed_port
            time.sleep(0.25)

        for candidate in candidate_serial_ports(port):
            if Path(candidate).exists():
                touch_1200(candidate)
        flashed_port = try_flash_matrix(port, env, attempts=2)
        if flashed_port:
            return flashed_port

        log(f"Catcher attempt {attempt} complete; still waiting for ROM sync.")
        time.sleep(0.25)

    log("Hard-reset catcher timed out before flashing.")
    return None


def log_status_summary(status: dict) -> None:
    counts = status.get("counts") if isinstance(status.get("counts"), dict) else {}
    log(
        "FOF_STATUS ok: "
        f"version={status.get('version')} mode={status.get('mode')} "
        f"safe={status.get('safe_mode')} recovery={status.get('recovery_mode')} "
        f"reset={status.get('reset_reason')} expected={status.get('reset_expected')} "
        f"crashes={status.get('crash_count')} "
        f"usb_age={status.get('usb_control_age_s')}s "
        f"stack_usb={status.get('stack_usb_free')} "
        f"stack_uart_ble={status.get('stack_uart_ble_free')} "
        f"stack_uart_wifi={status.get('stack_uart_wifi_free')} "
        f"drone={counts.get('drone')} meta={counts.get('meta')}"
    )


def collect_boot_status(port: str, seconds: int,
                        require_pass_markers: bool) -> tuple[bool, str, dict | None]:
    log(f"Verifying serial boot for {seconds}s...")
    reboot_count = 0
    data = ""
    try:
        with serial.Serial(port, 115200, timeout=0.2, write_timeout=0.5) as ser:
            ser.dtr = False
            ser.rts = False
            end = time.time() + seconds
            last_status_request = 0.0
            while time.time() < end:
                now = time.time()
                if now - last_status_request >= 5.0:
                    request_status(ser)
                    last_status_request = now
                chunk = ser.read(4096)
                if chunk:
                    log_serial_bytes(chunk)
                    text = chunk.decode("utf-8", "replace")
                    log_process_line(text)
                    data += text
                    reboot_count += text.count("Rebooting...")
                    if any(marker in data for marker in BAD_LOG_MARKERS):
                        return False, data, None
                    if reboot_count >= 2:
                        return False, data, None

            log("Requesting final FOF_STATUS over USB...")
            request_status(ser)
            data += read_text(ser, 3.0)
    except Exception as exc:
        log(f"serial verify failed: {exc}")
        return False, data, None

    if require_pass_markers:
        missing = [marker for marker in PASS_LOG_MARKERS if marker not in data]
    else:
        missing = []
    if missing:
        log("Missing required boot markers: " + ", ".join(missing))
        return False, data, None

    status = latest_status_from_text(data)
    if not status:
        log("Missing FOF_STATUS JSON response")
        return False, data, None
    if status.get("mode") not in {"local_ap", "backend", "usb_only"}:
        log("FOF_STATUS JSON missing valid mode")
        return False, data, status
    log_status_summary(status)
    return True, data, status


def clear_safe_mode_and_reboot(port: str) -> tuple[bool, str]:
    data = ""
    try:
        with serial.Serial(port, 115200, timeout=0.2, write_timeout=0.5) as ser:
            ser.dtr = False
            ser.rts = False
            log("Clearing forced safe USB mode after flash...")
            clear_forced_safe_mode(ser)
            data += read_text(ser, 2.0)
            if "FOF_CTL_OK" not in data:
                log("Safe-mode clear did not return FOF_CTL_OK")
                return False, data
            request_status(ser)
            data += read_text(ser, 2.0)
            status = latest_status_from_text(data)
            if status:
                log_status_summary(status)
            log("Rebooting once so normal scanner/display tasks start outside safe USB mode...")
            request_reboot(ser)
            data += read_text(ser, 1.0)
    except Exception as exc:
        log(f"safe-mode clear failed: {exc}")
        return False, data

    time.sleep(2.0)
    wait_for_serial_port(port, 20)
    return True, data


def verify_serial(port: str, seconds: int) -> tuple[bool, str]:
    ok, data, status = collect_boot_status(port, seconds, require_pass_markers=False)
    if not ok or not status:
        return False, data
    if status.get("safe_mode") or int(status.get("crash_count") or 0) > 0:
        cleared, clear_log = clear_safe_mode_and_reboot(port)
        data += clear_log
        if not cleared:
            return False, data
        ok, normal_data, normal_status = collect_boot_status(
            port, seconds, require_pass_markers=True)
        data += normal_data
        if not ok or not normal_status:
            return False, data
        if normal_status.get("safe_mode"):
            log("Badge remained in safe mode after post-flash clear and reboot")
            return False, data
        return True, data

    missing = [marker for marker in PASS_LOG_MARKERS if marker not in data]
    if missing:
        log("Missing required boot markers: " + ", ".join(missing))
        return False, data
    return True, data


def capture_camera(output: Path) -> bool:
    log("Capturing camera frame after serial pass...")
    output.parent.mkdir(parents=True, exist_ok=True)
    for pid in pgrep(r"ffmpeg.*avfoundation"):
        kill_pid(pid, "ffmpeg avfoundation")
    cmd = [
        "ffmpeg",
        "-y",
        "-f",
        "avfoundation",
        "-pixel_format",
        "nv12",
        "-framerate",
        "30",
        "-video_size",
        "1280x720",
        "-i",
        "0:none",
        "-frames:v",
        "1",
        str(output),
    ]
    rc = run_stream(cmd, REPO_DIR, timeout=10)
    if rc == 0 and output.exists() and output.stat().st_size > 0:
        log(f"Camera frame saved: {output}")
        return True
    log("Camera capture did not complete; serial verification remains authoritative.")
    return False


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default=DEFAULT_PORT)
    parser.add_argument("--env", default=DEFAULT_ENV)
    parser.add_argument("--verify-seconds", type=int, default=DEFAULT_VERIFY_SECONDS)
    parser.add_argument(
        "--operator-timeout",
        type=int,
        default=0,
        help="Seconds to wait in the no-button USB catcher; 0 means infinite.",
    )
    parser.add_argument("--log-dir", default=None)
    parser.add_argument("--skip-camera", action="store_true")
    parser.add_argument("--skip-normal-upload", action="store_true")
    parser.add_argument("--hard-reset-catcher", action="store_true",
                        help="Skip short recovery and wait in long no-reset ROM sync windows")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        port = args.port or detect_usb_port(required=False)
    except RuntimeError as exc:
        log(str(exc))
        return 2
    env = args.env
    setup_logging(args.log_dir)

    if not PIO.exists():
        log(f"PlatformIO not found: {PIO}")
        return 2
    if not PENV_PYTHON.exists() or not ESPTOOL.exists():
        log("PlatformIO esptool runtime not found")
        return 2

    if port and not Path(port).exists():
        log(f"Port not currently present: {port}; catcher will wait for USB replug")
    elif not port:
        log("No badge USB serial port currently present; catcher will wait for USB replug")
    kill_stale_processes(port)
    if port:
        usb_reset_esp32s3_jtag()
    if not build_firmware(env):
        return 1

    flashed = False
    flashed_port = port
    if not args.skip_normal_upload and not args.hard_reset_catcher and port and Path(port).exists():
        flashed = normal_upload(env, port)
    elif args.skip_normal_upload:
        log("Skipping normal upload by request.")
    elif args.hard_reset_catcher:
        log("Skipping normal upload for hard-reset catcher mode.")
    else:
        log("Skipping normal upload because the port is not present.")
    if not flashed:
        timeout = None if args.operator_timeout == 0 else args.operator_timeout
        flashed_port = no_button_catcher_upload(port, env, timeout)
        flashed = flashed_port is not None
    if not flashed:
        log("Recovery failed before flashing.")
        return 1

    if not flashed_port:
        log("Recovery flashed but no verification port was recorded.")
        return 1
    ok, _serial_log = verify_serial(flashed_port, args.verify_seconds)
    if not ok:
        log("Serial verification failed.")
        return 1

    if not args.skip_camera:
        capture_camera(REPO_DIR / "images" / "screenshots" / "badge_camera_check.jpg")

    log("Badge recovery PASS.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
