#!/usr/bin/env python3
"""Shared ESP32 release-identity parsing and verification helpers."""

from __future__ import annotations

import argparse
import json
import re
import struct
import sys
from pathlib import Path
from typing import Mapping, NamedTuple


_VERSION_MACROS = {
    "production": "FOF_VERSION_PROD",
    "badge": "FOF_VERSION_BADGE",
    "badge_canary": "FOF_VERSION_BADGE_CANARY",
}
_APP_DESC_OFFSET = 0x20
_APP_DESC_MIN_SIZE = 112
_APP_DESC_MAGIC = 0xABCD5432
_ESP_IMAGE_MAGIC = 0xE9


class FirmwareIdentity(NamedTuple):
    """Identity embedded in the ESP-IDF application descriptor."""

    project: str
    version: str


class TargetIdentity(NamedTuple):
    """Expected immutable release identity for one PlatformIO environment."""

    target: str
    project: str
    hardware: str
    version: str


_TARGET_PROJECT_HARDWARE_TRACK = {
    "scanner-s3-combo": ("fof_scanner", "esp32-s3-devkitc-1", "production"),
    "scanner-s3-combo-seed": (
        "fof_scanner_seed",
        "esp32-s3-devkitc-1",
        "production",
    ),
    "scanner-s3-combo-fof_badge": (
        "fof_badge_scanner",
        "seeed_xiao_esp32s3",
        "badge",
    ),
    "scanner-s3-combo-fof_badge-con-crud-canary": (
        "fof_badge_scanner",
        "seeed_xiao_esp32s3",
        "badge_canary",
    ),
    "uplink-s3": ("fof_uplink", "esp32-s3-devkitc-1", "production"),
    "uplink-s3-fof_badge": (
        "fof_badge_uplink",
        "seeed_xiao_esp32s3",
        "badge",
    ),
    "uplink-s3-fof_badge-con-crud-canary": (
        "fof_badge_uplink",
        "seeed_xiao_esp32s3",
        "badge_canary",
    ),
}
_RUNTIME_TARGET_BY_ENV = {
    "scanner-s3-combo-fof_badge-con-crud-canary": (
        "scanner-s3-combo-fof_badge"
    ),
    "uplink-s3-fof_badge-con-crud-canary": "uplink-s3-fof_badge",
}


def read_version_tracks(header: Path) -> dict[str, str]:
    text = header.read_text()
    versions: dict[str, str] = {}
    for track, macro in _VERSION_MACROS.items():
        match = re.search(
            rf'^\s*#define\s+{re.escape(macro)}\s+"([^"]+)"\s*$',
            text,
            re.MULTILINE,
        )
        if not match:
            raise ValueError(f"Missing {macro} in {header}")
        versions[track] = match.group(1)
    return versions


def expected_version_for_env(header: Path, environment: str) -> str:
    versions = read_version_tracks(header)
    mapped = _TARGET_PROJECT_HARDWARE_TRACK.get(environment)
    track = mapped[2] if mapped else (
        "badge" if "fof_badge" in environment else "production"
    )
    return versions[track]


def expected_identity_for_env(header: Path, environment: str) -> TargetIdentity:
    try:
        project, hardware, track = _TARGET_PROJECT_HARDWARE_TRACK[environment]
    except KeyError as exc:
        raise ValueError(f"Unknown firmware target: {environment}") from exc
    versions = read_version_tracks(header)
    return TargetIdentity(environment, project, hardware, versions[track])


def runtime_target_for_env(environment: str) -> str:
    if environment not in _TARGET_PROJECT_HARDWARE_TRACK:
        raise ValueError(f"Unknown firmware target: {environment}")
    return _RUNTIME_TARGET_BY_ENV.get(environment, environment)


def invalidate_stale_cmake_cache(build_dir: Path, expected_version: str) -> bool:
    """Force PlatformIO to reconfigure CMake when PROJECT_VER is stale."""
    cache = build_dir / "CMakeCache.txt"
    if not cache.exists():
        return False

    current_version: str | None = None
    description = build_dir / "project_description.json"
    try:
        payload = json.loads(description.read_text())
        if isinstance(payload, dict):
            value = payload.get("project_version")
            if isinstance(value, str):
                current_version = value
    except (OSError, json.JSONDecodeError):
        pass

    if current_version == expected_version:
        return False

    cache.unlink()
    return True


