#!/usr/bin/env python3
"""Fail-closed direct-USB canary migration for backend sensor firmware.

This tool deliberately has no erase command and never delegates to the badge
flasher.  Read-only inventory and verified backups precede every explicit,
one-use write approval.  Native badge 0.67 firmware remains the default; the
backend/Lite family is selected only through an exact release index and an
operator challenge bound to a physical chip MAC.
"""

from __future__ import annotations

import argparse
import contextlib
from dataclasses import asdict, dataclass, field, replace
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import select
import shutil
import stat
import struct
import subprocess
import sys
import termios
import time
from typing import Any, Callable, Literal, Mapping, Sequence
import urllib.error
import urllib.parse
import urllib.request
import zlib


BOARD_ROLES = ("scanner0", "scanner1", "uplink")
COMPONENTS = ("uplink", "scanner0", "scanner1")
FLASH_SIZE = 0x800000
NVS_OFFSET = 0x9000
NVS_SIZE = 0x6000
PARTITION_OFFSET = 0x8000
PARTITION_SECTOR_SIZE = 0x1000
OTA_DATA_OFFSET = 0xF000
OTA_DATA_SIZE = 0x2000
APP_OFFSET = 0x20000
APP_CAPACITY = 0x200000
CHALLENGE_TTL_SECONDS = 300
CATALOG_TTL_SECONDS = 300
PROBE_TTL_SECONDS = 300
MAX_SERIAL_LINE = 8192
MAX_HTTP_JSON = 1024 * 1024
BACKEND_VERSION = "0.1.0-backend"
BACKEND_HARDWARE = "seeed_xiao_esp32s3"

PartitionEntry = tuple[str, str, str, int, int]

COMMON_PARTITIONS: tuple[PartitionEntry, ...] = (
    ("nvs", "data", "nvs", 0x9000, 0x6000),
    ("otadata", "data", "ota", 0xF000, 0x2000),
    ("phy_init", "data", "phy", 0x11000, 0x1000),
    ("ota_0", "app", "ota_0", 0x20000, 0x200000),
    ("ota_1", "app", "ota_1", 0x220000, 0x200000),
)
SCANNER_TAIL: tuple[PartitionEntry, ...] = (
    ("storage", "data", "spiffs", 0x420000, 0x100000),
    ("reserved", "data", "fat", 0x520000, 0x2E0000),
)
LEGACY_UPLINK_TAIL: tuple[PartitionEntry, ...] = (
    ("fw_scanner_s3", "data", "0x40", 0x420000, 0x200000),
    ("storage", "data", "spiffs", 0x620000, 0x100000),
    ("reserved", "data", "fat", 0x720000, 0x0E0000),
)
BACKEND_UPLINK_TAIL: tuple[PartitionEntry, ...] = (
    ("fw_scanner_be", "data", "0x40", 0x420000, 0x200000),
    ("storage", "data", "spiffs", 0x620000, 0x100000),
    ("reserved", "data", "fat", 0x720000, 0x0E0000),
)

LEGACY_PARTITIONS: dict[str, tuple[PartitionEntry, ...]] = {
    "scanner": COMMON_PARTITIONS + SCANNER_TAIL,
    "uplink": COMMON_PARTITIONS + LEGACY_UPLINK_TAIL,
}
BACKEND_PARTITIONS: dict[str, tuple[PartitionEntry, ...]] = {
    "scanner": COMMON_PARTITIONS + SCANNER_TAIL,
    "uplink": COMMON_PARTITIONS + BACKEND_UPLINK_TAIL,
}

BACKEND_IDENTITIES = {
    "scanner": (
        "scanner-s3-combo-backend",
        "fof_backend_scanner",
        BACKEND_HARDWARE,
        BACKEND_VERSION,
    ),
    "uplink": (
        "uplink-s3-backend",
        "fof_backend_uplink",
        BACKEND_HARDWARE,
        BACKEND_VERSION,
    ),
}
BACKEND_IDENTITY_CRC32 = {
    "scanner": 0x9DD382FF,
    "uplink": 0xF08BCDE4,
}

# Direct USB migration may recognize these installed identities.  Recognition
# never authorizes a legacy image for writing; it only selects the exact
# source-audited preflash table that must be backed up first.
LEGACY_IDENTITIES = frozenset(
    {
        (
            "scanner-s3-combo-fof_badge",
            "fof_badge_scanner",
            "seeed_xiao_esp32s3",
            "0.67.2-badge-defcon34",
            "scanner",
        ),
        (
            "uplink-s3-fof_badge",
            "fof_badge_uplink",
            "seeed_xiao_esp32s3",
            "0.67.2-badge-defcon34",
            "uplink",
        ),
        (
            "scanner-s3-combo-seed",
            "fof_scanner_seed",
            "esp32-s3-devkitc-1",
            "0.64.68-live-follow",
            "scanner",
        ),
    }
)

# SHA-256 of the source-audited pinned firmware-image admission contract
# (`esp32/shared/firmware_image_contract.c` at VENDOR_BASE).  The digest is
# evidence only; this tool never imports or flashes a protected badge artifact.
PINNED_UPDATER_ADMISSION_SHA256 = (
    "2989ea7d74c375ac7aa9ce41e58efd3a8c19e4ec429baebcf0cdbfd8b71a7eb3"
)

EXPECTED_PART_OFFSETS = {
    "bootloader": 0x0000,
    "partition-table": 0x8000,
    "ota-data-initial": 0xF000,
    "firmware": 0x20000,
}
DANGEROUS_FLASH_OPTIONS = frozenset(
    {
        "--force",
        "--erase-all",
        "--erase_all",
        "erase_flash",
        "erase_all",
        "--encrypt",
        "--encrypt-files",
        "--ignore-flash-encryption-efuse-setting",
    }
)
SECRET_KEY = re.compile(
    r"password|secret|credential|token|authorization|cookie|set-cookie|api_key",
    re.IGNORECASE,
)
HEX64 = re.compile(r"^[0-9a-fA-F]{64}$")
MAC_RE = re.compile(r"^[0-9A-F]{2}(?::[0-9A-F]{2}){5}$")


class CanaryError(RuntimeError):
    """Base error for an admission failure."""


class CanaryOrderError(CanaryError):
    pass


class CanaryApprovalError(CanaryError):
    pass


class CanaryInventoryError(CanaryError):
    pass


class CanaryBackupError(CanaryError):
    pass


class CanaryReleaseError(CanaryError):
    pass


class CanarySecurityError(CanaryApprovalError):
    pass


@dataclass(frozen=True)
class ToolchainReceipt:
    pio_path: str
    platformio_version: str
    python_exe: str
    core_dir: str
    esptool_path: str
    esptool_version: str
    esptool_sha256: str

    @property
    def sha256(self) -> str:
        return hashlib.sha256(_canonical_json(asdict(self))).hexdigest()


@dataclass(frozen=True)
class BoardIdentity:
    role: Literal["scanner0", "scanner1", "uplink"]
    port: str
    chip: str
    mac: str
    flash_size: int
    secure_boot_enabled: bool
    flash_encryption_enabled: bool
    installed_target: str
    installed_project: str
    installed_hardware: str
    installed_version: str
    installed_role: str
    installed_partition_sha256: str
    updater_admission_evidence_sha256: str
    xiao_sense_sd_attached: bool = False


@dataclass(frozen=True)
class BackupRecord:
    role: str
    kind: Literal["original", "backend-baseline"]
    mac: str
    full_path: str
    full_size: int
    full_sha256: str
    nvs_path: str
    nvs_size: int
    nvs_sha256: str
    partition_path: str
    partition_size: int
    partition_sha256: str
    decoded_partition_sha256: str
    toolchain_sha256: str


@dataclass(frozen=True)
class ApprovalChallenge:
    challenge_id: str
    role: str
    port: str
    mac: str
    operation: Literal["flash-initial", "restore-full", "ota-apply"]
    component: Literal["uplink", "scanner0", "scanner1"] | None
    ota_mode: Literal["newer-only", "same-version-recovery"] | None
    artifact_sha256: str
    artifact_crc32: int | None
    offsets_sha256: str
    state_generation: int
    target_slot: int | None
    target_mac: str | None
    target_boot_id: int | None
    topology_generation: int | None
    pio_path: str
    toolchain_sha256: str
    lite_sensor_confirmed: bool
    no_sd_expansion_confirmed: bool
    restore_source: Literal["original", "backend-baseline"] | None
    expires_at: int
    consumed_at: int | None


@dataclass(frozen=True)
class OtaEvidence:
    schema: int
    operation_id: int
    mode: str
    component: str
    component_slot: int
    uplink_mac: str
    expected_target_mac: str
    actual_target_mac: str
    expected_target_boot_id: int
    actual_target_boot_id: int
    expected_topology_generation: int
    actual_topology_generation: int
    catalog_name: str
    target: str
    project: str
    hardware: str
    version: str
    sha256: str
    crc32: int
    size: int
    partition_capacity: int
    allow_same_version: bool
    decision: str
    complete_image_validated: bool
    image_writes_before: int
    image_writes_after: int
    boot_id_before: int
    boot_id_after: int
    rollback_clear: bool
    converged: bool
    captured_at: int = 0


@dataclass(frozen=True)
class VerifiedReleaseArtifact:
    index_path: str
    index_sha256: str
    artifact_directory: str
    kind: str
    target: str
    project: str
    hardware: str
    version: str
    identity_crc32: int
    firmware_path: str
    firmware_size: int
    firmware_sha256: str
    firmware_crc32: int
    offsets_sha256: str
    parts: tuple[dict[str, Any], ...]


@dataclass
class BoardRecord:
    role: str
    installed: BoardIdentity | None = None
    inventory: BoardIdentity | None = None
    inventory_toolchain_sha256: str | None = None
    lite_sensor_confirmed: bool = False
    no_sd_expansion_confirmed: bool = False
    backups: dict[str, BackupRecord] = field(default_factory=dict)
    provisional: dict[str, Any] | None = None
    final_health: dict[str, Any] | None = None
    status: str = "pending"
    failure_phase: str | None = None
    failure_reason: str | None = None
    flashed_backend_partition_sha256: str | None = None


def _canonical_json(value: Any) -> bytes:
    return json.dumps(
        value,
        sort_keys=True,
        separators=(",", ":"),
        ensure_ascii=True,
    ).encode("ascii")


def _require_hex64(value: str, label: str) -> str:
    if not isinstance(value, str) or HEX64.fullmatch(value) is None:
        raise CanarySecurityError(f"{label} must be an exact 64-hex SHA-256")
    return value.lower()


def normalize_mac(value: str) -> str:
    if not isinstance(value, str):
        raise CanaryInventoryError("MAC is missing")
    normalized = value.upper()
    if MAC_RE.fullmatch(normalized) is None:
        raise CanaryInventoryError(f"invalid MAC: {value!r}")
    first = int(normalized[:2], 16)
    if first & 1 or normalized == "00:00:00:00:00:00":
        raise CanaryInventoryError("MAC must be a nonzero unicast address")
    return normalized


def _role_kind(role: str) -> str:
    if role not in BOARD_ROLES:
        raise CanaryInventoryError(f"unknown board role: {role}")
    return "uplink" if role == "uplink" else "scanner"


def _private_mode(path: Path, expected: int, label: str) -> None:
    try:
        mode = stat.S_IMODE(path.stat().st_mode)
    except FileNotFoundError as exc:
        raise CanaryBackupError(f"missing {label}: {path}") from exc
    if mode != expected:
        raise CanaryBackupError(
            f"{label} must have mode {expected:04o}, found {mode:04o}: {path}"
        )


def _hash_file(path: Path) -> tuple[int, str, int]:
    digest = hashlib.sha256()
    crc = 0
    size = 0
    with path.open("rb") as handle:
        while True:
            block = handle.read(1024 * 1024)
            if not block:
                break
            size += len(block)
            digest.update(block)
            crc = zlib.crc32(block, crc)
    return size, digest.hexdigest(), crc & 0xFFFFFFFF


def _write_all(descriptor: int, payload: bytes) -> None:
    view = memoryview(payload)
    while view:
        written = os.write(descriptor, view)
        if written <= 0:
            raise CanarySecurityError("short private file write")
        view = view[written:]


def redact_secrets(value: Any) -> Any:
    if isinstance(value, Mapping):
        return {
            str(key): "[REDACTED]" if SECRET_KEY.search(str(key))
            else redact_secrets(item)
            for key, item in value.items()
        }
    if isinstance(value, list):
        return [redact_secrets(item) for item in value]
    if isinstance(value, tuple):
        return tuple(redact_secrets(item) for item in value)
    return value


def secure_directory(path: Path) -> Path:
    resolved = path.expanduser().resolve()
    resolved.mkdir(parents=True, exist_ok=True, mode=0o700)
    os.chmod(resolved, 0o700)
    _private_mode(resolved, 0o700, "canary directory")
    return resolved


def secure_write_json(path: Path, value: Any, *, exclusive: bool = True) -> None:
    parent = secure_directory(path.parent)
    destination = (parent / path.name).resolve()
    if destination.parent != parent:
        raise CanarySecurityError("secure output path escapes its directory")
    flags = os.O_WRONLY | os.O_CREAT | (os.O_EXCL if exclusive else os.O_TRUNC)
    descriptor = os.open(destination, flags, 0o600)
    try:
        payload = json.dumps(
            redact_secrets(value), sort_keys=True, indent=2
        ).encode("utf-8") + b"\n"
        _write_all(descriptor, payload)
        os.fsync(descriptor)
    finally:
        os.close(descriptor)
    os.chmod(destination, 0o600)
    directory_fd = os.open(parent, os.O_RDONLY)
    try:
        os.fsync(directory_fd)
    finally:
        os.close(directory_fd)


def secure_write_bytes(path: Path, payload: bytes, *, exclusive: bool = True) -> None:
    """Create a private file without following a pre-existing destination."""
    if not isinstance(payload, bytes):
        raise CanarySecurityError("secure payload must be bytes")
    parent = secure_directory(path.parent)
    destination = (parent / path.name).resolve()
    if destination.parent != parent:
        raise CanarySecurityError("secure output path escapes its directory")
    flags = os.O_WRONLY | os.O_CREAT | (os.O_EXCL if exclusive else os.O_TRUNC)
    descriptor = os.open(destination, flags, 0o600)
    try:
        _write_all(descriptor, payload)
        os.fsync(descriptor)
    finally:
        os.close(descriptor)
    os.chmod(destination, 0o600)
    directory_fd = os.open(parent, os.O_RDONLY)
    try:
        os.fsync(directory_fd)
    finally:
        os.close(directory_fd)


