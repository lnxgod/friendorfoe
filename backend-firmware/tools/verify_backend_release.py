#!/usr/bin/env python3
"""Fail-closed verification for the isolated backend firmware package."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import struct
import subprocess
import sys
from typing import Iterable, Mapping, Sequence
import zlib

try:
    from tools.firmware_identity import FirmwareIdentityError, verify_backend_image
except ModuleNotFoundError as exc:  # Direct ``python tools/...`` invocation.
    if exc.name != "tools":
        raise
    from firmware_identity import FirmwareIdentityError, verify_backend_image


PROTECTED_PREFIXES = (
    "esp32/",
    "scripts/",
    "tools/badge_flasher/",
)
PROTECTED_FILES = frozenset({".github/workflows/esp32-web-flasher.yml"})

RELEASE_SCHEMA = 1
RELEASE_VERSION = "0.1.0-backend"
BACKEND_HARDWARE = "seeed_xiao_esp32s3"
PINNED_VENDOR_BASE = "2cca5ad8df17ebd8d5f48dc72051441e30df1b8f"
FLASH_SIZE = 0x800000
APP_OFFSET = 0x20000
APP_CAPACITY = 0x200000
APP_END = APP_OFFSET + APP_CAPACITY
NVS_RANGE = (0x9000, 0xF000)

EXPECTED_TARGETS: Mapping[str, Mapping[str, object]] = {
    "scanner-s3-combo-backend": {
        "kind": "scanner",
        "project": "fof_backend_scanner",
        "manifest": "manifest-scanner-s3-combo-backend.json",
        "manifest_name": "Friend or Foe Backend Scanner (XIAO ESP32-S3)",
        "identity_crc32": 0x9DD382FF,
    },
    "uplink-s3-backend": {
        "kind": "uplink",
        "project": "fof_backend_uplink",
        "manifest": "manifest-uplink-s3-backend.json",
        "manifest_name": "Friend or Foe Backend Uplink (XIAO ESP32-S3)",
        "identity_crc32": 0xF08BCDE4,
    },
}
EXPECTED_PARTS = (
    ("bootloader", 0x0000),
    ("partition-table", 0x8000),
    ("ota-data-initial", 0xF000),
    ("firmware", APP_OFFSET),
)

_PARTITION_ENTRY = struct.Struct("<HBBII16sI")
_PARTITION_MAGIC = 0x50AA
_PARTITION_MD5_MAGIC = b"\xeb\xeb"
_HEX_SHA256 = re.compile(r"[0-9a-f]{64}\Z")
_EVENT_SHA = re.compile(r"[0-9a-fA-F]{40}\Z")

_COMMON_PARTITIONS = {
    "nvs": (1, 2, 0x9000, 0x6000),
    "otadata": (1, 0, 0xF000, 0x2000),
    "phy_init": (1, 1, 0x11000, 0x1000),
    "ota_0": (0, 0x10, 0x20000, 0x200000),
    "ota_1": (0, 0x11, 0x220000, 0x200000),
}
_TAIL_PARTITIONS = {
    "scanner": {
        "storage": (1, 0x82, 0x420000, 0x100000),
        "reserved": (1, 0x81, 0x520000, 0x2E0000),
    },
    "uplink": {
        "fw_scanner_be": (1, 0x40, 0x420000, 0x200000),
        "storage": (1, 0x82, 0x620000, 0x100000),
        "reserved": (1, 0x81, 0x720000, 0x0E0000),
    },
}


class ReleaseVerificationError(ValueError):
    """The backend release package failed a fail-closed contract check."""


@dataclass(frozen=True)
class VerifiedRelease:
    index: Path
    flasher: Path
    version: str
    hardware: str
    targets: tuple[str, ...]


@dataclass(frozen=True, order=True)
class ProtectedViolation:
    path: str
    status: str


@dataclass(frozen=True)
class _Partition:
    partition_type: int
    subtype: int
    offset: int
    size: int
    label: str

    @property
    def end(self) -> int:
        return self.offset + self.size


def protected_changes(paths: Iterable[str]) -> list[str]:
    """Return deterministic protected paths from a Git path collection."""

    return sorted(
        {
            path
            for path in paths
            if path in PROTECTED_FILES or path.startswith(PROTECTED_PREFIXES)
        }
    )


def _run_git(
    repository: Path,
    arguments: Sequence[str],
    *,
    binary: bool = False,
) -> bytes | str:
    command = ["git", *arguments]
    try:
        result = subprocess.run(
            command,
            cwd=repository,
            check=False,
            capture_output=True,
            text=not binary,
        )
    except OSError as exc:
        raise ReleaseVerificationError(f"cannot execute Git: {exc}") from exc
    if result.returncode != 0:
        stderr = result.stderr
        if isinstance(stderr, bytes):
            stderr_text = stderr.decode("utf-8", errors="replace")
        else:
            stderr_text = stderr
        detail = stderr_text.strip() or f"exit status {result.returncode}"
        raise ReleaseVerificationError(
            f"Git command failed ({' '.join(arguments)}): {detail}"
        )
    return result.stdout


def _repository_root(repository: Path) -> Path:
    try:
        requested = Path(repository).expanduser().resolve(strict=True)
    except OSError as exc:
        raise ReleaseVerificationError(
            f"repository does not resolve: {repository}: {exc}"
        ) from exc
    output = _run_git(requested, ["rev-parse", "--show-toplevel"])
    assert isinstance(output, str)
    root = Path(output.strip()).resolve(strict=True)
    if root != requested:
        raise ReleaseVerificationError(
            f"repository must be the Git worktree root: {requested}"
        )
    return root


def _resolved_ancestor(repository: Path, revision: str) -> str:
    if not revision or revision.startswith("-") or any(
        character.isspace() for character in revision
    ):
        raise ReleaseVerificationError("audit base is invalid")
    output = _run_git(
        repository,
        ["rev-parse", "--verify", "--quiet", "--end-of-options", f"{revision}^{{commit}}"],
    )
    assert isinstance(output, str)
    resolved = output.strip()
    if not _EVENT_SHA.fullmatch(resolved):
        raise ReleaseVerificationError("audit base did not resolve to one commit")
    try:
        _run_git(repository, ["merge-base", "--is-ancestor", resolved, "HEAD"])
    except ReleaseVerificationError as exc:
        raise ReleaseVerificationError(
            f"audit base is not an ancestor of HEAD: {resolved}"
        ) from exc
    return resolved


def resolve_audit_base(
    *,
    repository: Path,
    event_name: str,
    event_base: str,
    default_branch: str,
) -> str:
    """Resolve the exact fail-closed Git baseline used by the workflow."""

    root = _repository_root(repository)
    if event_name not in {"pull_request", "push"}:
        raise ReleaseVerificationError(f"unsupported audit event: {event_name!r}")
    if not isinstance(event_base, str) or not _EVENT_SHA.fullmatch(event_base):
        raise ReleaseVerificationError("event audit base must be exactly 40 hex digits")
    zero = "0" * 40
    if event_name == "pull_request":
        if event_base == zero:
            raise ReleaseVerificationError("pull-request audit base may not be zero")
        resolved = _resolved_ancestor(root, event_base)
        if resolved.lower() != event_base.lower():
            raise ReleaseVerificationError("event audit base is not the exact commit SHA")
        return event_base
    if event_base != zero:
        resolved = _resolved_ancestor(root, event_base)
        if resolved.lower() != event_base.lower():
            raise ReleaseVerificationError("event audit base is not the exact commit SHA")
        return event_base

    if (
        not isinstance(default_branch, str)
        or not default_branch
        or default_branch.startswith("-")
        or default_branch.startswith("refs/")
        or any(character.isspace() for character in default_branch)
    ):
        raise ReleaseVerificationError("repository default branch is missing or invalid")
    _run_git(root, ["check-ref-format", "--branch", default_branch])
    remote_ref = f"refs/remotes/origin/{default_branch}"
    source_ref = f"refs/heads/{default_branch}"
    _run_git(
        root,
        [
            "fetch",
            "--no-tags",
            "--prune",
            "origin",
            f"+{source_ref}:{remote_ref}",
        ],
    )
    tip_output = _run_git(root, ["show-ref", "--verify", "--hash", remote_ref])
    assert isinstance(tip_output, str)
    tips = [line for line in tip_output.splitlines() if line]
    if len(tips) != 1 or not _EVENT_SHA.fullmatch(tips[0]):
        raise ReleaseVerificationError("default branch remote tip is missing or ambiguous")
    merge_base_output = _run_git(root, ["merge-base", tips[0], "HEAD"])
    assert isinstance(merge_base_output, str)
    merge_bases = [line for line in merge_base_output.splitlines() if line]
    if len(merge_bases) != 1 or not _EVENT_SHA.fullmatch(merge_bases[0]):
        raise ReleaseVerificationError("default-branch merge base is missing or ambiguous")
    return _resolved_ancestor(root, merge_bases[0])


def _decode_git_path(value: bytes) -> str:
    return value.decode("utf-8", errors="surrogateescape")


def _diff_entries(raw: bytes) -> list[tuple[str, tuple[str, ...]]]:
    fields = raw.split(b"\0")
    if fields and fields[-1] == b"":
        fields.pop()
    entries: list[tuple[str, tuple[str, ...]]] = []
    index = 0
    while index < len(fields):
        status = _decode_git_path(fields[index])
        index += 1
        if not status or status[0] not in "ACMRTD":
            raise ReleaseVerificationError(f"unsupported Git diff status: {status!r}")
        path_count = 2 if status[0] in "CR" else 1
        if index + path_count > len(fields):
            raise ReleaseVerificationError("truncated Git name-status output")
        paths = tuple(_decode_git_path(value) for value in fields[index : index + path_count])
        if any(not path for path in paths):
            raise ReleaseVerificationError("empty path in Git name-status output")
        entries.append((status, paths))
        index += path_count
    return entries


def _porcelain_entries(raw: bytes) -> list[tuple[str, tuple[str, ...]]]:
    records = raw.split(b"\0")
    if records and records[-1] == b"":
        records.pop()
    entries: list[tuple[str, tuple[str, ...]]] = []
    index = 0
    while index < len(records):
        record = records[index]
        index += 1
        if len(record) < 4 or record[2:3] != b" ":
            raise ReleaseVerificationError("malformed Git porcelain output")
        status = _decode_git_path(record[:2])
        paths = [_decode_git_path(record[3:])]
        if any(character in "RC" for character in status):
            if index >= len(records):
                raise ReleaseVerificationError("truncated Git porcelain rename")
            paths.append(_decode_git_path(records[index]))
            index += 1
        if any(not path for path in paths):
            raise ReleaseVerificationError("empty path in Git porcelain output")
        entries.append((status, tuple(paths)))
    return entries


def _porcelain_changed_paths(
    status: str, paths: tuple[str, ...]
) -> tuple[str, ...]:
    """Return paths changed by porcelain status, whose C order is dest/source."""

    return paths[:1] if "C" in status else paths


def audit_protected(*, repository: Path, base: str) -> list[ProtectedViolation]:
    """Audit committed and untracked paths without an empty-diff fallback."""

    root = _repository_root(repository)
    resolved = _resolved_ancestor(root, base)
    raw_diff = _run_git(
        root,
        [
            "diff",
            "--name-status",
            "-z",
            "--find-renames",
            "--find-copies",
            "--find-copies-harder",
            resolved,
            "HEAD",
            "--",
        ],
        binary=True,
    )
    assert isinstance(raw_diff, bytes)
    violations: set[ProtectedViolation] = set()
    for status, paths in _diff_entries(raw_diff):
        # A rename changes/removes its source, so both paths are relevant. A
        # copy leaves its source byte-identical; auditing the source would turn
        # intentional backend vendoring into a false protected-path mutation.
        # The copy destination is still checked and therefore any copy *into*
        # a protected path fails closed.
        changed_paths = paths[1:] if status.startswith("C") else paths
        for path in protected_changes(changed_paths):
            violations.add(ProtectedViolation(path=path, status=status))

    raw_status = _run_git(
        root,
        ["status", "--porcelain=v1", "-z", "--untracked-files=all"],
        binary=True,
    )
    assert isinstance(raw_status, bytes)
    for status, paths in _porcelain_entries(raw_status):
        for path in protected_changes(_porcelain_changed_paths(status, paths)):
            violations.add(ProtectedViolation(path=path, status=status))
    return sorted(violations)


def verify_vendor_base(path: Path) -> str:
    """Require the detector-source provenance pin to remain byte-exact."""

    try:
        raw = Path(path).read_text(encoding="ascii")
    except (OSError, UnicodeError) as exc:
        raise ReleaseVerificationError(f"cannot read VENDOR_BASE: {exc}") from exc
    if raw not in {PINNED_VENDOR_BASE, PINNED_VENDOR_BASE + "\n"}:
        raise ReleaseVerificationError(
            f"VENDOR_BASE must remain pinned to {PINNED_VENDOR_BASE}"
        )
    return PINNED_VENDOR_BASE


def verify_source_isolation(root: Path) -> list[str]:
    """Run the repository's backend-firmware source-boundary audit."""

    try:
        if __package__:
            from tools.check_source_isolation import audit_tree
        else:
            from check_source_isolation import audit_tree
        findings = audit_tree(Path(root))
    except (OSError, ValueError) as exc:
        raise ReleaseVerificationError(f"source-isolation audit failed: {exc}") from exc
    if findings:
        details = "\n".join(findings)
        raise ReleaseVerificationError(
            f"source-isolation audit found {len(findings)} violation(s):\n{details}"
        )
    return findings


