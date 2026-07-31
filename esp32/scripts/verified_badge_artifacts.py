#!/usr/bin/env python3
"""Snapshot-producing badge artifact verifiers with immutable handoff."""

from __future__ import annotations

import json
import os
import shlex
import stat
import struct
from dataclasses import dataclass
from pathlib import Path
from typing import Mapping

from secure_artifact_tree import (
    MAX_ARTIFACT_MEMBER_BYTES,
    FrozenArtifactSet,
    SecureArtifactError,
    SecureArtifactTree,
    SnapshotFileSpec,
    VerifiedBadgeArtifactSnapshot,
    run_private_partition_generator,
)


PARTITION_GENERATOR_ENV = "ESP_IDF_PARTITION_GENERATOR"
PARTITION_GENERATOR_RELATIVE = Path(
    "components/partition_table/gen_esp32part.py"
)


def _test_hook(_stage: str, _path: Path | None = None) -> None:
    """Private race-schedule seam for verifier behavior tests."""


class _DuplicateJsonKeyError(ValueError):
    pass


def _json_object_without_duplicates(
    pairs: list[tuple[str, object]],
) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise _DuplicateJsonKeyError(
                f"duplicate JSON key {key!r}"
            )
        result[key] = value
    return result


@dataclass(frozen=True, slots=True)
class BadgeArtifactRole:
    name: str
    app_filename: str
    app_offset: int
    ota_layout: bool
    text_manifests: tuple[str, ...]

    @property
    def full_mapping(self) -> dict[int, str]:
        result = {
            0x00000: "bootloader/bootloader.bin",
            0x08000: "partition_table/partition-table.bin",
            self.app_offset: self.app_filename,
        }
        if self.ota_layout:
            result[0x0F000] = "ota_data_initial.bin"
        return result

    @property
    def app_mapping(self) -> dict[int, str]:
        return {self.app_offset: self.app_filename}

    @property
    def aliases(self) -> dict[str, str]:
        return {
            "bootloader/bootloader.bin": "bootloader.bin",
            "partition_table/partition-table.bin": "partitions.bin",
            self.app_filename: "firmware.bin",
        }


@dataclass(frozen=True, slots=True)
class PrivateGameArtifactAcceptance:
    sha256: str
    image_bytes: int
    max_image_bytes: int
    internal_ram_bytes: int
    max_internal_ram_bytes: int


@dataclass(frozen=True, slots=True)
class PrivateGameAcceptance:
    candidate_version: str
    scanner: PrivateGameArtifactAcceptance
    uplink: PrivateGameArtifactAcceptance
    physically_accepted: bool
    physical_evidence: tuple[str, ...]


def _acceptance_exact_int(
    value: object,
    *,
    name: str,
) -> int:
    if type(value) is not int or value <= 0:
        raise SecureArtifactError(f"{name} must be a positive integer")
    return value


def _parse_private_game_artifact(
    value: object,
    *,
    role: str,
) -> PrivateGameArtifactAcceptance:
    if type(value) is not dict or set(value) != {
        "sha256",
        "image_bytes",
        "max_image_bytes",
        "internal_ram_bytes",
        "max_internal_ram_bytes",
    }:
        raise SecureArtifactError(
            f"{role} acceptance fields are not exact"
        )
    sha256 = value["sha256"]
    if (
        type(sha256) is not str
        or len(sha256) != 64
        or any(ch not in "0123456789abcdef" for ch in sha256)
    ):
        raise SecureArtifactError(
            f"{role} sha256 must be 64 lowercase hexadecimal characters"
        )
    image_bytes = _acceptance_exact_int(
        value["image_bytes"], name=f"{role} image_bytes"
    )
    max_image_bytes = _acceptance_exact_int(
        value["max_image_bytes"], name=f"{role} max_image_bytes"
    )
    internal_ram_bytes = _acceptance_exact_int(
        value["internal_ram_bytes"],
        name=f"{role} internal_ram_bytes",
    )
    max_internal_ram_bytes = _acceptance_exact_int(
        value["max_internal_ram_bytes"],
        name=f"{role} max_internal_ram_bytes",
    )
    if image_bytes > max_image_bytes:
        raise SecureArtifactError(f"{role} image exceeds acceptance budget")
    if internal_ram_bytes > max_internal_ram_bytes:
        raise SecureArtifactError(
            f"{role} internal RAM exceeds acceptance budget"
        )
    return PrivateGameArtifactAcceptance(
        sha256=sha256,
        image_bytes=image_bytes,
        max_image_bytes=max_image_bytes,
        internal_ram_bytes=internal_ram_bytes,
        max_internal_ram_bytes=max_internal_ram_bytes,
    )


