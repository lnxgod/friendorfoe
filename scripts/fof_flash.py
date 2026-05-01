#!/usr/bin/env python3
"""Friend or Foe — remote scanner flash via uplink UART OTA relay.

Stages a scanner firmware bin to the uplink's flash, then triggers the
uplink→scanner UART relay (v0.59+ staged-handshake protocol). Returns
structured stage/error info so bad flashes surface quickly.

Usage:
    python3 scripts/fof_flash.py <node> [--scanner s3-combo|s3-combo-seed]
                                         [--uart ble|wifi]
                                         [--relay-mode auto|staged|staged-legacy|direct-legacy]
                                         [--backend http://HOST:8000]
                                         [--bin /path/to/firmware.bin]

Examples:
    # Flash Pool's BLE scanner with scanner-s3-combo build
    python3 scripts/fof_flash.py pool --scanner s3-combo --uart ble

    # Flash Gate's seed scanner with scanner-s3-combo-seed build
    python3 scripts/fof_flash.py gate --scanner s3-combo-seed --uart ble

The node name is resolved to an IP via the backend's GET /nodes table.
Pass an IP directly (e.g. 192.168.1.42) to skip the lookup.
"""

import argparse
import json
import os
import re
import sys
import time
from pathlib import Path
from urllib.request import Request, urlopen
from urllib.error import HTTPError, URLError

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_BACKEND = os.environ.get("FOF_BACKEND", "http://localhost:8000")

FW_PATHS = {
    "s3-combo":      REPO_ROOT / "esp32/scanner/.pio/build/scanner-s3-combo/firmware.bin",
    "s3-combo-seed": REPO_ROOT / "esp32/scanner/.pio/build/scanner-s3-combo-seed/firmware.bin",
}

FW_NAMES = {
    "s3-combo": "scanner-s3-combo",
    "s3-combo-seed": "scanner-s3-combo-seed",
}

NODE_ALIASES = {
    "pool":      "uplink_CC59FC",
    "area51":    "uplink_24DBB4",
    "chomper":   "uplink_CB77A4",
    "frontyard": "uplink_CC558C",
    "patio":     "uplink_D0A6AC",
    "spare":     "uplink_9AB838",
    "lamb":      "uplink_E3A56C",
    "gate":      "uplink_D0A148",
}


def _http_get_json(url, timeout=10):
    with urlopen(Request(url), timeout=timeout) as r:
        return json.loads(r.read().decode("utf-8"))


def _http_post(url, data=None, timeout=600, content_type="application/octet-stream"):
    req = Request(url, data=data, method="POST")
    if data is not None and content_type:
        req.add_header("Content-Type", content_type)
    with urlopen(req, timeout=timeout) as r:
        body = r.read().decode("utf-8")
        try:
            return r.status, json.loads(body)
        except ValueError:
            return r.status, body


def resolve_node(backend, target):
    """Resolve a node alias or device_id to (device_id, ip) via backend /nodes."""
    if re.match(r"^\d+\.\d+\.\d+\.\d+$", target):
        return target, target
    device_id = NODE_ALIASES.get(target.lower(), target)
    try:
        data = _http_get_json(f"{backend}/detections/nodes/status")
        for n in data.get("nodes", []):
            if n.get("device_id") == device_id:
                ip = n.get("ip")
                if ip:
                    return device_id, ip
    except URLError:
        pass
    try:
        data = _http_get_json(f"{backend}/nodes")
    except URLError as e:
        sys.exit(f"ERROR: can't reach backend at {backend}: {e}")
    nodes = data.get("nodes", data.get("items", data if isinstance(data, list) else []))
    for n in nodes:
        if n.get("device_id") == device_id:
            ip = n.get("last_ip") or n.get("ip") or n.get("static_ip")
            if ip:
                return device_id, ip
    # Fallback — operator-local device_id → static IP map. Loaded from
    # `scripts/fof_flash.local.json` (gitignored). See
    # `scripts/fof_flash.local.json.example` for the file layout. Empty
    # dict if the file is missing, which is the right behaviour for a
    # fresh clone (we just rely on the backend lookup above).
    fallback_path = REPO_ROOT / "scripts" / "fof_flash.local.json"
    fallback: dict[str, str] = {}
    if fallback_path.exists():
        try:
            fallback = json.loads(fallback_path.read_text()).get("device_ip", {})
        except (json.JSONDecodeError, OSError) as e:
            print(f"[warn] {fallback_path.name}: {e} — ignoring", file=sys.stderr)
    if device_id in fallback:
        return device_id, fallback[device_id]
    sys.exit(f"ERROR: couldn't resolve {target!r} to an IP")


def resolve_node_ip(backend, target):
    """Backward-compatible resolver for callers that only need the IP."""
    return resolve_node(backend, target)[1]


def current_repo_firmware_version():
    version_h = REPO_ROOT / "esp32/shared/version.h"
    m = re.search(r'#define\s+FOF_VERSION\s+"([^"]+)"', version_h.read_text())
    return m.group(1) if m else "unknown"


def stage_firmware(node_ip, bin_path, firmware_name, version=None):
    from urllib.parse import urlencode
    query = urlencode({
        "name": firmware_name,
        "version": version or current_repo_firmware_version(),
    })
    url = f"http://{node_ip}/api/fw/upload?{query}"
    size = bin_path.stat().st_size
    print(f"[stage] POST {url} ({size} bytes)")
    t0 = time.time()
    with bin_path.open("rb") as f:
        data = f.read()
    status, body = _http_post(url, data=data, timeout=600)
    elapsed = time.time() - t0
    if status != 200 or not isinstance(body, dict) or not body.get("ok"):
        sys.exit(f"[stage] FAILED status={status} body={body}")
    print(f"[stage] OK — {body.get('size')} bytes, partition={body.get('partition')}, "
          f"checksum={body.get('checksum')}, {elapsed:.1f}s")
    return body


