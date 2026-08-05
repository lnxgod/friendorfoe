"""Verify backend builds and package the Lite uplink release."""

from __future__ import annotations

import argparse
import csv
from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import sys
import tempfile
from typing import Any, Iterable, Mapping, Sequence
import zlib

try:
    from tools.firmware_identity import FirmwareIdentityError, verify_backend_image
except ModuleNotFoundError as exc:  # Direct ``python tools/...`` invocation.
    if exc.name is None or not exc.name.startswith("tools"):
        raise
    from firmware_identity import FirmwareIdentityError, verify_backend_image


FLASH_SIZE = 0x800000
APP_PARTITION_CAPACITY = 0x200000
RAW_PARTITION_TABLE_SIZE = 0xC00
PUBLISHED_PARTITION_TABLE_SIZE = 0x1000
BACKEND_HARDWARE = "seeed_xiao_esp32s3"

EXPECTED_PART_OFFSETS = {
    "bootloader": 0x0000,
    "partition-table": 0x8000,
    "ota-data-initial": 0xF000,
    "firmware": 0x20000,
}

_SOURCE_FILENAMES = {
    "bootloader": "bootloader.bin",
    "partition-table": "partitions.bin",
    "ota-data-initial": "ota_data_initial.bin",
    "firmware": "firmware.bin",
}

EXPECTED_COMMON_PARTITIONS = {
    "nvs": ("data", "nvs", 0x9000, 0x6000),
    "otadata": ("data", "ota", 0xF000, 0x2000),
    "phy_init": ("data", "phy", 0x11000, 0x1000),
    "ota_0": ("app", "ota_0", 0x20000, 0x200000),
    "ota_1": ("app", "ota_1", 0x220000, 0x200000),
}

EXPECTED_SCANNER_TAIL = {
    "storage": ("data", "spiffs", 0x420000, 0x100000),
    "reserved": ("data", "fat", 0x520000, 0x2E0000),
}

EXPECTED_UPLINK_TAIL = {
    "fw_scanner_be": ("data", "0x40", 0x420000, 0x200000),
    "storage": ("data", "spiffs", 0x620000, 0x100000),
    "reserved": ("data", "fat", 0x720000, 0x0E0000),
}

_PARTITION_ENTRY = struct.Struct("<HBBII16sI")
_PARTITION_MAGIC = 0x50AA
_PARTITION_MD5_MAGIC = b"\xeb\xeb"
_TYPE_NAMES = {0x00: "app", 0x01: "data"}
_SUBTYPE_NAMES = {
    ("data", 0x00): "ota",
    ("data", 0x01): "phy",
    ("data", 0x02): "nvs",
    ("data", 0x81): "fat",
    ("data", 0x82): "spiffs",
    ("app", 0x10): "ota_0",
    ("app", 0x11): "ota_1",
    ("data", 0x40): "0x40",
}


class BuildVerificationError(ValueError):
    """Raised when build metadata or published artifacts drift from contract."""


@dataclass(frozen=True)
class BackendReleaseSpec:
    kind: str
    image_kind: int
    environment: str
    target: str
    project: str
    hardware: str
    artifact_directory: str
    partition_csv: str


BACKEND_RELEASES = {
    "uplink": BackendReleaseSpec(
        kind="uplink",
        image_kind=0,
        environment="uplink-s3-backend",
        target="uplink-s3-backend",
        project="fof_backend_uplink",
        hardware=BACKEND_HARDWARE,
        artifact_directory="uplink-s3-backend",
        partition_csv="partitions_backend_uplink_8mb.csv",
    ),
    "scanner": BackendReleaseSpec(
        kind="scanner",
        image_kind=1,
        environment="scanner-s3-combo-backend",
        target="scanner-s3-combo-backend",
        project="fof_backend_scanner",
        hardware=BACKEND_HARDWARE,
        artifact_directory="scanner-s3-combo-backend",
        partition_csv="partitions_backend_scanner_8mb.csv",
    ),
}


@dataclass(frozen=True)
class PartitionEntry:
    label: str
    part_type: str
    subtype: str
    offset: int
    size: int
    flags: int = 0


@dataclass(frozen=True)
class VerifiedPart:
    logical_name: str
    name: str
    source_path: Path
    offset: int
    source_size: int
    size: int
    sha256: str
    crc32: int

    @property
    def relative_path(self) -> Path:
        target = self.name[: -len(f"-{self.logical_name}.bin")]
        return Path(target) / self.name


@dataclass(frozen=True)
class VerifiedArtifactSet:
    kind: str
    image_kind: int
    environment: str
    target: str
    project: str
    hardware: str
    version: str
    artifact_directory: str
    identity_crc32: int
    partition_capacity: int
    build_dir: Path
    parts: tuple[VerifiedPart, ...]