def load_private_game_acceptance(
    path: os.PathLike[str] | str,
    *,
    expected_version: str,
) -> PrivateGameAcceptance:
    source = Path(path)
    if source.is_symlink() or not source.is_file():
        raise SecureArtifactError(
            "private game acceptance must be a regular file"
        )
    try:
        raw = source.read_bytes()
    except OSError as exc:
        raise SecureArtifactError(
            "private game acceptance is unreadable"
        ) from exc
    if len(raw) == 0 or len(raw) > 65_536:
        raise SecureArtifactError(
            "private game acceptance size is invalid"
        )
    try:
        payload = json.loads(
            raw.decode("utf-8"),
            object_pairs_hook=_json_object_without_duplicates,
        )
    except (UnicodeDecodeError, json.JSONDecodeError,
            _DuplicateJsonKeyError) as exc:
        raise SecureArtifactError(
            "private game acceptance is not strict JSON"
        ) from exc
    if type(payload) is not dict or set(payload) != {
        "schema_version",
        "candidate_version",
        "artifacts",
        "physically_accepted",
        "physical_evidence",
    }:
        raise SecureArtifactError(
            "private game acceptance fields are not exact"
        )
    if payload["schema_version"] != 1:
        raise SecureArtifactError(
            "private game acceptance schema version is unsupported"
        )
    version = payload["candidate_version"]
    if type(version) is not str or version != expected_version:
        raise SecureArtifactError(
            "private game candidate version does not match"
        )
    role_payloads = payload["artifacts"]
    if type(role_payloads) is not dict or set(role_payloads) != {
        "scanner", "uplink"
    }:
        raise SecureArtifactError(
            "private game artifact roles are not exact"
        )
    physically_accepted = payload["physically_accepted"]
    evidence = payload["physical_evidence"]
    if type(physically_accepted) is not bool:
        raise SecureArtifactError(
            "physically_accepted must be boolean"
        )
    if (
        type(evidence) is not list
        or any(type(item) is not str or not item for item in evidence)
    ):
        raise SecureArtifactError(
            "physical evidence must be a list of nonempty strings"
        )
    if physically_accepted and not evidence:
        raise SecureArtifactError(
            "physical evidence is required for accepted firmware"
        )
    return PrivateGameAcceptance(
        candidate_version=version,
        scanner=_parse_private_game_artifact(
            role_payloads["scanner"], role="scanner"
        ),
        uplink=_parse_private_game_artifact(
            role_payloads["uplink"], role="uplink"
        ),
        physically_accepted=physically_accepted,
        physical_evidence=tuple(evidence),
    )


SCANNER_ROLE = BadgeArtifactRole(
    name="scanner",
    app_filename="fof_badge_scanner.bin",
    app_offset=0x20000,
    ota_layout=True,
    text_manifests=(
        "flash_args",
        "app-flash_args",
        "flash_app_args",
        "flash_project_args",
    ),
)
UPLINK_ROLE = BadgeArtifactRole(
    name="uplink",
    app_filename="fof_badge_uplink.bin",
    app_offset=0x20000,
    ota_layout=True,
    text_manifests=(
        "flash_args",
        "flash_app_args",
        "flash_project_args",
    ),
)
FACTORY_PROBE_ROLE = BadgeArtifactRole(
    name="factory-probe",
    app_filename="fof_badge_factory_probe.bin",
    app_offset=0x10000,
    ota_layout=False,
    text_manifests=(
        "flash_args",
        "app-flash_args",
        "flash_app_args",
        "flash_project_args",
    ),
)