def _object_without_duplicate_keys(pairs: list[tuple[str, object]]) -> dict:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise ReleaseVerificationError(f"duplicate JSON key: {key}")
        result[key] = value
    return result


def _load_json(path: Path, description: str) -> dict:
    try:
        raw = path.read_text(encoding="utf-8")
        value = json.loads(raw, object_pairs_hook=_object_without_duplicate_keys)
    except ReleaseVerificationError:
        raise
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise ReleaseVerificationError(
            f"cannot read {description}: {path}: {exc}"
        ) from exc
    if not isinstance(value, dict):
        raise ReleaseVerificationError(f"{description} must be a JSON object")
    return value


def _require_exact_keys(
    value: Mapping[str, object], expected: set[str], description: str
) -> None:
    actual = set(value)
    if actual != expected:
        raise ReleaseVerificationError(
            f"{description} keys mismatch: expected {sorted(expected)}, "
            f"got {sorted(actual)}"
        )


def _integer(value: object, description: str, *, positive: bool = False) -> int:
    if not isinstance(value, int) or isinstance(value, bool):
        raise ReleaseVerificationError(f"{description} must be an integer")
    if positive and value <= 0:
        raise ReleaseVerificationError(f"{description} must be positive")
    return value


def _decode_c_string(field: bytes, description: str) -> str:
    try:
        nul = field.index(0)
    except ValueError as exc:
        raise ReleaseVerificationError(
            f"{description} is not NUL terminated"
        ) from exc
    if any(field[nul + 1 :]):
        raise ReleaseVerificationError(f"{description} has a nonzero tail")
    try:
        value = field[:nul].decode("ascii")
    except UnicodeDecodeError as exc:
        raise ReleaseVerificationError(f"{description} is not ASCII") from exc
    if not value:
        raise ReleaseVerificationError(f"{description} is empty")
    return value


