#!/usr/bin/env python3
"""Create the reproducible, self-validating FoF badge factory bundle."""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import struct
import sys
import tempfile
import zipfile
from dataclasses import dataclass
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT))
sys.path.insert(0, str(REPO_ROOT / "esp32/scripts"))

from firmware_version import parse_firmware_identity  # noqa: E402
from tools.badge_flasher.bundles import load_bundle  # noqa: E402


@dataclass(frozen=True, slots=True)
class AcceptedApplication:
    version: str
    size: int
    sha256: str


ACCEPTED_VERSION = "0.67.2-badge-defcon34"
ACCEPTED_APPLICATIONS = {
    "uplink": AcceptedApplication(
        ACCEPTED_VERSION,
        1_468_464,
        "78ef3b6dafe61e8e2fdc3fb28447372aaf76da38cd57ca0961828bbbdc08c434",
    ),
    "scanner": AcceptedApplication(
        ACCEPTED_VERSION,
        1_216_800,
        "2d0e84501baf3bc929eed03a0b9c1f0272ed66baa9b81dd4513d6dc3fa2c032b",
    ),
}

PROBE_BUILD = (
    REPO_ROOT / "esp32/factory-probe/.pio/build/factory-probe-s3",
    "factory-probe-s3",
    "fof_badge_factory_probe",
)

BUILD_PROFILES = {
    "production": {
        "probe": PROBE_BUILD,
        "uplink": (
            REPO_ROOT / "esp32/uplink/.pio/build/uplink-s3-fof_badge",
            "uplink-s3-fof_badge",
            "fof_badge_uplink",
        ),
        "scanner": (
            REPO_ROOT / "esp32/scanner/.pio/build/scanner-s3-combo-fof_badge",
            "scanner-s3-combo-fof_badge",
            "fof_badge_scanner",
        ),
    },
    "con-crud-0.67.2": {
        "probe": PROBE_BUILD,
        "uplink": (
            REPO_ROOT / "esp32/uplink/.pio/build/uplink-s3-fof_badge-con-crud-canary",
            "uplink-s3-fof_badge",
            "fof_badge_uplink",
        ),
        "scanner": (
            REPO_ROOT / "esp32/scanner/.pio/build/scanner-s3-combo-fof_badge-con-crud-canary",
            "scanner-s3-combo-fof_badge",
            "fof_badge_scanner",
        ),
    },
}


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def verify_accepted_application(
    path: Path,
    expected_project: str,
    pin: AcceptedApplication,
) -> None:
    identity = parse_firmware_identity(path.read_bytes())
    if identity is None or identity.project != expected_project:
        raise RuntimeError(f"{path}: project identity mismatch")
    if identity.version != pin.version:
        raise RuntimeError(f"{path}: accepted version mismatch")
    if path.stat().st_size != pin.size:
        raise RuntimeError(f"{path}: accepted size mismatch")
    if sha256(path) != pin.sha256:
        raise RuntimeError(f"{path}: accepted SHA-256 mismatch")


def compiled_partition_offsets(path: Path) -> tuple[int, int | None]:
    """Read app/otadata offsets from the compiled ESP-IDF partition table.

    This is intentionally authoritative over ``flasher_args.json`` because
    PlatformIO can retain the framework's single-app offset even while
    ``board_build.partitions`` compiled an OTA table.
    """
    data = path.read_bytes()
    app_offset: int | None = None
    ota_data_offset: int | None = None
    for index in range(0, len(data) - 31, 32):
        entry = data[index:index + 32]
        magic, partition_type, subtype, offset, _size = struct.unpack_from(
            "<HBBII", entry
        )
        if magic == 0xFFFF:
            break
        if magic != 0x50AA:
            continue
        if partition_type == 0x00 and subtype in (0x00, 0x10) and app_offset is None:
            app_offset = offset
        if partition_type == 0x01 and subtype == 0x00:
            ota_data_offset = offset
    if app_offset is None:
        raise RuntimeError(f"compiled partition table has no bootable app: {path}")
    return app_offset, ota_data_offset


