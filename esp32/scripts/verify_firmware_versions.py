#!/usr/bin/env python3
"""Verify release firmware binaries embed the expected FoF version tracks."""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import sys
import zipfile
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tools.badge_flasher.bundles import BundleError, load_bundle

from firmware_version import (
    expected_identity_for_env,
    parse_firmware_identity,
    verify_firmware_images,
)


WEB_FLASHER_TARGETS = {
    "scanner-s3-combo": {
        "manifest": "manifest-scanner.json",
        "firmware_dir": "firmware/scanner",
        "parts": (
            ("bootloader.bin", 0),
            ("partition-table.bin", 32768),
            ("firmware.bin", 131072),
        ),
    },
    "scanner-s3-combo-seed": {
        "manifest": "manifest-scanner-seed.json",
        "firmware_dir": "firmware/scanner-seed",
        "parts": (
            ("bootloader.bin", 0),
            ("partition-table.bin", 32768),
            ("firmware.bin", 131072),
        ),
    },
    "uplink-s3": {
        "manifest": "manifest-uplink-s3.json",
        "firmware_dir": "firmware/uplink-s3",
        "parts": (
            ("bootloader.bin", 0),
            ("partition-table.bin", 32768),
            ("firmware.bin", 131072),
        ),
    },
    "scanner-s3-combo-fof_badge": {
        "manifest": "manifest-badge-scanner.json",
        "firmware_dir": "firmware/badge-scanner",
        "parts": (
            ("bootloader.bin", 0),
            ("partition-table.bin", 32768),
            ("ota-data-initial.bin", 61440),
            ("firmware.bin", 131072),
        ),
    },
    "uplink-s3-fof_badge": {
        "manifest": "manifest-badge-uplink.json",
        "firmware_dir": "firmware/badge-uplink",
        "parts": (
            ("bootloader.bin", 0),
            ("partition-table.bin", 32768),
            ("ota-data-initial.bin", 61440),
            ("firmware.bin", 131072),
        ),
    },
}

BADGE_BUNDLE_ROLES = {
    "scanner": "scanner-s3-combo-fof_badge",
    "uplink": "uplink-s3-fof_badge",
}
BADGE_BUNDLE_OUTPUT_NAMES = {
    "bootloader.bin": "bootloader.bin",
    "partitions.bin": "partition-table.bin",
    "ota_data_initial.bin": "ota-data-initial.bin",
    "firmware.bin": "firmware.bin",
}


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


def site_release_images(site_root: Path) -> dict[str, Path]:
    return {
        target: site_root / spec["firmware_dir"] / "firmware.bin"
        for target, spec in WEB_FLASHER_TARGETS.items()
    }


def verify_accepted_badge_promotion(
    version_header: Path,
    manifest_root: Path,
    accepted_bundle_path: Path,
) -> list[str]:
    """Require public badge metadata to name the exact accepted bundle."""
    errors: list[str] = []
    accepted_bundle_path = Path(accepted_bundle_path)
    archive_source = accepted_bundle_path.is_file()
    try:
        bundle = load_bundle(
            accepted_bundle_path,
            source="accepted public badge release",
        )
    except (BundleError, OSError, zipfile.BadZipFile) as exc:
        return [f"accepted badge bundle is invalid: {exc}"]

    try:
        for role, target in BADGE_BUNDLE_ROLES.items():
            expected = expected_identity_for_env(version_header, target)
            identity = bundle.layout(role)["identity"]
            if bundle.version != expected.version:
                errors.append(
                    f"{target}: accepted bundle version {bundle.version!r}, "
                    f"expected public version {expected.version!r}"
                )
            if identity.get("version") != expected.version:
                errors.append(
                    f"{target}: accepted identity version "
                    f"{identity.get('version')!r}, expected {expected.version!r}"
                )

            manifest_path = manifest_root / WEB_FLASHER_TARGETS[target]["manifest"]
            try:
                manifest = json.loads(manifest_path.read_text())
            except (OSError, json.JSONDecodeError) as exc:
                errors.append(
                    f"{target}: cannot read public manifest {manifest_path}: {exc}"
                )
                continue
            if not isinstance(manifest, dict):
                errors.append(f"{target}: public manifest must be an object")
            elif manifest.get("version") != bundle.version:
                errors.append(
                    f"{target}: public manifest version "
                    f"{manifest.get('version')!r}, accepted {bundle.version!r}"
                )
    finally:
        if archive_source:
            shutil.rmtree(bundle.root, ignore_errors=True)
    return errors