def _decode_partition_table(data: bytes, kind: str) -> dict[str, _Partition]:
    if not data or len(data) > 0x1000 or len(data) % _PARTITION_ENTRY.size:
        raise ReleaseVerificationError("partition table binary has an invalid size")
    partitions: dict[str, _Partition] = {}
    md5_seen = False
    entry_bytes = bytearray()
    for offset in range(0, len(data), _PARTITION_ENTRY.size):
        record = data[offset : offset + _PARTITION_ENTRY.size]
        if record == b"\xff" * _PARTITION_ENTRY.size:
            if any(value != 0xFF for value in data[offset:]):
                raise ReleaseVerificationError("partition table has data after terminator")
            break
        if record[:2] == _PARTITION_MD5_MAGIC:
            if md5_seen or record[2:16] != b"\xff" * 14:
                raise ReleaseVerificationError("partition table MD5 record is invalid")
            if record[16:32] != hashlib.md5(entry_bytes).digest():
                raise ReleaseVerificationError("partition table MD5 digest mismatch")
            md5_seen = True
            if any(value != 0xFF for value in data[offset + 32 :]):
                raise ReleaseVerificationError("partition table has data after MD5 record")
            break
        if md5_seen:
            raise ReleaseVerificationError("partition entry follows MD5 record")
        (
            magic,
            partition_type,
            subtype,
            part_offset,
            size,
            raw_label,
            flags,
        ) = _PARTITION_ENTRY.unpack(record)
        if magic != _PARTITION_MAGIC or flags != 0:
            raise ReleaseVerificationError("partition table entry is invalid")
        label = _decode_c_string(raw_label, "partition label")
        if label in partitions:
            raise ReleaseVerificationError(f"duplicate partition label: {label}")
        if size <= 0 or part_offset + size > FLASH_SIZE:
            raise ReleaseVerificationError(f"partition range is invalid: {label}")
        partitions[label] = _Partition(
            partition_type=partition_type,
            subtype=subtype,
            offset=part_offset,
            size=size,
            label=label,
        )
        entry_bytes.extend(record)
    if not md5_seen:
        raise ReleaseVerificationError("partition table MD5 record is missing")

    expected = dict(_COMMON_PARTITIONS)
    expected.update(_TAIL_PARTITIONS[kind])
    actual = {
        label: (
            part.partition_type,
            part.subtype,
            part.offset,
            part.size,
        )
        for label, part in partitions.items()
    }
    if actual != expected:
        raise ReleaseVerificationError(
            f"decoded {kind} partition layout does not match the backend contract"
        )
    return partitions


