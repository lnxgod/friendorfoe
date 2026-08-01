#!/usr/bin/env python3
"""Package the hardware-accepted badge bundle for public release channels."""

from __future__ import annotations

import argparse
import hashlib
import shutil
import sys
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
ESP32_SCRIPTS = REPO_ROOT / "esp32" / "scripts"
for import_root in (REPO_ROOT, ESP32_SCRIPTS):
    if str(import_root) not in sys.path:
        sys.path.insert(0, str(import_root))

from firmware_version import read_version_tracks
from tools.badge_flasher.bundles import load_bundle


ACCEPTED_BUNDLE = (
    REPO_ROOT
    / "tools"
    / "badge_flasher"
    / "resources"
    / "badge-factory-flasher-embedded.zip"
)
VERSION_HEADER = REPO_ROOT / "esp32" / "shared" / "version.h"
ROLE_OUTPUTS = {
    "scanner": (
        "badge-scanner",
        {
            "bootloader.bin": "bootloader.bin",
            "partitions.bin": "partition-table.bin",
            "ota_data_initial.bin": "ota-data-initial.bin",
            "firmware.bin": "firmware.bin",
        },
    ),
    "uplink": (
        "badge-uplink",
        {
            "bootloader.bin": "bootloader.bin",
            "partitions.bin": "partition-table.bin",
            "ota_data_initial.bin": "ota-data-initial.bin",
            "firmware.bin": "firmware.bin",
        },
    ),
}


def _sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def package_public_badge_release(
    bundle_path: Path,
    output_root: Path,
    *,
    expected_version: str,
) -> str:
    """Copy the exact validated scanner/uplink images into release layout."""
    bundle_path = Path(bundle_path).resolve()
    output_root = Path(output_root)
    archive_source = bundle_path.is_file()
    bundle = load_bundle(bundle_path, source="accepted public badge release")
    try:
        if bundle.version != expected_version:
            raise ValueError(
                f"accepted bundle version {bundle.version}; "
                f"expected {expected_version}"
            )

        for role, (output_name, filename_map) in ROLE_OUTPUTS.items():
            output_dir = output_root / output_name
            output_dir.mkdir(parents=True, exist_ok=True)
            declared = {
                Path(part["path"]).name: part
                for part in bundle.layout(role)["parts"]
            }
            if set(declared) != set(filename_map):
                raise ValueError(f"accepted {role} bundle layout is incomplete")

            expected_outputs: set[str] = set()
            for source_name, output_filename in filename_map.items():
                part = declared[source_name]
                source = bundle.root / part["path"]
                destination = output_dir / output_filename
                shutil.copyfile(source, destination)
                expected_outputs.add(output_filename)
                if (
                    destination.stat().st_size != part["size"]
                    or _sha256(destination) != part["sha256"]
                ):
                    raise ValueError(
                        f"packaged {role} artifact differs from accepted bundle: "
                        f"{output_filename}"
                    )

            actual_outputs = {
                path.name for path in output_dir.iterdir() if path.is_file()
            }
            if actual_outputs != expected_outputs:
                raise ValueError(
                    f"public {role} release directory contains unexpected files"
                )
        return bundle.version
    finally:
        if archive_source:
            shutil.rmtree(bundle.root, ignore_errors=True)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--bundle", type=Path, default=ACCEPTED_BUNDLE)
    parser.add_argument("--output-root", type=Path, required=True)
    parser.add_argument("--version-header", type=Path, default=VERSION_HEADER)
    args = parser.parse_args()

    expected_version = read_version_tracks(args.version_header)["badge"]
    version = package_public_badge_release(
        args.bundle,
        args.output_root,
        expected_version=expected_version,
    )
    print(f"Packaged accepted public badge firmware {version}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
