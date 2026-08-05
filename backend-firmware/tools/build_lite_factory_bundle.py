#!/usr/bin/env python3
"""Build the deterministic Backend Badge Lite factory ZIP."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import sys
import tempfile
import zipfile
from pathlib import Path
from typing import Any


BACKEND_ROOT = Path(__file__).resolve().parents[1]
REPOSITORY_ROOT = BACKEND_ROOT.parent
for candidate in (REPOSITORY_ROOT, BACKEND_ROOT):
    if str(candidate) not in sys.path:
        sys.path.insert(0, str(candidate))

from tools.firmware_identity import parse_esp_app_identity  # noqa: E402
from tools.badge_flasher.bundles import load_bundle as load_badge_bundle  # noqa: E402
from tools.lite_factory_flasher.bundles import (  # noqa: E402
    EXPECTED_HARDWARE,
    load_bundle,
    sha256,
)


ACCEPTED_BADGE_BUNDLE_SHA256 = (
    "038d83adcc3e6a561a9192e8bed26ec205e7e7c9374eb6ff800baf573bb44576"
)
DEFAULT_BADGE_BUNDLE = (
    REPOSITORY_ROOT
    / "tools/badge_flasher/resources/badge-factory-flasher-embedded.zip"
)
DEFAULT_BACKEND_INDEX = BACKEND_ROOT / "release/backend-release-index.json"
DEFAULT_BACKEND_FIRMWARE = BACKEND_ROOT / "web-flasher/firmware"

EXPECTED_BACKEND_PARTS = {
    0x0000: "uplink-s3-backend-bootloader.bin",
    0x8000: "uplink-s3-backend-partition-table.bin",
    0xF000: "uplink-s3-backend-ota-data-initial.bin",
    0x20000: "uplink-s3-backend-firmware.bin",
}


def _read_index(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise RuntimeError(f"cannot read backend release index: {exc}") from exc
    if not isinstance(value, dict) or set(value) != {"schema", "version", "targets"}:
        raise RuntimeError("backend release index shape mismatch")
    if value["schema"] != 1 or not isinstance(value["version"], str):
        raise RuntimeError("backend release index schema/version mismatch")
    return value


def _copy_part(source: Path, destination: Path) -> dict[str, Any]:
    if not source.is_file() or source.is_symlink():
        raise RuntimeError(f"missing regular firmware part: {source}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, destination)
    return {
        "size": destination.stat().st_size,
        "sha256": sha256(destination),
    }


def _accepted_badge_layouts(
    archive: Path,
    destination: Path,
) -> dict[str, dict[str, Any]]:
    digest = sha256(archive)
    if digest != ACCEPTED_BADGE_BUNDLE_SHA256:
        raise RuntimeError("accepted scanner/probe source bundle digest mismatch")
    bundle = load_badge_bundle(archive, source="accepted-badge-factory")
    if bundle.bundle_sha256 != ACCEPTED_BADGE_BUNDLE_SHA256:
        raise RuntimeError("accepted scanner/probe bundle is not canonical")
    result: dict[str, dict[str, Any]] = {}
    for role in ("probe", "scanner"):
        source_layout = bundle.layout(role)
        parts: list[dict[str, Any]] = []
        for part in sorted(source_layout["parts"], key=lambda value: value["offset"]):
            name = Path(str(part["path"])).name
            target = destination / role / name
            copied = _copy_part(bundle.root / part["path"], target)
            if copied != {"size": part["size"], "sha256": part["sha256"]}:
                raise RuntimeError(f"accepted {role} bytes changed during copy")
            parts.append({
                "offset": part["offset"],
                "path": f"{role}/{name}",
                **copied,
            })
        result[role] = {
            "chip": "ESP32-S3",
            "flash_size": "8MB",
            "hardware": EXPECTED_HARDWARE,
            "identity": dict(source_layout["identity"]),
            "parts": parts,
        }
    return result


def _backend_uplink_layout(
    index_path: Path,
    firmware_root: Path,
    destination: Path,
) -> tuple[str, dict[str, Any]]:
    index = _read_index(index_path)
    targets = index["targets"]
    if not isinstance(targets, dict) or set(targets) != {"uplink-s3-backend"}:
        raise RuntimeError("backend release must contain only the Lite uplink")
    target = targets["uplink-s3-backend"]
    if not isinstance(target, dict):
        raise RuntimeError("backend uplink release record is invalid")
    required = {
        "kind": "uplink",
        "target": "uplink-s3-backend",
        "project": "fof_backend_uplink",
        "hardware": EXPECTED_HARDWARE,
    }
    for key, expected in required.items():
        if target.get(key) != expected:
            raise RuntimeError(f"backend release {key} mismatch")
    source_parts = target.get("parts")
    if not isinstance(source_parts, list) or len(source_parts) != 4:
        raise RuntimeError("backend release must contain four uplink parts")
    by_offset: dict[int, dict[str, Any]] = {}
    for part in source_parts:
        if not isinstance(part, dict) or type(part.get("offset")) is not int:
            raise RuntimeError("backend release part schema mismatch")
        by_offset[part["offset"]] = part
    if set(by_offset) != set(EXPECTED_BACKEND_PARTS):
        raise RuntimeError("backend release partition offsets mismatch")

    output_parts: list[dict[str, Any]] = []
    for offset, expected_name in EXPECTED_BACKEND_PARTS.items():
        part = by_offset[offset]
        if part.get("name") != expected_name:
            raise RuntimeError("backend release part name mismatch")
        relative = part.get("path")
        if not isinstance(relative, str):
            raise RuntimeError("backend release part path mismatch")
        source = firmware_root / relative
        target_name = {
            0x0000: "bootloader.bin",
            0x8000: "partitions.bin",
            0xF000: "ota_data_initial.bin",
            0x20000: "firmware.bin",
        }[offset]
        copied = _copy_part(source, destination / "uplink" / target_name)
        if copied != {"size": part.get("size"), "sha256": part.get("sha256")}:
            raise RuntimeError(f"backend release digest mismatch: {expected_name}")
        output_parts.append({
            "offset": offset,
            "path": f"uplink/{target_name}",
            **copied,
        })

    firmware = destination / "uplink/firmware.bin"
    project, embedded_version = parse_esp_app_identity(firmware.read_bytes())
    if project != "fof_backend_uplink" or embedded_version != index["version"]:
        raise RuntimeError("backend uplink embedded identity mismatch")
    image = firmware.read_bytes()
    for marker in (b"uplink-s3-backend", b"seeed_xiao_esp32s3"):
        if marker not in image:
            raise RuntimeError("backend uplink embedded marker missing")
    return index["version"], {
        "chip": "ESP32-S3",
        "flash_size": "8MB",
        "hardware": EXPECTED_HARDWARE,
        "identity": {
            "project": "fof_backend_uplink",
            "target": "uplink-s3-backend",
            "version": index["version"],
        },
        "parts": output_parts,
    }


def _write_deterministic_zip(root: Path, output: Path) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(
        output,
        "w",
        compression=zipfile.ZIP_DEFLATED,
        compresslevel=9,
    ) as archive:
        for source in sorted(path for path in root.rglob("*") if path.is_file()):
            relative = source.relative_to(root).as_posix()
            info = zipfile.ZipInfo(relative, date_time=(2020, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o100644 << 16
            archive.writestr(
                info,
                source.read_bytes(),
                compress_type=zipfile.ZIP_DEFLATED,
                compresslevel=9,
            )


def _publish_validated_bundle(root: Path, output: Path) -> None:
    """Validate and fsync a sibling candidate before atomic publication."""

    output.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{output.name}.",
        suffix=".tmp",
        dir=output.parent,
    )
    os.close(descriptor)
    candidate = Path(temporary_name)
    try:
        _write_deterministic_zip(root, candidate)
        load_bundle(candidate, source="builder-verification")
        with candidate.open("rb") as handle:
            os.fsync(handle.fileno())
        candidate.replace(output)
        directory_descriptor = os.open(output.parent, os.O_RDONLY)
        try:
            os.fsync(directory_descriptor)
        finally:
            os.close(directory_descriptor)
    finally:
        candidate.unlink(missing_ok=True)


def build_bundle(
    output: Path,
    *,
    requested_version: str | None = None,
    badge_bundle: Path = DEFAULT_BADGE_BUNDLE,
    backend_index: Path = DEFAULT_BACKEND_INDEX,
    backend_firmware: Path = DEFAULT_BACKEND_FIRMWARE,
) -> Path:
    with tempfile.TemporaryDirectory(prefix="fof-lite-factory-build-") as temporary:
        root = Path(temporary)
        layouts = _accepted_badge_layouts(badge_bundle.resolve(), root)
        version, uplink = _backend_uplink_layout(
            backend_index.resolve(),
            backend_firmware.resolve(),
            root,
        )
        if requested_version and requested_version.lstrip("v") != version.lstrip("v"):
            raise RuntimeError(
                f"requested version {requested_version} does not match {version}"
            )
        layouts["uplink"] = uplink
        manifest = {
            "schema": 1,
            "family": "badge_lite",
            "version": version,
            "min_flasher": "1.0.0",
            "assembly": {
                "board_count": 3,
                "layouts": {
                    "scanner0": "scanner",
                    "scanner1": "scanner",
                    "uplink": "uplink",
                },
                "flash_order": ["scanner0", "scanner1", "uplink"],
            },
            "layouts": layouts,
        }
        (root / "manifest.json").write_text(
            json.dumps(manifest, sort_keys=True, separators=(",", ":")) + "\n",
            encoding="utf-8",
        )
        _publish_validated_bundle(root, output)
    return output


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--version")
    parser.add_argument("--badge-bundle", type=Path, default=DEFAULT_BADGE_BUNDLE)
    parser.add_argument("--backend-index", type=Path, default=DEFAULT_BACKEND_INDEX)
    parser.add_argument(
        "--backend-firmware",
        type=Path,
        default=DEFAULT_BACKEND_FIRMWARE,
    )
    args = parser.parse_args(argv)
    try:
        result = build_bundle(
            args.output.resolve(),
            requested_version=args.version,
            badge_bundle=args.badge_bundle,
            backend_index=args.backend_index,
            backend_firmware=args.backend_firmware,
        )
    except (OSError, ValueError, RuntimeError, zipfile.BadZipFile) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1
    print(result)
    print(hashlib.sha256(result.read_bytes()).hexdigest())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