def _safe_index_path(value: object, target: str, name: str) -> PurePosixPath:
    if not isinstance(value, str) or "\\" in value:
        raise ReleaseVerificationError("artifact path is invalid")
    path = PurePosixPath(value)
    if path.is_absolute() or value != path.as_posix():
        raise ReleaseVerificationError("artifact path is not canonical")
    if path.parts != (target, name):
        raise ReleaseVerificationError("artifact path/name disagreement")
    return path


def _validate_ranges(
    parts: list[dict], partitions: Mapping[str, _Partition], target: str
) -> None:
    ranges: list[tuple[int, int, str]] = []
    expected_offsets = dict(EXPECTED_PARTS)
    for part in parts:
        name = part["name"]
        logical = name[len(target) + 1 : -4]
        offset = _integer(part["offset"], f"{name} offset")
        size = _integer(part["size"], f"{name} size", positive=True)
        if offset != expected_offsets[logical]:
            raise ReleaseVerificationError(f"{name} offset is not exact")
        end = offset + size
        if end <= offset or end > FLASH_SIZE:
            raise ReleaseVerificationError(f"{name} range exceeds 8-MB flash")
        if logical == "bootloader" and end > 0x8000:
            raise ReleaseVerificationError("bootloader crosses partition table")
        if logical == "partition-table" and (offset, end) != (0x8000, 0x9000):
            raise ReleaseVerificationError("partition table boundary is not exact")
        if logical == "ota-data-initial" and (offset, end) != (0xF000, 0x11000):
            raise ReleaseVerificationError("OTA-data boundary is not exact")
        if logical == "firmware":
            if end > APP_END:
                raise ReleaseVerificationError("application exceeds OTA slot")
            ota_0 = partitions["ota_0"]
            if offset < ota_0.offset or end > ota_0.end:
                raise ReleaseVerificationError("application is outside decoded OTA slot")
        if offset < NVS_RANGE[1] and NVS_RANGE[0] < end:
            raise ReleaseVerificationError(f"{name} intersects protected NVS")
        ranges.append((offset, end, name))
    for index, left in enumerate(ranges):
        for right in ranges[index + 1 :]:
            if left[0] < right[1] and right[0] < left[1]:
                raise ReleaseVerificationError(
                    f"packaged parts overlap: {left[2]} and {right[2]}"
                )