@dataclass(frozen=True, slots=True)
class FrozenBadgeArtifactSnapshots:
    scanner: FrozenArtifactSet
    uplink: FrozenArtifactSet
    probe: FrozenArtifactSet


def _lexical_absolute(
    path: os.PathLike[str] | str,
    label: str,
) -> Path:
    try:
        rendered = os.fspath(path)
    except TypeError as exc:
        raise SecureArtifactError(f"{label} path is malformed") from exc
    if type(rendered) is not str or not rendered or "\x00" in rendered:
        raise SecureArtifactError(f"{label} path is malformed")
    absolute = os.path.abspath(rendered)
    if not os.path.isabs(absolute) or os.path.normpath(absolute) != absolute:
        raise SecureArtifactError(f"{label} path is not lexical absolute")
    return Path(absolute)


def _find_partition_generator() -> Path:
    override = os.environ.get(PARTITION_GENERATOR_ENV)
    if override:
        return _lexical_absolute(
            os.path.expanduser(override),
            "partition generator",
        )

    candidates: list[Path] = []
    idf_path = os.environ.get("IDF_PATH")
    if idf_path:
        candidates.append(
            _lexical_absolute(idf_path, "IDF path")
            / PARTITION_GENERATOR_RELATIVE
        )
    configured_core = os.environ.get("PLATFORMIO_CORE_DIR")
    core_dirs: list[Path] = []
    if configured_core:
        core_dirs.append(
            _lexical_absolute(configured_core, "PlatformIO core")
        )
    core_dirs.append(
        _lexical_absolute(
            Path.home() / ".platformio",
            "PlatformIO core",
        )
    )
    for core_dir in core_dirs:
        packages = core_dir / "packages"
        candidates.append(
            packages / "framework-espidf" / PARTITION_GENERATOR_RELATIVE
        )
        try:
            names = sorted(os.listdir(packages))
        except OSError:
            names = []
        for name in names:
            if name.startswith("framework-espidf@"):
                candidates.append(
                    packages / name / PARTITION_GENERATOR_RELATIVE
                )

    for candidate in candidates:
        try:
            info = os.stat(candidate, follow_symlinks=False)
        except OSError:
            continue
        if stat.S_ISREG(info.st_mode):
            return candidate
    raise SecureArtifactError(
        "ESP-IDF partition generator unavailable"
    )


def materialize_role_aliases(
    build_dir: os.PathLike[str] | str,
    aliases: Mapping[str, str],
) -> None:
    build = _lexical_absolute(build_dir, "badge build")
    tree = SecureArtifactTree.open(build)
    try:
        for alias, canonical in sorted(aliases.items()):
            tree.materialize_alias(
                SnapshotFileSpec(
                    logical_name=(
                        "canonical."
                        + canonical.replace("/", ".")
                    ),
                    relative=canonical,
                ),
                alias,
            )
    finally:
        tree.close()


def _parse_text_manifest(
    logical_name: str,
    content: bytes,
) -> dict[int, str]:
    try:
        text = content.decode("utf-8", errors="strict")
    except UnicodeDecodeError as exc:
        raise SecureArtifactError(
            f"{logical_name}: manifest is not strict UTF-8"
        ) from exc
    entries: dict[int, str] = {}
    for line_number, line in enumerate(text.splitlines(), 1):
        try:
            fields = shlex.split(line)
        except ValueError as exc:
            raise SecureArtifactError(
                f"{logical_name}:{line_number}: invalid shell syntax"
            ) from exc
        index = 0
        while index < len(fields):
            token = fields[index]
            if token in {
                "--flash_mode",
                "--flash_freq",
                "--flash_size",
            }:
                if (
                    index + 1 >= len(fields)
                    or fields[index + 1].startswith("--")
                ):
                    raise SecureArtifactError(
                        f"{logical_name}: option has no value"
                    )
                index += 2
                continue
            if token.startswith("--") or index + 1 >= len(fields):
                raise SecureArtifactError(
                    f"{logical_name}: unexpected manifest token"
                )
            try:
                offset = int(token, 0)
            except ValueError as exc:
                raise SecureArtifactError(
                    f"{logical_name}: invalid flash offset"
                ) from exc
            if offset in entries:
                raise SecureArtifactError(
                    f"{logical_name}: duplicate decoded offset"
                )
            relative = fields[index + 1]
            if (
                not relative
                or relative.startswith("/")
                or "\\" in relative
                or any(
                    part in ("", ".", "..")
                    for part in relative.split("/")
                )
            ):
                raise SecureArtifactError(
                    f"{logical_name}: unsafe referenced path"
                )
            entries[offset] = relative
            index += 2
    return entries