def expected_packaged_parts(target: str) -> dict[str, int]:
    if target not in {release.target for release in BACKEND_RELEASES.values()}:
        raise BuildVerificationError(f"not an exact backend target: {target!r}")
    return {
        f"{target}-{logical}.bin": offset
        for logical, offset in EXPECTED_PART_OFFSETS.items()
    }


def _expected_partitions(kind: str) -> dict[str, tuple[str, str, int, int]]:
    tail = EXPECTED_SCANNER_TAIL if kind == "scanner" else EXPECTED_UPLINK_TAIL
    return {**EXPECTED_COMMON_PARTITIONS, **tail}


def _strict_json_pairs(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise BuildVerificationError(f"duplicate JSON key: {key!r}")
        result[key] = value
    return result


def _load_json(path: Path, label: str) -> dict[str, Any]:
    try:
        value = json.loads(
            path.read_text(encoding="utf-8"), object_pairs_hook=_strict_json_pairs
        )
    except BuildVerificationError:
        raise
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise BuildVerificationError(f"cannot parse {label}: {path}") from exc
    if not isinstance(value, dict):
        raise BuildVerificationError(f"{label} must contain a JSON object")
    return value


def _require_build_file(build_dir: Path, filename: str, label: str) -> Path:
    path = build_dir / filename
    if not path.is_file() or path.is_symlink():
        raise BuildVerificationError(f"missing exact {label} build input: {path}")
    return path


def _strict_c_string(field: bytes, label: str) -> str:
    try:
        terminator = field.index(0)
    except ValueError as exc:
        raise BuildVerificationError(f"partition {label} is not NUL-terminated") from exc
    if any(field[terminator:]):
        raise BuildVerificationError(f"partition {label} has a nonzero string tail")
    try:
        value = field[:terminator].decode("ascii", errors="strict")
    except UnicodeDecodeError as exc:
        raise BuildVerificationError(f"partition {label} is not ASCII") from exc
    if not value:
        raise BuildVerificationError(f"partition {label} is empty")
    return value


def _decode_partition_table(payload: bytes) -> tuple[PartitionEntry, ...]:
    if len(payload) != RAW_PARTITION_TABLE_SIZE:
        raise BuildVerificationError(
            "raw partition table must be exactly 3072 bytes before publication"
        )
    entries: list[PartitionEntry] = []
    cursor = 0
    while cursor + _PARTITION_ENTRY.size <= len(payload):
        block = payload[cursor : cursor + _PARTITION_ENTRY.size]
        if block[:2] == _PARTITION_MD5_MAGIC:
            if block[2:16] != b"\xff" * 14:
                raise BuildVerificationError("partition table MD5 record is malformed")
            expected_md5 = hashlib.md5(payload[:cursor]).digest()
            if block[16:] != expected_md5:
                raise BuildVerificationError("partition table MD5 mismatch")
            if any(value != 0xFF for value in payload[cursor + 32 :]):
                raise BuildVerificationError("partition table unused bytes are not erased")
            if not entries:
                raise BuildVerificationError("partition table contains no entries")
            return tuple(entries)
        magic, raw_type, raw_subtype, offset, size, label_field, flags = (
            _PARTITION_ENTRY.unpack(block)
        )
        if magic != _PARTITION_MAGIC:
            raise BuildVerificationError("partition table entry magic is invalid")
        part_type = _TYPE_NAMES.get(raw_type)
        if part_type is None:
            raise BuildVerificationError("partition table type is unsupported")
        subtype = _SUBTYPE_NAMES.get((part_type, raw_subtype))
        if subtype is None:
            raise BuildVerificationError("partition table subtype is unsupported")
        if flags != 0:
            raise BuildVerificationError("partition table flags must be zero")
        if not isinstance(offset, int) or not isinstance(size, int) or size <= 0:
            raise BuildVerificationError("partition table range is invalid")
        entries.append(
            PartitionEntry(
                label=_strict_c_string(label_field, "label"),
                part_type=part_type,
                subtype=subtype,
                offset=offset,
                size=size,
                flags=flags,
            )
        )
        cursor += _PARTITION_ENTRY.size
    raise BuildVerificationError("partition table is missing its MD5 record")


def _parse_int(text: str, label: str) -> int:
    try:
        value = int(text.strip(), 0)
    except (TypeError, ValueError) as exc:
        raise BuildVerificationError(f"{label} is not an integer") from exc
    if value < 0:
        raise BuildVerificationError(f"{label} must not be negative")
    return value


def _parse_partition_csv(path: Path) -> tuple[PartitionEntry, ...]:
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except (OSError, UnicodeError) as exc:
        raise BuildVerificationError(f"cannot read partition CSV: {path}") from exc
    entries: list[PartitionEntry] = []
    rows = csv.reader(line for line in lines if line.strip() and not line.lstrip().startswith("#"))
    for row_number, row in enumerate(rows, start=1):
        if len(row) != 6:
            raise BuildVerificationError(
                f"partition CSV row {row_number} must have exactly six fields"
            )
        label, part_type, subtype, offset_text, size_text, flags_text = (
            item.strip() for item in row
        )
        if not label or not label.isascii():
            raise BuildVerificationError("partition CSV label is invalid")
        if flags_text not in ("", "0", "0x0"):
            raise BuildVerificationError("partition CSV flags must be zero")
        entries.append(
            PartitionEntry(
                label=label,
                part_type=part_type,
                subtype=subtype.lower(),
                offset=_parse_int(offset_text, "partition CSV offset"),
                size=_parse_int(size_text, "partition CSV size"),
                flags=0,
            )
        )
    if not entries:
        raise BuildVerificationError("partition CSV contains no entries")
    return tuple(entries)


def _expected_partition_entries(kind: str) -> tuple[PartitionEntry, ...]:
    return tuple(
        PartitionEntry(label, part_type, subtype, offset, size)
        for label, (part_type, subtype, offset, size) in _expected_partitions(kind).items()
    )


def _verify_partition_inputs(
    payload: bytes, partition_csv: Path, kind: str
) -> tuple[PartitionEntry, ...]:
    binary_entries = _decode_partition_table(payload)
    csv_entries = _parse_partition_csv(partition_csv)
    expected = _expected_partition_entries(kind)
    if binary_entries != csv_entries:
        raise BuildVerificationError("binary partition table differs from backend CSV")
    if binary_entries != expected:
        raise BuildVerificationError("partition table differs from exact backend layout")
    labels = [entry.label for entry in binary_entries]
    if len(labels) != len(set(labels)):
        raise BuildVerificationError("partition table labels are duplicated")
    return binary_entries


def _parse_sdkconfig(path: Path) -> dict[str, str]:
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except (OSError, UnicodeError) as exc:
        raise BuildVerificationError(f"cannot read generated sdkconfig: {path}") from exc
    config: dict[str, str] = {}
    unset = re.compile(r"^#\s+(CONFIG_[A-Z0-9_]+) is not set$")
    define = re.compile(r"^#define\s+(CONFIG_[A-Z0-9_]+)(?:\s+(.*))?$")
    assignment = re.compile(r"^(CONFIG_[A-Z0-9_]+)=(.*)$")
    for line in lines:
        stripped = line.strip()
        match = unset.match(stripped)
        if match:
            key, value = match.group(1), "n"
        else:
            match = define.match(stripped)
            if match:
                key = match.group(1)
                raw_value = (match.group(2) or "1").strip()
                value = "y" if raw_value == "1" else raw_value
            else:
                match = assignment.match(stripped)
                if not match:
                    continue
                key, value = match.group(1), match.group(2).strip()
        if len(value) >= 2 and value[0] == value[-1] == '"':
            value = value[1:-1]
        previous = config.get(key)
        if previous is not None and previous != value:
            raise BuildVerificationError(f"generated sdkconfig repeats {key} inconsistently")
        config[key] = value
    return config


def _require_config(config: Mapping[str, str], key: str, expected: str) -> None:
    actual = config.get(key, "n")
    if actual != expected:
        raise BuildVerificationError(
            f"generated sdkconfig {key} must be {expected!r}, got {actual!r}"
        )


def _verify_sdkconfig(path: Path, spec: BackendReleaseSpec) -> None:
    config = _parse_sdkconfig(path)
    required = {
        "CONFIG_ESPTOOLPY_FLASHSIZE_8MB": "y",
        "CONFIG_ESPTOOLPY_FLASHSIZE": "8MB",
        "CONFIG_PARTITION_TABLE_CUSTOM": "y",
        "CONFIG_PARTITION_TABLE_CUSTOM_FILENAME": spec.partition_csv,
        "CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE": "y",
        "CONFIG_SPIRAM": "y",
        "CONFIG_SPIRAM_MODE_OCT": "y",
        "CONFIG_ESP_WIFI_ENABLED": "y",
    }
    if spec.kind == "scanner":
        required.update(
            {
                "CONFIG_FOF_BACKEND_GLASSES_DETECTION": "y",
                "CONFIG_BT_ENABLED": "y",
                "CONFIG_BT_NIMBLE_ENABLED": "y",
                "CONFIG_BT_NIMBLE_ROLE_OBSERVER": "y",
                "CONFIG_BT_NIMBLE_ROLE_CENTRAL": "y",
            }
        )
    else:
        required.update(
            {
                "CONFIG_BT_ENABLED": "n",
                "CONFIG_BT_BLUEDROID_ENABLED": "n",
                "CONFIG_BT_NIMBLE_ENABLED": "n",
            }
        )
    for key, expected in required.items():
        _require_config(config, key, expected)


def _path_value(value: Any, label: str) -> Path:
    if not isinstance(value, str) or not value:
        raise BuildVerificationError(f"project description {label} must be a path")
    path = Path(value)
    if not path.is_absolute():
        raise BuildVerificationError(f"project description {label} must be absolute")
    return path.resolve(strict=False)


def _component_paths(description: Mapping[str, Any]) -> Iterable[Path]:
    raw_paths = description.get("build_component_paths")
    if not isinstance(raw_paths, list) or not raw_paths:
        raise BuildVerificationError("project description has no component paths")
    component_names = description.get("build_components")
    if not isinstance(component_names, list) or len(component_names) != len(raw_paths):
        raise BuildVerificationError(
            "project description component names and paths do not align"
        )
    for index, (component_name, value) in enumerate(
        zip(component_names, raw_paths, strict=True)
    ):
        if value == "":
            if index == len(raw_paths) - 1 and component_name == "":
                continue
            raise BuildVerificationError(
                "project description has a noncanonical empty component path"
            )
        if not isinstance(component_name, str) or not component_name:
            raise BuildVerificationError("project description component name is invalid")
        yield _path_value(value, "component path")
    for key in ("all_component_info", "build_component_info"):
        components = description.get(key, {})
        if components is None:
            continue
        if not isinstance(components, dict):
            raise BuildVerificationError(f"project description {key} must be an object")
        for component in components.values():
            if not isinstance(component, dict):
                raise BuildVerificationError(f"project description {key} entry is invalid")
            if "dir" in component:
                yield _path_value(component["dir"], "component directory")


def _verify_project_description(
    description: Mapping[str, Any],
    build_dir: Path,
    spec: BackendReleaseSpec,
    version: str,
) -> Path:
    exact_values = {
        "project_name": spec.project,
        "project_version": version,
        "target": "esp32s3",
        "app_bin": f"{spec.project}.bin",
    }
    for key, expected in exact_values.items():
        actual = description.get(key)
        if actual != expected:
            raise BuildVerificationError(
                f"project description {key} must be {expected!r}, got {actual!r}"
            )
    project_root = _path_value(description.get("project_path"), "project_path")
    described_build = _path_value(description.get("build_dir"), "build_dir")
    if described_build != build_dir.resolve():
        raise BuildVerificationError("project description build_dir does not match input")
    if project_root != build_dir.parents[2].resolve():
        raise BuildVerificationError("project description project_path does not match build")
    if project_root.name != spec.kind or project_root.parent.name != "backend-firmware":
        raise BuildVerificationError("project is not under the exact backend-firmware tree")

    backend_root = project_root.parent
    repository_root = backend_root.parent
    for component_path in _component_paths(description):
        if component_path.is_relative_to(repository_root) and not component_path.is_relative_to(
            backend_root
        ):
            raise BuildVerificationError(
                "repo-local component path escapes backend-firmware isolation"
            )
    return project_root


def _normalized_flash_files(value: Any) -> dict[int, str]:
    if not isinstance(value, dict) or len(value) != 4:
        raise BuildVerificationError("flasher metadata must contain exactly four files")
    normalized: dict[int, str] = {}
    for raw_offset, filename in value.items():
        if not isinstance(raw_offset, str) or not isinstance(filename, str):
            raise BuildVerificationError("flasher file mapping is malformed")
        offset = _parse_int(raw_offset, "flasher offset")
        if offset in normalized:
            raise BuildVerificationError("flasher offsets are duplicated")
        normalized[offset] = filename
    return normalized


def _verify_flasher_args(
    data: Mapping[str, Any], spec: BackendReleaseSpec
) -> dict[str, int]:
    settings = data.get("flash_settings")
    if settings != {
        "flash_mode": "dio",
        "flash_size": "8MB",
        "flash_freq": "80m",
    }:
        raise BuildVerificationError("flasher settings must be dio/80m/8MB")
    if data.get("write_flash_args") != [
        "--flash_mode",
        "dio",
        "--flash_size",
        "8MB",
        "--flash_freq",
        "80m",
    ]:
        raise BuildVerificationError("flasher write arguments drifted from dio/80m/8MB")
    extra = data.get("extra_esptool_args")
    if not isinstance(extra, dict) or extra.get("chip") != "esp32s3":
        raise BuildVerificationError("flasher chip must be esp32s3")

    flash_files = _normalized_flash_files(data.get("flash_files"))
    expected_files = {
        0x0000: "bootloader/bootloader.bin",
        0x8000: "partition_table/partition-table.bin",
        0xF000: "ota_data_initial.bin",
        0x20000: f"{spec.project}.bin",
    }
    if flash_files != expected_files:
        raise BuildVerificationError("flasher offsets or raw IDF filenames are not exact")
    return {
        logical: offset for logical, offset in EXPECTED_PART_OFFSETS.items()
    }


def _resolve_contract_file(
    explicit: Path | None,
    *,
    project_root: Path,
    build_dir: Path,
    expected_name: str,
    fallback: Path | None = None,
    label: str,
) -> Path:
    candidate = Path(explicit) if explicit is not None else project_root / expected_name
    if explicit is None and not candidate.is_file() and fallback is not None:
        candidate = fallback
    if not candidate.is_file() or candidate.is_symlink():
        raise BuildVerificationError(f"missing exact {label}: {candidate}")
    resolved = candidate.resolve()
    if not resolved.is_relative_to(project_root.resolve()):
        raise BuildVerificationError(f"{label} must remain under the backend project")
    if label == "partition CSV" and resolved.name != expected_name:
        raise BuildVerificationError("partition CSV filename is not exact")
    if label == "generated sdkconfig" and resolved.name not in {
        expected_name,
        "sdkconfig.h",
    }:
        raise BuildVerificationError("generated sdkconfig filename is not exact")
    return resolved


def _published_payload(logical_name: str, source_payload: bytes) -> bytes:
    if logical_name == "partition-table":
        if len(source_payload) != RAW_PARTITION_TABLE_SIZE:
            raise BuildVerificationError("raw partition table size changed before publication")
        return source_payload + b"\xff" * (
            PUBLISHED_PARTITION_TABLE_SIZE - RAW_PARTITION_TABLE_SIZE
        )
    return source_payload


def _ranges_intersect(start_a: int, end_a: int, start_b: int, end_b: int) -> bool:
    return start_a < end_b and start_b < end_a


def _verify_packaged_ranges(
    offsets: Mapping[str, int],
    published_payloads: Mapping[str, bytes],
    partitions: Sequence[PartitionEntry],
) -> None:
    ranges: list[tuple[str, int, int]] = []
    for logical in EXPECTED_PART_OFFSETS:
        start = offsets[logical]
        size = len(published_payloads[logical])
        if isinstance(start, bool) or not isinstance(start, int):
            raise BuildVerificationError("packaged part offset must be an integer")
        if size <= 0:
            raise BuildVerificationError("packaged part size must be positive")
        end = start + size
        if start < 0 or end <= start:
            raise BuildVerificationError("packaged part range is invalid")
        if end > FLASH_SIZE:
            raise BuildVerificationError("packaged part ends beyond 8-MB flash")
        ranges.append((logical, start, end))

    by_name = {entry.label: entry for entry in partitions}
    boot = next(item for item in ranges if item[0] == "bootloader")
    table = next(item for item in ranges if item[0] == "partition-table")
    ota_data = next(item for item in ranges if item[0] == "ota-data-initial")
    firmware = next(item for item in ranges if item[0] == "firmware")
    if boot[1] != 0 or boot[2] > 0x8000:
        raise BuildVerificationError("bootloader crosses the partition table")
    if table[1:] != (0x8000, 0x9000):
        raise BuildVerificationError("published partition table range is not exact")
    nvs = by_name["nvs"]
    nvs_end = nvs.offset + nvs.size
    for logical, start, end in ranges:
        if _ranges_intersect(start, end, nvs.offset, nvs_end):
            raise BuildVerificationError(f"packaged {logical} intersects protected NVS")
    otadata = by_name["otadata"]
    if ota_data[1:] != (otadata.offset, otadata.offset + otadata.size):
        raise BuildVerificationError("OTA data range is not exact")
    ota_0 = by_name["ota_0"]
    if firmware[1] != ota_0.offset or firmware[2] > ota_0.offset + ota_0.size:
        raise BuildVerificationError("application exceeds its OTA partition")
    for index, (name_a, start_a, end_a) in enumerate(ranges):
        for name_b, start_b, end_b in ranges[index + 1 :]:
            if _ranges_intersect(start_a, end_a, start_b, end_b):
                raise BuildVerificationError(
                    f"packaged ranges overlap: {name_a} and {name_b}"
                )


def _part_from_payload(
    *,
    logical_name: str,
    source_path: Path,
    source_payload: bytes,
    published_payload: bytes,
    target: str,
    offset: int,
) -> VerifiedPart:
    return VerifiedPart(
        logical_name=logical_name,
        name=f"{target}-{logical_name}.bin",
        source_path=source_path,
        offset=offset,
        source_size=len(source_payload),
        size=len(published_payload),
        sha256=hashlib.sha256(published_payload).hexdigest(),
        crc32=zlib.crc32(published_payload) & 0xFFFFFFFF,
    )


def verify_artifact_set(
    build_dir: Path,
    *,
    kind: str,
    version: str,
    partition_csv: Path | None = None,
    sdkconfig: Path | None = None,
) -> VerifiedArtifactSet:
    """Validate one unpublished PlatformIO build without writing release files."""

    spec = BACKEND_RELEASES.get(kind)
    if spec is None:
        raise BuildVerificationError(f"kind must be scanner or uplink, got {kind!r}")
    if not isinstance(version, str) or not version:
        raise BuildVerificationError("version must be a nonempty string")
    build = Path(build_dir)
    if not build.is_dir() or build.is_symlink():
        raise BuildVerificationError(f"build directory does not exist: {build}")
    if build.name != spec.environment:
        raise BuildVerificationError(
            f"build environment must be {spec.environment!r}, got {build.name!r}"
        )
    if build.parent.name != "build" or build.parent.parent.name != ".pio":
        raise BuildVerificationError("input is not an exact PlatformIO build directory")
    build = build.resolve()

    project_description_path = _require_build_file(
        build, "project_description.json", "project description"
    )
    flasher_args_path = _require_build_file(build, "flasher_args.json", "flasher metadata")
    description = _load_json(project_description_path, "project description")
    project_root = _verify_project_description(description, build, spec, version)
    csv_path = _resolve_contract_file(
        partition_csv,
        project_root=project_root,
        build_dir=build,
        expected_name=spec.partition_csv,
        label="partition CSV",
    )
    sdkconfig_path = _resolve_contract_file(
        sdkconfig,
        project_root=project_root,
        build_dir=build,
        expected_name=f"sdkconfig.{spec.environment}",
        fallback=build / "config" / "sdkconfig.h",
        label="generated sdkconfig",
    )

    source_paths = {
        logical: _require_build_file(build, filename, logical.replace("-", " "))
        for logical, filename in _SOURCE_FILENAMES.items()
    }
    app_alias = _require_build_file(build, f"{spec.project}.bin", "project app alias")
    source_payloads = {
        logical: path.read_bytes() for logical, path in source_paths.items()
    }
    if not source_payloads["bootloader"]:
        raise BuildVerificationError("bootloader is empty")
    if len(source_payloads["partition-table"]) != RAW_PARTITION_TABLE_SIZE:
        raise BuildVerificationError("raw partition table has the wrong size")
    if len(source_payloads["ota-data-initial"]) != 0x2000:
        raise BuildVerificationError("ota data must be exactly 8192 bytes")
    if source_payloads["ota-data-initial"] != b"\xff" * 0x2000:
        raise BuildVerificationError("ota data is not the initial all-0xff image")
    firmware_payload = source_payloads["firmware"]
    if not firmware_payload:
        raise BuildVerificationError("firmware is empty")
    if len(firmware_payload) > APP_PARTITION_CAPACITY:
        raise BuildVerificationError("application exceeds its 2-MB OTA slot")
    try:
        alias_payload = app_alias.read_bytes()
    except OSError as exc:
        raise BuildVerificationError("cannot read project app alias") from exc
    if alias_payload != firmware_payload:
        raise BuildVerificationError("project app alias differs from firmware.bin")

    partitions = _verify_partition_inputs(
        source_payloads["partition-table"], csv_path, kind
    )
    _verify_sdkconfig(sdkconfig_path, spec)
    offsets = _verify_flasher_args(
        _load_json(flasher_args_path, "flasher metadata"), spec
    )
    published_payloads = {
        logical: _published_payload(logical, payload)
        for logical, payload in source_payloads.items()
    }
    _verify_packaged_ranges(offsets, published_payloads, partitions)

    try:
        image = verify_backend_image(
            source_paths["firmware"],
            target=spec.target,
            project=spec.project,
            hardware=spec.hardware,
            version=version,
            partition_capacity=APP_PARTITION_CAPACITY,
        )
    except FirmwareIdentityError as exc:
        raise BuildVerificationError(f"firmware identity verification failed: {exc}") from exc
    if image.sha256 != hashlib.sha256(firmware_payload).hexdigest():
        raise BuildVerificationError("firmware changed while it was being verified")
    if image.image_kind != spec.image_kind:
        raise BuildVerificationError("firmware image kind differs from release kind")

    parts = tuple(
        _part_from_payload(
            logical_name=logical,
            source_path=source_paths[logical],
            source_payload=source_payloads[logical],
            published_payload=published_payloads[logical],
            target=spec.target,
            offset=offsets[logical],
        )
        for logical in EXPECTED_PART_OFFSETS
    )
    if {part.name: part.offset for part in parts} != expected_packaged_parts(spec.target):
        raise BuildVerificationError("published artifact names or offsets are not exact")
    return VerifiedArtifactSet(
        kind=spec.kind,
        image_kind=spec.image_kind,
        environment=spec.environment,
        target=spec.target,
        project=spec.project,
        hardware=spec.hardware,
        version=version,
        artifact_directory=spec.artifact_directory,
        identity_crc32=image.identity_crc32,
        partition_capacity=APP_PARTITION_CAPACITY,
        build_dir=build,
        parts=parts,
    )


def _read_current_published_payload(part: VerifiedPart) -> bytes:
    try:
        raw = part.source_path.read_bytes()
    except OSError as exc:
        raise BuildVerificationError(f"cannot reread build input: {part.source_path}") from exc
    payload = _published_payload(part.logical_name, raw)
    if len(payload) != part.size:
        raise BuildVerificationError(f"source changed for published part {part.name}")
    if hashlib.sha256(payload).hexdigest() != part.sha256:
        raise BuildVerificationError(f"source changed for published part {part.name}")
    if zlib.crc32(payload) & 0xFFFFFFFF != part.crc32:
        raise BuildVerificationError(f"source changed for published part {part.name}")
    return payload


def verify_packaged_target(
    target_directory: Path, release: VerifiedArtifactSet
) -> None:
    """Verify one already-materialized target directory against a checked build."""

    directory = Path(target_directory)
    if not directory.is_dir() or directory.is_symlink():
        raise BuildVerificationError(f"published target directory is missing: {directory}")
    if directory.name != release.artifact_directory:
        raise BuildVerificationError("published target directory name is not exact")
    actual_names = {path.name for path in directory.iterdir()}
    expected_names = {part.name for part in release.parts}
    if actual_names != expected_names:
        raise BuildVerificationError("published target contains missing or extra artifacts")
    for part in release.parts:
        path = directory / part.name
        if not path.is_file() or path.is_symlink():
            raise BuildVerificationError(f"published artifact is not a regular file: {path}")
        payload = path.read_bytes()
        if part.logical_name == "partition-table":
            if len(payload) != PUBLISHED_PARTITION_TABLE_SIZE:
                raise BuildVerificationError("published partition table is not 4096 bytes")
            if payload[RAW_PARTITION_TABLE_SIZE:] != b"\xff" * (
                PUBLISHED_PARTITION_TABLE_SIZE - RAW_PARTITION_TABLE_SIZE
            ):
                raise BuildVerificationError(
                    "published partition table padding is not all 0xff"
                )
        if len(payload) != part.size:
            raise BuildVerificationError(f"published artifact size mismatch: {part.name}")
        if hashlib.sha256(payload).hexdigest() != part.sha256:
            raise BuildVerificationError(f"published artifact SHA-256 mismatch: {part.name}")
        if zlib.crc32(payload) & 0xFFFFFFFF != part.crc32:
            raise BuildVerificationError(f"published artifact CRC32 mismatch: {part.name}")


def _release_index(releases: Sequence[VerifiedArtifactSet]) -> dict[str, Any]:
    if len(releases) != 1 or releases[0].kind != "uplink":
        raise BuildVerificationError("release index requires exactly the Lite uplink")
    versions = {release.version for release in releases}
    targets: dict[str, Any] = {}
    for release in sorted(releases, key=lambda item: item.target):
        parts: list[dict[str, Any]] = []
        for part in sorted(release.parts, key=lambda item: item.offset):
            relative = Path(release.artifact_directory) / part.name
            if relative.name != part.name or relative.parent != Path(release.target):
                raise BuildVerificationError("release part path is not target-scoped")
            if not part.name.startswith(f"{release.target}-"):
                raise BuildVerificationError("release part basename lacks target prefix")
            parts.append(
                {
                    "name": part.name,
                    "path": relative.as_posix(),
                    "offset": part.offset,
                    "size": part.size,
                    "sha256": part.sha256,
                    "crc32": part.crc32,
                }
            )
        if len(parts) != 4:
            raise BuildVerificationError("release target does not have exactly four parts")
        targets[release.target] = {
            "kind": release.kind,
            "target": release.target,
            "project": release.project,
            "hardware": release.hardware,
            "identity_crc32": release.identity_crc32,
            "partition_capacity": release.partition_capacity,
            "parts": parts,
        }
    return {"schema": 1, "version": versions.pop(), "targets": targets}


def _write_part(path: Path, payload: bytes) -> None:
    try:
        with path.open("xb") as handle:
            handle.write(payload)
            handle.flush()
            os.fsync(handle.fileno())
    except OSError as exc:
        raise BuildVerificationError(f"cannot materialize published artifact: {path}") from exc


def _write_index_temp(index_path: Path, release_index: Mapping[str, Any]) -> Path:
    rendered = json.dumps(release_index, indent=2, sort_keys=True) + "\n"
    try:
        file_descriptor, temporary_name = tempfile.mkstemp(
            prefix=f".{index_path.name}.", suffix=".tmp", dir=index_path.parent
        )
        os.fchmod(file_descriptor, 0o600)
        with os.fdopen(file_descriptor, "w", encoding="utf-8") as handle:
            handle.write(rendered)
            handle.flush()
            os.fsync(handle.fileno())
    except OSError as exc:
        raise BuildVerificationError("cannot write temporary release index") from exc
    return Path(temporary_name)


def verify_uplink_release(
    *,
    uplink_build_dir: Path,
    uplink_partition_csv: Path,
    uplink_sdkconfig: Path,
    output_dir: Path,
    index_path: Path,
    version: str,
    check_only: bool = False,
) -> dict[str, Any]:
    """Validate and atomically expose only the Lite uplink release files."""

    uplink = verify_artifact_set(
        uplink_build_dir,
        kind="uplink",
        version=version,
        partition_csv=uplink_partition_csv,
        sdkconfig=uplink_sdkconfig,
    )
    releases = (uplink,)
    release_index = _release_index(releases)
    if check_only:
        return release_index

    output = Path(output_dir)
    index = Path(index_path)
    if output.exists() and (not output.is_dir() or output.is_symlink()):
        raise BuildVerificationError("release output must be a real directory")
    if output.exists():
        unexpected = sorted(
            entry.name for entry in output.iterdir() if entry.name != ".gitkeep"
        )
        if unexpected:
            raise BuildVerificationError(
                "release output is not empty: " + ", ".join(unexpected)
            )
    for release in releases:
        destination = output / release.artifact_directory
        if destination.exists() or destination.is_symlink():
            raise BuildVerificationError(
                f"release target already exists; refusing to overwrite: {destination}"
            )
    if index.exists() and (not index.is_file() or index.is_symlink()):
        raise BuildVerificationError("release index destination is not a regular file")

    output.parent.mkdir(parents=True, exist_ok=True)
    index.parent.mkdir(parents=True, exist_ok=True)
    stage_root = Path(
        tempfile.mkdtemp(prefix=f".{output.name}.stage-", dir=output.parent)
    )
    temporary_index: Path | None = None
    try:
        for release in releases:
            staged_target = stage_root / release.artifact_directory
            staged_target.mkdir(mode=0o700)
            for part in release.parts:
                _write_part(staged_target / part.name, _read_current_published_payload(part))
            verify_packaged_target(staged_target, release)
        temporary_index = _write_index_temp(index, release_index)
        output.mkdir(parents=True, exist_ok=True)
        for release in releases:
            os.replace(
                stage_root / release.artifact_directory,
                output / release.artifact_directory,
            )
        os.replace(temporary_index, index)
        temporary_index = None
    except BuildVerificationError:
        raise
    except OSError as exc:
        raise BuildVerificationError("cannot publish verified uplink release") from exc
    finally:
        if temporary_index is not None:
            try:
                temporary_index.unlink(missing_ok=True)
            except OSError:
                pass
        shutil.rmtree(stage_root, ignore_errors=True)
    return release_index


def _repository_version() -> str:
    header = Path(__file__).resolve().parents[1] / "shared" / "backend_version.h"
    try:
        text = header.read_text(encoding="utf-8")
    except OSError as exc:
        raise BuildVerificationError("cannot read backend version header") from exc
    matches = re.findall(r'^#define\s+FOF_VERSION_BACKEND\s+"([^"]+)"\s*$', text, re.MULTILINE)
    if len(matches) != 1:
        raise BuildVerificationError("backend version header is not exact")
    return matches[0]


def _argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Verify and package the backend Lite uplink firmware"
    )
    commands = parser.add_subparsers(dest="command", required=True)
    uplink = commands.add_parser("uplink", help="verify and package only the Lite uplink")
    uplink.add_argument("--uplink-build-dir", type=Path, required=True)
    uplink.add_argument("--uplink-partition-csv", type=Path, required=True)
    uplink.add_argument("--uplink-sdkconfig", type=Path, required=True)
    uplink.add_argument("--output-dir", type=Path, required=True)
    uplink.add_argument("--index", dest="index_path", type=Path, required=True)
    uplink.add_argument("--check-only", action="store_true")
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = _argument_parser().parse_args(argv)
    try:
        if args.command != "uplink":
            raise BuildVerificationError("only uplink releases are supported")
        release_index = verify_uplink_release(
            uplink_build_dir=args.uplink_build_dir,
            uplink_partition_csv=args.uplink_partition_csv,
            uplink_sdkconfig=args.uplink_sdkconfig,
            output_dir=args.output_dir,
            index_path=args.index_path,
            version=_repository_version(),
            check_only=args.check_only,
        )
    except BuildVerificationError as exc:
        print(f"backend release verification failed: {exc}", file=sys.stderr)
        return 2
    print(json.dumps(release_index, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
