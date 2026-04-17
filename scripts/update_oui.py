#!/usr/bin/env python3
"""Manually refresh backend/app/data/manuf.txt from Wireshark's upstream.

Run when you want a fresh copy of the IEEE OUI registry + vendor names.
Wireshark publishes this weekly; commit the updated file alongside code.

Usage:
    python3 scripts/update_oui.py
"""
from __future__ import annotations

import sys
import time
from pathlib import Path
from urllib.request import urlopen

URL = "https://www.wireshark.org/download/automated/data/manuf"
REPO_ROOT = Path(__file__).resolve().parent.parent
TARGET = REPO_ROOT / "backend" / "app" / "data" / "manuf.txt"


def main() -> int:
    TARGET.parent.mkdir(parents=True, exist_ok=True)
    tmp = TARGET.with_suffix(".txt.new")
    print(f"downloading {URL} …")
    t0 = time.time()
    try:
        with urlopen(URL, timeout=60) as resp:
            data = resp.read()
    except Exception as e:
        print(f"ERROR: download failed: {e}")
        return 1
    if len(data) < 100_000:
        print(f"ERROR: refusing to write a {len(data)}-byte file — likely bad fetch")
        return 1
    tmp.write_bytes(data)
    lines = data.count(b"\n")
    tmp.replace(TARGET)
    print(f"wrote {TARGET} ({len(data):,} bytes, {lines:,} lines) in {time.time()-t0:.1f}s")
    print("remember to restart the backend or POST /admin/oui/refresh to pick up the changes.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