def _parse_json_manifest(
    content: bytes,
    role: BadgeArtifactRole,
) -> dict[int, str]:
    try:
        payload = json.loads(
            content.decode("utf-8", errors="strict"),
            object_pairs_hook=_json_object_without_duplicates,
            parse_constant=lambda value: (_ for _ in ()).throw(
                ValueError(f"non-finite JSON value {value}")
            ),
        )
    except (
        UnicodeDecodeError,
        json.JSONDecodeError,
        _DuplicateJsonKeyError,
        ValueError,
    ) as exc:
        raise SecureArtifactError(
            "flasher_args.json is not strict canonical input"
        ) from exc
    if type(payload) is not dict:
        raise SecureArtifactError(
            "flasher_args.json must be one object"
        )
    registered_paths = frozenset(role.full_mapping.values())

    def validate_referenced_files(value: object) -> None:
        if type(value) is dict:
            for key, child in value.items():
                if key == "file":
                    if (
                        type(child) is not str
                        or child not in registered_paths
                        or child.startswith("/")
                        or "\\" in child
                        or any(
                            part in ("", ".", "..")
                            for part in child.split("/")
                        )
                    ):
                        raise SecureArtifactError(
                            "flasher_args.json references an "
                            "unregistered file"
                        )
                validate_referenced_files(child)
        elif type(value) is list:
            for child in value:
                validate_referenced_files(child)

    validate_referenced_files(payload)
    files = payload.get("flash_files")
    if type(files) is not dict:
        raise SecureArtifactError(
            "flasher_args.json flash_files must be one object"
        )
    entries: dict[int, str] = {}
    for raw_offset, relative in files.items():
        if type(raw_offset) is not str or type(relative) is not str:
            raise SecureArtifactError(
                "flasher_args.json flash mapping is malformed"
            )
        try:
            offset = int(raw_offset, 0)
        except ValueError as exc:
            raise SecureArtifactError(
                "flasher_args.json flash offset is invalid"
            ) from exc
        if offset in entries:
            raise SecureArtifactError(
                "flasher_args.json has duplicate decoded offset"
            )
        if (
            not relative
            or relative.startswith("/")
            or "\\" in relative
            or any(
                part in ("", ".", "..")
                for part in relative.split("/")
            )
        ):
            raise SecureArtifactError(
                "flasher_args.json referenced path is unsafe"
            )
        entries[offset] = relative
    app = payload.get("app")
    if (
        type(app) is not dict
        or app.get("offset") != f"{role.app_offset:#x}"
        or app.get("file") != role.app_filename
    ):
        raise SecureArtifactError(
            "flasher_args.json application identity is wrong"
        )
    bootloader = payload.get("bootloader")
    if (
        type(bootloader) is not dict
        or bootloader.get("offset") != "0x0"
        or bootloader.get("file") != "bootloader/bootloader.bin"
    ):
        raise SecureArtifactError(
            "flasher_args.json bootloader identity is wrong"
        )
    partition_table = payload.get("partition-table")
    if (
        type(partition_table) is not dict
        or partition_table.get("offset") != "0x8000"
        or partition_table.get("file")
        != "partition_table/partition-table.bin"
    ):
        raise SecureArtifactError(
            "flasher_args.json partition table identity is wrong"
        )
    return entries