def _scanner_version_from_status(backend, device_id, node_ip, uart):
    try:
        data = _http_get_json(f"{backend}/detections/nodes/status", timeout=10)
    except Exception:
        return None
    for node in data.get("nodes", []):
        if node.get("device_id") != device_id and node.get("ip") != node_ip:
            continue
        for scanner in node.get("scanners", []):
            if scanner.get("uart") == uart:
                return scanner.get("ver") or scanner.get("firmware_version") or scanner.get("version")
    return None


def _norm_version(value):
    value = (value or "").strip()
    return value[1:] if value[:1].lower() == "v" else value


def wait_for_scanner_version(backend, device_id, node_ip, uart, target_version, timeout=90):
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        last = _scanner_version_from_status(backend, device_id, node_ip, uart)
        if _norm_version(last) == _norm_version(target_version):
            print(f"[verify] OK — {uart} scanner reports {last}")
            return True
        time.sleep(3)
    print(f"[verify] FAILED — {uart} scanner still reports {last or 'unknown'}, wanted {target_version}")
    return False


def _flash_once(node_ip, uart, relay_mode, bin_path):
    if relay_mode == "direct-legacy":
        url = f"http://{node_ip}/api/ota/relay?uart={uart}&legacy=1"
        with bin_path.open("rb") as f:
            data = f.read()
        timeout = 900
    else:
        url = f"http://{node_ip}/api/fw/relay?uart={uart}"
        if relay_mode == "staged-legacy":
            url += "&legacy=1"
        data = b""
        timeout = 900
    print(f"[flash] POST {url}")
    t0 = time.time()
    try:
        status, body = _http_post(url, data=data, timeout=timeout)
    except HTTPError as e:
        sys.exit(f"[flash] HTTP {e.code}: {e.read().decode('utf-8', 'replace')}")
    elapsed = time.time() - t0
    if not isinstance(body, dict):
        sys.exit(f"[flash] Unexpected response: {body}")
    if body.get("ok"):
        print(f"[flash] OK — {body.get('chunks')} chunks, "
              f"{body.get('nacks', 0)} nacks, {body.get('retries', 0)} retries, "
              f"{body.get('elapsed_s', elapsed)}s")
        if body.get("scanner_response"):
            print(f"[flash] scanner_response={body.get('scanner_response')}")
        print(f"[flash] Transfer complete; waiting for heartbeat version proof.")
        return True, body
    else:
        stage = body.get("stage", "?")
        err = body.get("error", "?")
        print(f"[flash] FAILED @ stage={stage}: {err}")
        print(f"[flash]   chunks={body.get('chunks')} nacks={body.get('nacks')} "
              f"retries={body.get('retries')} elapsed={body.get('elapsed_s')}s")
        return False, body


def flash_scanner(node_ip, device_id, backend, uart, relay_mode, bin_path, target_version):
    modes = [relay_mode]
    if relay_mode == "auto":
        modes = ["staged", "staged-legacy", "direct-legacy"]

    last_body = None
    for mode in modes:
        ok, body = _flash_once(node_ip, uart, mode, bin_path)
        last_body = body
        if not ok:
            continue
        if wait_for_scanner_version(backend, device_id, node_ip, uart, target_version):
            return True
        print(f"[flash] {mode} transfer was not version-verified; treating as failed.")
    print(f"[flash] All relay modes failed or were unverified. Last response: {last_body}")
    return False


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("node", help="Node alias (pool|area51|frontyard|spare|lamb), device_id, or IP")
    p.add_argument("--scanner", choices=list(FW_PATHS.keys()), default="s3-combo",
                   help="Scanner firmware variant (default: s3-combo)")
    p.add_argument("--uart", choices=("ble", "wifi"), default="ble",
                   help="UART slot on the uplink (default: ble)")
    p.add_argument("--relay-mode", choices=("auto", "staged", "staged-legacy", "direct-legacy"),
                   default="auto",
                   help="Relay strategy (default: auto, with version verification)")
    p.add_argument("--backend", default=DEFAULT_BACKEND,
                   help=f"Backend URL (default: {DEFAULT_BACKEND})")
    p.add_argument("--bin", type=Path, default=None,
                   help="Override path to firmware.bin (default: auto from --scanner)")
    p.add_argument("--skip-stage", action="store_true",
                   help="Skip upload step (firmware already staged)")
    args = p.parse_args()

    bin_path = args.bin or FW_PATHS[args.scanner]
    if not bin_path.exists():
        sys.exit(f"ERROR: firmware not found at {bin_path}\n"
                 f"       Build it first: cd esp32/scanner && pio run -e scanner-{args.scanner}")

    device_id, ip = resolve_node(args.backend, args.node)
    target_version = current_repo_firmware_version()
    print(f"[resolve] {args.node!r} → {device_id} / {ip}")

    if not args.skip_stage and args.relay_mode != "direct-legacy":
        stage_firmware(ip, bin_path, FW_NAMES[args.scanner])

    ok = flash_scanner(ip, device_id, args.backend, args.uart,
                       args.relay_mode, bin_path, target_version)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
