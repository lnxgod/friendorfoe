#!/usr/bin/env python3
"""Fail closed unless every backend build produced its sdkconfig artifact."""

from __future__ import annotations

import argparse
from pathlib import Path
import sys


_COMMON = {
    "CONFIG_ESPTOOLPY_FLASHMODE": '"dio"',
    "CONFIG_ESPTOOLPY_FLASHFREQ_80M": "y",
    "CONFIG_PARTITION_TABLE_CUSTOM": "y",
    "CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE": "y",
    "CONFIG_SPIRAM_MODE_OCT": "y",
    "CONFIG_ESP_WIFI_ENABLED": "y",
}

SDKCONFIG_CONTRACTS = {
    Path("scanner/sdkconfig.scanner-s3-combo-backend"): {
        **_COMMON,
        "CONFIG_ESPTOOLPY_FLASHSIZE_8MB": "y",
        "CONFIG_ESPTOOLPY_FLASHSIZE": '"8MB"',
        "CONFIG_ESPTOOLPY_FLASHMODE_DIO": "y",
        "CONFIG_PARTITION_TABLE_CUSTOM_FILENAME": (
            '"partitions_backend_scanner_8mb.csv"'
        ),
        "CONFIG_BT_ENABLED": "y",
        "CONFIG_BT_NIMBLE_ENABLED": "y",
    },
    Path("scanner/sdkconfig.scanner-s3-combo-fullsize-backend"): {
        **_COMMON,
        "CONFIG_ESPTOOLPY_FLASHSIZE_16MB": "y",
        "CONFIG_ESPTOOLPY_FLASHSIZE": '"16MB"',
        "CONFIG_ESPTOOLPY_FLASHMODE_QIO": "y",
        "CONFIG_PARTITION_TABLE_CUSTOM_FILENAME": (
            '"partitions_backend_scanner_fullsize_16mb.csv"'
        ),
        "CONFIG_BT_ENABLED": "y",
        "CONFIG_BT_NIMBLE_ENABLED": "y",
    },
    Path("uplink/sdkconfig.uplink-s3-backend"): {
        **_COMMON,
        "CONFIG_ESPTOOLPY_FLASHSIZE_8MB": "y",
        "CONFIG_ESPTOOLPY_FLASHSIZE": '"8MB"',
        "CONFIG_ESPTOOLPY_FLASHMODE_DIO": "y",
        "CONFIG_PARTITION_TABLE_CUSTOM_FILENAME": (
            '"partitions_backend_uplink_8mb.csv"'
        ),
        "CONFIG_BT_ENABLED": "n",
        "CONFIG_BT_NIMBLE_ENABLED": "n",
    },
    Path("uplink/sdkconfig.uplink-s3-fullsize-backend"): {
        **_COMMON,
        "CONFIG_ESPTOOLPY_FLASHSIZE_16MB": "y",
        "CONFIG_ESPTOOLPY_FLASHSIZE": '"16MB"',
        "CONFIG_ESPTOOLPY_FLASHMODE_QIO": "y",
        "CONFIG_PARTITION_TABLE_CUSTOM_FILENAME": (
            '"partitions_backend_uplink_fullsize_16mb.csv"'
        ),
        "CONFIG_BT_ENABLED": "n",
        "CONFIG_BT_NIMBLE_ENABLED": "n",
    },
}


def _sdkconfig_values(path: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    for raw_line in path.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if line.startswith("# CONFIG_") and line.endswith(" is not set"):
            values[line[len("# ") : -len(" is not set")]] = "n"
            continue
        if not line or line.startswith("#"):
            continue
        key, separator, value = line.partition("=")
        if separator:
            values[key] = value
    return values


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    args = parser.parse_args()

    errors: list[str] = []
    for relative_path, expected_values in SDKCONFIG_CONTRACTS.items():
        path = args.root / relative_path
        if not path.is_file():
            errors.append(f"missing generated sdkconfig: {relative_path}")
            continue
        actual_values = _sdkconfig_values(path)
        for key, expected in expected_values.items():
            missing_value = "n" if expected == "n" else "<missing>"
            actual = actual_values.get(key, missing_value)
            if actual != expected:
                errors.append(
                    "generated sdkconfig mismatch: "
                    f"{relative_path}: {key}: expected {expected}, got {actual}"
                )
    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