def _partition_app_offsets(content: bytes) -> list[tuple[int, int]]:
    entries: list[tuple[int, int]] = []
    entry_size = struct.calcsize("<HBBII16sI")
    for index in range(0, len(content) - entry_size + 1, entry_size):
        magic, entry_type, subtype, offset, _size, _label, _flags = (
            struct.unpack(
                "<HBBII16sI",
                content[index:index + entry_size],
            )
        )
        if magic in (0xFFFF, 0xEBEB):
            break
        if magic != 0x50AA:
            raise SecureArtifactError(
                "partitions.bin contains invalid entry magic"
            )
        if entry_type == 0:
            entries.append((subtype, offset))
    return entries


def _csv_app_offset(content: bytes, role: BadgeArtifactRole) -> int:
    try:
        text = content.decode("utf-8", errors="strict")
    except UnicodeDecodeError as exc:
        raise SecureArtifactError(
            "partition CSV is not strict UTF-8"
        ) from exc
    wanted = "ota_0" if role.ota_layout else "factory"
    matches: list[int] = []
    for line in text.splitlines():
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        fields = [field.strip() for field in stripped.split(",")]
        if len(fields) >= 5 and fields[0] == wanted:
            try:
                matches.append(int(fields[3], 0))
            except ValueError as exc:
                raise SecureArtifactError(
                    "partition CSV app offset is invalid"
                ) from exc
    if matches != [role.app_offset]:
        raise SecureArtifactError(
            "partition CSV app offset differs from required layout"
        )
    return matches[0]


def _base_specs(
    root: Path,
    *,
    build: Path,
    partition_source: Path,
    sdkconfig: Path,
    generator: Path,
    role: BadgeArtifactRole,
) -> tuple[SnapshotFileSpec, ...]:
    def relative(path: Path) -> str:
        rendered = os.path.relpath(os.fspath(path), os.fspath(root))
        if (
            rendered == "."
            or rendered.startswith("../")
            or rendered == ".."
            or os.path.isabs(rendered)
        ):
            raise SecureArtifactError(
                "artifact source is outside common descriptor root"
            )
        return rendered

    specs: list[SnapshotFileSpec] = [
        SnapshotFileSpec(
            f"manifest.{name}",
            relative(build / name),
        )
        for name in (*role.text_manifests, "flasher_args.json")
    ]
    specs.extend(
        (
            SnapshotFileSpec(
                "artifact.bootloader",
                relative(build / "bootloader.bin"),
            ),
            SnapshotFileSpec(
                "artifact.partitions",
                relative(build / "partitions.bin"),
            ),
            SnapshotFileSpec(
                "artifact.firmware",
                relative(build / "firmware.bin"),
            ),
            SnapshotFileSpec(
                "alias.bootloader",
                relative(build / "bootloader/bootloader.bin"),
            ),
            SnapshotFileSpec(
                "alias.partitions",
                relative(
                    build / "partition_table/partition-table.bin"
                ),
            ),
            SnapshotFileSpec(
                "alias.application",
                relative(build / role.app_filename),
            ),
            SnapshotFileSpec(
                "partition.csv",
                relative(partition_source),
            ),
            SnapshotFileSpec(
                "build.sdkconfig",
                relative(sdkconfig),
            ),
            SnapshotFileSpec(
                "partition.generator",
                relative(generator),
                allowed_modes=(
                    0o500,
                    0o600,
                    0o640,
                    0o644,
                    0o700,
                    0o750,
                    0o755,
                ),
            ),
        )
    )
    if role == UPLINK_ROLE:
        specs.append(
            SnapshotFileSpec(
                "artifact.elf",
                relative(build / "firmware.elf"),
                allowed_modes=(
                    0o600,
                    0o640,
                    0o644,
                    0o700,
                    0o750,
                    0o755,
                ),
                max_size=MAX_ARTIFACT_MEMBER_BYTES,
            )
        )
    if role.ota_layout:
        specs.append(
            SnapshotFileSpec(
                "artifact.ota_data_initial",
                relative(build / "ota_data_initial.bin"),
            )
        )
    return tuple(specs)


