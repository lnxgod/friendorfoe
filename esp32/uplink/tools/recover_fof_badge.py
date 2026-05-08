#!/usr/bin/env python3
"""Recover and verify the FoF Badge uplink firmware on Seeed XIAO ESP32-S3.

This script is intentionally more stubborn than PlatformIO upload. It first
tries the normal path, then repeatedly looks for the bad app's early USB
command window and ROM bootloader sync opportunities.
"""

from __future__ import annotations

import argparse
import datetime as _dt
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
    print(f"pyserial is required: {exc}", file=sys.stderr)
    sys.exit(2)

try:
    import usb.core
except Exception:  # pragma: no cover - optional recovery acceleration
    usb = None


DEFAULT_PORT = "/dev/cu.usbmodem1101"
DEFAULT_ENV = "uplink-s3-fof_badge"
DEFAULT_VERIFY_SECONDS = 60
BAUDS = (115200, 921600)
FLASH_BAUDS = (115200, 460800, 921600)
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
        text=True,
        bufsize=1,
    )
    start = time.time()
    assert proc.stdout is not None
    try:
        while True:
            ready, _, _ = select.select([proc.stdout], [], [], 0.2)
            if ready:
                line = proc.stdout.readline()
                if line:
                    log_process_line(line)
            if proc.poll() is not None:
                rest = proc.stdout.read()
                if rest:
                    log_process_line(rest)
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


def paired_serial_ports(port: str) -> list[str]:
    ports = [port]
    if "/dev/cu." in port:
        ports.append(port.replace("/dev/cu.", "/dev/tty."))
    elif "/dev/tty." in port:
        ports.append(port.replace("/dev/tty.", "/dev/cu."))
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


def kill_stale_processes(port: str) -> None:
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


def esptool_write_flash(port: str, env: str, before: str, attempts: int, baud: int = 460800) -> bool:
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
    return run_stream(cmd, UPLINK_DIR, timeout=max(45, attempts * 2.5)) == 0


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


def any_candidate_port_present(port: str) -> bool:
    return any(Path(candidate).exists() for candidate in paired_serial_ports(port))


def try_flash_matrix(port: str, env: str, attempts: int) -> bool:
    if not any_candidate_port_present(port):
        return False
    usb_reset_esp32s3_jtag()
    for candidate in paired_serial_ports(port):
        if not Path(candidate).exists():
            continue
        for baud in FLASH_BAUDS:
            for before in ("default-reset", "usb-reset", "no-reset"):
                if not Path(candidate).exists():
                    log(f"Port disappeared during flash matrix: {candidate}")
                    break
                if esptool_write_flash(candidate, env, before=before,
                                       attempts=attempts, baud=baud):
                    return True
    return False


def no_button_catcher_upload(port: str, env: str, timeout_s: int | None) -> bool:
    log("")
    log("Entering no-button USB catcher. AP recovery is disabled.")
    log("Leave the script running; unplug/replug USB-C whenever the badge loops.")
    log("The catcher will spam early USB commands, toggle DTR/RTS/1200-baud,")
    log("and immediately try full ROM write-flash whenever the port appears.")
    start = time.time()
    attempt = 0
    last_present = any_candidate_port_present(port)
    log(f"Initial port state: {'present' if last_present else 'missing'} ({', '.join(paired_serial_ports(port))})")

    while timeout_s is None or time.time() - start < timeout_s:
        present = any_candidate_port_present(port)
        if present != last_present:
            log(f"USB port {'appeared' if present else 'disappeared'}: {', '.join(paired_serial_ports(port))}")
            last_present = present
            if present:
                time.sleep(0.35)

        if not present:
            time.sleep(0.2)
            continue

        attempt += 1
        if attempt == 1 or (attempt % 5) == 0:
            kill_stale_processes(port)

        log(f"No-button catcher attempt {attempt}")
        usb_reset_esp32s3_jtag()
        for candidate in paired_serial_ports(port):
            if Path(candidate).exists():
                serial_recovery_pass(candidate)
        if try_flash_matrix(port, env, attempts=8):
            return True

        for candidate in paired_serial_ports(port):
            if Path(candidate).exists():
                touch_1200(candidate)
        if try_flash_matrix(port, env, attempts=6):
            return True

        log(f"Catcher attempt {attempt} complete; still waiting for ROM sync.")
        time.sleep(0.25)

    log("No-button catcher timed out before flashing.")
    return False


def verify_serial(port: str, seconds: int) -> tuple[bool, str]:
    log(f"Verifying serial boot for {seconds}s...")
    reboot_count = 0
    data = ""
    try:
        with serial.Serial(port, 115200, timeout=0.2, write_timeout=0.5) as ser:
            ser.dtr = False
            ser.rts = False
            end = time.time() + seconds
            while time.time() < end:
                chunk = ser.read(4096)
                if chunk:
                    log_serial_bytes(chunk)
                    text = chunk.decode("utf-8", "replace")
                    log_process_line(text)
                    data += text
                    reboot_count += text.count("Rebooting...")
                    if any(marker in data for marker in BAD_LOG_MARKERS):
                        return False, data
                    if reboot_count >= 2:
                        return False, data

            log("Requesting FOF_STATUS over USB...")
            ser.write(b"\nFOF_STATUS\n")
            ser.flush()
            data += read_text(ser, 3.0)
    except Exception as exc:
        log(f"serial verify failed: {exc}")
        return False, data

    missing = [marker for marker in PASS_LOG_MARKERS if marker not in data]
    if missing:
        log("Missing required boot markers: " + ", ".join(missing))
        return False, data

    match = re.search(r"FOF_STATUS:(\{.*?\})(?:\r?\n|$)", data, re.DOTALL)
    if not match:
        log("Missing FOF_STATUS JSON response")
        return False, data
    try:
        status = json.loads(match.group(1))
    except json.JSONDecodeError as exc:
        log(f"Invalid FOF_STATUS JSON: {exc}")
        return False, data
    if status.get("mode") not in {"local_ap", "backend", "usb_only"}:
        log("FOF_STATUS JSON missing valid mode")
        return False, data
    log(f"FOF_STATUS ok: mode={status.get('mode')} threat={status.get('threat_score')}")
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
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    port = args.port
    env = args.env
    setup_logging(args.log_dir)

    if not PIO.exists():
        log(f"PlatformIO not found: {PIO}")
        return 2
    if not PENV_PYTHON.exists() or not ESPTOOL.exists():
        log("PlatformIO esptool runtime not found")
        return 2

    if not Path(port).exists():
        log(f"Port not currently present: {port}; catcher will wait for USB replug")
    kill_stale_processes(port)
    usb_reset_esp32s3_jtag()
    if not build_firmware(env):
        return 1

    flashed = False
    if not args.skip_normal_upload and Path(port).exists():
        flashed = normal_upload(env, port)
    elif args.skip_normal_upload:
        log("Skipping normal upload by request.")
    else:
        log("Skipping normal upload because the port is not present.")
    if not flashed:
        timeout = None if args.operator_timeout == 0 else args.operator_timeout
        flashed = no_button_catcher_upload(port, env, timeout)
    if not flashed:
        log("Recovery failed before flashing.")
        return 1

    ok, _serial_log = verify_serial(port, args.verify_seconds)
    if not ok:
        log("Serial verification failed.")
        return 1

    if not args.skip_camera:
        capture_camera(REPO_DIR / "images" / "screenshots" / "badge_camera_check.jpg")

    log("Badge recovery PASS.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