def _verify_site_badge_artifacts(
    site_root: Path,
    accepted_bundle_path: Path,
) -> list[str]:
    errors: list[str] = []
    accepted_bundle_path = Path(accepted_bundle_path)
    archive_source = accepted_bundle_path.is_file()
    try:
        bundle = load_bundle(
            accepted_bundle_path,
            source="accepted public badge release",
        )
    except (BundleError, OSError, zipfile.BadZipFile) as exc:
        return [f"accepted badge bundle is invalid: {exc}"]

    try:
        for role, target in BADGE_BUNDLE_ROLES.items():
            firmware_dir = WEB_FLASHER_TARGETS[target]["firmware_dir"]
            for part in bundle.layout(role)["parts"]:
                source_name = Path(part["path"]).name
                output_name = BADGE_BUNDLE_OUTPUT_NAMES[source_name]
                artifact = site_root / firmware_dir / output_name
                try:
                    payload = artifact.read_bytes()
                except OSError as exc:
                    errors.append(f"{target}: cannot read {artifact}: {exc}")
                    continue
                digest = hashlib.sha256(payload).hexdigest()
                if len(payload) != part["size"] or digest != part["sha256"]:
                    errors.append(
                        f"{target}: {output_name} differs from the "
                        "hardware-accepted badge bundle"
                    )
    finally:
        if archive_source:
            shutil.rmtree(bundle.root, ignore_errors=True)
    return errors


def verify_web_flasher_site(
    version_header: Path,
    site_root: Path,
    accepted_badge_bundle: Path | None = None,
) -> list[str]:
    """Fail closed unless Pages contains one exact five-target release set."""
    errors = verify_firmware_images(
        version_header,
        site_release_images(site_root),
    )
    for target, spec in WEB_FLASHER_TARGETS.items():
        expected_parts = spec["parts"]
        expected = expected_identity_for_env(version_header, target)
        manifest_path = site_root / spec["manifest"]
        try:
            manifest = json.loads(manifest_path.read_text())
        except (OSError, json.JSONDecodeError) as exc:
            errors.append(f"{target}: cannot read manifest {manifest_path}: {exc}")
            continue
        if not isinstance(manifest, dict):
            errors.append(f"{target}: manifest must be an object")
            continue
        if manifest.get("version") != expected.version:
            errors.append(
                f"{target}: manifest version {manifest.get('version')!r}, "
                f"expected {expected.version!r}"
            )

        builds = manifest.get("builds")
        if not isinstance(builds, list) or len(builds) != 1 or not isinstance(builds[0], dict):
            errors.append(f"{target}: manifest must contain exactly one build")
            continue
        build = builds[0]
        if build.get("chipFamily") != "ESP32-S3":
            errors.append(f"{target}: manifest chipFamily must be ESP32-S3")
        parts = build.get("parts")
        if not isinstance(parts, list) or len(parts) != len(expected_parts):
            errors.append(
                f"{target}: manifest must contain exactly "
                f"{len(expected_parts)} parts"
            )
            continue

        expected_dir = spec["firmware_dir"]
        for part, (filename, offset) in zip(parts, expected_parts, strict=True):
            expected_path = f"{expected_dir}/{filename}"
            if not isinstance(part, dict) or part.get("path") != expected_path or part.get("offset") != offset:
                errors.append(
                    f"{target}: manifest part must be "
                    f"path={expected_path!r} offset={offset}"
                )
                continue
            artifact = site_root / expected_path
            try:
                if artifact.stat().st_size <= 0:
                    errors.append(f"{target}: empty firmware part {artifact}")
            except OSError as exc:
                errors.append(f"{target}: cannot read firmware part {artifact}: {exc}")
    if accepted_badge_bundle is not None:
        errors.extend(verify_accepted_badge_promotion(
            version_header,
            site_root,
            accepted_badge_bundle,
        ))
        errors.extend(_verify_site_badge_artifacts(
            site_root,
            accepted_badge_bundle,
        ))
    return errors


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", type=Path, default=Path.cwd())
    parser.add_argument(
        "--site-root",
        type=Path,
        help="verify a packaged Pages tree and all five manifests",
    )
    args = parser.parse_args()

    root = args.repo_root.resolve()
    header = root / "esp32/shared/version.h"
    accepted_badge_bundle = (
        root
        / "tools/badge_flasher/resources/badge-factory-flasher-embedded.zip"
    )
    site_root = args.site_root.resolve() if args.site_root else None
    images = site_release_images(site_root) if site_root else release_images(root)
    if site_root:
        errors = verify_web_flasher_site(
            header,
            site_root,
            accepted_badge_bundle,
        )
    else:
        errors = verify_accepted_badge_promotion(
            header,
            root / "esp32/web-flasher",
            accepted_badge_bundle,
        )
        errors.extend(verify_firmware_images(header, images))
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
