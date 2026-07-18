#!/usr/bin/env python3
"""Verify release firmware binaries embed the expected FoF version tracks."""

from __future__ import annotations

import argparse
from pathlib import Path

from firmware_version import (
    expected_identity_for_env,
    parse_firmware_identity,
    verify_firmware_images,
)


def release_images(repo_root: Path) -> dict[str, Path]:
    esp32 = repo_root / "esp32"
    return {
        "scanner-s3-combo": esp32 / "scanner/.pio/build/scanner-s3-combo/firmware.bin",
        "scanner-s3-combo-seed": (
            esp32 / "scanner/.pio/build/scanner-s3-combo-seed/firmware.bin"
        ),
        "scanner-s3-combo-fof_badge": (
            esp32 / "scanner/.pio/build/scanner-s3-combo-fof_badge/firmware.bin"
        ),
        "uplink-s3": esp32 / "uplink/.pio/build/uplink-s3/firmware.bin",
        "uplink-s3-fof_badge": (
            esp32 / "uplink/.pio/build/uplink-s3-fof_badge/firmware.bin"
        ),
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", type=Path, default=Path.cwd())
    args = parser.parse_args()

    root = args.repo_root.resolve()
    header = root / "esp32/shared/version.h"
    images = release_images(root)
    errors = verify_firmware_images(header, images)
    if errors:
        for error in errors:
            print(f"ERROR: {error}")
        return 1

    for target, image_path in images.items():
        embedded = parse_firmware_identity(image_path.read_bytes())
        expected = expected_identity_for_env(header, target)
        print(
            f"OK: target={target} project={embedded.project} "
            f"hardware={expected.hardware} version={embedded.version}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