def _validate_frozen_inputs(
    frozen: FrozenArtifactSet,
    role: BadgeArtifactRole,
) -> None:
    full_manifests = {"flash_args", "flash_project_args"}
    registry = set(role.full_mapping.values())
    for name in role.text_manifests:
        entries = _parse_text_manifest(
            f"manifest.{name}",
            frozen.member_bytes(f"manifest.{name}"),
        )
        expected = (
            role.full_mapping
            if name in full_manifests
            else role.app_mapping
        )
        if entries != expected or not set(entries.values()) <= registry:
            raise SecureArtifactError(
                f"{name}: mappings differ from exact role layout"
            )
    json_entries = _parse_json_manifest(
        frozen.member_bytes("manifest.flasher_args.json"),
        role,
    )
    if (
        json_entries != role.full_mapping
        or not set(json_entries.values()) <= registry
    ):
        raise SecureArtifactError(
            "flasher_args.json mappings differ from exact role layout"
        )
    alias_pairs = (
        ("alias.bootloader", "artifact.bootloader"),
        ("alias.partitions", "artifact.partitions"),
        ("alias.application", "artifact.firmware"),
    )
    for alias, canonical in alias_pairs:
        if frozen.member_bytes(alias) != frozen.member_bytes(canonical):
            raise SecureArtifactError(
                "materialized alias differs from canonical artifact"
            )
    partition_bytes = frozen.member_bytes("artifact.partitions")
    app_entries = _partition_app_offsets(partition_bytes)
    expected_subtype = 0x10 if role.ota_layout else 0
    if (expected_subtype, role.app_offset) not in app_entries:
        raise SecureArtifactError(
            "compiled partition table differs from required app layout"
        )
    _csv_app_offset(frozen.member_bytes("partition.csv"), role)
    sdkconfig = frozen.member_bytes("build.sdkconfig")
    if not sdkconfig:
        raise SecureArtifactError("sdkconfig must not be empty")
    if role is SCANNER_ROLE:
        try:
            lines = set(
                sdkconfig.decode("utf-8", errors="strict").splitlines()
            )
        except UnicodeDecodeError as exc:
            raise SecureArtifactError(
                "scanner sdkconfig is not strict UTF-8"
            ) from exc
        required = {
            "CONFIG_PARTITION_TABLE_CUSTOM=y",
            (
                'CONFIG_PARTITION_TABLE_CUSTOM_FILENAME='
                '"partitions_s3_scanner_8mb.csv"'
            ),
            "CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y",
        }
        if not required <= lines:
            raise SecureArtifactError(
                "scanner sdkconfig lacks rollback/custom layout controls"
            )


