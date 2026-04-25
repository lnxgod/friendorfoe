#!/usr/bin/env python3
"""Friend or Foe — remote scanner flash via uplink UART OTA relay.

Stages a scanner firmware bin to the uplink's flash, then triggers the
uplink→scanner UART relay (v0.59+ staged-handshake protocol). Returns
structured stage/error info so bad flashes surface quickly.

Usage:
    python3 scripts/fof_flash.py <node> [--scanner s3-combo|s3-combo-seed]
                                         [--uart ble|wifi]
                                         [--backend http://HOST:8000]
                                         [--bin /path/to/firmware.bin]

Examples:
    # Flash Pool's BLE scanner with scanner-s3-combo build
    python3 scripts/fof_flash.py pool --scanner s3-combo --uart ble

    # Flash Gate's seed scanner with scanner-s3-combo-seed build
    python3 scripts/fof_flash.py gate --scanner s3-combo-seed --uart ble

The node name is resolved to an IP via the backend's GET /nodes table.
Pass an IP directly (e.g. 192.168.42.201) to skip the lookup.
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

NODE_ALIASES = {
    "pool":      "uplink_CC59FC",
    "area51":    "uplink_03BBD8",
    "frontyard": "uplink_001D6C",
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


def resolve_node_ip(backend, target):
    """Resolve a node alias or device_id to an IP via backend /nodes."""
    if re.match(r"^\d+\.\d+\.\d+\.\d+$", target):
        return target
    device_id = NODE_ALIASES.get(target.lower(), target)
    try:
        data = _http_get_json(f"{backend}/nodes")
    except URLError as e:
        sys.exit(f"ERROR: can't reach backend at {backend}: {e}")
    nodes = data.get("nodes", data.get("items", data if isinstance(data, list) else []))
    for n in nodes:
        if n.get("device_id") == device_id:
            ip = n.get("last_ip") or n.get("ip") or n.get("static_ip")
            if ip:
                return ip
    # Fallback — hardcoded LAN map for known fleet
    fallback = {
        "uplink_CC59FC": "192.168.42.201",
        "uplink_03BBD8": "192.168.42.170",
        "uplink_001D6C": "192.168.42.213",
        "uplink_9AB838": "192.168.42.59",
        "uplink_E3A56C": "192.168.42.169",
    }
    if device_id in fallback:
        return fallback[device_id]
    sys.exit(f"ERROR: couldn't resolve {target!r} to an IP")


def stage_firmware(node_ip, bin_path, version="v0.59.0"):
    url = f"http://{node_ip}/api/fw/upload?version={version}"
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


def flash_scanner(node_ip, uart="ble"):
    url = f"http://{node_ip}/api/fw/relay?uart={uart}"
    print(f"[flash] POST {url}")
    t0 = time.time()
    try:
        status, body = _http_post(url, data=b"", timeout=900)
    except HTTPError as e:
        sys.exit(f"[flash] HTTP {e.code}: {e.read().decode('utf-8', 'replace')}")
    elapsed = time.time() - t0
    if not isinstance(body, dict):
        sys.exit(f"[flash] Unexpected response: {body}")
    if body.get("ok"):
        print(f"[flash] OK — {body.get('chunks')} chunks, "
              f"{body.get('nacks', 0)} nacks, {body.get('retries', 0)} retries, "
              f"{body.get('elapsed_s', elapsed)}s")
        print(f"[flash] Scanner is rebooting into the new firmware.")
        return True
    else:
        stage = body.get("stage", "?")
        err = body.get("error", "?")
        print(f"[flash] FAILED @ stage={stage}: {err}")
        print(f"[flash]   chunks={body.get('chunks')} nacks={body.get('nacks')} "
              f"retries={body.get('retries')} elapsed={body.get('elapsed_s')}s")
        return False


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("node", help="Node alias (pool|area51|frontyard|spare|lamb), device_id, or IP")
    p.add_argument("--scanner", choices=list(FW_PATHS.keys()), default="s3-combo",
                   help="Scanner firmware variant (default: s3-combo)")
    p.add_argument("--uart", choices=("ble", "wifi"), default="ble",
                   help="UART slot on the uplink (default: ble)")
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

    ip = resolve_node_ip(args.backend, args.node)
    print(f"[resolve] {args.node!r} → {ip}")

    if not args.skip_stage:
        stage_firmware(ip, bin_path)

    ok = flash_scanner(ip, uart=args.uart)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