def _parse_descriptor_ascii(raw: bytes) -> str | None:
    value = raw.split(b"\x00", 1)[0]
    try:
        decoded = value.decode("ascii")
    except UnicodeDecodeError:
        return None
    if not decoded or decoded != decoded.strip() or any(char.isspace() for char in decoded):
        return None
    return decoded


def parse_firmware_identity(image: bytes) -> FirmwareIdentity | None:
    if len(image) < _APP_DESC_OFFSET + _APP_DESC_MIN_SIZE:
        return None
    if image[0] != _ESP_IMAGE_MAGIC:
        return None

    description = image[_APP_DESC_OFFSET:_APP_DESC_OFFSET + _APP_DESC_MIN_SIZE]
    if struct.unpack_from("<I", description)[0] != _APP_DESC_MAGIC:
        return None

    version = _parse_descriptor_ascii(description[16:48])
    project = _parse_descriptor_ascii(description[48:80])
    if version is None or project is None:
        return None
    return FirmwareIdentity(project=project, version=version)


def parse_app_desc_version(image: bytes) -> str | None:
    """Backward-compatible version-only descriptor helper."""
    identity = parse_firmware_identity(image)
    return identity.version if identity else None


def _missing_identity_marker(image: bytes, expected: TargetIdentity) -> str | None:
    runtime_target = runtime_target_for_env(expected.target)
    if runtime_target.encode("ascii") not in image:
        return f'target marker "{runtime_target}"'
    if expected.hardware.encode("ascii") not in image:
        return f'hardware marker "{expected.hardware}"'
    return None


def verify_firmware_images(
    version_header: Path,
    images: Mapping[str, Path],
) -> list[str]:
    errors: list[str] = []
    for target, image_path in images.items():
        try:
            expected = expected_identity_for_env(version_header, target)
        except ValueError as exc:
            errors.append(str(exc))
            continue
        try:
            image = image_path.read_bytes()
        except OSError as exc:
            errors.append(f"{target}: cannot read {image_path}: {exc}")
            continue
        embedded = parse_firmware_identity(image)
        if embedded is None:
            errors.append(
                f"{target}: invalid or missing embedded app identity in {image_path}"
            )
        elif embedded.project != expected.project:
            errors.append(
                f"{target}: embedded project {embedded.project}, "
                f"expected {expected.project}"
            )
        elif embedded.version != expected.version:
            errors.append(
                f"{target}: embedded version {embedded.version}, "
                f"expected {expected.version}"
            )
        elif missing := _missing_identity_marker(image, expected):
            errors.append(f"{target}: missing {missing} in {image_path}")
    return errors


def _expected_for_project(
    header: Path,
    project: str,
    version: str,
) -> TargetIdentity | None:
    for target in _TARGET_PROJECT_HARDWARE_TRACK:
        identity = expected_identity_for_env(header, target)
        if identity.project == project and identity.version == version:
            return identity
    return None


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Inspect an ESP32 application image release identity"
    )
    parser.add_argument("image", type=Path)
    parser.add_argument("--json", action="store_true", help="print the full identity")
    parser.add_argument(
        "--version-header",
        type=Path,
        default=Path(__file__).resolve().parents[1] / "shared" / "version.h",
    )
    args = parser.parse_args()

    try:
        image = args.image.read_bytes()
    except OSError as exc:
        print(f"ERROR: cannot read {args.image}: {exc}", file=sys.stderr)
        return 1
    embedded = parse_firmware_identity(image)
    if embedded is None:
        print(f"ERROR: invalid or missing app identity in {args.image}", file=sys.stderr)
        return 1

    if not args.json:
        print(embedded.version)
        return 0

    expected = _expected_for_project(
        args.version_header,
        embedded.project,
        embedded.version,
    )
    if expected is None:
        print(f"ERROR: unknown embedded project {embedded.project}", file=sys.stderr)
        return 1
    if embedded.version != expected.version:
        print(
            f"ERROR: {expected.target}: embedded version {embedded.version}, "
            f"expected {expected.version}",
            file=sys.stderr,
        )
        return 1
    if missing := _missing_identity_marker(image, expected):
        print(f"ERROR: {expected.target}: missing {missing}", file=sys.stderr)
        return 1
    print(json.dumps({
        "target": runtime_target_for_env(expected.target),
        "project": embedded.project,
        "hardware": expected.hardware,
        "version": embedded.version,
    }, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
