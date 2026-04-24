#!/usr/bin/env python3
"""Run the local preflight checks we expect before touching live code.

Examples:
  python3 scripts/preflight.py backend
  python3 scripts/preflight.py esp32-native esp32-live-builds
  python3 scripts/preflight.py all
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]


def _which(*candidates: str) -> str:
    for candidate in candidates:
        if not candidate:
            continue
        if Path(candidate).exists():
            return candidate
        resolved = shutil.which(candidate)
        if resolved:
            return resolved
    raise FileNotFoundError(f"None of these executables were found: {', '.join(candidates)}")


def _run(cmd: list[str], *, cwd: Path) -> None:
    print(f"\n==> ({cwd}) {' '.join(cmd)}", flush=True)
    subprocess.run(cmd, cwd=str(cwd), check=True)


def run_backend() -> None:
    pytest_bin = _which(
        str(REPO_ROOT / "backend/.venv/bin/pytest"),
        "pytest",
    )
    _run([pytest_bin, "tests", "-q"], cwd=REPO_ROOT / "backend")


def run_esp32_native() -> None:
    pio_bin = _which(
        str(REPO_ROOT / "esp32/.venv312/bin/pio"),
        "pio",
    )
    _run([pio_bin, "test", "-e", "test"], cwd=REPO_ROOT / "esp32")


def run_esp32_live_builds() -> None:
    pio_bin = _which(
        str(REPO_ROOT / "esp32/.venv312/bin/pio"),
        "pio",
    )
    for env in ("scanner-s3-combo", "scanner-s3-combo-seed", "uplink-s3"):
        workdir = REPO_ROOT / ("esp32/scanner" if env.startswith("scanner") else "esp32/uplink")
        _run([pio_bin, "run", "-e", env], cwd=workdir)


PROFILES: dict[str, tuple[str, callable]] = {
    "backend": ("Backend pytest suite", run_backend),
    "esp32-native": ("ESP32 native unit tests", run_esp32_native),
    "esp32-live-builds": ("Live fleet firmware builds", run_esp32_live_builds),
}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "profiles",
        nargs="*",
        default=["backend", "esp32-native", "esp32-live-builds"],
        help="Profiles to run: backend, esp32-native, esp32-live-builds, all",
    )
    args = parser.parse_args()

    requested = args.profiles
    if "all" in requested:
        requested = list(PROFILES)

    for name in requested:
        if name not in PROFILES:
            parser.error(f"unknown profile '{name}'")

    print("Friend or Foe preflight")
    for name in requested:
        print(f" - {name}: {PROFILES[name][0]}")

    try:
        for name in requested:
            PROFILES[name][1]()
    except (subprocess.CalledProcessError, FileNotFoundError) as exc:
        print(f"\nPreflight failed: {exc}", file=sys.stderr)
        return 1

    print("\nPreflight passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
