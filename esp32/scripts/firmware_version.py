"""Shared ESP32 build-version parsing and verification helpers."""

from __future__ import annotations

import json
import re
import struct
from pathlib import Path
from typing import Mapping


_VERSION_MACROS = {
    "production": "FOF_VERSION_PROD",
    "badge": "FOF_VERSION_BADGE",
}
_APP_DESC_OFFSET = 0x20
_APP_DESC_MIN_SIZE = 112
_APP_DESC_MAGIC = 0xABCD5432
_ESP_IMAGE_MAGIC = 0xE9


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
    track = "badge" if "fof_badge" in environment else "production"
    return versions[track]


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


def parse_app_desc_version(image: bytes) -> str | None:
    if len(image) < _APP_DESC_OFFSET + _APP_DESC_MIN_SIZE:
        return None
    if image[0] != _ESP_IMAGE_MAGIC:
        return None

    description = image[_APP_DESC_OFFSET:_APP_DESC_OFFSET + _APP_DESC_MIN_SIZE]
    if struct.unpack_from("<I", description)[0] != _APP_DESC_MAGIC:
        return None

    raw_version = description[16:48].split(b"\x00", 1)[0]
    try:
        version = raw_version.decode("ascii")
    except UnicodeDecodeError:
        return None
    if not version or version != version.strip() or any(char.isspace() for char in version):
        return None
    return version


def verify_firmware_images(
    version_header: Path,
    images: Mapping[str, Path],
) -> list[str]:
    errors: list[str] = []
    for target, image_path in images.items():
        expected = expected_version_for_env(version_header, target)
        try:
            embedded = parse_app_desc_version(image_path.read_bytes())
        except OSError as exc:
            errors.append(f"{target}: cannot read {image_path}: {exc}")
            continue
        if embedded is None:
            errors.append(f"{target}: invalid or missing embedded app version in {image_path}")
        elif embedded != expected:
            errors.append(f"{target}: embedded {embedded}, expected {expected}")
    return errors