def layout_for(
    role: str,
    builds: dict,
    bundle_root: Path,
) -> dict:
    build_dir, target, project = builds[role]
    flasher = json.loads((build_dir / "flasher_args.json").read_text())
    app_file = build_dir / "firmware.bin"
    embedded = parse_firmware_identity(app_file.read_bytes())
    if embedded is None or embedded.project != project:
        raise RuntimeError(f"{role}: invalid embedded project identity in {app_file}")

    source_by_name = {
        "bootloader": build_dir / "bootloader.bin",
        "partition": build_dir / "partitions.bin",
        "app": app_file,
    }
    app_offset, ota_data_offset = compiled_partition_offsets(
        source_by_name["partition"]
    )
    required_offsets = {
        int(flasher["bootloader"]["offset"], 0): source_by_name["bootloader"],
        int(flasher["partition-table"]["offset"], 0): source_by_name["partition"],
        app_offset: source_by_name["app"],
    }
    if ota_data_offset is not None:
        ota_data = build_dir / "ota_data_initial.bin"
        if not ota_data.is_file():
            raise RuntimeError(f"{role}: compiled OTA layout is missing {ota_data}")
        required_offsets[ota_data_offset] = ota_data
    parts = []
    for offset, source in sorted(required_offsets.items()):
        if not source.is_file():
            raise RuntimeError(f"{role}: missing build artifact {source}")
        name = "firmware.bin" if source == app_file else source.name
        relative = Path(role) / name
        destination = bundle_root / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, destination)
        parts.append({
            "offset": offset,
            "path": relative.as_posix(),
            "size": destination.stat().st_size,
            "sha256": sha256(destination),
        })
    return {
        "chip": "ESP32-S3",
        "flash_size": str(flasher["flash_settings"]["flash_size"]),
        "identity": {"project": project, "target": target, "version": embedded.version},
        "parts": parts,
    }


def build_bundle(
    output: Path,
    requested_version: str | None = None,
    profile: str = "production",
) -> Path:
    with tempfile.TemporaryDirectory(prefix="fof-factory-build-") as temporary:
        root = Path(temporary)
        builds = BUILD_PROFILES[profile]
        if profile == "con-crud-0.67.2":
            for role, pin in ACCEPTED_APPLICATIONS.items():
                build_dir, _, project = builds[role]
                verify_accepted_application(build_dir / "firmware.bin", project, pin)
        layouts = {
            role: layout_for(role, builds, root)
            for role in builds
        }
        version = layouts["uplink"]["identity"]["version"]
        if layouts["scanner"]["identity"]["version"] != version:
            raise RuntimeError("uplink and scanner build versions do not match")
        if requested_version and requested_version.lstrip("v") != str(version).lstrip("v"):
            raise RuntimeError(f"requested version {requested_version} does not match embedded {version}")
        manifest = {
            "schema": 1,
            "version": version,
            "min_flasher": "1.0.0",
            "layouts": layouts,
        }
        (root / "manifest.json").write_text(
            json.dumps(manifest, sort_keys=True, separators=(",", ":")) + "\n",
            encoding="utf-8",
        )
        output.parent.mkdir(parents=True, exist_ok=True)
        with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
            for source in sorted(path for path in root.rglob("*") if path.is_file()):
                relative = source.relative_to(root).as_posix()
                info = zipfile.ZipInfo(relative, date_time=(2020, 1, 1, 0, 0, 0))
                info.compress_type = zipfile.ZIP_DEFLATED
                info.external_attr = 0o100644 << 16
                archive.writestr(info, source.read_bytes(), compress_type=zipfile.ZIP_DEFLATED, compresslevel=9)
    load_bundle(output, source="builder-verification")
    return output


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--version")
    parser.add_argument(
        "--profile",
        choices=tuple(BUILD_PROFILES),
        default="production",
    )
    args = parser.parse_args()
    try:
        result = build_bundle(args.output.resolve(), args.version, args.profile)
    except (OSError, ValueError, RuntimeError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1
    print(result)
    print(sha256(result))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