def verify_descriptor_rooted_frozen_role_authority(
    frozen: FrozenArtifactSet,
    *,
    role: BadgeArtifactRole,
    environment: str,
) -> list[str]:
    """Prove one frozen set is the exact descriptor-rooted role authority."""
    if (
        type(frozen) is not FrozenArtifactSet or
        type(role) is not BadgeArtifactRole or
        type(environment) is not str or
        not environment
    ):
        return ["frozen role authority arguments are malformed"]
    authority = frozen.authority
    if authority is None:
        return [
            f"{role.name}: frozen set lacks descriptor-rooted authority"
        ]
    expected_names = {
        *(
            f"manifest.{name}"
            for name in (*role.text_manifests, "flasher_args.json")
        ),
        "artifact.bootloader",
        "artifact.partitions",
        "artifact.firmware",
        "alias.bootloader",
        "alias.partitions",
        "alias.application",
        "partition.csv",
        "build.sdkconfig",
        "partition.generator",
        "partition.generated",
    }
    if role.ota_layout:
        expected_names.add("artifact.ota_data_initial")
    if role is UPLINK_ROLE:
        expected_names.add("artifact.elf")
    actual_names = {member.logical_name for member in frozen.members}
    if (
        actual_names != expected_names or
        len(frozen.members) != len(expected_names)
    ):
        return [
            f"{role.name}: frozen member inventory is not the exact "
            "required role inventory"
        ]

    bindings = {
        logical_name: source_relative
        for logical_name, source_relative, _size, _sha256
        in authority.source_bindings
    }
    if set(bindings) != expected_names:
        return [
            f"{role.name}: descriptor receipt inventory is inconsistent"
        ]
    if bindings.get("partition.generated") != (
        "generated/partition.generated"
    ):
        return [
            f"{role.name}: partition.generated lacks private generator "
            "authority"
        ]

    role_dir = "uplink" if role is UPLINK_ROLE else (
        "scanner" if role is SCANNER_ROLE else "factory-probe"
    )
    build_prefix = f"{role_dir}/.pio/build/{environment}/"
    build_sources = {
        **{
            f"manifest.{name}": name
            for name in (*role.text_manifests, "flasher_args.json")
        },
        "artifact.bootloader": "bootloader.bin",
        "artifact.partitions": "partitions.bin",
        "artifact.firmware": "firmware.bin",
        "alias.bootloader": "bootloader/bootloader.bin",
        "alias.partitions": "partition_table/partition-table.bin",
        "alias.application": role.app_filename,
    }
    if role.ota_layout:
        build_sources["artifact.ota_data_initial"] = "ota_data_initial.bin"
    if role is UPLINK_ROLE:
        build_sources["artifact.elf"] = "firmware.elf"
    for logical_name, relative in build_sources.items():
        if not bindings[logical_name].endswith(
            build_prefix + relative
        ):
            return [
                f"{role.name}: descriptor source for {logical_name} "
                "is not bound to the selected PlatformIO environment"
            ]
    expected_partition = (
        "uplink/partitions_s3_fof_badge_8mb.csv"
        if role is UPLINK_ROLE else
        "scanner/partitions_s3_scanner_8mb.csv"
        if role is SCANNER_ROLE else
        "factory-probe/partitions.csv"
    )
    if not bindings["partition.csv"].endswith(expected_partition):
        return [
            f"{role.name}: descriptor partition source is not canonical"
        ]
    if not bindings["build.sdkconfig"].endswith(
        f"{role_dir}/sdkconfig.{environment}"
    ):
        return [
            f"{role.name}: descriptor sdkconfig source is not bound to "
            "the selected PlatformIO environment"
        ]
    if not bindings["partition.generator"].endswith(
        "gen_esp32part.py"
    ):
        return [
            f"{role.name}: descriptor partition generator is not canonical"
        ]
    try:
        _validate_frozen_inputs(frozen, role)
        if (
            frozen.member_bytes("partition.generated") !=
            frozen.member_bytes("artifact.partitions")
        ):
            raise SecureArtifactError(
                "private partition generator output differs from "
                "partitions.bin"
            )
    except SecureArtifactError as exc:
        return [f"{role.name}: frozen role validation failed: {exc}"]
    return []