def _copy_exclusive(source: Path, destination: Path) -> None:
    parent = secure_directory(destination.parent)
    resolved = destination.resolve()
    if resolved.parent != parent:
        raise CanarySecurityError("backup destination escapes its directory")
    descriptor = os.open(resolved, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    try:
        with source.open("rb") as incoming, os.fdopen(
            os.dup(descriptor), "wb", closefd=True
        ) as outgoing:
            shutil.copyfileobj(incoming, outgoing, length=1024 * 1024)
            outgoing.flush()
            os.fsync(outgoing.fileno())
    finally:
        os.close(descriptor)
    os.chmod(resolved, 0o600)


def _run_checked(
    command: Sequence[str],
    *,
    runner: Callable[..., subprocess.CompletedProcess[str]] = subprocess.run,
) -> subprocess.CompletedProcess[str]:
    try:
        completed = runner(
            list(command),
            check=False,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            env={**os.environ, "PYTHONUNBUFFERED": "1"},
        )
    except OSError as exc:
        raise CanarySecurityError(
            f"unable to execute pinned command: {command[0]}"
        ) from exc
    if completed.returncode != 0:
        output = redact_text(completed.stdout or "")
        raise CanarySecurityError(
            f"pinned command failed ({completed.returncode}): {output[-1000:]}"
        )
    return completed


def redact_text(value: str) -> str:
    """Redact common HTTP/JSON secret assignments from diagnostic text."""
    result = value
    patterns = (
        r'(?i)(password|secret|credential|token|authorization|cookie|set-cookie|api_key)(\s*[=:]\s*)[^\s,}\"]+',
        r'(?i)("(?:password|secret|credential|token|authorization|cookie|set-cookie|api_key)"\s*:\s*)"[^"]*"',
    )
    for pattern in patterns:
        result = re.sub(pattern, lambda match: match.group(1) + "[REDACTED]", result)
    return result


def resolve_toolchain(
    requested_pio: Path,
    *,
    runner: Callable[..., subprocess.CompletedProcess[str]] = subprocess.run,
) -> ToolchainReceipt:
    """Resolve PlatformIO and its Python/esptool as one immutable receipt."""
    try:
        pio = requested_pio.expanduser().resolve(strict=True)
    except OSError as exc:
        raise CanarySecurityError("requested PlatformIO executable is missing") from exc
    if not pio.is_file() or not os.access(pio, os.X_OK):
        raise CanarySecurityError("requested PlatformIO path is not executable")
    info_result = _run_checked(
        [str(pio), "system", "info", "--json-output"], runner=runner
    )
    try:
        info = _load_json_no_duplicates(info_result.stdout)
    except CanaryInventoryError as exc:
        raise CanarySecurityError("PlatformIO system info is not strict JSON") from exc
    if not isinstance(info, dict):
        raise CanarySecurityError("PlatformIO system info is not an object")
    version = info.get("platformio_version")
    python_value = info.get("python_exe")
    core_value = info.get("core_dir")
    if not all(isinstance(item, str) and item for item in (version, python_value, core_value)):
        raise CanarySecurityError("PlatformIO version/python/core receipt is incomplete")
    try:
        python_exe = Path(python_value).expanduser().resolve(strict=True)
        core_dir = Path(core_value).expanduser().resolve(strict=True)
        esptool = (
            core_dir / "packages" / "tool-esptoolpy" / "esptool.py"
        ).resolve(strict=True)
    except OSError as exc:
        raise CanarySecurityError("PlatformIO Python/core/esptool is missing") from exc
    if not python_exe.is_file() or not os.access(python_exe, os.X_OK):
        raise CanarySecurityError("PlatformIO Python is not executable")
    if not core_dir.is_dir() or not esptool.is_file():
        raise CanarySecurityError("PlatformIO core/esptool path is invalid")
    size, esptool_sha, _crc = _hash_file(esptool)
    if size <= 0:
        raise CanarySecurityError("PlatformIO esptool is empty")
    version_result = _run_checked(
        [str(python_exe), str(esptool), "version"], runner=runner
    )
    match = re.search(r"(?i)esptool(?:\.py)?\s+v?([0-9]+(?:\.[0-9]+)+)", version_result.stdout)
    if match is None:
        raise CanarySecurityError("unable to parse pinned esptool version")
    return ToolchainReceipt(
        pio_path=str(pio),
        platformio_version=version,
        python_exe=str(python_exe),
        core_dir=str(core_dir),
        esptool_path=str(esptool),
        esptool_version=match.group(1),
        esptool_sha256=esptool_sha,
    )


def require_toolchain_binding(
    state: "CanaryState", receipt: ToolchainReceipt, *, initialize: bool = False
) -> None:
    if state.toolchain is None:
        if not initialize:
            raise CanarySecurityError("canary state has no PlatformIO receipt")
        state.toolchain = receipt
        state._touch()
        return
    if state.toolchain != receipt or state.toolchain.sha256 != receipt.sha256:
        raise CanarySecurityError("PlatformIO/toolchain receipt changed")


def canonical_partition_sha256(entries: Sequence[PartitionEntry]) -> str:
    canonical = [
        {
            "label": label,
            "type": part_type,
            "subtype": subtype,
            "offset": offset,
            "size": size,
        }
        for label, part_type, subtype, offset, size in entries
    ]
    return hashlib.sha256(_canonical_json(canonical)).hexdigest()


def decode_partition_table(data: bytes) -> tuple[PartitionEntry, ...]:
    if not isinstance(data, bytes) or len(data) < 32:
        raise CanaryReleaseError("partition table is missing or truncated")
    type_names = {0x00: "app", 0x01: "data"}
    subtype_names = {
        (0x00, 0x10): "ota_0",
        (0x00, 0x11): "ota_1",
        (0x01, 0x00): "ota",
        (0x01, 0x01): "phy",
        (0x01, 0x02): "nvs",
        (0x01, 0x40): "0x40",
        (0x01, 0x81): "fat",
        (0x01, 0x82): "spiffs",
    }
    entries: list[PartitionEntry] = []
    seen_labels: set[str] = set()
    for position in range(0, len(data) - 31, 32):
        raw = data[position:position + 32]
        if raw == bytes([0xFF]) * 32:
            break
        magic, part_type, subtype, offset, size, label_raw, _flags = struct.unpack(
            "<HBBII16sI", raw
        )
        if magic == 0xEBEB:
            break
        if magic != 0x50AA or part_type not in type_names:
            raise CanaryReleaseError("partition table contains an invalid entry")
        try:
            end = label_raw.index(0)
            label = label_raw[:end].decode("ascii", "strict")
        except (ValueError, UnicodeDecodeError) as exc:
            raise CanaryReleaseError("partition label is not a strict C string") from exc
        if not label or label in seen_labels or size <= 0:
            raise CanaryReleaseError("partition labels/sizes must be unique and positive")
        subtype_name = subtype_names.get((part_type, subtype), f"0x{subtype:02x}")
        entries.append((label, type_names[part_type], subtype_name, offset, size))
        seen_labels.add(label)
    if not entries:
        raise CanaryReleaseError("partition table contains no entries")
    previous_end = 0
    for _label, _type, _subtype, offset, size in entries:
        end = offset + size
        if offset < previous_end or end > FLASH_SIZE:
            raise CanaryReleaseError("partition range overlaps or exceeds 8 MB")
        previous_end = end
    return tuple(entries)


def _esptool_prefix(
    esptool: Path,
    python_exe: Path | None = None,
) -> list[str]:
    tool = esptool.expanduser()
    python = python_exe.expanduser() if python_exe is not None else Path(sys.executable)
    return [str(python), str(tool)]


def _base_esptool_command(
    esptool: Path,
    port: str,
    *,
    python_exe: Path | None = None,
    after: str = "no_reset",
) -> list[str]:
    if not port or not port.startswith("/dev/"):
        raise CanaryInventoryError("an explicit /dev serial port is required")
    return _esptool_prefix(esptool, python_exe) + [
        "--chip", "esp32s3",
        "--port", port,
        "--before", "default_reset",
        "--after", after,
    ]


def _reject_dangerous(values: Sequence[str] | None) -> None:
    if values is None:
        return
    for value in values:
        normalized = value.strip().lower()
        if normalized in DANGEROUS_FLASH_OPTIONS or "force" in normalized or "encrypt" in normalized or "erase" in normalized:
            raise CanarySecurityError(f"dangerous flash option rejected: {value}")


def build_backup_commands(
    esptool: Path,
    port: str,
    output_dir: Path,
    role: str,
    flash_size: int,
    *,
    python_exe: Path | None = None,
) -> list[list[str]]:
    _role_kind(role)
    if flash_size != FLASH_SIZE:
        raise CanaryInventoryError("backup requires exactly 8 MB flash")
    output = output_dir
    base = _base_esptool_command(
        esptool, port, python_exe=python_exe, after="no_reset"
    )
    reads = (
        ("0x0", "0x800000", output / f"{role}-full-a.tmp"),
        ("0x0", "0x800000", output / f"{role}-full-b.tmp"),
        ("0x9000", "0x6000", output / f"{role}-nvs-a.tmp"),
        ("0x9000", "0x6000", output / f"{role}-nvs-b.tmp"),
        ("0x8000", "0x1000", output / f"{role}-partition.tmp"),
    )
    return [base + ["read_flash", offset, size, str(path)] for offset, size, path in reads]


def build_inventory_commands(
    receipt: ToolchainReceipt,
    port: str,
) -> dict[str, list[str]]:
    base = _base_esptool_command(
        Path(receipt.esptool_path),
        port,
        python_exe=Path(receipt.python_exe),
        after="no_reset",
    )
    return {
        "security": base + ["get_security_info"],
        "flash": base + ["flash_id"],
        "mac": base + ["read_mac"],
    }


def _one_regex(pattern: str, text: str, label: str) -> str:
    values = {match.group(1) for match in re.finditer(pattern, text, re.IGNORECASE)}
    if len(values) != 1:
        raise CanaryInventoryError(f"live {label} is missing or conflicting")
    return next(iter(values))


def parse_live_inventory(
    installed: BoardIdentity,
    *,
    role: str,
    port: str,
    outputs: Mapping[str, str],
) -> BoardIdentity:
    if set(outputs) != {"security", "flash", "mac"}:
        raise CanaryInventoryError("live inventory probe set is incomplete")
    security = outputs["security"]
    flash = outputs["flash"]
    mac_output = outputs["mac"]
    chip = _one_regex(r"Chip is\s+([^\r\n(]+)", security, "chip").strip()
    normalized_chip = chip.upper().replace("_", "-").replace(" ", "")
    if normalized_chip not in ("ESP32-S3", "ESP32S3"):
        raise CanaryInventoryError("live chip is not ESP32-S3")

    def disabled(label: str) -> bool:
        match = re.search(
            rf"{label}\s*(?:is|:|=)?\s*(Enabled|Disabled|True|False|Yes|No)",
            security,
            re.IGNORECASE,
        )
        if match is None:
            raise CanaryInventoryError(f"live {label} state is missing")
        return match.group(1).lower() in ("disabled", "false", "no")

    secure_boot_disabled = disabled(r"Secure\s+Boot")
    encryption_disabled = disabled(r"Flash\s+Encryption")
    if not secure_boot_disabled:
        raise CanaryInventoryError("secure boot is enabled; hardware write refused")
    if not encryption_disabled:
        raise CanaryInventoryError("flash encryption is enabled; hardware write refused")
    flash_size = _one_regex(
        r"(?:Detected\s+)?flash\s+size\s*:\s*([0-9]+\s*(?:MB|MiB))",
        flash,
        "flash size",
    ).replace(" ", "").upper()
    if flash_size not in ("8MB", "8MIB"):
        raise CanaryInventoryError("live flash is not exactly 8 MB")
    mac = normalize_mac(
        _one_regex(r"\bMAC\s*:\s*([0-9A-Fa-f:]{17})\b", mac_output, "MAC")
    )
    if role != installed.role:
        raise CanaryInventoryError("live role does not match installed evidence")
    return replace(
        installed,
        port=port,
        chip="ESP32-S3",
        mac=mac,
        flash_size=FLASH_SIZE,
        secure_boot_enabled=False,
        flash_encryption_enabled=False,
    )


def probe_live_inventory(
    installed: BoardIdentity,
    *,
    role: str,
    port: str,
    receipt: ToolchainReceipt,
    runner: Callable[..., subprocess.CompletedProcess[str]] = subprocess.run,
    transcript_path: Path | None = None,
) -> BoardIdentity:
    outputs: dict[str, str] = {}
    commands = build_inventory_commands(receipt, port)
    for name in ("security", "flash", "mac"):
        outputs[name] = _run_checked(commands[name], runner=runner).stdout
    identity = parse_live_inventory(
        installed, role=role, port=port, outputs=outputs
    )
    if transcript_path is not None:
        secure_write_json(
            transcript_path,
            {
                "role": role,
                "port": port,
                "toolchain_sha256": receipt.sha256,
                "outputs": outputs,
                "identity": asdict(identity),
            },
        )
    return identity


def _artifact_target(artifact_dir: Path) -> str:
    target = artifact_dir.name
    if target not in ("scanner-s3-combo-backend", "uplink-s3-backend"):
        raise CanaryReleaseError("artifact directory is not an exact backend target")
    return target


def validate_flash_ranges(ranges: Sequence[tuple[int, int]]) -> None:
    if len(ranges) != 4:
        raise CanaryReleaseError("exactly four flash ranges are required")
    expected_offsets = (0x0, 0x8000, 0xF000, 0x20000)
    normalized: list[tuple[int, int]] = []
    for index, ((offset, size), expected) in enumerate(zip(ranges, expected_offsets)):
        if not isinstance(offset, int) or not isinstance(size, int) or offset != expected or size <= 0:
            raise CanaryReleaseError("flash range offset/size is not exact")
        end = offset + size
        if end > FLASH_SIZE:
            raise CanaryReleaseError("flash range exceeds 8 MB")
        if index == 0 and end > 0x8000:
            raise CanaryReleaseError("bootloader crosses partition table or NVS")
        if index == 1 and (offset, end) != (0x8000, 0x9000):
            raise CanaryReleaseError("partition table must occupy 0x8000..0x8fff")
        if index == 2 and (offset, end) != (0xF000, 0x11000):
            raise CanaryReleaseError("OTA data must occupy 0xf000..0x10fff")
        if index == 3 and end > 0x220000:
            raise CanaryReleaseError("application crosses its 2 MB partition")
        if offset < NVS_OFFSET + NVS_SIZE and end > NVS_OFFSET:
            raise CanaryReleaseError("flash range intersects protected NVS")
        normalized.append((offset, end))
    for left_index, left in enumerate(normalized):
        for right in normalized[left_index + 1:]:
            if left[0] < right[1] and right[0] < left[1]:
                raise CanaryReleaseError("flash ranges overlap")


def build_initial_flash_command(
    esptool: Path,
    port: str,
    artifact_dir: Path,
    *,
    python_exe: Path | None = None,
    extra_args: Sequence[str] | None = None,
) -> list[str]:
    _reject_dangerous(extra_args)
    target = _artifact_target(artifact_dir)
    files = {
        logical: artifact_dir / f"{target}-{logical}.bin"
        for logical in EXPECTED_PART_OFFSETS
    }
    ranges = (
        (0x0, files["bootloader"].stat().st_size if files["bootloader"].is_file() else 1),
        (0x8000, 0x1000),
        (0xF000, 0x2000),
        (0x20000, files["firmware"].stat().st_size if files["firmware"].is_file() else 1),
    )
    validate_flash_ranges(ranges)
    command = _base_esptool_command(
        esptool, port, python_exe=python_exe, after="no_reset"
    )
    command += [
        "write_flash",
        "--flash_mode", "dio",
        "--flash_freq", "80m",
        "--flash_size", "8MB",
        "--verify",
    ]
    for logical, offset in EXPECTED_PART_OFFSETS.items():
        command += [hex(offset), str(files[logical])]
    return command


def build_restore_command(
    esptool: Path,
    port: str,
    backup: BackupRecord,
    *,
    python_exe: Path | None = None,
) -> list[str]:
    if backup.full_size != FLASH_SIZE:
        raise CanaryBackupError("restore source must be one exact 8 MB backup")
    command = _base_esptool_command(
        esptool, port, python_exe=python_exe, after="no_reset"
    )
    return command + [
        "write_flash",
        "--flash_mode", "dio",
        "--flash_freq", "80m",
        "--flash_size", "8MB",
        "--verify",
        "0x0", backup.full_path,
    ]


def build_run_command(
    receipt: ToolchainReceipt,
    port: str,
) -> list[str]:
    return _base_esptool_command(
        Path(receipt.esptool_path),
        port,
        python_exe=Path(receipt.python_exe),
        after="hard_reset",
    ) + ["run"]


def build_nvs_read_command(
    receipt: ToolchainReceipt,
    port: str,
    output: Path,
) -> list[str]:
    return _base_esptool_command(
        Path(receipt.esptool_path),
        port,
        python_exe=Path(receipt.python_exe),
        after="no_reset",
    ) + ["read_flash", hex(NVS_OFFSET), hex(NVS_SIZE), str(output)]


def _precreate_private(path: Path) -> None:
    parent = secure_directory(path.parent)
    resolved = path.resolve()
    if resolved.parent != parent:
        raise CanarySecurityError("temporary path escapes private directory")
    descriptor = os.open(resolved, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    os.close(descriptor)


def _link_exclusive(source: Path, destination: Path) -> None:
    parent = secure_directory(destination.parent)
    resolved = destination.resolve()
    if resolved.parent != parent:
        raise CanarySecurityError("retained backup escapes private directory")
    try:
        os.link(source, resolved, follow_symlinks=False)
    except FileExistsError as exc:
        raise CanaryBackupError(f"backup filename already exists: {resolved}") from exc
    os.chmod(resolved, 0o600)
    descriptor = os.open(resolved, os.O_RDONLY)
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def execute_backup(
    state: "CanaryState",
    *,
    role: str,
    kind: str,
    output_dir: Path,
    receipt: ToolchainReceipt,
    runner: Callable[..., subprocess.CompletedProcess[str]] = subprocess.run,
    now: int | None = None,
) -> BackupRecord:
    """Perform the duplicate full/NVS/table read proof without any write."""
    _role_kind(role)
    if kind not in ("original", "backend-baseline"):
        raise CanaryBackupError("backup kind must be original or backend-baseline")
    require_toolchain_binding(state, receipt)
    board = state.boards[role]
    inventory = board.inventory
    if inventory is None:
        raise CanaryOrderError(f"{role} inventory is required before backup")
    if any(item.inventory is None for item in state.boards.values()):
        raise CanaryOrderError("all three inventories are required before backup")
    if kind in board.backups:
        raise CanaryBackupError(f"{kind} backup already exists for {role}")
    if kind == "original":
        completed = [
            item for item in BOARD_ROLES
            if "original" in state.boards[item].backups
        ]
        required = BOARD_ROLES[len(completed)] if len(completed) < 3 else None
        if role != required:
            raise CanaryOrderError(f"original backup order requires {required}")
    else:
        if any(item.final_health is None for item in state.boards.values()):
            raise CanaryOrderError(
                "all final health evidence is required before backend-baseline backup"
            )
        if state.catalog_evidence_sha256 is None:
            raise CanaryOrderError(
                "catalog preflight is required before backend-baseline backup"
            )

    mac_slug = inventory.mac.replace(":", "")
    directory = secure_directory(
        output_dir.expanduser().resolve() / kind / f"{role}-{mac_slug}"
    )
    commands = build_backup_commands(
        Path(receipt.esptool_path),
        inventory.port,
        directory,
        role,
        inventory.flash_size,
        python_exe=Path(receipt.python_exe),
    )
    temp_paths = [Path(command[-1]) for command in commands]
    for path in temp_paths:
        _precreate_private(path)
    transcripts: list[dict[str, Any]] = []
    for command in commands:
        result = _run_checked(command, runner=runner)
        output_path = Path(command[-1])
        os.chmod(output_path, 0o600)
        transcripts.append(
            {"command": command[:-1] + [output_path.name], "output": result.stdout}
        )

    full_a, full_b, nvs_a, nvs_b, table = temp_paths
    full_a_info = _hash_file(full_a)
    full_b_info = _hash_file(full_b)
    nvs_a_info = _hash_file(nvs_a)
    nvs_b_info = _hash_file(nvs_b)
    table_info = _hash_file(table)
    if full_a_info[:2] != full_b_info[:2] or full_a_info[0] != FLASH_SIZE:
        raise CanaryBackupError("independent full-flash reads differ or are not 8 MB")
    if nvs_a_info[:2] != nvs_b_info[:2] or nvs_a_info[0] != NVS_SIZE:
        raise CanaryBackupError("independent focused NVS reads differ")
    if table_info[0] != PARTITION_SECTOR_SIZE:
        raise CanaryBackupError("focused partition-table read is not one sector")
    focused_nvs = nvs_a.read_bytes()
    focused_table = table.read_bytes()
    for full in (full_a, full_b):
        with full.open("rb") as handle:
            handle.seek(NVS_OFFSET)
            if handle.read(NVS_SIZE) != focused_nvs:
                raise CanaryBackupError("full-flash and focused NVS slices differ")
            handle.seek(PARTITION_OFFSET)
            if handle.read(PARTITION_SECTOR_SIZE) != focused_table:
                raise CanaryBackupError(
                    "full-flash and focused partition-table slices differ"
                )
    decoded = decode_partition_table(focused_table)
    expected = (LEGACY_PARTITIONS if kind == "original" else BACKEND_PARTITIONS)[
        _role_kind(role)
    ]
    decoded_sha = canonical_partition_sha256(decoded)
    if decoded_sha != canonical_partition_sha256(expected):
        raise CanaryBackupError(
            f"{kind} partition table is not the exact recognized {_role_kind(role)} table"
        )

    names = {
        "full": directory / f"{role}-{mac_slug}-{kind}-full.bin",
        "nvs": directory / f"{role}-{mac_slug}-{kind}-nvs.bin",
        "partition": directory / f"{role}-{mac_slug}-{kind}-partition.bin",
    }
    _link_exclusive(full_a, names["full"])
    _link_exclusive(nvs_a, names["nvs"])
    _link_exclusive(table, names["partition"])
    for temporary in temp_paths:
        temporary.unlink()
    secure_write_json(
        directory / f"{role}-{mac_slug}-{kind}-transcript.json",
        {
            "schema": 1,
            "role": role,
            "kind": kind,
            "mac": inventory.mac,
            "toolchain_sha256": receipt.sha256,
            "commands": transcripts,
        },
    )
    record = BackupRecord(
        role=role,
        kind=kind,
        mac=inventory.mac,
        full_path=str(names["full"]),
        full_size=full_a_info[0],
        full_sha256=full_a_info[1],
        nvs_path=str(names["nvs"]),
        nvs_size=nvs_a_info[0],
        nvs_sha256=nvs_a_info[1],
        partition_path=str(names["partition"]),
        partition_size=table_info[0],
        partition_sha256=table_info[1],
        decoded_partition_sha256=decoded_sha,
        toolchain_sha256=receipt.sha256,
    )
    state.record_backup(role, kind, record)
    if kind == "original" and role == "uplink":
        state.record_original_uplink_quiesced(
            port=inventory.port,
            mac=inventory.mac,
            toolchain_sha256=receipt.sha256,
            now=int(time.time()) if now is None else now,
        )
    else:
        _run_checked(build_run_command(receipt, inventory.port), runner=runner)
    return record


def build_ota_apply_line(
    *,
    component: str,
    sha256: str,
    mode: str,
    target_mac: str,
    target_boot_id: int,
    topology_generation: int,
) -> str:
    if component not in COMPONENTS:
        raise CanaryApprovalError("OTA component is not exact")
    digest = _require_hex64(sha256, "OTA artifact SHA")
    if mode not in ("newer-only", "same-version-recovery"):
        raise CanaryApprovalError("OTA apply mode is not exact")
    mac = normalize_mac(target_mac)
    if target_boot_id <= 0 or topology_generation < 0:
        raise CanaryApprovalError("OTA boot/topology binding is invalid")
    if component != "uplink" and topology_generation == 0:
        raise CanaryApprovalError("scanner OTA requires a topology generation")
    if component == "uplink" and topology_generation != 0:
        raise CanaryApprovalError("uplink OTA topology generation must be zero")
    return (
        f"FOF_BACKEND_OTA_APPLY {component} {digest} {mode} "
        f"{mac} {target_boot_id} {topology_generation}\n"
    )


def build_ota_probe_line(
    *, component: str, catalog_name: str, expected_sha256: str
) -> str:
    if component not in COMPONENTS:
        raise CanaryApprovalError("OTA probe component is not exact")
    expected_catalog = BACKEND_IDENTITIES[_role_kind(component)][0]
    if catalog_name != expected_catalog:
        raise CanaryApprovalError("OTA probe catalog is not the exact backend target")
    digest = _require_hex64(expected_sha256, "OTA probe SHA")
    return f"FOF_BACKEND_OTA_PROBE {component} {catalog_name} {digest}\n"


def _open_raw_serial(port: str) -> int:
    if not port.startswith("/dev/"):
        raise CanaryInventoryError("serial port must be an explicit /dev path")
    flags = os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK
    descriptor = os.open(port, flags)
    try:
        attributes = termios.tcgetattr(descriptor)
        attributes[0] = 0
        attributes[1] = 0
        attributes[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
        attributes[3] = 0
        attributes[4] = termios.B921600
        attributes[5] = termios.B921600
        attributes[6][termios.VMIN] = 0
        attributes[6][termios.VTIME] = 0
        termios.tcsetattr(descriptor, termios.TCSANOW, attributes)
        termios.tcflush(descriptor, termios.TCIOFLUSH)
    except BaseException:
        os.close(descriptor)
        raise
    return descriptor


def serial_exchange(
    *,
    port: str,
    command: str,
    expected_prefix: str,
    timeout: int,
    opener: Callable[[str], int] = _open_raw_serial,
) -> dict[str, Any]:
    """Write one canonical command and accept one bounded evidence record."""
    if timeout <= 0:
        raise CanaryInventoryError("serial timeout must be positive")
    if not command.endswith("\n") or "\n" in command[:-1] or "\r" in command:
        raise CanarySecurityError("serial command must be one canonical newline")
    encoded = command.encode("ascii", "strict")
    descriptor = opener(port)
    buffer = bytearray()
    records: list[dict[str, Any]] = []
    deadline = time.monotonic() + timeout
    quiet_deadline: float | None = None
    try:
        written = os.write(descriptor, encoded)
        if written != len(encoded):
            raise CanarySecurityError("serial command was not written atomically")
        while time.monotonic() < deadline:
            remaining = deadline - time.monotonic()
            if quiet_deadline is not None:
                remaining = min(remaining, max(0.0, quiet_deadline - time.monotonic()))
                if remaining <= 0:
                    break
            readable, _writable, _errors = select.select(
                [descriptor], [], [descriptor], min(remaining, 0.25)
            )
            if not readable:
                continue
            try:
                chunk = os.read(descriptor, 1024)
            except BlockingIOError:
                continue
            if not chunk:
                continue
            buffer.extend(chunk)
            if len(buffer) > MAX_SERIAL_LINE * 8:
                raise CanaryInventoryError("serial evidence transcript is oversized")
            while b"\n" in buffer:
                raw, _, tail = buffer.partition(b"\n")
                buffer[:] = tail
                if len(raw) > MAX_SERIAL_LINE:
                    raise CanaryInventoryError("serial evidence line is oversized")
                try:
                    line = raw.rstrip(b"\r").decode("utf-8", "strict")
                except UnicodeError as exc:
                    raise CanaryInventoryError("serial evidence is not UTF-8") from exc
                if line.startswith(expected_prefix + " "):
                    records.append(parse_evidence_line(line, expected_prefix))
                    if len(records) > 1:
                        raise CanaryInventoryError(
                            f"expected exactly one {expected_prefix} evidence line"
                        )
                    quiet_deadline = time.monotonic() + 0.2
        if len(records) != 1:
            raise CanaryInventoryError(
                f"timed out waiting for exactly one {expected_prefix} evidence line"
            )
        return records[0]
    finally:
        os.close(descriptor)


OTA_ACCEPTED_KEYS = {
    "schema", "operation_id", "component", "component_slot", "sha256",
    "crc32", "uplink_mac", "expected_target_mac", "actual_target_mac",
    "expected_target_boot_id", "actual_target_boot_id",
    "expected_topology_generation", "actual_topology_generation",
    "boot_id_before",
}


def parse_ota_accepted(value: Mapping[str, Any]) -> dict[str, Any]:
    _require_exact_keys(value, OTA_ACCEPTED_KEYS, "OTA accepted evidence")
    component = value["component"]
    if component not in COMPONENTS:
        raise CanaryApprovalError("OTA accepted component is not exact")
    expected_slot = {"uplink": -1, "scanner0": 0, "scanner1": 1}[component]
    if value["component_slot"] != expected_slot:
        raise CanaryApprovalError("OTA accepted physical slot changed")
    normalized = dict(value)
    normalized["sha256"] = _require_hex64(value["sha256"], "OTA accepted SHA")
    for key in ("uplink_mac", "expected_target_mac", "actual_target_mac"):
        normalized[key] = normalize_mac(value[key])
    if normalized["expected_target_mac"] != normalized["actual_target_mac"]:
        raise CanaryApprovalError("OTA accepted target MAC binding changed")
    if value["expected_target_boot_id"] != value["actual_target_boot_id"]:
        raise CanaryApprovalError("OTA accepted target boot ID binding changed")
    if value["boot_id_before"] != value["actual_target_boot_id"]:
        raise CanaryApprovalError("OTA accepted pre-apply boot ID changed")
    if value["expected_topology_generation"] != value["actual_topology_generation"]:
        raise CanaryApprovalError("OTA accepted topology generation changed")
    if value["schema"] != 1 or not isinstance(value["operation_id"], int) or value["operation_id"] <= 0:
        raise CanaryApprovalError("OTA accepted schema/operation ID is invalid")
    if not isinstance(value["crc32"], int) or isinstance(value["crc32"], bool) or not 0 <= value["crc32"] <= 0xFFFFFFFF:
        raise CanaryApprovalError("OTA accepted CRC32 is invalid")
    return normalized


def load_private_json(path: Path, *, label: str) -> tuple[dict[str, Any], str]:
    resolved = path.expanduser().resolve(strict=True)
    _private_mode(resolved.parent, 0o700, f"{label} directory")
    _private_mode(resolved, 0o600, label)
    raw = resolved.read_bytes()
    try:
        parsed = _load_json_no_duplicates(raw.decode("utf-8", "strict"))
    except (UnicodeError, CanaryInventoryError) as exc:
        raise CanarySecurityError(f"{label} is not strict JSON") from exc
    if not isinstance(parsed, dict):
        raise CanarySecurityError(f"{label} is not a JSON object")
    return parsed, hashlib.sha256(raw).hexdigest()


def validate_catalog_evidence(
    value: Mapping[str, Any],
    *,
    index: Path,
    now: int,
) -> str:
    expected_keys = {
        "schema", "backend_base", "index_path", "index_sha256",
        "captured_at", "targets",
    }
    if set(value) != expected_keys or value.get("schema") != 1:
        raise CanaryReleaseError("catalog evidence fields are not exact")
    document, raw_index = _load_release_index(index)
    if value.get("index_sha256") != hashlib.sha256(raw_index).hexdigest():
        raise CanaryReleaseError("catalog evidence index hash changed")
    captured = value.get("captured_at")
    if not isinstance(captured, int) or now < captured or now - captured > CATALOG_TTL_SECONDS:
        raise CanaryApprovalError("catalog evidence is missing, future, or stale")
    targets = value.get("targets")
    if not isinstance(targets, dict) or set(targets) != set(document["targets"]):
        raise CanaryReleaseError("catalog evidence target set changed")
    for target in targets:
        part = _index_firmware(document, target)
        expected = targets[target]
        if not isinstance(expected, dict):
            raise CanaryReleaseError("catalog target evidence is malformed")
        for key, wanted in (
            ("target", target),
            ("project", document["targets"][target]["project"]),
            ("hardware", document["targets"][target]["hardware"]),
            ("version", BACKEND_VERSION),
            ("size", part["size"]),
            ("sha256", part["sha256"]),
            ("crc32", part["crc32"]),
            ("firmware_basename", part["name"]),
        ):
            if expected.get(key) != wanted:
                raise CanaryReleaseError(
                    f"catalog evidence {target} {key} changed"
                )
    return hashlib.sha256(_canonical_json(value)).hexdigest()


def run_ota_probe(
    state: "CanaryState",
    *,
    component: str,
    catalog_name: str,
    expected_sha256: str,
    catalog_evidence: Mapping[str, Any],
    index: Path,
    port: str,
    receipt: ToolchainReceipt,
    timeout: int,
    output: Path | None = None,
    now: int | None = None,
    record: bool = True,
) -> OtaEvidence:
    require_toolchain_binding(state, receipt)
    if any(board.final_health is None for board in state.boards.values()):
        raise CanaryOrderError("all final health is required before OTA probe")
    uplink = state.boards["uplink"].inventory
    if uplink is None or port != uplink.port:
        raise CanaryInventoryError("OTA probe port is not the recorded uplink")
    current = int(time.time()) if now is None else now
    validate_catalog_evidence(catalog_evidence, index=index, now=current)
    document, _raw_index = _load_release_index(index)
    target = BACKEND_IDENTITIES[_role_kind(component)][0]
    if catalog_name != target:
        raise CanaryApprovalError("OTA probe catalog/component pairing changed")
    part = _index_firmware(document, target)
    digest = _require_hex64(expected_sha256, "OTA probe expected SHA")
    if digest != part["sha256"]:
        raise CanaryReleaseError("OTA probe SHA does not match release index")
    command = build_ota_probe_line(
        component=component,
        catalog_name=catalog_name,
        expected_sha256=digest,
    )
    raw = serial_exchange(
        port=port,
        command=command,
        expected_prefix="FOF_BACKEND_OTA_EVIDENCE",
        timeout=timeout,
    )
    evidence = parse_ota_evidence(raw, captured_at=current)
    if evidence.mode != "probe" or evidence.decision != "admit":
        raise CanaryApprovalError("OTA probe was not admitted in read-only mode")
    if (
        evidence.catalog_name != target
        or evidence.sha256 != part["sha256"]
        or evidence.crc32 != part["crc32"]
        or evidence.size != part["size"]
        or evidence.partition_capacity != APP_CAPACITY
    ):
        raise CanaryReleaseError("OTA probe manifest does not match release index")
    component_board = state.boards[component]
    if component_board.inventory is None or component_board.provisional is None:
        raise CanaryOrderError("OTA target inventory/provisional evidence is missing")
    if evidence.uplink_mac != uplink.mac:
        raise CanaryApprovalError("OTA probe uplink MAC changed")
    if evidence.actual_target_mac != component_board.inventory.mac:
        raise CanaryApprovalError("OTA probe target MAC changed")
    if evidence.actual_target_boot_id != component_board.provisional["boot_id"]:
        raise CanaryApprovalError("OTA probe target boot ID changed")
    if output is not None:
        secure_write_json(output.expanduser().resolve(), asdict(evidence))
    if record:
        state.record_ota_probe(evidence, now=current)
    return evidence


def issue_ota_apply_challenge(
    state: "CanaryState",
    *,
    component: str,
    mode: str,
    probe: OtaEvidence,
    catalog_evidence: Mapping[str, Any],
    catalog_evidence_sha256: str,
    catalog_evidence_path: Path,
    index: Path,
    receipt: ToolchainReceipt,
    output: Path,
    now: int | None = None,
) -> tuple[ApprovalChallenge, str]:
    require_toolchain_binding(state, receipt)
    current = int(time.time()) if now is None else now
    canonical_catalog_sha = validate_catalog_evidence(
        catalog_evidence, index=index, now=current
    )
    # Bind both file bytes and canonical content. A whitespace rewrite or a
    # semantic mutation therefore requires a new preflight/challenge.
    combined_catalog_sha = hashlib.sha256(
        bytes.fromhex(catalog_evidence_sha256)
        + bytes.fromhex(canonical_catalog_sha)
    ).hexdigest()
    state.record_catalog_preflight(
        combined_catalog_sha,
        now=catalog_evidence["captured_at"],
        evidence_path=catalog_evidence_path,
    )
    recorded = state.ota_probes.get(component)
    if recorded is None or asdict(recorded) != asdict(probe):
        raise CanaryApprovalError("OTA challenge probe is not the latest recorded probe")
    document, _raw = _load_release_index(index)
    target = BACKEND_IDENTITIES[_role_kind(component)][0]
    part = _index_firmware(document, target)
    if probe.sha256 != part["sha256"] or probe.crc32 != part["crc32"]:
        raise CanaryReleaseError("OTA challenge artifact differs from release index")
    return state.issue_ota_challenge(
        component=component,
        artifact_sha256=probe.sha256,
        artifact_crc32=probe.crc32,
        mode=mode,
        now=current,
        receipt_path=output,
    )


def _reopen_ota_status(*, port: str, timeout: int) -> dict[str, Any]:
    deadline = time.monotonic() + timeout
    last_error: BaseException | None = None
    while time.monotonic() < deadline:
        remaining = max(1, int(deadline - time.monotonic()))
        try:
            return serial_exchange(
                port=port,
                command="FOF_BACKEND_OTA_STATUS\n",
                expected_prefix="FOF_BACKEND_OTA_EVIDENCE",
                timeout=min(remaining, 5),
            )
        except (OSError, CanaryInventoryError) as exc:
            last_error = exc
            # Poll only this same recorded path. No alternate device and no
            # resend of the mutating apply command is permitted.
            time.sleep(0.25)
    raise CanaryInventoryError(
        f"OTA status did not reappear on the recorded path: {last_error}"
    )


def execute_ota_apply(
    state: "CanaryState",
    *,
    component: str,
    mode: str,
    index: Path,
    port: str,
    challenge_id: str,
    token: str,
    receipt: ToolchainReceipt,
    output: Path,
    timeout: int,
    catalog_evidence: Mapping[str, Any],
    catalog_evidence_sha256: str,
    now: int | None = None,
) -> tuple[dict[str, Any], OtaEvidence]:
    require_toolchain_binding(state, receipt)
    challenge = state.challenges.get(challenge_id)
    if (
        challenge is None
        or challenge.operation != "ota-apply"
        or challenge.component != component
        or challenge.ota_mode != mode
    ):
        raise CanaryApprovalError("challenge is not this exact OTA apply")
    uplink = state.boards["uplink"].inventory
    if uplink is None or port != uplink.port:
        raise CanaryInventoryError("OTA apply port is not the recorded uplink")
    document, _raw = _load_release_index(index)
    target = BACKEND_IDENTITIES[_role_kind(component)][0]
    part = _index_firmware(document, target)
    if challenge.artifact_sha256 != part["sha256"] or challenge.artifact_crc32 != part["crc32"]:
        raise CanaryReleaseError("OTA challenge artifact no longer matches release index")
    current = int(time.time()) if now is None else now
    canonical_catalog_sha = validate_catalog_evidence(
        catalog_evidence, index=index, now=current
    )
    combined_catalog_sha = hashlib.sha256(
        bytes.fromhex(_require_hex64(
            catalog_evidence_sha256, "catalog evidence file SHA"
        ))
        + bytes.fromhex(canonical_catalog_sha)
    ).hexdigest()
    if (
        combined_catalog_sha != challenge.offsets_sha256
        or combined_catalog_sha != state.catalog_evidence_sha256
    ):
        raise CanaryApprovalError("catalog evidence file/content changed after approval")
    latest = run_ota_probe(
        state,
        component=component,
        catalog_name=target,
        expected_sha256=part["sha256"],
        catalog_evidence=catalog_evidence,
        index=index,
        port=port,
        receipt=receipt,
        timeout=min(timeout, PROBE_TTL_SECONDS),
        now=current,
        record=False,
    )
    expected_bindings = (
        (latest.component_slot if component != "uplink" else None, challenge.target_slot),
        (latest.actual_target_mac, challenge.target_mac),
        (latest.actual_target_boot_id, challenge.target_boot_id),
        (latest.actual_topology_generation, challenge.topology_generation),
        (latest.sha256, challenge.artifact_sha256),
        (latest.crc32, challenge.artifact_crc32),
    )
    if any(actual != expected for actual, expected in expected_bindings):
        raise CanaryApprovalError("latest OTA target snapshot changed after approval")
    state.consume_challenge(
        challenge_id,
        token,
        now=current,
        port=port,
        mac=uplink.mac,
        artifact_sha256=part["sha256"],
        artifact_crc32=part["crc32"],
        offsets_sha256=challenge.offsets_sha256,
        toolchain=receipt,
        target_slot=challenge.target_slot,
        target_mac=challenge.target_mac,
        target_boot_id=challenge.target_boot_id,
        topology_generation=challenge.topology_generation,
    )
    apply_sent = False
    try:
        line = build_ota_apply_line(
            component=component,
            sha256=challenge.artifact_sha256,
            mode=mode,
            target_mac=challenge.target_mac or "",
            target_boot_id=challenge.target_boot_id or 0,
            topology_generation=challenge.topology_generation or 0,
        )
        apply_sent = True
        accepted_raw = serial_exchange(
            port=port,
            command=line,
            expected_prefix="FOF_BACKEND_OTA_ACCEPTED",
            timeout=timeout,
        )
        accepted = parse_ota_accepted(accepted_raw)
        for key, expected in (
            ("component", component),
            ("sha256", challenge.artifact_sha256),
            ("crc32", challenge.artifact_crc32),
            ("uplink_mac", uplink.mac),
            ("actual_target_mac", challenge.target_mac),
            ("actual_target_boot_id", challenge.target_boot_id),
            ("actual_topology_generation", challenge.topology_generation),
        ):
            if accepted.get(key) != expected:
                raise CanaryApprovalError(f"OTA accepted {key} binding changed")
        final_raw = _reopen_ota_status(port=port, timeout=timeout)
        final = parse_ota_evidence(final_raw, captured_at=int(time.time()))
        if (
            final.operation_id != accepted["operation_id"]
            or final.mode != mode
            or final.component != component
            or final.decision != "applied"
            or final.sha256 != challenge.artifact_sha256
            or final.crc32 != challenge.artifact_crc32
            or final.actual_target_mac != challenge.target_mac
            or final.expected_target_boot_id != challenge.target_boot_id
            or final.actual_topology_generation != challenge.topology_generation
            or final.boot_id_before != challenge.target_boot_id
            or final.boot_id_after == final.boot_id_before
            or final.image_writes_after <= final.image_writes_before
            or final.rollback_clear is not True
            or final.converged is not True
            or final.complete_image_validated is not True
        ):
            raise CanaryApprovalError("OTA final evidence did not converge exactly")
        secure_write_json(
            output.expanduser().resolve(),
            {"schema": 1, "accepted": accepted, "final": asdict(final)},
        )
        target_board = state.boards[component]
        if target_board.provisional is None:
            raise CanaryOrderError("OTA target provisional record disappeared")
        target_board.provisional["boot_id"] = final.boot_id_after
        for board in state.boards.values():
            board.final_health = None
        state._touch()
        final_timeout = min(timeout, 180)
        for role in BOARD_ROLES:
            role_inventory = state.boards[role].inventory
            if role_inventory is None:
                raise CanaryOrderError(f"{role} inventory disappeared")
            verify_final_serial(
                state,
                role=role,
                port=role_inventory.port,
                timeout=final_timeout,
            )
        return accepted, final
    except BaseException as exc:
        if apply_sent:
            state.record_flash_failure(
                component, phase="ota", reason=redact_text(str(exc))[:1000]
            )
        raise


def _load_json_no_duplicates(text: str) -> Any:
    def pairs(values: list[tuple[str, Any]]) -> dict[str, Any]:
        output: dict[str, Any] = {}
        for key, value in values:
            if key in output:
                raise CanaryInventoryError(f"duplicate JSON key: {key}")
            output[key] = value
        return output

    try:
        return json.loads(text, object_pairs_hook=pairs)
    except CanaryInventoryError:
        raise
    except (json.JSONDecodeError, UnicodeError) as exc:
        raise CanaryInventoryError("evidence is not one strict JSON object") from exc


def parse_evidence_line(text: str, prefix: str) -> dict[str, Any]:
    if not isinstance(text, str) or len(text.encode("utf-8")) > MAX_SERIAL_LINE:
        raise CanaryInventoryError("serial evidence line is missing or oversized")
    matches = [
        line[len(prefix) + 1:]
        for line in text.splitlines()
        if line.startswith(prefix + " ")
    ]
    if len(matches) != 1:
        raise CanaryInventoryError(f"expected exactly one {prefix} evidence line")
    value = _load_json_no_duplicates(matches[0])
    if not isinstance(value, dict):
        raise CanaryInventoryError("serial evidence must be a JSON object")
    return value


class _NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):  # type: ignore[no-untyped-def]
        raise CanarySecurityError("HTTP redirects are forbidden in canary evidence")


def normalize_http_base(value: str) -> str:
    parsed = urllib.parse.urlsplit(value)
    if (
        parsed.scheme not in ("http", "https")
        or not parsed.hostname
        or parsed.username is not None
        or parsed.password is not None
        or parsed.query
        or parsed.fragment
    ):
        raise CanarySecurityError("HTTP base must be a credential-free http(s) origin/path")
    hostname = parsed.hostname.lower()
    default_port = 80 if parsed.scheme == "http" else 443
    port = parsed.port or default_port
    host = hostname if port == default_port else f"{hostname}:{port}"
    path = parsed.path.rstrip("/")
    return urllib.parse.urlunsplit((parsed.scheme.lower(), host, path, "", ""))


def _same_origin(left: str, right: str) -> bool:
    a = urllib.parse.urlsplit(normalize_http_base(left))
    b = urllib.parse.urlsplit(normalize_http_base(right))
    a_port = a.port or (80 if a.scheme == "http" else 443)
    b_port = b.port or (80 if b.scheme == "http" else 443)
    return (a.scheme, a.hostname, a_port) == (b.scheme, b.hostname, b_port)


def http_get_bytes(
    url: str,
    *,
    max_bytes: int,
    timeout: float = 10.0,
    opener: Any | None = None,
) -> tuple[bytes, dict[str, str]]:
    normalized = normalize_http_base(url)
    client = opener or urllib.request.build_opener(_NoRedirect())
    request = urllib.request.Request(
        normalized,
        method="GET",
        headers={"Accept": "application/json, application/octet-stream"},
    )
    try:
        with client.open(request, timeout=timeout) as response:
            status = getattr(response, "status", response.getcode())
            final_url = response.geturl()
            if status != 200:
                raise CanarySecurityError(f"HTTP evidence returned status {status}")
            if final_url != normalized:
                raise CanarySecurityError("HTTP evidence URL changed")
            length = response.headers.get("Content-Length")
            if length is not None:
                try:
                    parsed_length = int(length)
                except ValueError as exc:
                    raise CanarySecurityError("HTTP Content-Length is invalid") from exc
                if parsed_length < 0 or parsed_length > max_bytes:
                    raise CanarySecurityError("HTTP response length exceeds canary bound")
            payload = response.read(max_bytes + 1)
            if len(payload) > max_bytes:
                raise CanarySecurityError("HTTP response exceeds canary bound")
            headers = {key.lower(): value for key, value in response.headers.items()}
            return payload, headers
    except CanaryError:
        raise
    except (urllib.error.URLError, OSError) as exc:
        raise CanarySecurityError(f"HTTP evidence fetch failed: {normalized}") from exc


def http_get_json(
    url: str,
    *,
    timeout: float = 10.0,
    opener: Any | None = None,
) -> tuple[dict[str, Any], bytes, dict[str, str]]:
    payload, headers = http_get_bytes(
        url, max_bytes=MAX_HTTP_JSON, timeout=timeout, opener=opener
    )
    content_type = headers.get("content-type", "").split(";", 1)[0].strip().lower()
    if content_type and content_type != "application/json":
        raise CanarySecurityError("HTTP JSON evidence has wrong Content-Type")
    try:
        parsed = _load_json_no_duplicates(payload.decode("utf-8", "strict"))
    except (UnicodeError, CanaryInventoryError) as exc:
        raise CanarySecurityError("HTTP evidence is not strict JSON") from exc
    if not isinstance(parsed, dict):
        raise CanarySecurityError("HTTP evidence JSON is not an object")
    return parsed, payload, headers


def _load_release_index(index: Path) -> tuple[dict[str, Any], bytes]:
    try:
        raw = index.expanduser().resolve(strict=True).read_bytes()
        document = _load_json_no_duplicates(raw.decode("utf-8", "strict"))
    except (OSError, UnicodeError, CanaryInventoryError) as exc:
        raise CanaryReleaseError("release index is unavailable or invalid") from exc
    targets = document.get("targets") if isinstance(document, dict) else None
    if (
        not isinstance(document, dict)
        or document.get("schema") != 1
        or document.get("version") != BACKEND_VERSION
        or not isinstance(targets, dict)
        or set(targets) != {"scanner-s3-combo-backend", "uplink-s3-backend"}
    ):
        raise CanaryReleaseError("release index must contain exactly both backend targets")
    for kind, identity in BACKEND_IDENTITIES.items():
        target, project, hardware, _version = identity
        entry = targets[target]
        if (
            not isinstance(entry, dict)
            or set(entry) != {
                "kind", "target", "project", "hardware", "identity_crc32",
                "partition_capacity", "parts",
            }
            or entry.get("kind") != kind
            or entry.get("target") != target
            or entry.get("project") != project
            or entry.get("hardware") != hardware
            or entry.get("identity_crc32") != BACKEND_IDENTITY_CRC32[kind]
            or entry.get("partition_capacity") != APP_CAPACITY
        ):
            raise CanaryReleaseError(f"release index {kind} identity is not exact")
        parts = entry.get("parts")
        if not isinstance(parts, list) or len(parts) != 4:
            raise CanaryReleaseError(f"release index {kind} parts are not exact")
        expected_names = _expected_names(target)
        ranges: list[tuple[int, int]] = []
        seen: set[str] = set()
        for part in parts:
            if not isinstance(part, dict) or set(part) != {
                "name", "path", "offset", "size", "sha256", "crc32"
            }:
                raise CanaryReleaseError(f"release index {kind} part fields are not exact")
            name = part.get("name")
            if (
                name not in expected_names
                or name in seen
                or part.get("offset") != expected_names[name]
                or Path(part.get("path", "")) != Path("firmware") / target / name
                or not isinstance(part.get("size"), int)
                or isinstance(part["size"], bool)
                or part["size"] <= 0
            ):
                raise CanaryReleaseError(f"release index {kind} part path/range is unsafe")
            _require_hex64(part.get("sha256"), f"release {kind} part SHA")
            if not isinstance(part.get("crc32"), int) or isinstance(part["crc32"], bool) or not 0 <= part["crc32"] <= 0xFFFFFFFF:
                raise CanaryReleaseError(f"release index {kind} part CRC32 is invalid")
            seen.add(name)
            ranges.append((part["offset"], part["size"]))
        if seen != set(expected_names):
            raise CanaryReleaseError(f"release index {kind} part set is incomplete")
        validate_flash_ranges(sorted(ranges))
    return document, raw


def _index_firmware(index_document: Mapping[str, Any], target: str) -> dict[str, Any]:
    entry = index_document["targets"][target]
    candidates = [
        part for part in entry["parts"]
        if isinstance(part, dict) and part.get("offset") == APP_OFFSET
    ]
    if len(candidates) != 1:
        raise CanaryReleaseError("release index firmware part is not unique")
    part = candidates[0]
    expected_name = f"{target}-firmware.bin"
    if (
        set(part) != {"name", "path", "offset", "size", "sha256", "crc32"}
        or part.get("name") != expected_name
        or Path(part.get("path", "")).name != expected_name
        or not isinstance(part.get("size"), int)
        or part["size"] <= 0
        or part["size"] > APP_CAPACITY
    ):
        raise CanaryReleaseError("release index firmware basename/range is unsafe")
    _require_hex64(part.get("sha256"), "release firmware SHA")
    if not isinstance(part.get("crc32"), int) or isinstance(part["crc32"], bool) or not 0 <= part["crc32"] <= 0xFFFFFFFF:
        raise CanaryReleaseError("release firmware CRC32 is invalid")
    return part


def verify_catalog_preflight(
    *,
    backend_base: str,
    index: Path,
    output: Path,
    opener: Any | None = None,
    now: int | None = None,
) -> dict[str, Any]:
    base = normalize_http_base(backend_base)
    document, raw_index = _load_release_index(index)
    evidence: dict[str, Any] = {
        "schema": 1,
        "backend_base": base,
        "index_path": str(index.expanduser().resolve()),
        "index_sha256": hashlib.sha256(raw_index).hexdigest(),
        "captured_at": int(time.time()) if now is None else now,
        "targets": {},
    }
    for target in ("scanner-s3-combo-backend", "uplink-s3-backend"):
        entry = document["targets"][target]
        part = _index_firmware(document, target)
        metadata_url = base + f"/nodes/firmware/latest/{target}"
        metadata, _raw, _metadata_headers = http_get_json(
            metadata_url, opener=opener
        )
        expected_metadata = {
            "name": target,
            "target": target,
            "project": entry["project"],
            "hardware": entry["hardware"],
            "version": BACKEND_VERSION,
            "size": part["size"],
            "sha256": part["sha256"],
            "crc32": part["crc32"],
            "download_url": f"/nodes/firmware/download/{target}",
        }
        for key, expected in expected_metadata.items():
            if metadata.get(key) != expected:
                raise CanaryReleaseError(
                    f"catalog {target} metadata {key} does not match release index"
                )
        download_url = urllib.parse.urljoin(base + "/", metadata["download_url"])
        if not _same_origin(base, download_url):
            raise CanarySecurityError("catalog download changed backend origin")
        image, headers = http_get_bytes(
            download_url, max_bytes=APP_CAPACITY, opener=opener
        )
        digest = hashlib.sha256(image).hexdigest()
        crc = zlib.crc32(image) & 0xFFFFFFFF
        if len(image) != part["size"] or digest != part["sha256"] or crc != part["crc32"]:
            raise CanaryReleaseError("catalog downloaded bytes do not match release index")
        required_headers = {
            "content-type": "application/octet-stream",
            "content-length": str(part["size"]),
            "etag": f'"{part["sha256"]}"',
            "x-fof-firmware-version": BACKEND_VERSION,
            "x-fof-firmware-sha256": part["sha256"],
            "x-fof-firmware-target": target,
            "x-fof-app-project": entry["project"],
            "x-fof-hardware-type": entry["hardware"],
        }
        for key, expected in required_headers.items():
            actual = headers.get(key, "").split(";", 1)[0] if key == "content-type" else headers.get(key)
            if actual != expected:
                raise CanaryReleaseError(
                    f"catalog {target} response header {key} is not exact"
                )
        evidence["targets"][target] = {
            **expected_metadata,
            "firmware_basename": part["name"],
            "download_url": download_url,
            "headers": required_headers,
        }
    secure_write_json(output.expanduser().resolve(), evidence)
    return evidence


def _required_text(value: Mapping[str, Any], keys: Sequence[str], label: str) -> str:
    found = [value.get(key) for key in keys if isinstance(value.get(key), str) and value.get(key)]
    if not found:
        raise CanaryInventoryError(f"installed {label} is missing")
    if len(set(found)) != 1:
        raise CanaryInventoryError(f"installed {label} is conflicting")
    return found[0]


def _prompt_port(role: str, input_func: Callable[[str], str]) -> str:
    port = input_func(
        f"Exact USB serial port for physical no-screen Lite {role}: "
    ).strip()
    if not port.startswith("/dev/"):
        raise CanaryInventoryError(f"{role} requires one explicit /dev serial port")
    return port


def capture_installed_evidence(
    *,
    state_path: Path,
    uplink_url: str,
    backend_base: str,
    output_dir: Path,
    opener: Any | None = None,
    input_func: Callable[[str], str] = input,
) -> CanaryState:
    """Capture operational identity/registry/continuity before ROM mode."""
    if state_path.expanduser().resolve().exists():
        raise CanarySecurityError("capture-installed refuses an existing state file")
    uplink_base = normalize_http_base(uplink_url)
    backend = normalize_http_base(backend_base)
    raw_evidence: dict[str, Any] = {}
    for name, suffix in (
        ("status", "/api/status"),
        ("ota_info", "/api/ota/info"),
        ("fw_info", "/api/fw/info"),
    ):
        parsed, _raw, _headers = http_get_json(
            uplink_base + suffix, opener=opener
        )
        raw_evidence[name] = parsed
    status = raw_evidence["status"]
    ota_info = raw_evidence["ota_info"]
    fw_info = raw_evidence["fw_info"]
    device_id = _required_text(status, ("device_id",), "uplink device_id")
    uplink_target = _required_text(
        status, ("firmware_name", "target"), "uplink target"
    )
    uplink_project = _required_text(
        status, ("app_project", "project"), "uplink project"
    )
    uplink_hardware = _required_text(
        status, ("hardware_type", "hardware"), "uplink hardware"
    )
    uplink_version = _required_text(
        status, ("version", "app_version"), "uplink version"
    )
    for field, expected in (
        ("firmware_name", uplink_target),
        ("app_project", uplink_project),
        ("hardware_type", uplink_hardware),
        ("version", uplink_version),
    ):
        if ota_info.get(field) != expected:
            raise CanaryInventoryError(
                f"running uplink status and OTA identity disagree on {field}"
            )
    if not isinstance(fw_info.get("stored"), bool) or not isinstance(
        fw_info.get("auto_update_enabled"), bool
    ):
        raise CanaryInventoryError(
            "original uplink updater-admission contract is unavailable"
        )

    nodes, _raw_nodes, _headers = http_get_json(backend + "/nodes", opener=opener)
    raw_evidence["nodes"] = nodes
    rows = nodes.get("nodes")
    if not isinstance(rows, list) or nodes.get("count") != len(rows):
        raise CanaryInventoryError("backend node registry response is malformed")
    matching = [row for row in rows if isinstance(row, dict) and row.get("device_id") == device_id]
    if len(matching) != 1:
        raise CanaryInventoryError(
            "backend registry must contain exactly one matching device row"
        )
    row = matching[0]
    for field in ("name", "lat", "lon", "position_mode"):
        if field not in row:
            raise CanaryInventoryError(f"registered fixed node lacks {field}")
    if row.get("position_mode") not in ("active", "excluded"):
        raise CanaryInventoryError("registered fixed node position_mode is invalid")
    continuity_url = (
        backend
        + "/detections/calibrate/continuity/"
        + urllib.parse.quote(device_id, safe="")
    )
    continuity, _raw_continuity, _headers = http_get_json(
        continuity_url, opener=opener
    )
    raw_evidence["continuity"] = continuity
    continuity_keys = {
        "schema", "device_id", "calibration_status", "session_id",
        "applied_at", "listener_model_present", "listener_model_schema",
        "listener_model_sha256",
    }
    if set(continuity) != continuity_keys or continuity.get("schema") != 1 or continuity.get("device_id") != device_id:
        raise CanaryInventoryError("calibration continuity receipt is not exact")
    if continuity.get("calibration_status") not in ("defaults", "trusted", "untrusted"):
        raise CanaryInventoryError("calibration continuity trust status is invalid")
    if continuity.get("listener_model_schema") != "rssi-ref-path-loss-v1":
        raise CanaryInventoryError("calibration listener model schema changed")
    digest = continuity.get("listener_model_sha256")
    if digest is not None:
        _require_hex64(digest, "calibration listener model SHA")

    scanners = status.get("scanners")
    if not isinstance(scanners, list) or len(scanners) != 2:
        raise CanaryInventoryError("original uplink must report exactly two scanner slots")
    by_slot: dict[int, Mapping[str, Any]] = {}
    for scanner in scanners:
        if not isinstance(scanner, dict) or scanner.get("slot") not in (0, 1):
            raise CanaryInventoryError("scanner slot evidence is invalid")
        slot = scanner["slot"]
        if slot in by_slot:
            raise CanaryInventoryError("scanner slot evidence is duplicated")
        by_slot[slot] = scanner
    ports = {role: _prompt_port(role, input_func) for role in BOARD_ROLES}
    if len(set(ports.values())) != 3:
        raise CanaryInventoryError("three unique physical USB ports are required")

    identities: dict[str, BoardIdentity] = {}
    for role, slot in (("scanner0", 0), ("scanner1", 1)):
        scanner = by_slot[slot]
        mac = normalize_mac(
            _required_text(scanner, ("hardware_id", "hardware_mac", "mac"), f"{role} MAC")
        )
        identity = BoardIdentity(
            role=role,
            port=ports[role],
            chip="ESP32-S3",
            mac=mac,
            flash_size=FLASH_SIZE,
            secure_boot_enabled=False,
            flash_encryption_enabled=False,
            installed_target=_required_text(
                scanner, ("firmware_name", "target"), f"{role} target"
            ),
            installed_project=_required_text(
                scanner, ("app_project", "project"), f"{role} project"
            ),
            installed_hardware=_required_text(
                scanner, ("hardware_type", "hardware"), f"{role} hardware"
            ),
            installed_version=_required_text(
                scanner, ("ver", "version"), f"{role} version"
            ),
            installed_role=role,
            installed_partition_sha256=canonical_partition_sha256(
                LEGACY_PARTITIONS["scanner"]
            ),
            updater_admission_evidence_sha256=PINNED_UPDATER_ADMISSION_SHA256,
        )
        _validate_installed(identity)
        identities[role] = identity
    uplink_mac_text = next(
        (
            status.get(key)
            for key in ("hardware_mac", "mac")
            if isinstance(status.get(key), str) and status.get(key)
        ),
        None,
    )
    if uplink_mac_text is None:
        uplink_mac_text = input_func(
            "Original uplink base MAC (AA:BB:CC:DD:EE:FF): "
        ).strip()
    uplink_identity = BoardIdentity(
        role="uplink",
        port=ports["uplink"],
        chip="ESP32-S3",
        mac=normalize_mac(uplink_mac_text),
        flash_size=FLASH_SIZE,
        secure_boot_enabled=False,
        flash_encryption_enabled=False,
        installed_target=uplink_target,
        installed_project=uplink_project,
        installed_hardware=uplink_hardware,
        installed_version=uplink_version,
        installed_role="uplink",
        installed_partition_sha256=canonical_partition_sha256(
            LEGACY_PARTITIONS["uplink"]
        ),
        updater_admission_evidence_sha256=PINNED_UPDATER_ADMISSION_SHA256,
    )
    _validate_installed(uplink_identity)
    identities["uplink"] = uplink_identity
    if len({identity.mac for identity in identities.values()}) != 3:
        raise CanaryInventoryError("captured chip MACs must be unique")

    evidence_dir = secure_directory(output_dir.expanduser().resolve())
    evidence_hashes: dict[str, str] = {}
    for name, value in raw_evidence.items():
        path = evidence_dir / f"capture-installed-{name}.json"
        secure_write_json(path, value)
        evidence_hashes[name] = _hash_file(path)[1]
    state = CanaryState.create(state_path)
    for role in BOARD_ROLES:
        state.record_installed_evidence(identities[role])
    state.captured_device_id = device_id
    state.installed_capture = {
        "uplink_base": uplink_base,
        "backend_base": backend,
        "evidence_sha256": evidence_hashes,
        "registry": {
            key: row.get(key)
            for key in ("device_id", "name", "lat", "lon", "alt", "position_mode")
        },
        "continuity": continuity,
        "uart_wiring": {
            "scanner0": "D0(GPIO1)=RX,D1(GPIO2)=TX",
            "scanner1": "D2(GPIO3)=TX,D3(GPIO4)=RX",
            "gpio3_sd_conflict": True,
        },
    }
    state._touch()
    return state


def _require_exact_keys(
    value: Mapping[str, Any], expected: set[str], label: str
) -> None:
    actual = set(value)
    if actual != expected:
        missing = sorted(expected - actual)
        extra = sorted(actual - expected)
        raise CanaryApprovalError(
            f"{label} keys are not exact; missing={missing}, extra={extra}"
        )


OTA_EVIDENCE_KEYS = {
    "schema", "operation_id", "mode", "component", "component_slot",
    "uplink_mac", "expected_target_mac", "actual_target_mac",
    "expected_target_boot_id", "actual_target_boot_id",
    "expected_topology_generation", "actual_topology_generation",
    "catalog_name", "target", "project", "hardware", "version", "sha256",
    "crc32", "size", "partition_capacity", "allow_same_version", "decision",
    "complete_image_validated", "image_writes_before", "image_writes_after",
    "boot_id_before", "boot_id_after", "rollback_clear", "converged",
}
OTA_DECISIONS = frozenset(
    {
        "admit", "no_update", "reject_identity", "reject_version",
        "reject_digest", "reject_size", "reject_capacity", "reject_busy",
        "applied", "failed",
    }
)


def parse_ota_evidence(
    value: Mapping[str, Any], *, captured_at: int = 0
) -> OtaEvidence:
    _require_exact_keys(value, OTA_EVIDENCE_KEYS, "OTA evidence")
    if value["schema"] != 1 or not isinstance(value["operation_id"], int) or isinstance(value["operation_id"], bool) or value["operation_id"] <= 0:
        raise CanaryApprovalError("OTA evidence schema/operation ID is invalid")
    component = value["component"]
    if component not in COMPONENTS:
        raise CanaryApprovalError("OTA component is not exact")
    slot = {"uplink": -1, "scanner0": 0, "scanner1": 1}[component]
    if value["component_slot"] != slot:
        raise CanaryApprovalError("OTA physical component slot changed")
    if value["mode"] not in ("probe", "newer-only", "same-version-recovery"):
        raise CanaryApprovalError("OTA mode is not exact")
    if value["decision"] not in OTA_DECISIONS:
        raise CanaryApprovalError("OTA decision spelling is not exact")
    expected_identity = BACKEND_IDENTITIES[_role_kind(component)]
    actual_identity = (
        value["target"], value["project"], value["hardware"], value["version"]
    )
    if actual_identity != expected_identity:
        raise CanaryApprovalError("OTA evidence has a non-backend identity")
    catalog = expected_identity[0]
    if value["catalog_name"] != catalog:
        raise CanaryApprovalError("OTA catalog name does not match component")
    digest = _require_hex64(value["sha256"], "OTA evidence SHA")
    for label in ("crc32", "size", "partition_capacity"):
        if not isinstance(value[label], int) or isinstance(value[label], bool):
            raise CanaryApprovalError(f"OTA {label} is missing or invalid")
    if not 0 <= value["crc32"] <= 0xFFFFFFFF:
        raise CanaryApprovalError("OTA CRC32 is outside uint32")
    for label in (
        "expected_target_boot_id", "actual_target_boot_id",
        "expected_topology_generation", "actual_topology_generation",
        "image_writes_before", "image_writes_after", "boot_id_before",
        "boot_id_after",
    ):
        if not isinstance(value[label], int) or isinstance(value[label], bool) or value[label] < 0:
            raise CanaryApprovalError(f"OTA {label} is invalid")
    for label in (
        "allow_same_version", "complete_image_validated", "rollback_clear",
        "converged",
    ):
        if not isinstance(value[label], bool):
            raise CanaryApprovalError(f"OTA {label} is not boolean")
    expected_mac = normalize_mac(value["expected_target_mac"])
    actual_mac = normalize_mac(value["actual_target_mac"])
    uplink_mac = normalize_mac(value["uplink_mac"])
    if expected_mac != actual_mac:
        raise CanaryApprovalError("OTA target MAC binding changed")
    if value["expected_target_boot_id"] != value["actual_target_boot_id"] or value["expected_target_boot_id"] <= 0:
        raise CanaryApprovalError("OTA target boot ID binding changed")
    if value["expected_topology_generation"] != value["actual_topology_generation"]:
        raise CanaryApprovalError("OTA topology generation binding changed")
    topology = value["expected_topology_generation"]
    if component == "uplink" and topology != 0:
        raise CanaryApprovalError("uplink OTA topology generation must be zero")
    if component != "uplink" and topology <= 0:
        raise CanaryApprovalError("scanner OTA topology generation is missing")
    if value["mode"] == "probe":
        if not value["complete_image_validated"]:
            raise CanaryApprovalError("OTA probe did not validate the complete image")
        if value["image_writes_before"] != value["image_writes_after"]:
            raise CanaryApprovalError("OTA read-only probe changed image-write counters")
        if value["boot_id_before"] != value["boot_id_after"]:
            raise CanaryApprovalError("OTA read-only probe changed boot ID")
    normalized = {key: value[key] for key in OTA_EVIDENCE_KEYS}
    normalized.update(
        uplink_mac=uplink_mac,
        expected_target_mac=expected_mac,
        actual_target_mac=actual_mac,
        sha256=digest,
    )
    return OtaEvidence(**normalized, captured_at=captured_at)


def _missing(value: Mapping[str, Any], fields: Sequence[str]) -> None:
    for name in fields:
        if name not in value:
            raise CanaryInventoryError(f"missing {name}")


def validate_provisional_identity(
    role: str,
    value: Mapping[str, Any],
    expected_mac: str,
) -> dict[str, Any]:
    kind = _role_kind(role)
    common = (
        "target", "project", "hardware", "version", "mac", "boot_id",
        "nvs_erased", "ota_state",
    )
    role_fields = ("uart_ingress",) if kind == "scanner" else (
        "device_id", "config_state", "config_generation",
        "auto_update_enabled", "uart0_started", "uart1_started",
        "network_state",
    )
    _missing(value, common + role_fields)
    identity = (
        value["target"], value["project"], value["hardware"], value["version"]
    )
    if identity != BACKEND_IDENTITIES[kind]:
        expected = ("target", "project", "hardware", "version")
        for index, field_name in enumerate(expected):
            if identity[index] != BACKEND_IDENTITIES[kind][index]:
                raise CanaryInventoryError(f"provisional {field_name} mismatch")
        raise CanaryInventoryError("provisional backend identity mismatch")
    if normalize_mac(value["mac"]) != normalize_mac(expected_mac):
        raise CanaryInventoryError("provisional MAC does not match inventory")
    if not isinstance(value["boot_id"], int) or value["boot_id"] <= 0:
        raise CanaryInventoryError("provisional boot_id must be nonzero")
    if value["nvs_erased"] is not False:
        raise CanaryInventoryError("provisional evidence says NVS was erased")
    if value["ota_state"] != "valid":
        raise CanaryInventoryError("direct USB provisional OTA state must be valid")
    if kind == "scanner" and value["uart_ingress"] is not True:
        raise CanaryInventoryError("scanner uart_ingress is not healthy")
    if kind == "uplink":
        if not value["device_id"] or value["config_state"] not in ("loaded", "migrated") or value["config_generation"] <= 0:
            raise CanaryInventoryError("uplink config identity was not preserved")
        if value["auto_update_enabled"] is not False:
            raise CanaryInventoryError("automatic updates must remain disabled")
        if value["uart0_started"] is not True or value["uart1_started"] is not True:
            raise CanaryInventoryError("uplink UART workers did not start")
        if value["network_state"] not in ("ap", "sta"):
            raise CanaryInventoryError("uplink network state is not AP or STA")
    result = dict(value)
    result["mac"] = normalize_mac(value["mac"])
    return result


def validate_final_health_set(
    health: Mapping[str, Mapping[str, Any]],
    provisional: Mapping[str, Mapping[str, Any]],
    *,
    device_id: str,
) -> None:
    if set(health) != set(BOARD_ROLES) or set(provisional) != set(BOARD_ROLES):
        raise CanaryInventoryError("final health requires all three roles")
    scanner_roles: set[str] = set()
    for role in ("scanner0", "scanner1"):
        value = health[role]
        _missing(value, (
            "target", "mac", "boot_id", "nvs_erased", "role",
            "command_ingress_boot_id", "radio_healthy", "rollback_clear",
        ))
        if value["target"] != BACKEND_IDENTITIES["scanner"][0]:
            raise CanaryInventoryError("scanner final target mismatch")
        if normalize_mac(value["mac"]) != normalize_mac(provisional[role]["mac"]):
            raise CanaryInventoryError("scanner final MAC mismatch")
        if value["boot_id"] != provisional[role]["boot_id"]:
            raise CanaryInventoryError("scanner unexpectedly rebooted before final proof")
        if value["command_ingress_boot_id"] != value["boot_id"]:
            raise CanaryInventoryError("scanner command ingress is stale")
        if value["nvs_erased"] is not False or value["radio_healthy"] is not True or value["rollback_clear"] is not True:
            raise CanaryInventoryError("scanner final health is not rollback-clear")
        if value["role"] not in ("ble_primary", "wifi_primary"):
            raise CanaryInventoryError("scanner final role is invalid")
        scanner_roles.add(value["role"])
    if scanner_roles != {"ble_primary", "wifi_primary"}:
        raise CanaryInventoryError("scanner final roles must be one BLE and one Wi-Fi")
    uplink = health["uplink"]
    _missing(uplink, (
        "target", "mac", "boot_id", "device_id", "config_state",
        "config_generation", "nvs_loaded", "nvs_erased",
        "auto_update_enabled", "uart0_started", "uart1_started",
        "coordinator_started", "network_state", "rollback_clear",
    ))
    if uplink["target"] != BACKEND_IDENTITIES["uplink"][0] or normalize_mac(uplink["mac"]) != normalize_mac(provisional["uplink"]["mac"]) or uplink["boot_id"] != provisional["uplink"]["boot_id"]:
        raise CanaryInventoryError("uplink final identity/boot mismatch")
    if uplink["device_id"] != device_id or uplink["config_state"] not in ("loaded", "migrated") or uplink["config_generation"] <= 0:
        raise CanaryInventoryError("uplink final config identity is blank or changed")
    booleans = (
        "nvs_loaded", "uart0_started", "uart1_started",
        "coordinator_started", "rollback_clear",
    )
    if any(uplink[name] is not True for name in booleans) or uplink["nvs_erased"] is not False or uplink["auto_update_enabled"] is not False or uplink["network_state"] not in ("ap", "sta"):
        raise CanaryInventoryError("uplink final health is incomplete")


PROVISIONAL_SERIAL_KEYS = {
    "scanner": {
        "target", "project", "hardware", "version", "mac", "boot_id",
        "nvs_erased", "uart_ingress", "ota_state",
    },
    "uplink": {
        "target", "project", "hardware", "version", "mac", "boot_id",
        "device_id", "config_state", "config_generation", "nvs_erased",
        "auto_update_enabled", "uart0_started", "uart1_started",
        "network_state", "ota_state",
    },
}
FINAL_SERIAL_KEYS = {
    "scanner": {
        "target", "mac", "boot_id", "nvs_erased", "role",
        "command_ingress_boot_id", "radio_healthy", "rollback_clear",
    },
    "uplink": {
        "target", "mac", "boot_id", "device_id", "config_state",
        "config_generation", "nvs_loaded", "nvs_erased",
        "auto_update_enabled", "uart0_started", "uart1_started",
        "coordinator_started", "network_state", "rollback_clear",
    },
}


def verify_provisional_serial(
    state: "CanaryState", *, role: str, port: str, timeout: int
) -> dict[str, Any]:
    board = state.boards[role]
    if board.inventory is None or port != board.inventory.port:
        raise CanaryInventoryError("provisional port does not match inventory")
    value = serial_exchange(
        port=port,
        command="FOF_BACKEND_STATUS\n",
        expected_prefix="FOF_BACKEND_BOOT",
        timeout=timeout,
    )
    kind = _role_kind(role)
    if set(value) != PROVISIONAL_SERIAL_KEYS[kind]:
        raise CanaryInventoryError(
            "provisional boot record has missing or extra fields"
        )
    state.record_provisional_backend_identity(role, value)
    return value


def verify_final_serial(
    state: "CanaryState", *, role: str, port: str, timeout: int
) -> dict[str, Any]:
    board = state.boards[role]
    if board.inventory is None or port != board.inventory.port:
        raise CanaryInventoryError("final-health port does not match inventory")
    value = serial_exchange(
        port=port,
        command="FOF_BACKEND_STATUS\n",
        expected_prefix="FOF_BACKEND_HEALTH",
        timeout=timeout,
    )
    kind = _role_kind(role)
    if set(value) != FINAL_SERIAL_KEYS[kind]:
        raise CanaryInventoryError(
            "final health record has missing or extra fields"
        )
    state.record_final_backend_health(role, value)
    return value


def _expected_names(target: str) -> dict[str, int]:
    return {
        f"{target}-{logical}.bin": offset
        for logical, offset in EXPECTED_PART_OFFSETS.items()
    }


def verify_release_artifact(
    index: Path,
    artifact_dir: Path,
    *,
    role: str,
) -> VerifiedReleaseArtifact:
    kind = _role_kind(role)
    target = BACKEND_IDENTITIES[kind][0]
    selected_target = _artifact_target(artifact_dir)
    if selected_target != target:
        raise CanaryReleaseError(
            f"{selected_target} artifact cannot be used for {role}; exact {kind} "
            "target required"
        )
    document, raw_index = _load_release_index(index)
    targets = document["targets"]
    if target not in targets:
        raise CanaryReleaseError(f"release index has no exact {kind} target")
    entry = targets[target]
    if not isinstance(entry, dict):
        raise CanaryReleaseError("release target entry is invalid")
    for key, expected in zip(
        ("kind", "target", "project", "hardware"),
        (kind,) + BACKEND_IDENTITIES[kind][:3],
    ):
        if entry.get(key) != expected:
            raise CanaryReleaseError(f"release {key} does not match {kind}")
    if entry.get("partition_capacity") != APP_CAPACITY or not isinstance(entry.get("identity_crc32"), int):
        raise CanaryReleaseError("release capacity/identity CRC is invalid")
    directory = artifact_dir.expanduser().resolve()
    if (
        directory.name != target
        or directory.parent.name != "firmware"
        or not directory.is_dir()
    ):
        raise CanaryReleaseError("artifact directory is not the exact target")
    parts = entry.get("parts")
    if not isinstance(parts, list) or len(parts) != 4:
        raise CanaryReleaseError("release must contain exactly four parts")
    expected_names = _expected_names(target)
    found_names: dict[str, int] = {}
    ranges: list[tuple[int, int]] = []
    firmware: tuple[Path, int, str, int] | None = None
    normalized_parts: list[dict[str, Any]] = []
    for part in parts:
        if not isinstance(part, dict) or set(part) != {"name", "path", "offset", "size", "sha256", "crc32"}:
            raise CanaryReleaseError("release part fields are not exact")
        name = part["name"]
        if name not in expected_names or Path(part["path"]).name != name or Path(part["path"]).parent != Path("firmware") / target:
            raise CanaryReleaseError("release part path/name is unsafe")
        if part["offset"] != expected_names[name] or name in found_names:
            raise CanaryReleaseError("release part offset/name is duplicated or changed")
        resolved = (directory.parent.parent / part["path"]).resolve()
        if resolved.parent != directory or resolved.name != name:
            raise CanaryReleaseError("release part path escapes verified directory")
        if not resolved.is_file():
            raise CanaryReleaseError(f"release artifact is missing: {name}")
        size, digest, crc = _hash_file(resolved)
        if size != part["size"] or digest != part["sha256"] or crc != part["crc32"]:
            raise CanaryReleaseError(f"release artifact size/digest changed: {name}")
        found_names[name] = part["offset"]
        ranges.append((part["offset"], size))
        normalized_parts.append(dict(part))
        if name.endswith("-partition-table.bin"):
            decoded_sha = canonical_partition_sha256(
                decode_partition_table(resolved.read_bytes())
            )
            expected_partition_sha = canonical_partition_sha256(
                BACKEND_PARTITIONS[kind]
            )
            if decoded_sha != expected_partition_sha:
                raise CanaryReleaseError(
                    "release partition table is not the exact backend table"
                )
        if name.endswith("-firmware.bin"):
            firmware = (resolved, size, digest, crc)
    if found_names != expected_names or firmware is None:
        raise CanaryReleaseError("release part set is incomplete")
    ordered = sorted(zip(ranges, normalized_parts), key=lambda item: item[0][0])
    validate_flash_ranges([item[0] for item in ordered])
    offsets_payload = {
        "target": target,
        "kind": kind,
        "parts": [
            {"name": item[1]["name"], "offset": item[1]["offset"], "size": item[1]["size"]}
            for item in ordered
        ],
    }
    return VerifiedReleaseArtifact(
        index_path=str(index.expanduser().resolve()),
        index_sha256=hashlib.sha256(raw_index).hexdigest(),
        artifact_directory=str(directory),
        kind=kind,
        target=target,
        project=entry["project"],
        hardware=entry["hardware"],
        version=document["version"],
        identity_crc32=entry["identity_crc32"],
        firmware_path=str(firmware[0]),
        firmware_size=firmware[1],
        firmware_sha256=firmware[2],
        firmware_crc32=firmware[3],
        offsets_sha256=hashlib.sha256(_canonical_json(offsets_payload)).hexdigest(),
        parts=tuple(part for _range, part in ordered),
    )


def verify_strict_release_artifact(
    index: Path, artifact_dir: Path, *, role: str
) -> VerifiedReleaseArtifact:
    """Run Task 4's full release/flasher verifier before role admission."""
    directory = artifact_dir.expanduser().resolve()
    if directory.parent.name != "firmware":
        raise CanaryReleaseError(
            "artifact directory must be the verified backend web-flasher target"
        )
    flasher = directory.parent.parent
    try:
        from tools.verify_backend_release import (  # type: ignore[import-not-found]
            ReleaseVerificationError,
            verify_release,
        )
    except (ImportError, ModuleNotFoundError) as exc:
        raise CanaryReleaseError("strict backend release verifier is unavailable") from exc
    try:
        verified = verify_release(index=index.expanduser().resolve(), flasher=flasher)
    except ReleaseVerificationError as exc:
        raise CanaryReleaseError(f"strict backend release verification failed: {exc}") from exc
    if set(verified.targets) != {"scanner-s3-combo-backend", "uplink-s3-backend"}:
        raise CanaryReleaseError("strict verifier did not return both backend targets")
    return verify_release_artifact(index, directory, role=role)


def flash_binding_sha256(
    artifact: VerifiedReleaseArtifact,
    *,
    role: str,
    port: str,
    mac: str,
) -> str:
    _role_kind(role)
    payload = {
        "schema": 1,
        "role": role,
        "port": port,
        "mac": normalize_mac(mac),
        "index_sha256": artifact.index_sha256,
        "kind": artifact.kind,
        "target": artifact.target,
        "project": artifact.project,
        "hardware": artifact.hardware,
        "version": artifact.version,
        "identity_crc32": artifact.identity_crc32,
        "parts": [
            {
                "name": part["name"],
                "offset": part["offset"],
                "size": part["size"],
                "sha256": part["sha256"],
                "crc32": part["crc32"],
            }
            for part in artifact.parts
        ],
    }
    return hashlib.sha256(_canonical_json(payload)).hexdigest()


def _identity_fingerprint(identity: BoardIdentity) -> str:
    return hashlib.sha256(_canonical_json(asdict(identity))).hexdigest()


def _live_identity_matches(expected: BoardIdentity, actual: BoardIdentity) -> None:
    if expected != actual:
        raise CanaryInventoryError(
            "live chip/port/MAC/security/flash identity changed"
        )


def verify_original_uplink_quiescence(
    state: "CanaryState",
    *,
    receipt: ToolchainReceipt,
    runner: Callable[..., subprocess.CompletedProcess[str]] = subprocess.run,
    persist: bool = True,
) -> BoardIdentity:
    require_toolchain_binding(state, receipt)
    board = state.boards["uplink"]
    if not state.original_uplink_quiesced or board.inventory is None:
        raise CanaryOrderError("original uplink is not recorded quiescent")
    try:
        live = probe_live_inventory(
            board.installed or board.inventory,
            role="uplink",
            port=board.inventory.port,
            receipt=receipt,
            runner=runner,
        )
        _live_identity_matches(board.inventory, live)
    except BaseException:
        state.original_uplink_quiesced = False
        state.original_uplink_quiescence = {
            "invalidated_at": int(time.time()),
            "reason": "no-write ROM quiescence probe failed",
        }
        state._clear_challenges()
        state._touch()
        raise
    if persist:
        state.original_uplink_quiescence = {
            **(state.original_uplink_quiescence or {}),
            "port": live.port,
            "mac": live.mac,
            "security_flash_identity_sha256": _identity_fingerprint(live),
            "toolchain_sha256": receipt.sha256,
            "verified_at": int(time.time()),
            "after": "no_reset",
            "application_restarted": False,
        }
        state._touch()
    return live


def issue_initial_flash_challenge(
    state: "CanaryState",
    *,
    role: str,
    artifact: VerifiedReleaseArtifact,
    receipt: ToolchainReceipt,
    output: Path,
    runner: Callable[..., subprocess.CompletedProcess[str]] = subprocess.run,
    now: int | None = None,
) -> tuple[ApprovalChallenge, str]:
    require_toolchain_binding(state, receipt)
    board = state.boards[role]
    if board.inventory is None or board.installed is None:
        raise CanaryOrderError(f"{role} installed inventory is missing")
    if role.startswith("scanner"):
        verify_original_uplink_quiescence(
            state, receipt=receipt, runner=runner, persist=False
        )
    live = probe_live_inventory(
        board.installed,
        role=role,
        port=board.inventory.port,
        receipt=receipt,
        runner=runner,
    )
    _live_identity_matches(board.inventory, live)
    if role.startswith("scanner"):
        verify_original_uplink_quiescence(state, receipt=receipt, runner=runner)
    binding = flash_binding_sha256(
        artifact, role=role, port=live.port, mac=live.mac
    )
    challenge, token = state.issue_challenge(
        role=role,
        port=live.port,
        mac=live.mac,
        artifact_sha256=artifact.firmware_sha256,
        offsets_sha256=binding,
        artifact_kind=artifact.kind,
        receipt_path=output,
        now=int(time.time()) if now is None else now,
    )
    return challenge, token


def execute_initial_flash(
    state: "CanaryState",
    *,
    role: str,
    artifact: VerifiedReleaseArtifact,
    challenge_id: str,
    token: str,
    receipt: ToolchainReceipt,
    runner: Callable[..., subprocess.CompletedProcess[str]] = subprocess.run,
    now: int | None = None,
) -> None:
    require_toolchain_binding(state, receipt)
    board = state.boards[role]
    if board.inventory is None or board.installed is None:
        raise CanaryOrderError(f"{role} inventory is missing")
    challenge = state.challenges.get(challenge_id)
    if challenge is None or challenge.operation != "flash-initial" or challenge.role != role:
        raise CanaryApprovalError("challenge is not for this initial Lite write")
    binding = flash_binding_sha256(
        artifact,
        role=role,
        port=board.inventory.port,
        mac=board.inventory.mac,
    )
    live = probe_live_inventory(
        board.installed,
        role=role,
        port=board.inventory.port,
        receipt=receipt,
        runner=runner,
    )
    _live_identity_matches(board.inventory, live)
    if role.startswith("scanner"):
        verify_original_uplink_quiescence(
            state, receipt=receipt, runner=runner, persist=False
        )
    consumed_at = int(time.time()) if now is None else now
    state.consume_challenge(
        challenge_id,
        token,
        now=consumed_at,
        port=live.port,
        mac=live.mac,
        artifact_sha256=artifact.firmware_sha256,
        offsets_sha256=binding,
        toolchain=receipt,
    )
    # Final post-consumption TOCTOU check. A swapped board here is a zero-write
    # admission failure and does not create a false restore requirement.
    final_live = probe_live_inventory(
        board.installed,
        role=role,
        port=board.inventory.port,
        receipt=receipt,
        runner=runner,
    )
    _live_identity_matches(board.inventory, final_live)
    original = state.verify_backup(role, "original")
    expected_table = canonical_partition_sha256(
        BACKEND_PARTITIONS[_role_kind(role)]
    )
    partition_part = next(
        part for part in artifact.parts if part["offset"] == PARTITION_OFFSET
    )
    partition_path = (
        Path(artifact.artifact_directory).parent.parent / partition_part["path"]
    ).resolve()
    decoded_table = canonical_partition_sha256(
        decode_partition_table(partition_path.read_bytes())
    )
    if decoded_table != expected_table:
        raise CanaryReleaseError(
            "verified release partition table is not the exact backend table"
        )
    write_started = False
    try:
        command = build_initial_flash_command(
            Path(receipt.esptool_path),
            board.inventory.port,
            Path(artifact.artifact_directory),
            python_exe=Path(receipt.python_exe),
        )
        write_started = True
        _run_checked(command, runner=runner)
        private_root = secure_directory(
            Path(state.state_path).parent if state.state_path else Path(original.nvs_path).parent
        )
        nvs_after = private_root / (
            f".{role}-{board.inventory.mac.replace(':', '')}-nvs-after-"
            f"{secrets.token_hex(8)}.tmp"
        )
        _precreate_private(nvs_after)
        try:
            _run_checked(
                build_nvs_read_command(
                    receipt, board.inventory.port, nvs_after
                ),
                runner=runner,
            )
            size, digest, _crc = _hash_file(nvs_after)
            if size != NVS_SIZE or digest != original.nvs_sha256:
                raise CanaryBackupError(
                    "protected NVS changed during initial Lite flash"
                )
        finally:
            with contextlib.suppress(FileNotFoundError):
                nvs_after.unlink()
        board.flashed_backend_partition_sha256 = expected_table
        board.status = "initial-flashed-awaiting-provisional"
        state._touch()
        _run_checked(
            build_run_command(receipt, board.inventory.port), runner=runner
        )
    except BaseException as exc:
        if write_started:
            state.record_flash_failure(
                role, phase="initial", reason=redact_text(str(exc))[:1000]
            )
        raise


def issue_full_restore_challenge(
    state: "CanaryState",
    *,
    role: str,
    source: str,
    receipt: ToolchainReceipt,
    output: Path,
    runner: Callable[..., subprocess.CompletedProcess[str]] = subprocess.run,
    now: int | None = None,
) -> tuple[ApprovalChallenge, str]:
    require_toolchain_binding(state, receipt)
    board = state.boards[role]
    if board.inventory is None or board.installed is None:
        raise CanaryOrderError("restore inventory is missing")
    live = probe_live_inventory(
        board.installed,
        role=role,
        port=board.inventory.port,
        receipt=receipt,
        runner=runner,
    )
    _live_identity_matches(board.inventory, live)
    backup = state.verify_backup(role, source)
    return state.issue_restore_challenge(
        role,
        source=source,
        full_backup_sha256=backup.full_sha256,
        now=int(time.time()) if now is None else now,
        receipt_path=output,
    )


def execute_full_restore(
    state: "CanaryState",
    *,
    role: str,
    source: str,
    challenge_id: str,
    token: str,
    receipt: ToolchainReceipt,
    runner: Callable[..., subprocess.CompletedProcess[str]] = subprocess.run,
    now: int | None = None,
) -> None:
    require_toolchain_binding(state, receipt)
    board = state.boards[role]
    if board.inventory is None or board.installed is None:
        raise CanaryOrderError("restore inventory is missing")
    challenge = state.challenges.get(challenge_id)
    if (
        challenge is None
        or challenge.operation != "restore-full"
        or challenge.role != role
        or challenge.restore_source != source
    ):
        raise CanaryApprovalError("challenge is not this exact full restore")
    backup = state.verify_backup(role, source)
    live = probe_live_inventory(
        board.installed,
        role=role,
        port=board.inventory.port,
        receipt=receipt,
        runner=runner,
    )
    _live_identity_matches(board.inventory, live)
    current = int(time.time()) if now is None else now
    state.consume_challenge(
        challenge_id,
        token,
        now=current,
        port=live.port,
        mac=live.mac,
        artifact_sha256=backup.full_sha256,
        offsets_sha256=challenge.offsets_sha256,
        toolchain=receipt,
    )
    final_live = probe_live_inventory(
        board.installed,
        role=role,
        port=board.inventory.port,
        receipt=receipt,
        runner=runner,
    )
    _live_identity_matches(board.inventory, final_live)
    _run_checked(
        build_restore_command(
            Path(receipt.esptool_path),
            board.inventory.port,
            backup,
            python_exe=Path(receipt.python_exe),
        ),
        runner=runner,
    )
    _run_checked(build_run_command(receipt, board.inventory.port), runner=runner)
    board.status = "restore-observation-required"
    state._touch()
    if source == "backend-baseline":
        observed = serial_exchange(
            port=board.inventory.port,
            command="FOF_BACKEND_STATUS\n",
            expected_prefix="FOF_BACKEND_BOOT",
            timeout=30,
        )
        if set(observed) != PROVISIONAL_SERIAL_KEYS[_role_kind(role)]:
            raise CanaryInventoryError("restored backend boot identity is not exact")
        state.confirm_restored_identity(
            role, source=source, observed=observed
        )


def _challenge_payload(challenge: ApprovalChallenge) -> dict[str, Any]:
    payload = asdict(challenge)
    payload.pop("consumed_at", None)
    return payload


def _approval_hash(token: str, challenge: ApprovalChallenge) -> str:
    return hashlib.sha256(
        token.encode("ascii") + b"\0" + _canonical_json(_challenge_payload(challenge))
    ).hexdigest()


@dataclass
class CanaryState:
    schema: int = 1
    created_at: str = field(
        default_factory=lambda: datetime.now(timezone.utc).isoformat()
    )
    generation: int = 0
    boards: dict[str, BoardRecord] = field(
        default_factory=lambda: {role: BoardRecord(role) for role in BOARD_ROLES}
    )
    challenges: dict[str, ApprovalChallenge] = field(default_factory=dict)
    challenge_hashes: dict[str, str] = field(default_factory=dict)
    challenge_receipts: dict[str, str] = field(default_factory=dict)
    toolchain: ToolchainReceipt | None = None
    state_path: str | None = None
    original_uplink_quiesced: bool = False
    original_uplink_quiescence: dict[str, Any] | None = None
    installed_capture: dict[str, Any] | None = None
    captured_device_id: str = "uplink_CB77A4"
    catalog_evidence_sha256: str | None = None
    catalog_captured_at: int | None = None
    catalog_evidence_path: str | None = None
    ota_probes: dict[str, OtaEvidence] = field(default_factory=dict)

    @classmethod
    def create(
        cls, path: Path, *, toolchain: ToolchainReceipt | None = None
    ) -> "CanaryState":
        destination = path.expanduser().resolve()
        if destination.exists():
            raise CanarySecurityError("canary state already exists")
        state = cls(toolchain=toolchain, state_path=str(destination))
        state.save(initial=True)
        return state

    @classmethod
    def load(cls, path: Path) -> "CanaryState":
        destination = path.expanduser().resolve()
        _private_mode(destination.parent, 0o700, "canary state directory")
        _private_mode(destination, 0o600, "canary state")
        try:
            value = _load_json_no_duplicates(destination.read_text("utf-8"))
        except OSError as exc:
            raise CanarySecurityError("unable to read canary state") from exc
        if not isinstance(value, dict) or value.get("schema") != 1:
            raise CanarySecurityError("canary state schema is not exactly 1")
        return cls._from_dict(value, destination)

    @classmethod
    def _from_dict(cls, value: Mapping[str, Any], path: Path) -> "CanaryState":
        tool_value = value.get("toolchain")
        tool = ToolchainReceipt(**tool_value) if isinstance(tool_value, dict) else None
        boards: dict[str, BoardRecord] = {}
        for role in BOARD_ROLES:
            raw = value["boards"][role]
            installed = BoardIdentity(**raw["installed"]) if raw.get("installed") else None
            inventory = BoardIdentity(**raw["inventory"]) if raw.get("inventory") else None
            backups = {
                kind: BackupRecord(**backup)
                for kind, backup in raw.get("backups", {}).items()
            }
            boards[role] = BoardRecord(
                role=role,
                installed=installed,
                inventory=inventory,
                inventory_toolchain_sha256=raw.get("inventory_toolchain_sha256"),
                lite_sensor_confirmed=raw.get("lite_sensor_confirmed", False),
                no_sd_expansion_confirmed=raw.get(
                    "no_sd_expansion_confirmed", False
                ),
                backups=backups,
                provisional=raw.get("provisional"),
                final_health=raw.get("final_health"),
                status=raw.get("status", "pending"),
                failure_phase=raw.get("failure_phase"),
                failure_reason=raw.get("failure_reason"),
                flashed_backend_partition_sha256=raw.get("flashed_backend_partition_sha256"),
            )
        challenges = {
            key: ApprovalChallenge(**item)
            for key, item in value.get("challenges", {}).items()
        }
        probes = {
            key: OtaEvidence(**item)
            for key, item in value.get("ota_probes", {}).items()
        }
        return cls(
            schema=1,
            created_at=value["created_at"],
            generation=value["generation"],
            boards=boards,
            challenges=challenges,
            challenge_hashes=dict(value.get("challenge_hashes", {})),
            challenge_receipts=dict(value.get("challenge_receipts", {})),
            toolchain=tool,
            state_path=str(path),
            original_uplink_quiesced=value.get("original_uplink_quiesced", False),
            original_uplink_quiescence=value.get("original_uplink_quiescence"),
            installed_capture=value.get("installed_capture"),
            captured_device_id=value.get("captured_device_id", ""),
            catalog_evidence_sha256=value.get("catalog_evidence_sha256"),
            catalog_captured_at=value.get("catalog_captured_at"),
            catalog_evidence_path=value.get("catalog_evidence_path"),
            ota_probes=probes,
        )

    def _dict(self) -> dict[str, Any]:
        return {
            "schema": self.schema,
            "created_at": self.created_at,
            "generation": self.generation,
            "boards": {role: asdict(record) for role, record in self.boards.items()},
            "challenges": {key: asdict(item) for key, item in self.challenges.items()},
            "challenge_hashes": dict(self.challenge_hashes),
            "challenge_receipts": dict(self.challenge_receipts),
            "toolchain": asdict(self.toolchain) if self.toolchain else None,
            "original_uplink_quiesced": self.original_uplink_quiesced,
            "original_uplink_quiescence": self.original_uplink_quiescence,
            "installed_capture": self.installed_capture,
            "captured_device_id": self.captured_device_id,
            "catalog_evidence_sha256": self.catalog_evidence_sha256,
            "catalog_captured_at": self.catalog_captured_at,
            "catalog_evidence_path": self.catalog_evidence_path,
            "ota_probes": {key: asdict(item) for key, item in self.ota_probes.items()},
        }

    def save(self, *, initial: bool = False) -> None:
        if self.state_path is None:
            return
        destination = Path(self.state_path)
        parent = secure_directory(destination.parent)
        if not initial and destination.exists():
            _private_mode(destination, 0o600, "canary state")
        temporary = parent / f".{destination.name}.{secrets.token_hex(8)}.tmp"
        descriptor = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        try:
            payload = json.dumps(self._dict(), sort_keys=True, indent=2).encode("utf-8") + b"\n"
            _write_all(descriptor, payload)
            os.fsync(descriptor)
        finally:
            os.close(descriptor)
        os.chmod(temporary, 0o600)
        os.replace(temporary, destination)
        os.chmod(destination, 0o600)
        directory_fd = os.open(parent, os.O_RDONLY)
        try:
            os.fsync(directory_fd)
        finally:
            os.close(directory_fd)

    def _touch(self) -> None:
        self.generation += 1
        self.save()

    def record_installed_evidence(self, identity: BoardIdentity) -> None:
        identity = _normalize_identity(identity)
        board = self.boards[identity.role]
        if board.installed is not None:
            raise CanaryInventoryError(f"installed evidence already exists for {identity.role}")
        _validate_installed(identity)
        board.installed = identity
        self._touch()

    def record_inventory(
        self,
        identity: BoardIdentity,
        *,
        lite_sensor_confirmed: bool = False,
        no_sd_expansion_confirmed: bool = False,
    ) -> None:
        identity = _normalize_identity(identity)
        missing = [role for role in BOARD_ROLES if self.boards[role].installed is None]
        if missing:
            raise CanaryOrderError(f"all installed evidence is required first: {missing}")
        _validate_inventory(identity)
        if lite_sensor_confirmed is not True:
            raise CanaryInventoryError(
                "explicit physical confirmation that this port is a no-screen "
                "Lite sensor, not a native badge, is required"
            )
        if no_sd_expansion_confirmed is not True:
            raise CanaryInventoryError(
                "explicit confirmation that no XIAO Sense SD expansion is "
                "connected is required because GPIO3 is uplink slot1 UART TX"
            )
        board = self.boards[identity.role]
        for role, other in self.boards.items():
            if role == identity.role or other.inventory is None:
                continue
            if other.inventory.port == identity.port:
                raise CanaryInventoryError("the same serial port cannot represent two roles")
            if other.inventory.mac == identity.mac:
                raise CanaryInventoryError("the same chip MAC cannot represent two roles")
        if board.installed != identity:
            raise CanaryInventoryError("live inventory does not match captured installed evidence")
        board.inventory = identity
        board.inventory_toolchain_sha256 = self.toolchain.sha256 if self.toolchain else None
        board.lite_sensor_confirmed = True
        board.no_sd_expansion_confirmed = True
        board.status = "inventoried"
        self._touch()

    def verify_backup(self, role: str, kind: str) -> BackupRecord:
        _role_kind(role)
        if kind not in ("original", "backend-baseline"):
            raise CanaryBackupError("backup kind must be immutable")
        backup = self.boards[role].backups.get(kind)
        if backup is None:
            raise CanaryBackupError(f"missing {kind} backup for {role}")
        _verify_backup_record(
            backup,
            self.boards[role].inventory,
            self.toolchain.sha256 if self.toolchain else None,
        )
        return backup

    def record_backup(self, role: str, kind: str, backup: BackupRecord) -> None:
        _role_kind(role)
        if kind not in ("original", "backend-baseline") or backup.kind != kind or backup.role != role:
            raise CanaryBackupError("backup role/kind binding changed")
        if any(board.inventory is None for board in self.boards.values()):
            raise CanaryOrderError("all three inventories are required before backup")
        if kind in self.boards[role].backups:
            raise CanaryBackupError(f"{kind} backup already exists for {role}")
        if kind == "original":
            expected_order = list(BOARD_ROLES)
            completed = [
                item for item in expected_order
                if "original" in self.boards[item].backups
            ]
            next_role = expected_order[len(completed)] if len(completed) < 3 else None
            if role != next_role:
                raise CanaryOrderError(f"original backup order requires {next_role}")
        else:
            if any(board.final_health is None for board in self.boards.values()):
                raise CanaryOrderError("all final health evidence is required before backend-baseline backups")
            if self.catalog_evidence_sha256 is None:
                raise CanaryOrderError("catalog preflight is required before backend-baseline backups")
        if self.state_path is not None:
            private_root = Path(self.state_path).parent.resolve()
            for candidate in (
                backup.full_path, backup.nvs_path, backup.partition_path
            ):
                resolved = Path(candidate).resolve()
                if resolved != private_root and private_root not in resolved.parents:
                    raise CanaryBackupError(
                        "backup path must remain beneath the private canary state directory"
                    )
        _verify_backup_record(
            backup,
            self.boards[role].inventory,
            self.toolchain.sha256 if self.toolchain else None,
        )
        expected_partition = canonical_partition_sha256(
            (LEGACY_PARTITIONS if kind == "original" else BACKEND_PARTITIONS)[_role_kind(role)]
        )
        if backup.decoded_partition_sha256 != expected_partition:
            raise CanaryBackupError("backup decoded partition table is not recognized")
        self.boards[role].backups[kind] = backup
        self.boards[role].status = f"{kind}-backed-up"
        self._touch()

    def record_original_uplink_quiesced(
        self,
        *,
        port: str,
        mac: str,
        toolchain_sha256: str,
        now: int,
    ) -> None:
        backup = self.verify_backup("uplink", "original")
        inventory = self.boards["uplink"].inventory
        if inventory is None or port != inventory.port or normalize_mac(mac) != inventory.mac or backup.mac != inventory.mac:
            raise CanaryInventoryError("quiescent uplink does not match inventory/backup")
        if self.toolchain is None or toolchain_sha256 != self.toolchain.sha256:
            raise CanarySecurityError("quiescence toolchain receipt changed")
        self.original_uplink_quiesced = True
        self.original_uplink_quiescence = {
            "port": port,
            "mac": inventory.mac,
            "toolchain_sha256": toolchain_sha256,
            "recorded_at": now,
            "state_generation": self.generation + 1,
            "after": "no_reset",
            "application_restarted": False,
        }
        self._touch()

    def note_original_uplink_application_reboot(self, *, boot_id: int, now: int) -> None:
        self.original_uplink_quiesced = False
        self.original_uplink_quiescence = {
            "invalidated_at": now,
            "observed_application_boot_id": boot_id,
        }
        self._clear_challenges()
        self._touch()

    def _assert_no_failure(self) -> None:
        failed = [role for role, board in self.boards.items() if board.status in ("flash_failed", "restore_required", "ota_recovery_required")]
        if failed:
            raise CanaryOrderError(f"{failed[0]} requires restore before any other write")

    def _assert_original_ready(self, role: str) -> None:
        self._assert_no_failure()
        for item in BOARD_ROLES:
            if "original" not in self.boards[item].backups:
                raise CanaryOrderError(
                    f"{item} original backup is required before any write"
                )
            self.verify_backup(item, "original")
            board = self.boards[item]
            if not board.lite_sensor_confirmed:
                raise CanaryOrderError(
                    f"{item} lacks physical no-screen Lite sensor confirmation"
                )
            if not board.no_sd_expansion_confirmed:
                raise CanaryOrderError(
                    f"{item} lacks physical no-SD-expansion confirmation"
                )
        if not self.original_uplink_quiesced:
            raise CanaryOrderError("original uplink is not quiescent")
        if role == "scanner1" and self.boards["scanner0"].provisional is None:
            raise CanaryOrderError("scanner0 must be provisionally verified first")
        if role == "uplink":
            for scanner in ("scanner0", "scanner1"):
                if self.boards[scanner].provisional is None:
                    raise CanaryOrderError(f"{scanner} must be provisionally verified before uplink")

    def _issue(
        self,
        challenge: ApprovalChallenge,
        *,
        receipt_path: Path | None = None,
    ) -> tuple[ApprovalChallenge, str]:
        token = secrets.token_urlsafe(32)
        self.challenges[challenge.challenge_id] = challenge
        self.challenge_hashes[challenge.challenge_id] = _approval_hash(token, challenge)
        if receipt_path is not None:
            receipt = receipt_path.expanduser().resolve()
            secure_write_bytes(
                receipt,
                json.dumps(
                    {"challenge": asdict(challenge), "approval_token": token},
                    sort_keys=True,
                    indent=2,
                ).encode("utf-8") + b"\n",
            )
            self.challenge_receipts[challenge.challenge_id] = str(receipt)
        self.generation = challenge.state_generation
        self.save()
        return challenge, token

    def issue_challenge(
        self,
        *,
        role: str,
        port: str,
        mac: str,
        artifact_sha256: str,
        offsets_sha256: str,
        now: int,
        artifact_kind: str | None = None,
        receipt_path: Path | None = None,
    ) -> tuple[ApprovalChallenge, str]:
        kind = _role_kind(role)
        self._assert_original_ready(role)
        board = self.boards[role]
        if board.inventory is None or port != board.inventory.port or normalize_mac(mac) != board.inventory.mac:
            raise CanaryApprovalError("challenge port/MAC does not match live inventory")
        if artifact_kind is not None and artifact_kind != kind:
            raise CanaryApprovalError(f"a {kind} role cannot select a {artifact_kind} artifact")
        artifact = _require_hex64(artifact_sha256, "artifact SHA")
        offsets = _require_hex64(offsets_sha256, "offsets SHA")
        if self.toolchain is None:
            raise CanarySecurityError("PlatformIO toolchain receipt is missing")
        generation = self.generation + 1
        challenge = ApprovalChallenge(
            challenge_id=secrets.token_hex(16),
            role=role,
            port=port,
            mac=board.inventory.mac,
            operation="flash-initial",
            component=None,
            ota_mode=None,
            artifact_sha256=artifact,
            artifact_crc32=None,
            offsets_sha256=offsets,
            state_generation=generation,
            target_slot=None,
            target_mac=None,
            target_boot_id=None,
            topology_generation=None,
            pio_path=self.toolchain.pio_path,
            toolchain_sha256=self.toolchain.sha256,
            lite_sensor_confirmed=board.lite_sensor_confirmed,
            no_sd_expansion_confirmed=board.no_sd_expansion_confirmed,
            restore_source=None,
            expires_at=now + CHALLENGE_TTL_SECONDS,
            consumed_at=None,
        )
        return self._issue(challenge, receipt_path=receipt_path)

    def consume_challenge(
        self,
        challenge_id: str,
        token: str,
        *,
        now: int,
        port: str | None = None,
        mac: str | None = None,
        artifact_sha256: str | None = None,
        artifact_crc32: int | None = None,
        offsets_sha256: str | None = None,
        toolchain: ToolchainReceipt | None = None,
        target_slot: int | None = None,
        target_mac: str | None = None,
        target_boot_id: int | None = None,
        topology_generation: int | None = None,
    ) -> bool:
        challenge = self.challenges.get(challenge_id)
        if challenge is None:
            raise CanaryApprovalError("unknown or invalidated approval challenge")
        if challenge.consumed_at is not None:
            raise CanaryApprovalError("approval challenge was already consumed")
        if now > challenge.expires_at:
            self.challenges[challenge_id] = replace(challenge, consumed_at=now)
            self.challenge_hashes.pop(challenge_id, None)
            self._remove_receipt(challenge_id)
            self._touch()
            raise CanaryApprovalError("approval challenge expired")
        if challenge.state_generation != self.generation:
            raise CanaryApprovalError("approval state generation changed")
        stored_hash = self.challenge_hashes.get(challenge_id)
        if not token or stored_hash != _approval_hash(token, challenge):
            raise CanaryApprovalError("approval token is wrong or challenge fields changed")
        active_toolchain = toolchain or self.toolchain
        if active_toolchain is None or active_toolchain.pio_path != challenge.pio_path or active_toolchain.sha256 != challenge.toolchain_sha256:
            raise CanaryApprovalError("PlatformIO/toolchain binding changed")
        comparisons = (
            (port, challenge.port, "port"),
            (normalize_mac(mac) if mac is not None else None, challenge.mac, "MAC"),
            (artifact_sha256.lower() if artifact_sha256 else None, challenge.artifact_sha256, "artifact"),
            (artifact_crc32, challenge.artifact_crc32, "CRC32"),
            (offsets_sha256.lower() if offsets_sha256 else None, challenge.offsets_sha256, "offsets"),
            (target_slot, challenge.target_slot, "slot"),
            (normalize_mac(target_mac) if target_mac else None, challenge.target_mac, "target MAC"),
            (target_boot_id, challenge.target_boot_id, "target boot ID"),
            (topology_generation, challenge.topology_generation, "topology"),
        )
        for live, bound, label in comparisons:
            if live is not None and live != bound:
                raise CanaryApprovalError(f"approval {label} binding changed")
        if challenge.operation == "flash-initial" and not self.original_uplink_quiesced:
            raise CanaryApprovalError("original uplink is no longer quiescent")
        challenge_board = self.boards[challenge.role]
        if (
            challenge.lite_sensor_confirmed is not True
            or challenge.no_sd_expansion_confirmed is not True
            or challenge_board.lite_sensor_confirmed is not True
            or challenge_board.no_sd_expansion_confirmed is not True
        ):
            raise CanaryApprovalError(
                "physical Lite/no-SD confirmation binding changed"
            )
        self.challenges[challenge_id] = replace(challenge, consumed_at=now)
        self.challenge_hashes.pop(challenge_id, None)
        self._remove_receipt(challenge_id)
        self._touch()
        return True

    def _remove_receipt(self, challenge_id: str) -> None:
        raw = self.challenge_receipts.pop(challenge_id, None)
        if raw is not None:
            path = Path(raw)
            if path.exists():
                path.unlink()
                directory_fd = os.open(path.parent, os.O_RDONLY)
                try:
                    os.fsync(directory_fd)
                finally:
                    os.close(directory_fd)

    def _clear_challenges(self) -> None:
        for challenge_id in list(self.challenge_receipts):
            self._remove_receipt(challenge_id)
        self.challenges.clear()
        self.challenge_hashes.clear()

    def record_provisional_backend_identity(
        self, role: str, value: Mapping[str, Any]
    ) -> None:
        board = self.boards[role]
        if board.inventory is None:
            raise CanaryOrderError("inventory is required before provisional verification")
        if role == "scanner1" and self.boards["scanner0"].provisional is None:
            raise CanaryOrderError("scanner0 provisional verification is required first")
        if role == "uplink" and any(self.boards[item].provisional is None for item in ("scanner0", "scanner1")):
            raise CanaryOrderError("both scanner provisional records are required before uplink")
        validated = validate_provisional_identity(role, value, board.inventory.mac)
        if role == "uplink" and validated.get("device_id") != self.captured_device_id:
            raise CanaryInventoryError(
                "provisional uplink device_id changed from installed capture"
            )
        expected_table = canonical_partition_sha256(BACKEND_PARTITIONS[_role_kind(role)])
        observed = value.get("partition_sha256", board.flashed_backend_partition_sha256)
        if observed != expected_table:
            raise CanaryInventoryError("provisional backend partition table is missing or changed")
        board.provisional = validated
        board.status = "provisional-verified"
        self._touch()

    def record_final_backend_health_set(
        self,
        health: Mapping[str, Mapping[str, Any]],
        *,
        device_id: str,
    ) -> None:
        provisional = {
            role: board.provisional
            for role, board in self.boards.items()
            if board.provisional is not None
        }
        validate_final_health_set(health, provisional, device_id=device_id)
        for role in BOARD_ROLES:
            self.boards[role].final_health = dict(health[role])
            self.boards[role].status = "final-verified"
        self.captured_device_id = device_id
        self._touch()

    def record_final_backend_health(
        self, role: str, value: Mapping[str, Any]
    ) -> None:
        if role not in BOARD_ROLES:
            raise CanaryInventoryError("unknown final-health role")
        if self.boards[role].provisional is None:
            raise CanaryOrderError(
                f"{role} provisional verification is required before final health"
            )
        self.boards[role].final_health = dict(value)
        if all(board.final_health is not None for board in self.boards.values()):
            health = {
                item: self.boards[item].final_health
                for item in BOARD_ROLES
            }
            provisional = {
                item: self.boards[item].provisional
                for item in BOARD_ROLES
            }
            validate_final_health_set(
                health, provisional, device_id=self.captured_device_id
            )
            for item in BOARD_ROLES:
                self.boards[item].status = "final-verified"
        else:
            self.boards[role].status = "final-evidence-captured"
        self._touch()

    def record_catalog_preflight(
        self,
        evidence_sha256: str,
        *,
        now: int,
        evidence_path: Path | None = None,
    ) -> None:
        self.catalog_evidence_sha256 = _require_hex64(
            evidence_sha256, "catalog evidence SHA"
        )
        self.catalog_captured_at = now
        if evidence_path is not None:
            self.catalog_evidence_path = str(evidence_path.expanduser().resolve())
        self._touch()

    def record_ota_probe(
        self, evidence: OtaEvidence, *, now: int | None = None
    ) -> OtaEvidence:
        self._assert_no_failure()
        if evidence.mode != "probe" or evidence.decision != "admit":
            raise CanaryApprovalError("only an admitted read-only OTA probe is recordable")
        if not evidence.complete_image_validated or evidence.image_writes_before != evidence.image_writes_after or evidence.boot_id_before != evidence.boot_id_after:
            raise CanaryApprovalError("OTA probe was not read-only and complete")
        captured = replace(evidence, captured_at=evidence.captured_at if now is None else now)
        self.ota_probes[evidence.component] = captured
        self._touch()
        return captured

    def issue_ota_challenge(
        self,
        *,
        component: str,
        artifact_sha256: str,
        artifact_crc32: int,
        mode: str,
        now: int,
        receipt_path: Path | None = None,
    ) -> tuple[ApprovalChallenge, str]:
        self._assert_no_failure()
        if component not in COMPONENTS or mode not in ("newer-only", "same-version-recovery"):
            raise CanaryApprovalError("OTA component/mode is not exact")
        if any(board.final_health is None for board in self.boards.values()):
            raise CanaryOrderError("all final health is required before OTA")
        for role in BOARD_ROLES:
            self.verify_backup(role, "backend-baseline")
        if self.catalog_evidence_sha256 is None or self.catalog_captured_at is None or now < self.catalog_captured_at or now - self.catalog_captured_at > CATALOG_TTL_SECONDS:
            raise CanaryApprovalError("catalog preflight is missing or stale")
        probe = self.ota_probes.get(component)
        if probe is None or now < probe.captured_at or now - probe.captured_at > PROBE_TTL_SECONDS:
            raise CanaryApprovalError("OTA probe is missing or stale")
        digest = _require_hex64(artifact_sha256, "OTA challenge SHA")
        if digest != probe.sha256 or artifact_crc32 != probe.crc32:
            raise CanaryApprovalError("OTA challenge digest/whole-image CRC32 mismatch")
        if self.toolchain is None:
            raise CanarySecurityError("PlatformIO toolchain receipt is missing")
        uplink = self.boards["uplink"].inventory
        if uplink is None:
            raise CanaryOrderError("uplink inventory is missing")
        generation = self.generation + 1
        challenge = ApprovalChallenge(
            challenge_id=secrets.token_hex(16),
            role="uplink",
            port=uplink.port,
            mac=uplink.mac,
            operation="ota-apply",
            component=component,
            ota_mode=mode,
            artifact_sha256=digest,
            artifact_crc32=artifact_crc32,
            offsets_sha256=self.catalog_evidence_sha256,
            state_generation=generation,
            target_slot=None if component == "uplink" else probe.component_slot,
            target_mac=probe.actual_target_mac,
            target_boot_id=probe.actual_target_boot_id,
            topology_generation=probe.actual_topology_generation,
            pio_path=self.toolchain.pio_path,
            toolchain_sha256=self.toolchain.sha256,
            lite_sensor_confirmed=self.boards["uplink"].lite_sensor_confirmed,
            no_sd_expansion_confirmed=(
                self.boards["uplink"].no_sd_expansion_confirmed
            ),
            restore_source=None,
            expires_at=now + CHALLENGE_TTL_SECONDS,
            consumed_at=None,
        )
        return self._issue(challenge, receipt_path=receipt_path)

    def record_flash_failure(self, role: str, *, phase: str, reason: str) -> None:
        _role_kind(role)
        if phase not in ("initial", "ota"):
            raise CanaryOrderError("failure phase must be initial or ota")
        board = self.boards[role]
        board.status = "restore_required" if phase == "initial" else "ota_recovery_required"
        board.failure_phase = phase
        board.failure_reason = reason
        self._touch()

    def issue_restore_challenge(
        self,
        role: str,
        *,
        source: str,
        full_backup_sha256: str,
        now: int,
        receipt_path: Path | None = None,
    ) -> tuple[ApprovalChallenge, str]:
        kind = _role_kind(role)
        board = self.boards[role]
        if board.failure_phase is None:
            raise CanaryOrderError("restore is allowed only after a recorded partial failure")
        required = "original" if board.failure_phase == "initial" else "backend-baseline"
        if source != required:
            raise CanaryOrderError(f"{board.failure_phase} failure requires {required} restore source")
        backup = self.verify_backup(role, source)
        if _require_hex64(full_backup_sha256, "restore backup SHA") != backup.full_sha256:
            raise CanaryBackupError("restore backup SHA changed")
        if self.toolchain is None or board.inventory is None:
            raise CanarySecurityError("restore toolchain/inventory is missing")
        generation = self.generation + 1
        challenge = ApprovalChallenge(
            challenge_id=secrets.token_hex(16),
            role=role,
            port=board.inventory.port,
            mac=board.inventory.mac,
            operation="restore-full",
            component=None,
            ota_mode=None,
            artifact_sha256=backup.full_sha256,
            artifact_crc32=None,
            offsets_sha256=hashlib.sha256(
                _canonical_json({"offset": 0, "size": FLASH_SIZE, "source": source})
            ).hexdigest(),
            state_generation=generation,
            target_slot=None,
            target_mac=None,
            target_boot_id=None,
            topology_generation=None,
            pio_path=self.toolchain.pio_path,
            toolchain_sha256=self.toolchain.sha256,
            lite_sensor_confirmed=board.lite_sensor_confirmed,
            no_sd_expansion_confirmed=board.no_sd_expansion_confirmed,
            restore_source=source,
            expires_at=now + CHALLENGE_TTL_SECONDS,
            consumed_at=None,
        )
        return self._issue(challenge, receipt_path=receipt_path)

    def authorize_restore(
        self, challenge_id: str, token: str, *, now: int
    ) -> list[str]:
        challenge = self.challenges.get(challenge_id)
        if challenge is None or challenge.operation != "restore-full" or challenge.restore_source is None:
            raise CanaryApprovalError("challenge is not an exact restore approval")
        board = self.boards[challenge.role]
        backup = self.verify_backup(challenge.role, challenge.restore_source)
        self.consume_challenge(challenge_id, token, now=now)
        if self.toolchain is None:
            raise CanarySecurityError("restore toolchain is missing")
        return build_restore_command(
            Path(self.toolchain.esptool_path),
            challenge.port,
            backup,
            python_exe=Path(self.toolchain.python_exe),
        )

    def confirm_restored_identity(
        self,
        role: str,
        *,
        source: str,
        observed: BoardIdentity | Mapping[str, Any],
    ) -> None:
        board = self.boards[role]
        if board.failure_phase is None:
            raise CanaryOrderError("no restore observation is pending")
        required = "original" if board.failure_phase == "initial" else "backend-baseline"
        if source != required:
            raise CanaryOrderError(f"restore observation requires {required}")
        if board.inventory is None:
            raise CanaryOrderError("restore inventory is missing")
        if source == "original":
            if not isinstance(observed, BoardIdentity):
                raise CanaryInventoryError("original restore requires installed identity evidence")
            actual = _normalize_identity(observed)
            if actual != board.installed or actual.mac != board.inventory.mac:
                raise CanaryInventoryError("restored original identity/MAC does not match capture")
            board.status = "original-restored"
        else:
            if not isinstance(observed, Mapping):
                raise CanaryInventoryError("backend restore requires boot identity evidence")
            validated = validate_provisional_identity(role, observed, board.inventory.mac)
            board.provisional = validated
            board.status = "backend-baseline-restored"
        board.failure_phase = None
        board.failure_reason = None
        self._touch()


def _normalize_identity(identity: BoardIdentity) -> BoardIdentity:
    return replace(identity, mac=normalize_mac(identity.mac))


def _validate_installed(identity: BoardIdentity) -> None:
    kind = _role_kind(identity.role)
    for name in (
        "port", "chip", "installed_target", "installed_project",
        "installed_hardware", "installed_version", "installed_role",
        "installed_partition_sha256", "updater_admission_evidence_sha256",
    ):
        if not getattr(identity, name):
            raise CanaryInventoryError(f"missing installed {name}")
    if identity.installed_role != identity.role:
        raise CanaryInventoryError("installed scanner role/MAC mapping is missing")
    if identity.flash_size != FLASH_SIZE:
        raise CanaryInventoryError("installed board is not exactly 8 MB")
    expected_partition = canonical_partition_sha256(LEGACY_PARTITIONS[kind])
    if identity.installed_partition_sha256 != expected_partition:
        raise CanaryInventoryError("installed legacy partition table is not allowlisted")
    updater_evidence = _require_hex64(
        identity.updater_admission_evidence_sha256,
        "updater admission evidence",
    )
    if updater_evidence != PINNED_UPDATER_ADMISSION_SHA256:
        raise CanaryInventoryError(
            "updater admission evidence is not the pinned source-audited contract"
        )
    key = (
        identity.installed_target,
        identity.installed_project,
        identity.installed_hardware,
        identity.installed_version,
        kind,
    )
    if key not in LEGACY_IDENTITIES:
        raise CanaryInventoryError("installed target/project/hardware/version is not source-audited")


def _validate_inventory(identity: BoardIdentity) -> None:
    _validate_installed(identity)
    if identity.chip.strip().upper().replace("_", "-") not in ("ESP32-S3", "ESP32S3"):
        raise CanaryInventoryError("inventory chip is not ESP32-S3")
    if identity.secure_boot_enabled:
        raise CanaryInventoryError("secure boot is enabled; hardware write refused")
    if identity.flash_encryption_enabled:
        raise CanaryInventoryError("flash encryption is enabled; hardware write refused")
    if identity.xiao_sense_sd_attached:
        raise CanaryInventoryError(
            "XIAO Sense SD expansion is attached: GPIO3 SD CS conflicts with uplink slot1 UART TX"
        )


def _verify_backup_record(
    backup: BackupRecord,
    inventory: BoardIdentity | None,
    expected_toolchain_sha256: str | None = None,
) -> None:
    if inventory is None or backup.mac != inventory.mac:
        raise CanaryBackupError("backup MAC does not match inventory")
    if backup.full_size != FLASH_SIZE or backup.nvs_size != NVS_SIZE or backup.partition_size != PARTITION_SECTOR_SIZE:
        raise CanaryBackupError("backup has wrong length; full image must be exact 8 MB")
    if (
        expected_toolchain_sha256 is not None
        and backup.toolchain_sha256 != expected_toolchain_sha256
    ):
        raise CanaryBackupError("backup PlatformIO/toolchain receipt changed")
    paths = (
        (Path(backup.full_path), backup.full_size, backup.full_sha256, "full backup"),
        (Path(backup.nvs_path), backup.nvs_size, backup.nvs_sha256, "NVS backup"),
        (Path(backup.partition_path), backup.partition_size, backup.partition_sha256, "partition backup"),
    )
    for path, expected_size, expected_hash, label in paths:
        if not path.is_absolute() or path != path.resolve():
            raise CanaryBackupError(f"{label} path must be absolute and resolved")
        _private_mode(path.parent, 0o700, f"{label} directory")
        _private_mode(path, 0o600, label)
        size, digest, _crc = _hash_file(path)
        if size != expected_size or digest != expected_hash:
            raise CanaryBackupError(f"{label} size/SHA changed")
    with Path(backup.full_path).open("rb") as full:
        full.seek(NVS_OFFSET)
        full_nvs = full.read(NVS_SIZE)
        full.seek(PARTITION_OFFSET)
        full_table = full.read(PARTITION_SECTOR_SIZE)
    if full_nvs != Path(backup.nvs_path).read_bytes():
        raise CanaryBackupError("focused NVS read differs from full-flash NVS slice")
    if full_table != Path(backup.partition_path).read_bytes():
        raise CanaryBackupError("focused partition read differs from full-flash table slice")
    if not HEX64.fullmatch(backup.decoded_partition_sha256):
        raise CanaryBackupError("decoded partition SHA is invalid")


CANARY_ROOT = Path(__file__).resolve().parents[1] / ".canary"


def _require_canary_path(path: Path, *, directory: bool = False) -> Path:
    root = CANARY_ROOT.resolve()
    resolved = path.expanduser().resolve()
    if resolved != root and root not in resolved.parents:
        raise CanarySecurityError(
            f"sensitive canary path must remain beneath ignored {root}"
        )
    if resolved == root and not directory:
        raise CanarySecurityError("a concrete file beneath .canary is required")
    return resolved


def _load_state(path: str | Path) -> CanaryState:
    return CanaryState.load(_require_canary_path(Path(path)))


def _resolve_for_state(
    state: CanaryState, pio: str | Path, *, initialize: bool = False
) -> ToolchainReceipt:
    receipt = resolve_toolchain(Path(pio))
    require_toolchain_binding(state, receipt, initialize=initialize)
    return receipt


def _confirmation(prompt: str, exact: str) -> bool:
    return input(f"{prompt}\nType {exact} to confirm: ").strip() == exact


def _inventory_transcript(state: CanaryState, role: str) -> Path:
    root = Path(state.state_path).parent if state.state_path else CANARY_ROOT
    return root / "transcripts" / f"inventory-{role}-{time.time_ns()}.json"


def _handle_capture_installed(args: argparse.Namespace) -> None:
    capture_installed_evidence(
        state_path=_require_canary_path(Path(args.state)),
        uplink_url=args.uplink_url,
        backend_base=args.backend_base,
        output_dir=_require_canary_path(Path(args.output_dir), directory=True),
    )
    print("Installed three-board evidence captured; no firmware was written.")


def _handle_inventory(args: argparse.Namespace) -> None:
    state = _load_state(args.state)
    receipt = _resolve_for_state(state, args.pio, initialize=True)
    role = args.role
    board = state.boards[role]
    if board.installed is None:
        raise CanaryOrderError(f"{role} installed evidence is missing")
    print(
        "XIAO ESP32-S3 fixed wiring: D0=GPIO1, D1=GPIO2, D2=GPIO3, "
        "D3=GPIO4; USER_LED=GPIO21 active-low. GPIO3 is uplink slot1 "
        "UART TX and conflicts with XIAO Sense SD CS."
    )
    lite = _confirmation(
        f"Physically inspect {args.port}: it must be a no-screen Lite sensor, not the native badge.",
        f"LITE-{role}",
    )
    no_sd = _confirmation(
        f"Physically remove/verify absent the XIAO Sense SD expansion on {args.port}.",
        f"NO-SD-{role}",
    )
    if not lite:
        raise CanaryInventoryError(
            "explicit physical confirmation that this is a no-screen Lite sensor is absent"
        )
    if not no_sd:
        raise CanaryInventoryError(
            "XIAO Sense SD expansion must be absent because GPIO3 is slot1 UART TX"
        )
    live = probe_live_inventory(
        board.installed,
        role=role,
        port=args.port,
        receipt=receipt,
        transcript_path=_inventory_transcript(state, role),
    )
    state.record_inventory(
        live,
        lite_sensor_confirmed=lite,
        no_sd_expansion_confirmed=no_sd,
    )
    print(
        f"Inventoried physical Lite {role}: port={live.port} MAC={live.mac} "
        f"toolchain_sha256={receipt.sha256}"
    )


def _record_catalog_for_baseline(state: CanaryState) -> None:
    if state.catalog_evidence_sha256 is not None:
        return
    selected = input(
        "Path to fresh verify-catalog evidence inside .canary: "
    ).strip()
    path = _require_canary_path(Path(selected))
    evidence, file_sha = load_private_json(path, label="catalog evidence")
    index_path = Path(str(evidence.get("index_path", "")))
    canonical_sha = validate_catalog_evidence(
        evidence, index=index_path, now=int(time.time())
    )
    combined = hashlib.sha256(
        bytes.fromhex(file_sha) + bytes.fromhex(canonical_sha)
    ).hexdigest()
    state.record_catalog_preflight(
        combined,
        now=evidence["captured_at"],
        evidence_path=path,
    )


def _handle_backup(args: argparse.Namespace) -> None:
    state = _load_state(args.state)
    receipt = _resolve_for_state(state, args.pio)
    if args.kind == "backend-baseline":
        _record_catalog_for_baseline(state)
    board = state.boards[args.role]
    if board.installed is None or board.inventory is None:
        raise CanaryOrderError(f"{args.role} inventory is missing")
    live = probe_live_inventory(
        board.installed,
        role=args.role,
        port=board.inventory.port,
        receipt=receipt,
    )
    _live_identity_matches(board.inventory, live)
    record = execute_backup(
        state,
        role=args.role,
        kind=args.kind,
        output_dir=_require_canary_path(Path(args.output_dir), directory=True),
        receipt=receipt,
    )
    if args.kind == "backend-baseline":
        verify_final_serial(
            state,
            role=args.role,
            port=board.inventory.port,
            timeout=180,
        )
    print(
        f"Verified immutable {args.kind} backup for {args.role}: "
        f"full_sha256={record.full_sha256} nvs_sha256={record.nvs_sha256}"
    )


def _handle_verify_backup(args: argparse.Namespace) -> None:
    state = _load_state(args.state)
    backup = state.verify_backup(args.role, args.kind)
    print(
        f"Backup PASS role={args.role} kind={args.kind} "
        f"full_sha256={backup.full_sha256}"
    )


def _handle_quiescence(args: argparse.Namespace) -> None:
    state = _load_state(args.state)
    receipt = _resolve_for_state(state, args.pio)
    live = verify_original_uplink_quiescence(state, receipt=receipt)
    print(f"Original uplink remains ROM-quiescent: {live.port} {live.mac}")


def _handle_challenge_flash(args: argparse.Namespace) -> None:
    state = _load_state(args.state)
    receipt = _resolve_for_state(state, args.pio)
    artifact = verify_strict_release_artifact(
        Path(args.index), Path(args.artifact_dir), role=args.role
    )
    challenge, token = issue_initial_flash_challenge(
        state,
        role=args.role,
        artifact=artifact,
        receipt=receipt,
        output=_require_canary_path(Path(args.output)),
    )
    board = state.boards[args.role]
    summary = {
        "role": args.role,
        "port": challenge.port,
        "mac": challenge.mac,
        "physical_no_screen_lite_confirmed": challenge.lite_sensor_confirmed,
        "xiao_sense_sd_absent_confirmed": challenge.no_sd_expansion_confirmed,
        "installed_identity": asdict(board.installed) if board.installed else None,
        "original_backup": asdict(board.backups["original"]),
        "backend_identity": {
            "target": artifact.target,
            "project": artifact.project,
            "hardware": artifact.hardware,
            "version": artifact.version,
            "identity_crc32": artifact.identity_crc32,
        },
        "artifact_sha256": artifact.firmware_sha256,
        "parts": artifact.parts,
        "toolchain": asdict(receipt),
        "old_uplink_quiescence": state.original_uplink_quiescence,
        "expires_at": challenge.expires_at,
    }
    print(json.dumps(summary, sort_keys=True, indent=2))
    print(f"challenge_id={challenge.challenge_id}")
    print(f"approval_token={token}")


def _handle_flash_initial(args: argparse.Namespace) -> None:
    state = _load_state(args.state)
    receipt = _resolve_for_state(state, args.pio)
    artifact = verify_strict_release_artifact(
        Path(args.index), Path(args.artifact_dir), role=args.role
    )
    execute_initial_flash(
        state,
        role=args.role,
        artifact=artifact,
        challenge_id=args.challenge_id,
        token=args.token,
        receipt=receipt,
    )
    print(
        f"Initial Backend/Lite flash verified for {args.role}; protected NVS "
        "matched and application was started. Run verify-provisional next."
    )


def _handle_verify_provisional(args: argparse.Namespace) -> None:
    state = _load_state(args.state)
    evidence = verify_provisional_serial(
        state, role=args.role, port=args.port, timeout=args.timeout
    )
    print(json.dumps(evidence, sort_keys=True))


def _handle_verify_final(args: argparse.Namespace) -> None:
    state = _load_state(args.state)
    evidence = verify_final_serial(
        state, role=args.role, port=args.port, timeout=args.timeout
    )
    print(json.dumps(evidence, sort_keys=True))


def _handle_verify_catalog(args: argparse.Namespace) -> None:
    evidence = verify_catalog_preflight(
        backend_base=args.backend_base,
        index=Path(args.index),
        output=_require_canary_path(Path(args.output)),
    )
    print(
        "Catalog preflight PASS: "
        + ",".join(sorted(evidence["targets"]))
    )


def _handle_ota_probe(args: argparse.Namespace) -> None:
    state = _load_state(args.state)
    receipt = _resolve_for_state(state, args.pio)
    catalog_path = _require_canary_path(Path(args.catalog_evidence))
    catalog, _sha = load_private_json(catalog_path, label="catalog evidence")
    evidence = run_ota_probe(
        state,
        component=args.component,
        catalog_name=args.catalog_name,
        expected_sha256=args.expected_sha,
        catalog_evidence=catalog,
        index=Path(args.index),
        port=args.port,
        receipt=receipt,
        timeout=args.timeout,
        output=_require_canary_path(Path(args.output)),
    )
    print(json.dumps(asdict(evidence), sort_keys=True))


def _load_probe(path: Path) -> OtaEvidence:
    value, _sha = load_private_json(path, label="OTA probe evidence")
    captured = value.pop("captured_at", None)
    if not isinstance(captured, int):
        raise CanaryApprovalError("OTA probe capture time is missing")
    return parse_ota_evidence(value, captured_at=captured)


def _handle_challenge_ota(args: argparse.Namespace) -> None:
    state = _load_state(args.state)
    receipt = _resolve_for_state(state, args.pio)
    probe = _load_probe(_require_canary_path(Path(args.probe)))
    if probe.component != args.component:
        raise CanaryApprovalError("OTA probe component changed")
    catalog_path = _require_canary_path(Path(args.catalog_evidence))
    catalog, catalog_sha = load_private_json(
        catalog_path, label="catalog evidence"
    )
    challenge, token = issue_ota_apply_challenge(
        state,
        component=args.component,
        mode=args.mode,
        probe=probe,
        catalog_evidence=catalog,
        catalog_evidence_sha256=catalog_sha,
        catalog_evidence_path=catalog_path,
        index=Path(args.index),
        receipt=receipt,
        output=_require_canary_path(Path(args.output)),
    )
    print(json.dumps(asdict(challenge), sort_keys=True, indent=2))
    print(f"challenge_id={challenge.challenge_id}")
    print(f"approval_token={token}")


def _handle_ota_apply(args: argparse.Namespace) -> None:
    state = _load_state(args.state)
    receipt = _resolve_for_state(state, args.pio)
    if not state.catalog_evidence_path:
        raise CanaryOrderError("OTA challenge has no bound catalog evidence path")
    catalog, catalog_sha = load_private_json(
        Path(state.catalog_evidence_path), label="catalog evidence"
    )
    _accepted, final = execute_ota_apply(
        state,
        component=args.component,
        mode=args.mode,
        index=Path(args.index),
        port=args.port,
        challenge_id=args.challenge_id,
        token=args.token,
        receipt=receipt,
        output=_require_canary_path(Path(args.output)),
        timeout=args.timeout,
        catalog_evidence=catalog,
        catalog_evidence_sha256=catalog_sha,
    )
    print(
        f"OTA PASS component={args.component} operation_id={final.operation_id} "
        f"sha256={final.sha256}"
    )


def _handle_challenge_restore(args: argparse.Namespace) -> None:
    state = _load_state(args.state)
    receipt = _resolve_for_state(state, args.pio)
    challenge, token = issue_full_restore_challenge(
        state,
        role=args.role,
        source=args.source,
        receipt=receipt,
        output=_require_canary_path(Path(args.output)),
    )
    backup = state.boards[args.role].backups[args.source]
    print(
        "WARNING: full restore writes all 8 MB, including NVS and OTA state.\n"
        + json.dumps(
            {
                "role": args.role,
                "source": args.source,
                "port": challenge.port,
                "mac": challenge.mac,
                "full_path": backup.full_path,
                "full_sha256": backup.full_sha256,
                "toolchain_sha256": challenge.toolchain_sha256,
                "expires_at": challenge.expires_at,
            },
            sort_keys=True,
            indent=2,
        )
    )
    print(f"challenge_id={challenge.challenge_id}")
    print(f"approval_token={token}")


def _handle_restore_full(args: argparse.Namespace) -> None:
    state = _load_state(args.state)
    receipt = _resolve_for_state(state, args.pio)
    execute_full_restore(
        state,
        role=args.role,
        source=args.source,
        challenge_id=args.challenge_id,
        token=args.token,
        receipt=receipt,
    )
    board = state.boards[args.role]
    if board.failure_phase is None:
        print(f"Restore identity observed and verified for {args.role}.")
    else:
        print(
            f"Full restore verified for {args.role}; restore requirement remains "
            "until the original installed identity/MAC is observed again."
        )


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Fail-closed three-board backend sensor canary. Native badge 0.67 "
            "firmware remains the default; backend/Lite writes require an exact "
            "one-use approval. Scanners are direct USB first and uplink is last. "
            "The web flasher is not the canary path; restore is the only permitted "
            "next write after a partial failure."
        )
    )
    subcommands = parser.add_subparsers(dest="command", required=True)
    definitions: dict[str, tuple[str, ...]] = {
        "capture-installed": ("--state", "--uplink-url", "--backend-base", "--output-dir"),
        "inventory": ("--role", "--port", "--state", "--pio"),
        "backup": ("--kind", "--role", "--state", "--output-dir", "--pio"),
        "verify-backup": ("--kind", "--role", "--state"),
        "verify-uplink-quiesced": ("--state", "--pio"),
        "challenge-flash": ("--role", "--state", "--artifact-dir", "--index", "--pio", "--output"),
        "flash-initial": ("--role", "--state", "--artifact-dir", "--index", "--challenge-id", "--token", "--pio"),
        "verify-provisional": ("--role", "--state", "--port", "--timeout"),
        "verify-final": ("--role", "--state", "--port", "--timeout"),
        "verify-catalog": ("--backend-base", "--index", "--output"),
        "ota-probe": ("--component", "--catalog-name", "--expected-sha", "--catalog-evidence", "--index", "--state", "--port", "--pio", "--output", "--timeout"),
        "challenge-ota": ("--component", "--mode", "--probe", "--catalog-evidence", "--index", "--state", "--pio", "--output"),
        "ota-apply": ("--component", "--mode", "--index", "--state", "--port", "--challenge-id", "--token", "--pio", "--output", "--timeout"),
        "challenge-restore": ("--role", "--source", "--state", "--pio", "--output"),
        "restore-full": ("--role", "--source", "--state", "--challenge-id", "--token", "--pio"),
        "status": ("--state",),
    }
    for name, flags in definitions.items():
        child = subcommands.add_parser(name)
        for flag in flags:
            kwargs: dict[str, Any] = {"required": True}
            if flag == "--timeout":
                kwargs["type"] = int
            elif flag == "--role":
                kwargs["choices"] = BOARD_ROLES
            elif flag == "--component":
                kwargs["choices"] = COMPONENTS
            elif flag == "--kind":
                kwargs["choices"] = ("original", "backend-baseline")
            elif flag == "--source":
                kwargs["choices"] = ("original", "backend-baseline")
            elif flag == "--mode":
                kwargs["choices"] = ("newer-only", "same-version-recovery")
            elif flag == "--catalog-name":
                kwargs["choices"] = (
                    "scanner-s3-combo-backend", "uplink-s3-backend"
                )
            child.add_argument(flag, **kwargs)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    os.umask(0o077)
    parser = _build_parser()
    args = parser.parse_args(argv)
    handlers: dict[str, Callable[[argparse.Namespace], None]] = {
        "capture-installed": _handle_capture_installed,
        "inventory": _handle_inventory,
        "backup": _handle_backup,
        "verify-backup": _handle_verify_backup,
        "verify-uplink-quiesced": _handle_quiescence,
        "challenge-flash": _handle_challenge_flash,
        "flash-initial": _handle_flash_initial,
        "verify-provisional": _handle_verify_provisional,
        "verify-final": _handle_verify_final,
        "verify-catalog": _handle_verify_catalog,
        "ota-probe": _handle_ota_probe,
        "challenge-ota": _handle_challenge_ota,
        "ota-apply": _handle_ota_apply,
        "challenge-restore": _handle_challenge_restore,
        "restore-full": _handle_restore_full,
    }
    try:
        if args.command == "status":
            state = _load_state(args.state)
            print(
                json.dumps(
                    redact_secrets(state._dict()), sort_keys=True, indent=2
                )
            )
        else:
            handlers[args.command](args)
        return 0
    except KeyboardInterrupt:
        print("backend canary: interrupted; no command is retried", file=sys.stderr)
        return 130
    except CanaryError as exc:
        print(f"backend canary: FAIL: {redact_text(str(exc))}", file=sys.stderr)
        if args.command in ("flash-initial", "ota-apply"):
            source = "original" if args.command == "flash-initial" else "backend-baseline"
            print(
                "If state now requires recovery, the only permitted next write is:\n"
                f"  challenge-restore --role {args.role if hasattr(args, 'role') else args.component} "
                f"--source {source} --state {args.state} --pio {args.pio} --output <.canary/receipt.json>",
                file=sys.stderr,
            )
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