def _verify_target(
    *, target: str, release: object, version: str, flasher: Path
) -> set[Path]:
    spec = EXPECTED_TARGETS[target]
    if not isinstance(release, dict):
        raise ReleaseVerificationError(f"release target {target} must be an object")
    _require_exact_keys(
        release,
        {
            "kind",
            "target",
            "project",
            "hardware",
            "identity_crc32",
            "partition_capacity",
            "parts",
        },
        f"release target {target}",
    )
    exact_values = {
        "kind": spec["kind"],
        "target": target,
        "project": spec["project"],
        "hardware": BACKEND_HARDWARE,
    }
    for field, expected in exact_values.items():
        if release[field] != expected:
            raise ReleaseVerificationError(f"{target} {field} mismatch")
    identity_crc = _integer(release["identity_crc32"], "identity CRC32")
    if identity_crc != spec["identity_crc32"]:
        raise ReleaseVerificationError(f"{target} identity CRC32 mismatch")
    if release["partition_capacity"] != APP_CAPACITY:
        raise ReleaseVerificationError(f"{target} partition capacity mismatch")
    raw_parts = release["parts"]
    if not isinstance(raw_parts, list) or len(raw_parts) != len(EXPECTED_PARTS):
        raise ReleaseVerificationError(f"{target} must contain exactly four parts")

    parts: list[dict] = []
    artifact_paths: set[Path] = set()
    firmware_binding: tuple[Path, int, str, int] | None = None
    expected_names = [f"{target}-{logical}.bin" for logical, _ in EXPECTED_PARTS]
    for position, (raw_part, expected_name) in enumerate(
        zip(raw_parts, expected_names, strict=True)
    ):
        if not isinstance(raw_part, dict):
            raise ReleaseVerificationError(f"{target} part {position} must be an object")
        _require_exact_keys(
            raw_part,
            {"name", "path", "offset", "size", "sha256", "crc32"},
            f"{target} part {position}",
        )
        if raw_part["name"] != expected_name:
            raise ReleaseVerificationError(
                f"{target} part has a generic or unexpected basename"
            )
        relative = _safe_index_path(raw_part["path"], target, expected_name)
        artifact = flasher / "firmware" / Path(*relative.parts)
        if artifact.is_symlink():
            raise ReleaseVerificationError(f"artifact may not be a symlink: {artifact}")
        try:
            data = artifact.read_bytes()
        except OSError as exc:
            raise ReleaseVerificationError(f"cannot read artifact: {artifact}") from exc
        size = _integer(raw_part["size"], f"{expected_name} size", positive=True)
        sha256 = raw_part["sha256"]
        crc32 = _integer(raw_part["crc32"], f"{expected_name} CRC32")
        if not isinstance(sha256, str) or not _HEX_SHA256.fullmatch(sha256):
            raise ReleaseVerificationError(f"{expected_name} SHA-256 is invalid")
        if crc32 < 0 or crc32 > 0xFFFFFFFF:
            raise ReleaseVerificationError(f"{expected_name} CRC32 is out of range")
        if (
            size != len(data)
            or sha256 != hashlib.sha256(data).hexdigest()
            or crc32 != zlib.crc32(data) & 0xFFFFFFFF
        ):
            raise ReleaseVerificationError(
                f"artifact digest/size mismatch: {expected_name}"
            )
        if expected_name == f"{target}-firmware.bin":
            firmware_binding = (artifact, size, sha256, crc32)
        artifact_paths.add(artifact.resolve())
        parts.append(raw_part)

    table_name = f"{target}-partition-table.bin"
    table_data = (flasher / "firmware" / target / table_name).read_bytes()
    partitions = _decode_partition_table(table_data, str(spec["kind"]))
    _validate_ranges(parts, partitions, target)
    if firmware_binding is None:
        raise ReleaseVerificationError(f"{target} firmware artifact is missing")
    firmware_artifact, firmware_size, firmware_sha256, firmware_crc32 = (
        firmware_binding
    )
    try:
        verified_image = verify_backend_image(
            firmware_artifact,
            target=target,
            project=str(spec["project"]),
            hardware=BACKEND_HARDWARE,
            version=version,
            partition_capacity=APP_CAPACITY,
        )
    except FirmwareIdentityError as exc:
        raise ReleaseVerificationError(
            f"{target} firmware identity verification failed: {exc}"
        ) from exc
    expected_image_kind = 1 if spec["kind"] == "scanner" else 0
    if (
        verified_image.image_kind != expected_image_kind
        or verified_image.identity_crc32 != identity_crc
        or verified_image.size != firmware_size
        or verified_image.sha256 != firmware_sha256
        or verified_image.crc32 != firmware_crc32
    ):
        raise ReleaseVerificationError(
            f"{target} firmware identity or digest disagrees with the index"
        )

    manifest_path = flasher / str(spec["manifest"])
    manifest = _load_json(manifest_path, f"{target} manifest")
    _require_exact_keys(manifest, {"name", "version", "builds"}, f"{target} manifest")
    if manifest["name"] != spec["manifest_name"]:
        raise ReleaseVerificationError(f"{target} manifest name mismatch")
    if manifest["version"] != version:
        raise ReleaseVerificationError(f"{target} manifest version mismatch")
    builds = manifest["builds"]
    if not isinstance(builds, list) or len(builds) != 1:
        raise ReleaseVerificationError(f"{target} manifest must have one build")
    build = builds[0]
    if not isinstance(build, dict):
        raise ReleaseVerificationError(f"{target} manifest build is invalid")
    _require_exact_keys(build, {"chipFamily", "parts"}, f"{target} manifest build")
    if build["chipFamily"] != "ESP32-S3":
        raise ReleaseVerificationError(f"{target} manifest chip family mismatch")
    manifest_parts = build["parts"]
    if not isinstance(manifest_parts, list) or len(manifest_parts) != 4:
        raise ReleaseVerificationError(f"{target} manifest parts mismatch")
    for indexed, manifested in zip(parts, manifest_parts, strict=True):
        if not isinstance(manifested, dict):
            raise ReleaseVerificationError(f"{target} manifest part is invalid")
        _require_exact_keys(manifested, {"path", "offset"}, f"{target} manifest part")
        expected_path = f"firmware/{indexed['path']}"
        if manifested["path"] != expected_path or manifested["offset"] != indexed["offset"]:
            raise ReleaseVerificationError(f"{target} manifest does not match index")
    return artifact_paths