def prepare_verified_role_snapshot(
    role: BadgeArtifactRole,
    build_dir: os.PathLike[str] | str,
    partition_source: os.PathLike[str] | str,
    sdkconfig: os.PathLike[str] | str,
    *,
    private_parent: os.PathLike[str] | str,
    materialize_missing_aliases: bool,
) -> VerifiedBadgeArtifactSnapshot:
    if type(role) is not BadgeArtifactRole:
        raise SecureArtifactError("badge artifact role is malformed")
    if type(materialize_missing_aliases) is not bool:
        raise SecureArtifactError(
            "alias materialization policy must be boolean"
        )
    build = _lexical_absolute(build_dir, "badge build")
    partition = _lexical_absolute(
        partition_source,
        "partition source",
    )
    config = _lexical_absolute(sdkconfig, "sdkconfig")
    generator = _find_partition_generator()
    private = _lexical_absolute(private_parent, "private parent")

    if materialize_missing_aliases:
        materialize_role_aliases(build, role.aliases)

    common = Path(os.path.commonpath([
        os.fspath(build),
        os.fspath(partition.parent),
        os.fspath(config.parent),
        os.fspath(generator.parent),
    ]))
    specs = _base_specs(
        common,
        build=build,
        partition_source=partition,
        sdkconfig=config,
        generator=generator,
        role=role,
    )
    validation_snapshot: VerifiedBadgeArtifactSnapshot | None = None
    final_snapshot: VerifiedBadgeArtifactSnapshot | None = None
    tree = SecureArtifactTree.open(common)
    try:
        validation_snapshot = tree.prepare_snapshot(
            specs,
            private_parent=private,
        )
        expected_sources = {
            member.logical_name: member.source
            for member in validation_snapshot.files
        }
        frozen_validation = validation_snapshot.freeze_for_mutation()
        validation_snapshot.close()
        validation_snapshot = None
        _validate_frozen_inputs(frozen_validation, role)
        _test_hook("before_partition_generator_exec", generator)
        generated = run_private_partition_generator(
            frozen_validation,
            csv_logical_name="partition.csv",
            generator_logical_name="partition.generator",
            expected_logical_name="artifact.partitions",
            output_logical_name="partition.generated",
            private_parent=private,
        )
        _test_hook("after_partition_generator_exec", generator)
        final_snapshot = tree.prepare_snapshot(
            specs,
            generated_members=(generated,),
            private_parent=private,
        )
        actual_sources = {
            member.logical_name: member.source
            for member in final_snapshot.files
            if member.logical_name != "partition.generated"
        }
        if actual_sources != expected_sources:
            raise SecureArtifactError(
                "artifact source identity changed between validation snapshots"
            )
        final_snapshot.revalidate_sources()
        final_snapshot.revalidate_retained_files()
        result = final_snapshot
        final_snapshot = None
        return result
    except BaseException:
        if final_snapshot is not None:
            final_snapshot.close()
        if validation_snapshot is not None:
            validation_snapshot.close()
        raise
    finally:
        tree.close()


def prepare_verified_factory_probe_snapshot(
    build_dir: os.PathLike[str] | str,
    partition_source: os.PathLike[str] | str,
    sdkconfig: os.PathLike[str] | str,
    *,
    private_parent: os.PathLike[str] | str,
    materialize_missing_aliases: bool,
) -> VerifiedBadgeArtifactSnapshot:
    return prepare_verified_role_snapshot(
        FACTORY_PROBE_ROLE,
        build_dir,
        partition_source,
        sdkconfig,
        private_parent=private_parent,
        materialize_missing_aliases=materialize_missing_aliases,
    )


def freeze_verified_badge_artifact_snapshots(
    *,
    scanner: VerifiedBadgeArtifactSnapshot,
    uplink: VerifiedBadgeArtifactSnapshot,
    probe: VerifiedBadgeArtifactSnapshot,
) -> FrozenBadgeArtifactSnapshots:
    snapshots = (scanner, uplink, probe)
    if (
        any(
            type(snapshot) is not VerifiedBadgeArtifactSnapshot
            for snapshot in snapshots
        )
        or len({id(snapshot) for snapshot in snapshots}) != 3
    ):
        raise SecureArtifactError(
            "three distinct live badge snapshots are required"
        )
    try:
        for snapshot in snapshots:
            snapshot.revalidate_sources()
            snapshot.revalidate_retained_files()
        return FrozenBadgeArtifactSnapshots(
            scanner=scanner.freeze_for_mutation(),
            uplink=uplink.freeze_for_mutation(),
            probe=probe.freeze_for_mutation(),
        )
    except BaseException:
        cleanup_errors: list[str] = []
        for snapshot in snapshots:
            try:
                snapshot.close()
            except BaseException as exc:
                cleanup_errors.append(str(exc))
        if cleanup_errors:
            raise SecureArtifactError(
                "badge snapshot cleanup failed: "
                + "; ".join(cleanup_errors)
            )
        raise


__all__ = (
    "FACTORY_PROBE_ROLE",
    "FrozenBadgeArtifactSnapshots",
    "PrivateGameAcceptance",
    "PrivateGameArtifactAcceptance",
    "SCANNER_ROLE",
    "UPLINK_ROLE",
    "freeze_verified_badge_artifact_snapshots",
    "load_private_game_acceptance",
    "materialize_role_aliases",
    "prepare_verified_factory_probe_snapshot",
    "prepare_verified_role_snapshot",
    "verify_descriptor_rooted_frozen_role_authority",
)