def verify_release(*, index: Path, flasher: Path) -> VerifiedRelease:
    """Verify an exact scanner/uplink package without trusting its index."""

    index = Path(index)
    flasher = Path(flasher)
    body = _load_json(index, "backend release index")
    _require_exact_keys(body, {"schema", "version", "targets"}, "release index")
    if body["schema"] != RELEASE_SCHEMA:
        raise ReleaseVerificationError("release index schema must be 1")
    if body["version"] != RELEASE_VERSION:
        raise ReleaseVerificationError("release index version mismatch")
    targets = body["targets"]
    if not isinstance(targets, dict) or set(targets) != set(EXPECTED_TARGETS):
        raise ReleaseVerificationError("release index must contain exactly two targets")

    indexed_artifacts: set[Path] = set()
    for target in sorted(EXPECTED_TARGETS):
        indexed_artifacts.update(
            _verify_target(
                target=target,
                release=targets[target],
                version=RELEASE_VERSION,
                flasher=flasher,
            )
        )

    firmware_root = flasher / "firmware"
    actual_artifacts: set[Path] = set()
    if not firmware_root.is_dir():
        raise ReleaseVerificationError("web flasher firmware directory is missing")
    for artifact in firmware_root.rglob("*.bin"):
        if artifact.is_symlink() or not artifact.is_file():
            raise ReleaseVerificationError(f"invalid binary artifact: {artifact}")
        actual_artifacts.add(artifact.resolve())
    if actual_artifacts != indexed_artifacts:
        raise ReleaseVerificationError("extra or missing binary files beneath web flasher")

    return VerifiedRelease(
        index=index,
        flasher=flasher,
        version=RELEASE_VERSION,
        hardware=BACKEND_HARDWARE,
        targets=tuple(sorted(EXPECTED_TARGETS)),
    )


def _argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--index", type=Path)
    parser.add_argument("--flasher", type=Path)
    parser.add_argument("--audit-protected", metavar="BASE")
    parser.add_argument("--repository", type=Path)
    parser.add_argument("--resolve-audit-base", action="store_true")
    parser.add_argument("--event-name")
    parser.add_argument("--event-base")
    parser.add_argument("--default-branch")
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    parser = _argument_parser()
    arguments = parser.parse_args(argv)
    try:
        if arguments.resolve_audit_base:
            if any(
                value is not None
                for value in (
                    arguments.index,
                    arguments.flasher,
                    arguments.audit_protected,
                )
            ):
                parser.error(
                    "base-resolution mode does not accept release or audit paths"
                )
            missing = [
                name
                for name in ("repository", "event_name", "event_base", "default_branch")
                if getattr(arguments, name) is None
            ]
            if missing:
                parser.error(
                    "base-resolution mode requires: " + ", ".join(missing)
                )
            resolved = resolve_audit_base(
                repository=arguments.repository,
                event_name=arguments.event_name,
                event_base=arguments.event_base,
                default_branch=arguments.default_branch,
            )
            print(resolved)
            return 0

        if arguments.index is None or arguments.flasher is None:
            parser.error("release verification requires --index and --flasher")
        if arguments.repository is not None or any(
            value is not None
            for value in (
                arguments.event_name,
                arguments.event_base,
                arguments.default_branch,
            )
        ):
            parser.error("release verification does not accept resolver arguments")
        backend_root = Path(__file__).resolve().parents[1]
        repository = backend_root.parent
        result = verify_release(index=arguments.index, flasher=arguments.flasher)
        verify_vendor_base(backend_root / "VENDOR_BASE")
        verify_source_isolation(backend_root)
        if arguments.audit_protected is not None:
            violations = audit_protected(
                repository=repository,
                base=arguments.audit_protected,
            )
            if violations:
                for violation in violations:
                    print(
                        f"protected path changed: {violation.status} {violation.path}",
                        file=sys.stderr,
                    )
                return 1
        print(
            "backend release: PASS "
            f"version={result.version} targets={','.join(result.targets)}"
        )
        return 0
    except ReleaseVerificationError as exc:
        print(f"backend release: FAIL: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
