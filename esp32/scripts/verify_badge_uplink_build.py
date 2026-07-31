#!/usr/bin/env python3
"""Materialize and strictly verify badge-uplink ESP-IDF flash manifests."""

from __future__ import annotations

import argparse
from array import array
from bisect import bisect_right
from collections import namedtuple
import hashlib
import hmac
import json
import os
import re
import shlex
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

from firmware_version import (
    expected_identity_for_env,
    parse_firmware_identity,
    runtime_target_for_env,
)
from secure_artifact_tree import (
    FrozenArtifactSet,
    SecureArtifactError,
    VerifiedBadgeArtifactSnapshot,
)
from verified_badge_artifacts import (
    UPLINK_ROLE,
    materialize_role_aliases,
    prepare_verified_role_snapshot,
    verify_descriptor_rooted_frozen_role_authority,
)


APP_OFFSET = 0x20000
EXPECTED_FULL = {
    0x00000: "bootloader/bootloader.bin",
    0x08000: "partition_table/partition-table.bin",
    0x0F000: "ota_data_initial.bin",
    APP_OFFSET: "fof_badge_uplink.bin",
}
EXPECTED_APP = {APP_OFFSET: "fof_badge_uplink.bin"}
TEXT_MANIFESTS = {
    "flash_args": EXPECTED_FULL,
    "flash_app_args": EXPECTED_APP,
    "flash_project_args": EXPECTED_FULL,
}
ALL_MANIFESTS = (*TEXT_MANIFESTS, "flasher_args.json")
ALIASES = {
    "bootloader/bootloader.bin": "bootloader.bin",
    "partition_table/partition-table.bin": "partitions.bin",
    "fof_badge_uplink.bin": "firmware.bin",
}
CANARY_CONTROLLER_ONLY_SDKCONFIG = {
    "CONFIG_BT_ENABLED": "y",
    "CONFIG_BT_CONTROLLER_ONLY": "y",
    "CONFIG_BT_CONTROLLER_ENABLED": "y",
    "CONFIG_BT_CTRL_RUN_IN_FLASH_ONLY": "y",
    "CONFIG_BT_CTRL_DTM_ENABLE": "n",
    "CONFIG_BT_CTRL_HCI_MODE_VHCI": "y",
    "CONFIG_BT_CTRL_BLE_MAX_ACT": "1",
    "CONFIG_BT_CTRL_BLE_ADV": "y",
    "CONFIG_BT_CTRL_BLE_SCAN": "n",
    "CONFIG_BT_CTRL_BLE_MASTER": "n",
    "CONFIG_BT_CTRL_BLE_SECURITY_ENABLE": "n",
    "CONFIG_BT_BLUEDROID_ENABLED": "n",
    "CONFIG_BT_NIMBLE_ENABLED": "n",
    "CONFIG_SPIRAM": "y",
    "CONFIG_SPIRAM_USE_CAPS_ALLOC": "y",
    "CONFIG_SPIRAM_USE_MALLOC": "n",
}
UPLINK_FROZEN_COMMON_SDKCONFIG = {
    "CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE": "y",
    "CONFIG_ESPTOOLPY_FLASHMODE_DIO": "y",
    "CONFIG_ESPTOOLPY_FLASHFREQ_80M": "y",
    "CONFIG_ESPTOOLPY_FLASHSIZE_8MB": "y",
    "CONFIG_PARTITION_TABLE_CUSTOM": "y",
    "CONFIG_PARTITION_TABLE_CUSTOM_FILENAME": (
        '"partitions_s3_fof_badge_8mb.csv"'
    ),
}
UPLINK_PRODUCTION_ENV = "uplink-s3-fof_badge"
UPLINK_CANARY_ENV = "uplink-s3-fof_badge-con-crud-canary"
UPLINK_CANARY_MAP_FILENAME = "fof_badge_uplink.map"
UPLINK_CANARY_MAX_INTERNAL_RAM_BYTES = 212_992
UPLINK_CANARY_MAX_APP_BYTES = 1_468_464
UPLINK_PRODUCTION_RTC_NOINIT_BYTES = 0x14
UPLINK_CANARY_RTC_NOINIT_BYTES = 0xC8
UPLINK_RTC_NOINIT_ADDRESS = 0x50000000
UPLINK_RTC_SLOW_MEMORY_END = 0x50002000
UPLINK_RTC_STATE_SYMBOL = "g_fof_badge_rtc_state"
UPLINK_RTC_ALIAS_OFFSETS = {
    "fof_badge_rtc_usb_recovery_once_magic": 0,
    "fof_badge_rtc_expected_reboot_generation": 4,
    "fof_badge_rtc_expected_reboot_magic": 8,
}
UPLINK_CANARY_REQUIRED_SOURCE_FILES = (
    "badge_con_vhci.c",
)
UPLINK_CANARY_REQUIRED_SYMBOLS = (
    "badge_con_radio_runtime_poll",
    "badge_con_vhci_init",
    "esp_vhci_host_send_packet",
    "esp_vhci_host_register_callback",
)
UPLINK_CANARY_FORBIDDEN_SOURCE_FILES = (
    "esp_nimble_hci.c",
)
UPLINK_CANARY_FORBIDDEN_SYMBOLS = (
    "esp_nimble_hci_init",
    "nimble_port_init",
    "ble_gap_ext_adv_start",
    "ble_gap_ext_disc",
)
PRODUCTION_FORBIDDEN_RUNTIME_TOKENS = (
    b"FOF_DC34_GAME_CANARY",
    b"FoF-DC34-CONCRUD",
    b"CON CRUD",
    b'"update_campaign"',
    b'"last_expected_reboot_generation"',
    b'"game_seed"',
    b'"game_state"',
    b'"game_active"',
    b'"game_shield"',
    b'"game_max"',
    b'"game_scar"',
    b'"game_cured"',
    b'"game_dead"',
    b'"game_super"',
    b"badge_con_",
    b"update_maintenance",
)
PRODUCTION_SDKCONFIG_DEFAULTS = {
    "uplink-s3-fof_badge": "sdkconfig.esp32s3-fof_badge.defaults",
    "scanner-s3-combo-fof_badge": (
        "sdkconfig.scanner-s3-fof_badge.defaults"
    ),
}
PRODUCTION_ELF_FORBIDDEN_SYMBOL_FRAGMENTS = (
    "badge_con_",
    "badge_update_maintenance_",
    "update_campaign",
    "update_maintenance",
    "game_seed",
    "game_state",
    "last_expected_reboot_generation",
)
PRODUCTION_ELF_ALLOWED_SYMBOLS = {
    "s_last_expected_reboot_generation",
}
VERSION_HEADER = Path(__file__).resolve().parents[1] / "shared" / "version.h"
_DRAM_SECTION_PATTERN = re.compile(
    r"^(\.dram0\.(?:data|bss))\s+"
    r"0x[0-9A-Fa-f]+\s+0x([0-9A-Fa-f]+)\s*$",
    re.MULTILINE,
)
PARTITION_GENERATOR_ENV = "ESP_IDF_PARTITION_GENERATOR"
PARTITION_GENERATOR_RELATIVE = Path(
    "components/partition_table/gen_esp32part.py"
)
_ELF32_SECTION = namedtuple(
    "_ELF32_SECTION",
    (
        "index name section_type flags address file_offset size link info "
        "alignment entry_size"
    ),
)
_ELF32_SYMBOL = namedtuple(
    "_ELF32_SYMBOL",
    (
        "name value size binding symbol_type visibility section_index"
    ),
)
_ELF32_PROGRAM_HEADER = namedtuple(
    "_ELF32_PROGRAM_HEADER",
    (
        "index file_offset virtual_address physical_address "
        "file_size memory_size flags alignment"
    ),
)
_APP_IMAGE_SEGMENT = namedtuple(
    "_APP_IMAGE_SEGMENT",
    "index address data file_data_offset",
)
ESP32_S3_SEGMENT_RANGES = (
    (0x3C000000, 0x3E000000),
    (0x3FC88000, 0x3FD00000),
    (0x40370000, 0x403E0000),
    (0x42000000, 0x44000000),
    (0x50000000, 0x50002000),
    (0x600FE000, 0x60100000),
)
ESP32_S3_MAPPED_SEGMENT_RANGES = (
    (0x3C000000, 0x3E000000),
    (0x42000000, 0x44000000),
)
ESP32_S3_EXECUTABLE_SEGMENT_RANGES = (
    (0x40370000, 0x403E0000),
    (0x42000000, 0x44000000),
    (0x600FE000, 0x60100000),
)
UPLINK_ELF_SHA256_IMAGE_OFFSET = 0xB0
UPLINK_MAX_FROZEN_ELF_BYTES = 16 * 1024 * 1024
UPLINK_MAX_FROZEN_APP_BYTES = 8 * 1024 * 1024
UPLINK_MAX_ELF_PROGRAM_HEADERS = 16
UPLINK_MAX_ELF_SECTIONS = 128
UPLINK_MAX_ELF_SYMBOLS = 16_384
# Bound host work and retained names independently of the ELF file-size cap.
UPLINK_MAX_ELF_STRING_TABLE_BYTES = 1024 * 1024
UPLINK_MAX_ELF_REFERENCED_STRING_TABLE_BYTES = 2 * 1024 * 1024
UPLINK_MAX_ELF_SYMBOL_NAME_BYTES = 4 * 1024
UPLINK_MAX_ELF_UNIQUE_SYMBOL_NAME_BYTES = 512 * 1024
UPLINK_MAX_ELF_SYMBOL_NAME_REFERENCE_BYTES = 4 * 1024 * 1024


class _DuplicateJsonKeyError(ValueError):
    pass


def _json_object_without_duplicates(pairs: list[tuple[str, object]]) -> dict:
    result = {}
    for key, value in pairs:
        if key in result:
            raise _DuplicateJsonKeyError(f"duplicate JSON key {key!r}")
        result[key] = value
    return result


def materialize_badge_uplink_aliases(build_dir: Path) -> None:
    """Create regular-file aliases expected by ESP-IDF's generated manifests."""
    materialize_role_aliases(build_dir, ALIASES)


def _parse_sdkconfig(
    sdkconfig: Path,
) -> tuple[dict[str, str], list[str]]:
    sdkconfig = Path(sdkconfig)
    if sdkconfig.is_symlink() or not sdkconfig.is_file():
        return {}, [
            "sdkconfig: cannot read regular non-symlink file: "
            f"{sdkconfig}"
        ]
    try:
        lines = sdkconfig.read_text(encoding="utf-8").splitlines()
    except (OSError, UnicodeError) as exc:
        return {}, [f"sdkconfig: cannot read: {exc}"]

    values: dict[str, str] = {}
    for line in lines:
        stripped = line.strip()
        if stripped.startswith("CONFIG_") and "=" in stripped:
            key, value = stripped.split("=", 1)
            values[key] = value
        elif stripped.startswith("# CONFIG_") and stripped.endswith(
            " is not set"
        ):
            key = stripped[2:-len(" is not set")]
            values[key] = "n"
    return values, []


def _parse_frozen_sdkconfig(
    payload: bytes,
    *,
    label: str,
) -> tuple[dict[str, str], list[str]]:
    if type(payload) is not bytes or not payload:
        return {}, [f"{label}: frozen sdkconfig is empty or malformed"]
    try:
        lines = payload.decode("utf-8", errors="strict").splitlines()
    except UnicodeDecodeError:
        return {}, [f"{label}: frozen sdkconfig is not strict UTF-8"]
    values: dict[str, str] = {}
    for line in lines:
        stripped = line.strip()
        key: str | None = None
        value: str | None = None
        if stripped.startswith("CONFIG_") and "=" in stripped:
            key, value = stripped.split("=", 1)
        elif (
            stripped.startswith("# CONFIG_") and
            stripped.endswith(" is not set")
        ):
            key = stripped[2:-len(" is not set")]
            value = "n"
        if key is None or value is None:
            continue
        if key in values:
            return {}, [
                f"{label}: frozen sdkconfig contains duplicate {key}"
            ]
        values[key] = value
    return values, []


def _verify_expected_sdkconfig(
    sdkconfig: Path,
    expected_values: dict[str, str],
) -> list[str]:
    values, errors = _parse_sdkconfig(sdkconfig)
    if errors:
        return errors

    for key, expected in expected_values.items():
        actual = values.get(key)
        if actual == expected:
            continue
        requirement = (
            "enabled" if expected == "y"
            else "disabled" if expected == "n"
            else f"exactly {expected}"
        )
        errors.append(f"sdkconfig: {key} must be {requirement}; got {actual!r}")
    return errors


def verify_badge_uplink_canary_sdkconfig(sdkconfig: Path) -> list[str]:
    return _verify_expected_sdkconfig(
        sdkconfig, CANARY_CONTROLLER_ONLY_SDKCONFIG
    )


def _verify_canary_linker_map(
    linker_map: Path,
    *,
    max_internal_ram_bytes: int,
) -> list[str]:
    linker_map = Path(linker_map)
    if linker_map.is_symlink() or not linker_map.is_file():
        return [
            "linker map: required regular non-symlink file missing: "
            f"{linker_map}"
        ]
    try:
        contents = linker_map.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as exc:
        return [f"linker map: cannot read: {exc}"]

    errors: list[str] = []
    section_values: dict[str, list[int]] = {
        ".dram0.data": [],
        ".dram0.bss": [],
    }
    for section, raw_size in _DRAM_SECTION_PATTERN.findall(contents):
        section_values[section].append(int(raw_size, 16))
    for section, values in section_values.items():
        if len(values) != 1:
            errors.append(
                "linker map: expected exactly one top-level "
                f"{section} section; got {len(values)}"
            )
        elif values[0] <= 0:
            errors.append(
                f"linker map: {section} must have a positive size"
            )
    if all(len(values) == 1 for values in section_values.values()):
        internal_ram_bytes = sum(
            values[0] for values in section_values.values()
        )
        if internal_ram_bytes > max_internal_ram_bytes:
            errors.append(
                "linker map: internal RAM "
                f"{internal_ram_bytes} exceeds budget "
                f"{max_internal_ram_bytes} bytes"
            )
    return errors


def _read_regular_artifact(
    path: Path,
    label: str,
) -> tuple[bytes | None, list[str]]:
    path = Path(path)
    if path.is_symlink() or not path.is_file():
        return None, [
            f"{label}: must be a regular non-symlink file: {path}"
        ]
    try:
        payload = path.read_bytes()
    except OSError as exc:
        return None, [f"{label}: cannot read: {exc}"]
    if not payload:
        return None, [f"{label}: must not be empty: {path}"]
    return payload, []


def _read_elf32_evidence(
    source: Path | bytes,
    label: str,
) -> tuple[list[_ELF32_SECTION], list[_ELF32_SYMBOL], set[str], list[str]]:
    if type(source) is bytes:
        payload = source
        errors: list[str] = []
        if not payload:
            return [], [], set(), [f"{label}: must not be empty"]
    else:
        payload, errors = _read_regular_artifact(Path(source), label)
        if payload is None:
            return [], [], set(), errors
    if (
        len(payload) < 52 or
        payload[:4] != b"\x7fELF" or
        payload[4] != 1 or
        payload[5] != 1 or
        payload[6] != 1
    ):
        return [], [], set(), [
            f"{label}: must be a 32-bit little-endian ELF file"
        ]
    elf_type, machine, version = struct.unpack_from("<HHI", payload, 16)
    if elf_type != 2 or machine != 94 or version != 1:
        return [], [], set(), [
            f"{label}: must be an ESP32-S3 Xtensa executable ELF"
        ]

    section_offset = struct.unpack_from("<I", payload, 32)[0]
    elf_header_size = struct.unpack_from("<H", payload, 40)[0]
    section_entry_size = struct.unpack_from("<H", payload, 46)[0]
    section_count = struct.unpack_from("<H", payload, 48)[0]
    section_name_index = struct.unpack_from("<H", payload, 50)[0]
    if section_count > UPLINK_MAX_ELF_SECTIONS:
        return [], [], set(), [
            f"{label}: ELF section count exceeds "
            f"{UPLINK_MAX_ELF_SECTIONS}"
        ]
    if (
        elf_header_size != 52 or
        section_entry_size != 40 or
        section_count == 0 or
        section_offset > len(payload) or
        section_count > (
            (len(payload) - section_offset) // section_entry_size
        )
    ):
        return [], [], set(), [
            f"{label}: malformed ELF section table"
        ]
    if section_name_index >= section_count:
        return [], [], set(), [
            f"{label}: malformed ELF section-name table index"
        ]

    raw_sections = [
        struct.unpack_from(
            "<IIIIIIIIII",
            payload,
            section_offset + index * section_entry_size,
        )
        for index in range(section_count)
    ]
    section_name_table = raw_sections[section_name_index]
    if section_name_table[1] != 3:
        return [], [], set(), [
            f"{label}: ELF section-name table must be SHT_STRTAB"
        ]
    section_names_offset = section_name_table[4]
    section_names_size = section_name_table[5]
    if (
        section_names_offset > len(payload) or
        section_names_size > len(payload) - section_names_offset or
        section_names_size > UPLINK_MAX_ELF_STRING_TABLE_BYTES
    ):
        return [], [], set(), [
            f"{label}: malformed ELF section-name string table bounds"
        ]
    section_name_strings = payload[
        section_names_offset:section_names_offset + section_names_size
    ]

    sections: list[_ELF32_SECTION] = []
    for index, raw_section in enumerate(raw_sections):
        name_offset = raw_section[0]
        name = ""
        if name_offset >= len(section_name_strings):
            errors.append(
                f"{label}: ELF section name offset is out of range"
            )
        else:
            terminator = section_name_strings.find(b"\x00", name_offset)
            if terminator < 0:
                errors.append(
                    f"{label}: ELF section name is not terminated"
                )
            else:
                try:
                    name = section_name_strings[
                        name_offset:terminator
                    ].decode("ascii")
                except UnicodeDecodeError:
                    errors.append(
                        f"{label}: ELF section name is not ASCII"
                    )
        section_type = raw_section[1]
        file_offset = raw_section[4]
        section_size = raw_section[5]
        section_address = raw_section[3]
        section_alignment = raw_section[8]
        if (
            section_address + section_size > 0x1_0000_0000 or
            (
                section_alignment != 0 and
                section_alignment & (section_alignment - 1)
            )
        ):
            errors.append(
                f"{label}: ELF section {index} has malformed address/alignment"
            )
        if (
            section_type != 8 and
            section_size > 0 and
            (
                file_offset > len(payload) or
                section_size > len(payload) - file_offset
            )
        ):
            errors.append(
                f"{label}: ELF section {index} has malformed file bounds"
            )
        sections.append(_ELF32_SECTION(
            index,
            name,
            section_type,
            raw_section[2],
            section_address,
            file_offset,
            section_size,
            raw_section[6],
            raw_section[7],
            section_alignment,
            raw_section[9],
        ))

    symbols: list[_ELF32_SYMBOL] = []
    source_files: set[str] = set()
    symbol_table_seen = False
    total_symbol_entries = 0
    total_unique_symbol_name_bytes = 0
    total_symbol_name_reference_bytes = 0
    total_referenced_string_table_bytes = section_names_size
    string_table_cache = {
        section_name_index: section_name_strings,
    }
    symbol_name_distance_cache: dict[int, array] = {}
    symbol_name_cache: dict[tuple[int, int], str] = {}
    symbol_resource_limit_hit = False
    for section in sections:
        if section.section_type != 2:
            continue
        symbol_table_seen = True
        symbol_offset = section.file_offset
        symbol_size = section.size
        string_section_index = section.link
        symbol_entry_size = section.entry_size
        if (
            symbol_entry_size != 16 or
            symbol_size % symbol_entry_size != 0 or
            symbol_offset > len(payload) or
            symbol_size > len(payload) - symbol_offset or
            string_section_index >= len(sections)
        ):
            errors.append(f"{label}: malformed ELF symbol table")
            continue
        string_section = sections[string_section_index]
        if string_section.section_type != 3:
            errors.append(
                f"{label}: ELF symbol table does not link a string table"
            )
            continue
        string_offset = string_section.file_offset
        string_size = string_section.size
        if (
            string_offset > len(payload) or
            string_size > len(payload) - string_offset or
            string_size > UPLINK_MAX_ELF_STRING_TABLE_BYTES
        ):
            errors.append(f"{label}: malformed ELF string table")
            continue
        symbol_entries = symbol_size // symbol_entry_size
        total_symbol_entries += symbol_entries
        if total_symbol_entries > UPLINK_MAX_ELF_SYMBOLS:
            errors.append(
                f"{label}: ELF symbol count exceeds "
                f"{UPLINK_MAX_ELF_SYMBOLS}"
            )
            break
        if string_section_index not in string_table_cache:
            total_referenced_string_table_bytes += string_size
            if (
                total_referenced_string_table_bytes >
                UPLINK_MAX_ELF_REFERENCED_STRING_TABLE_BYTES
            ):
                errors.append(
                    f"{label}: referenced ELF string-table bytes exceed "
                    f"{UPLINK_MAX_ELF_REFERENCED_STRING_TABLE_BYTES}"
                )
                break
            string_table_cache[string_section_index] = payload[
                string_offset:string_offset + string_size
            ]
        strings = string_table_cache[string_section_index]
        if (
            not strings or
            strings[0] != 0 or
            strings[-1] != 0
        ):
            errors.append(
                f"{label}: ELF string table must start and end with NUL"
            )
            continue
        if string_section_index not in symbol_name_distance_cache:
            capped_distance = UPLINK_MAX_ELF_SYMBOL_NAME_BYTES + 1
            distances = array("H", [capped_distance]) * len(strings)
            distance = capped_distance
            for position in range(len(strings) - 1, -1, -1):
                if strings[position] == 0:
                    distance = 0
                elif distance < capped_distance:
                    distance += 1
                distances[position] = distance
            symbol_name_distance_cache[string_section_index] = distances
        name_distances = symbol_name_distance_cache[string_section_index]
        raw_symbols = [
            struct.unpack_from("<IIIBBH", payload, entry_offset)
            for entry_offset in range(
                symbol_offset,
                symbol_offset + symbol_size,
                symbol_entry_size,
            )
        ]
        requested_name_offsets = {
            raw_symbol[0]
            for raw_symbol in raw_symbols
            if raw_symbol[0] != 0
        }
        if any(
            name_offset >= len(strings)
            for name_offset in requested_name_offsets
        ):
            errors.append(
                f"{label}: ELF symbol name offset is out of range"
            )
            continue

        unresolved_name_offsets = {
            name_offset
            for name_offset in requested_name_offsets
            if (string_section_index, name_offset) not in symbol_name_cache
        }
        name_terminators: dict[int, int] = {}
        new_name_bytes = 0
        for name_offset in unresolved_name_offsets:
            name_size = name_distances[name_offset]
            if name_size > UPLINK_MAX_ELF_SYMBOL_NAME_BYTES:
                errors.append(
                    f"{label}: ELF symbol name exceeds "
                    f"{UPLINK_MAX_ELF_SYMBOL_NAME_BYTES} bytes"
                )
                symbol_resource_limit_hit = True
                break
            name_terminators[name_offset] = name_offset + name_size
            new_name_bytes += name_size
        if symbol_resource_limit_hit:
            break
        if (
            total_unique_symbol_name_bytes + new_name_bytes >
            UPLINK_MAX_ELF_UNIQUE_SYMBOL_NAME_BYTES
        ):
            errors.append(
                f"{label}: unique decoded ELF symbol-name bytes exceed "
                f"{UPLINK_MAX_ELF_UNIQUE_SYMBOL_NAME_BYTES}"
            )
            break

        decoded_names: dict[tuple[int, int], str] = {}
        for name_offset, terminator in name_terminators.items():
            try:
                decoded_names[
                    (string_section_index, name_offset)
                ] = strings[name_offset:terminator].decode("ascii")
            except UnicodeDecodeError:
                errors.append(
                    f"{label}: ELF symbol name is not ASCII"
                )
                symbol_resource_limit_hit = True
                break
        if symbol_resource_limit_hit:
            break
        symbol_name_cache.update(decoded_names)
        total_unique_symbol_name_bytes += new_name_bytes

        for (
            name_offset,
            value,
            size,
            info,
            other,
            section_index,
        ) in raw_symbols:
            name = (
                ""
                if name_offset == 0
                else symbol_name_cache[(string_section_index, name_offset)]
            )
            total_symbol_name_reference_bytes += len(name)
            if (
                total_symbol_name_reference_bytes >
                UPLINK_MAX_ELF_SYMBOL_NAME_REFERENCE_BYTES
            ):
                errors.append(
                    f"{label}: ELF symbol-name reference bytes exceed "
                    f"{UPLINK_MAX_ELF_SYMBOL_NAME_REFERENCE_BYTES}"
                )
                symbol_resource_limit_hit = True
                break
            if (
                name_offset == 0 and
                value == 0 and
                size == 0 and
                info == 0 and
                other == 0 and
                section_index == 0
            ):
                continue
            if (
                section_index >= len(sections) and
                section_index < 0xFF00
            ):
                errors.append(
                    f"{label}: ELF symbol section index is out of range"
                )
            symbol_type = info & 0x0F
            symbols.append(_ELF32_SYMBOL(
                name,
                value,
                size,
                info >> 4,
                symbol_type,
                other & 0x03,
                section_index,
            ))
            if symbol_type == 4 and section_index != 0:
                source_files.add(name)
        if symbol_resource_limit_hit:
            break
    if not symbol_table_seen:
        errors.append(f"{label}: required ELF symbol table is missing")
    return sections, symbols, source_files, errors


def _read_elf32_symbol_evidence(
    path: Path,
    label: str,
) -> tuple[set[str], set[str], list[str]]:
    _sections, symbols, source_files, errors = _read_elf32_evidence(
        path, label
    )
    defined_symbols = {
        symbol.name
        for symbol in symbols
        if (
            symbol.name and
            symbol.section_index != 0 and
            symbol.symbol_type != 4
        )
    }
    return defined_symbols, source_files, errors


def _verify_uplink_rtc_elf_payload(
    elf_payload: bytes,
    *,
    label: str,
    expected_size: int,
) -> list[str]:
    sections, symbols, _source_files, errors = _read_elf32_evidence(
        elf_payload, label
    )
    if errors:
        return errors

    rtc_sections = [
        section for section in sections
        if section.name == ".rtc_noinit"
    ]
    if len(rtc_sections) != 1:
        return [
            f"{label}: RTC ABI requires exactly one .rtc_noinit section; "
            f"got {len(rtc_sections)}"
        ]
    rtc_section = rtc_sections[0]

    def intersects_rtc_slow_memory(address: int, size: int) -> bool:
        if (
            type(address) is not int or
            type(size) is not int or
            address < 0 or
            size < 0 or
            address > 0xFFFF_FFFF or
            size > 0x1_0000_0000
        ):
            return False
        if size == 0:
            return (
                UPLINK_RTC_NOINIT_ADDRESS <= address <
                UPLINK_RTC_SLOW_MEMORY_END
            )
        end = address + size
        if end > 0x1_0000_0000:
            errors.append(
                f"{label}: RTC-addressed ELF range overflows 32 bits"
            )
            return True
        return (
            address < UPLINK_RTC_SLOW_MEMORY_END and
            end > UPLINK_RTC_NOINIT_ADDRESS
        )

    retained_rtc_sections = [
        section for section in sections
        if (
            section.section_type == 8 and
            section.flags & 0x3 == 0x3 and
            intersects_rtc_slow_memory(section.address, section.size)
        )
    ]
    for section in retained_rtc_sections:
        if section.index == rtc_section.index:
            continue
        rendered_name = section.name or "<anonymous>"
        errors.append(
            f"{label}: extra retained RTC slow-memory section "
            f"{rendered_name} at {section.address:#010x} is forbidden"
        )
    for section in sections:
        if (
            section.index == rtc_section.index or
            not intersects_rtc_slow_memory(
                section.address, section.size
            ) or
            section.flags & 0x2 == 0
        ):
            continue
        if section.section_type == 8:
            continue
        if section.section_type != 1 or section.size <= 0:
            rendered_name = section.name or "<anonymous>"
            errors.append(
                f"{label}: initialized RTC slow-memory section "
                f"{rendered_name} must be allocatable SHT_PROGBITS"
            )

    if rtc_section.section_type != 8:
        errors.append(
            f"{label}: RTC .rtc_noinit must be SHT_NOBITS"
        )
    if rtc_section.flags & 0x1 == 0:
        errors.append(
            f"{label}: RTC .rtc_noinit must be writable"
        )
    if rtc_section.flags & 0x2 == 0:
        errors.append(
            f"{label}: RTC .rtc_noinit must be allocatable"
        )
    if rtc_section.flags != 0x3:
        errors.append(
            f"{label}: RTC .rtc_noinit flags must be exactly "
            "SHF_WRITE|SHF_ALLOC"
        )
    if rtc_section.alignment != 4:
        errors.append(
            f"{label}: RTC .rtc_noinit alignment must be exactly 4 bytes"
        )
    if rtc_section.address != UPLINK_RTC_NOINIT_ADDRESS:
        errors.append(
            f"{label}: RTC .rtc_noinit base address must be exactly "
            f"{UPLINK_RTC_NOINIT_ADDRESS:#010x}"
        )
    if rtc_section.size != expected_size:
        errors.append(
            f"{label}: RTC .rtc_noinit must be exactly "
            f"{expected_size} bytes"
        )
    if (
        rtc_section.link != 0 or
        rtc_section.info != 0 or
        rtc_section.entry_size != 0
    ):
        errors.append(
            f"{label}: RTC .rtc_noinit link/info/entry-size metadata "
            "must be zero"
        )

    state_symbols = [
        symbol for symbol in symbols
        if symbol.name == UPLINK_RTC_STATE_SYMBOL
    ]
    if len(state_symbols) != 1:
        errors.append(
            f"{label}: RTC {UPLINK_RTC_STATE_SYMBOL} must have exactly "
            f"one symbol-table entry; got {len(state_symbols)}"
        )
    else:
        state_symbol = state_symbols[0]
        if (
            state_symbol.binding != 1 or
            state_symbol.symbol_type != 1
        ):
            errors.append(
                f"{label}: RTC {UPLINK_RTC_STATE_SYMBOL} must be "
                "GLOBAL OBJECT"
            )
        if state_symbol.visibility != 0:
            errors.append(
                f"{label}: RTC {UPLINK_RTC_STATE_SYMBOL} must have "
                "default visibility"
            )
        if state_symbol.section_index != rtc_section.index:
            errors.append(
                f"{label}: RTC {UPLINK_RTC_STATE_SYMBOL} must be in the "
                "same section as .rtc_noinit"
            )
        if state_symbol.value != rtc_section.address:
            errors.append(
                f"{label}: RTC {UPLINK_RTC_STATE_SYMBOL} must start at "
                "the .rtc_noinit section base"
            )
        if state_symbol.size != expected_size:
            errors.append(
                f"{label}: RTC {UPLINK_RTC_STATE_SYMBOL} must span "
                f"exactly {expected_size} bytes"
            )

    rtc_object_symbols = [
        symbol for symbol in symbols
        if (
            symbol.section_index == rtc_section.index and
            symbol.symbol_type == 1
        )
    ]
    if (
        len(rtc_object_symbols) != 1 or
        rtc_object_symbols[0].name != UPLINK_RTC_STATE_SYMBOL
    ):
        errors.append(
            f"{label}: RTC .rtc_noinit must contain exactly one OBJECT, "
            f"{UPLINK_RTC_STATE_SYMBOL}"
        )

    for symbol in symbols:
        if (
            symbol.section_index == 0 or
            symbol.name == UPLINK_RTC_STATE_SYMBOL or
            (
                symbol.symbol_type != 1 and
                symbol.size == 0
            ) or
            not intersects_rtc_slow_memory(symbol.value, symbol.size)
        ):
            continue
        declared_section = (
            sections[symbol.section_index]
            if 0 <= symbol.section_index < len(sections)
            else None
        )
        if declared_section is not None:
            declared_end = (
                declared_section.address + declared_section.size
            )
            symbol_end = symbol.value + symbol.size
            coherently_initialized = (
                declared_section.section_type == 1 and
                declared_section.flags & 0x2 != 0 and
                declared_section.address <= symbol.value and
                symbol_end <= declared_end
            )
            if coherently_initialized:
                continue
        rendered_name = symbol.name or "<anonymous>"
        errors.append(
            f"{label}: extra retained RTC slow-memory OBJECT/side block "
            f"{rendered_name} at {symbol.value:#010x} is forbidden"
        )

    for alias_name, expected_offset in UPLINK_RTC_ALIAS_OFFSETS.items():
        alias_symbols = [
            symbol for symbol in symbols
            if symbol.name == alias_name
        ]
        if len(alias_symbols) != 1:
            errors.append(
                f"{label}: RTC {alias_name} must have exactly one "
                f"symbol-table entry; got {len(alias_symbols)}"
            )
            continue
        alias_symbol = alias_symbols[0]
        if (
            alias_symbol.binding != 1 or
            alias_symbol.symbol_type != 0
        ):
            errors.append(
                f"{label}: RTC {alias_name} must be GLOBAL NOTYPE"
            )
        if alias_symbol.visibility != 0:
            errors.append(
                f"{label}: RTC {alias_name} must have default visibility"
            )
        if alias_symbol.section_index != rtc_section.index:
            errors.append(
                f"{label}: RTC {alias_name} must be in the same section "
                "as .rtc_noinit"
            )
        if alias_symbol.value != rtc_section.address + expected_offset:
            errors.append(
                f"{label}: RTC {alias_name} must be at .rtc_noinit "
                f"offset +{expected_offset}"
            )
        if alias_symbol.size != 0:
            errors.append(
                f"{label}: RTC {alias_name} must have zero size"
            )
    return errors


def _verify_uplink_rtc_final_elf(
    elf_path: Path,
    *,
    label: str,
    expected_size: int,
) -> list[str]:
    payload, errors = _read_regular_artifact(Path(elf_path), label)
    if payload is None:
        return errors
    return _verify_uplink_rtc_elf_payload(
        payload,
        label=label,
        expected_size=expected_size,
    )


def _read_elf32_load_evidence(
    payload: bytes,
    label: str,
) -> tuple[int, list[_ELF32_PROGRAM_HEADER], list[str]]:
    errors: list[str] = []
    if type(payload) is not bytes or len(payload) < 52:
        return 0, [], [f"{label}: ELF program header is unavailable"]
    entrypoint = struct.unpack_from("<I", payload, 24)[0]
    program_offset = struct.unpack_from("<I", payload, 28)[0]
    program_entry_size = struct.unpack_from("<H", payload, 42)[0]
    program_count = struct.unpack_from("<H", payload, 44)[0]
    if program_count > UPLINK_MAX_ELF_PROGRAM_HEADERS:
        return entrypoint, [], [
            f"{label}: ELF program-header count exceeds "
            f"{UPLINK_MAX_ELF_PROGRAM_HEADERS}"
        ]
    if (
        program_entry_size != 32 or
        program_count == 0 or
        program_offset > len(payload) or
        program_count > (
            (len(payload) - program_offset) // program_entry_size
        )
    ):
        return entrypoint, [], [
            f"{label}: malformed or missing ELF program-header table"
        ]

    loads: list[_ELF32_PROGRAM_HEADER] = []
    for index in range(program_count):
        raw = struct.unpack_from(
            "<IIIIIIII",
            payload,
            program_offset + index * program_entry_size,
        )
        if raw[0] != 1:
            continue
        (
            _program_type,
            file_offset,
            virtual_address,
            physical_address,
            file_size,
            memory_size,
            flags,
            alignment,
        ) = raw
        if (
            memory_size == 0 or
            file_size > memory_size or
            file_offset > len(payload) or
            file_size > len(payload) - file_offset or
            virtual_address + memory_size > 0x1_0000_0000 or
            physical_address != virtual_address or
            alignment == 0 or
            alignment & (alignment - 1) or
            flags & ~0x7
        ):
            errors.append(
                f"{label}: malformed ELF PT_LOAD program header {index}"
            )
            continue
        if file_offset % alignment != virtual_address % alignment:
            errors.append(
                f"{label}: ELF PT_LOAD {index} p_offset/p_vaddr alignment "
                "is incongruent"
            )
            continue
        if not any(
            start <= virtual_address and
            virtual_address + memory_size <= end
            for start, end in ESP32_S3_SEGMENT_RANGES
        ):
            errors.append(
                f"{label}: ELF PT_LOAD {index} is outside valid "
                "ESP32-S3 memory"
            )
            continue
        loads.append(_ELF32_PROGRAM_HEADER(
            index,
            file_offset,
            virtual_address,
            physical_address,
            file_size,
            memory_size,
            flags,
            alignment,
        ))
    if not loads:
        errors.append(f"{label}: ELF has no PT_LOAD program headers")
    file_backed = [load for load in loads if load.file_size > 0]
    by_file = sorted(file_backed, key=lambda item: item.file_offset)
    for previous, current in zip(by_file, by_file[1:]):
        if current.file_offset < previous.file_offset + previous.file_size:
            errors.append(
                f"{label}: overlapping ELF PT_LOAD file ranges"
            )
            break
    by_address = sorted(
        file_backed, key=lambda item: item.virtual_address
    )
    for previous, current in zip(by_address, by_address[1:]):
        if (
            current.virtual_address <
            previous.virtual_address + previous.file_size
        ):
            errors.append(
                f"{label}: overlapping file-backed ELF PT_LOAD ranges"
            )
            break
    return entrypoint, loads, errors


def validate_esp32_s3_image_bytes(
    payload: bytes,
    label: str,
    *,
    flash_offset: int,
    max_size: int = UPLINK_MAX_FROZEN_APP_BYTES,
) -> tuple[int, list[_APP_IMAGE_SEGMENT], list[str]]:
    """Strictly validate one complete digest-appended ESP32-S3 image."""
    errors: list[str] = []
    if (
        type(payload) is not bytes or
        type(label) is not str or
        not label or
        type(flash_offset) is not int or
        isinstance(flash_offset, bool) or
        not 0 <= flash_offset <= 0xFFFF_FFFF or
        type(max_size) is not int or
        isinstance(max_size, bool) or
        max_size <= 0
    ):
        return 0, [], [
            f"{label}: ESP32-S3 image arguments are invalid"
        ]
    if len(payload) < 24:
        return 0, [], [f"{label}: ESP32-S3 image header is truncated"]
    if len(payload) > max_size:
        return 0, [], [f"{label}: ESP32-S3 image size exceeds its bound"]
    (
        magic,
        segment_count,
        flash_mode,
        flash_size_frequency,
        entrypoint,
    ) = struct.unpack_from("<BBBBI", payload, 0)
    if magic != 0xE9:
        errors.append(f"{label}: ESP application image magic is not 0xE9")
    if not 1 <= segment_count <= 16:
        errors.append(
            f"{label}: ESP application segment count must be 1..16"
        )
    if flash_mode != 2:
        errors.append(
            f"{label}: ESP32-S3 flash mode must be DIO=2"
        )
    if flash_size_frequency != 0x3F:
        errors.append(
            f"{label}: ESP32-S3 flash size/frequency must be "
            "0x3f (8MB/80MHz)"
        )
    if entrypoint == 0:
        errors.append(f"{label}: ESP32-S3 entry point must be nonzero")
    chip_id = struct.unpack_from("<H", payload, 12)[0]
    if payload[8] != 0xEE:
        errors.append(
            f"{label}: ESP32-S3 write-protect pin must be 0xEE"
        )
    if payload[9:12] != b"\x00" * 3 or payload[19:23] != b"\x00" * 4:
        errors.append(
            f"{label}: ESP32-S3 extended-header reserved bytes are not zero"
        )
    if chip_id != 9:
        errors.append(f"{label}: ESP32-S3 chip id must be exactly 9")
    revision = (
        payload[14],
        struct.unpack_from("<H", payload, 15)[0],
        struct.unpack_from("<H", payload, 17)[0],
    )
    if revision != (0, 0, 0xFFFF):
        errors.append(
            f"{label}: ESP32-S3 revision tuple must be (0, 0, 0xffff)"
        )
    if payload[23] != 1:
        errors.append(
            f"{label}: ESP32-S3 append_digest must be exactly 1"
        )
    if errors:
        return entrypoint, [], errors

    segments: list[_APP_IMAGE_SEGMENT] = []
    spans: list[tuple[int, int, int]] = []
    checksum = 0xEF
    position = 24
    for index in range(segment_count):
        if position > len(payload) - 8:
            return entrypoint, [], [
                f"{label}: ESP application segment header {index} "
                "is truncated"
            ]
        address, size = struct.unpack_from("<II", payload, position)
        position += 8
        data_offset = position
        if size == 0 and address in (0, 4):
            continue
        if size == 0:
            return entrypoint, [], [
                f"{label}: ESP application segment {index} has zero size"
            ]
        if size & 3:
            return entrypoint, [], [
                f"{label}: ESP application segment {index} size is not "
                "4-byte aligned"
            ]
        address_end = address + size
        if address == 0 or address_end > 0xFFFF_FFFF:
            return entrypoint, [], [
                f"{label}: ESP application segment {index} address "
                "arithmetic overflow"
            ]
        if (
            size > max_size or
            position > len(payload) or
            size > len(payload) - position
        ):
            return entrypoint, [], [
                f"{label}: ESP application segment data {index} is "
                "truncated"
            ]
        if not any(
            start <= address and address_end <= end
            for start, end in ESP32_S3_SEGMENT_RANGES
        ):
            errors.append(
                f"{label}: ESP application segment {index} is outside "
                "valid ESP32-S3 memory"
            )
        if (
            any(
                start <= address and address_end <= end
                for start, end in ESP32_S3_MAPPED_SEGMENT_RANGES
            ) and
            (flash_offset + data_offset) % 0x10000 != address % 0x10000
        ):
            errors.append(
                f"{label}: ESP application segment {index} mapped flash "
                "offset mismatch"
            )
        data = payload[position:position + size]
        for value in data:
            checksum ^= value
        segments.append(_APP_IMAGE_SEGMENT(
            index,
            address,
            data,
            data_offset,
        ))
        spans.append((address, address_end, index))
        position += size

    ordered_spans = sorted(spans)
    for previous, current in zip(ordered_spans, ordered_spans[1:]):
        if current[0] < previous[1]:
            errors.append(
                f"{label}: overlapping ESP application load segments"
            )
            break
    containing_entry_segments = [
        span for span in spans if span[0] <= entrypoint < span[1]
    ]
    if len(containing_entry_segments) != 1:
        errors.append(
            f"{label}: entry point is outside one unique application segment"
        )
    if not any(
        start <= entrypoint < end
        for start, end in ESP32_S3_EXECUTABLE_SEGMENT_RANGES
    ):
        errors.append(
            f"{label}: entry point is outside valid ESP32-S3 executable "
            "memory"
        )

    checksum_offset = position + (15 - position % 16)
    if checksum_offset >= len(payload):
        errors.append(f"{label}: ESP application checksum is missing")
        return entrypoint, segments, errors
    if any(payload[position:checksum_offset]):
        errors.append(
            f"{label}: ESP application checksum padding is not all zero"
        )
    if payload[checksum_offset] != checksum:
        errors.append(f"{label}: ESP application checksum is invalid")
    expected_size = checksum_offset + 1 + hashlib.sha256().digest_size
    if len(payload) < expected_size:
        errors.append(
            f"{label}: ESP application appended digest is truncated"
        )
        return entrypoint, segments, errors
    if len(payload) > expected_size:
        errors.append(
            f"{label}: ESP application has trailing bytes after digest"
        )
        return entrypoint, segments, errors
    actual_digest = payload[-32:]
    expected_digest = hashlib.sha256(payload[:-32]).digest()
    if not hmac.compare_digest(actual_digest, expected_digest):
        errors.append(
            f"{label}: ESP application appended digest SHA-256 is invalid"
        )
    return entrypoint, segments, errors


def _parse_esp32s3_app_image(
    payload: bytes,
    label: str,
) -> tuple[int, list[_APP_IMAGE_SEGMENT], list[str]]:
    return validate_esp32_s3_image_bytes(
        payload,
        label,
        flash_offset=APP_OFFSET,
        max_size=UPLINK_MAX_FROZEN_APP_BYTES,
    )


def _verify_elf_app_image_provenance(
    elf: bytes,
    firmware: bytes,
    *,
    label: str,
) -> list[str]:
    sections, _symbols, _sources, errors = _read_elf32_evidence(
        elf, f"{label} ELF"
    )
    if errors:
        return errors
    elf_entrypoint, loads, load_errors = _read_elf32_load_evidence(
        elf, f"{label} ELF"
    )
    errors.extend(load_errors)
    app_entrypoint, app_segments, image_errors = _parse_esp32s3_app_image(
        firmware, f"{label} firmware.bin"
    )
    errors.extend(image_errors)
    if errors:
        return errors
    if app_entrypoint != elf_entrypoint:
        errors.append(
            f"{label}: ELF/application entrypoint provenance mismatch"
        )
    executable_entry_loads = [
        load for load in loads
        if (
            load.flags & 0x1 != 0 and
            load.virtual_address <= elf_entrypoint <
            load.virtual_address + load.file_size
        )
    ]
    if len(executable_entry_loads) != 1:
        errors.append(
            f"{label}: entrypoint is not backed by one executable "
            "ELF PT_LOAD"
        )

    appdesc_sections = [
        section for section in sections
        if (
            section.name == ".flash.appdesc" and
            section.section_type == 1 and
            section.flags & 0x2 != 0 and
            section.size > 0
        )
    ]
    if len(appdesc_sections) != 1:
        errors.append(
            f"{label}: ELF requires one allocatable .flash.appdesc"
        )
        return errors
    appdesc = appdesc_sections[0]
    elf_digest = hashlib.sha256(elf).digest()
    digest_end = UPLINK_ELF_SHA256_IMAGE_OFFSET + len(elf_digest)
    if digest_end > len(firmware):
        errors.append(
            f"{label}: application ELF SHA-256 field is unavailable"
        )
        return errors
    if not hmac.compare_digest(
        firmware[UPLINK_ELF_SHA256_IMAGE_OFFSET:digest_end],
        elf_digest,
    ):
        errors.append(
            f"{label}: application ELF SHA-256 provenance mismatch"
        )

    digest_address: int | None = None
    for segment in app_segments:
        start = segment.file_data_offset
        end = start + len(segment.data)
        if (
            start <= UPLINK_ELF_SHA256_IMAGE_OFFSET and
            digest_end <= end
        ):
            digest_address = (
                segment.address +
                UPLINK_ELF_SHA256_IMAGE_OFFSET -
                segment.file_data_offset
            )
            break
    if (
        digest_address is None or
        not (
            appdesc.address <= digest_address and
            digest_address + len(elf_digest) <=
            appdesc.address + appdesc.size
        )
    ):
        errors.append(
            f"{label}: application ELF SHA-256 is outside .flash.appdesc"
        )
        return errors
    digest_elf_offset = (
        appdesc.file_offset + digest_address - appdesc.address
    )
    if elf[
        digest_elf_offset:digest_elf_offset + len(elf_digest)
    ] != b"\x00" * len(elf_digest):
        errors.append(
            f"{label}: ELF .flash.appdesc SHA-256 placeholder is not zero"
        )

    allocatable_sections = [
        section for section in sections
        if (
            section.flags & 0x2 != 0 and
            section.address != 0 and
            section.size > 0
        )
    ]
    file_backed_sections = [
        section for section in allocatable_sections
        if section.section_type != 8
    ]
    nobits_sections = [
        section for section in allocatable_sections
        if section.section_type == 8
    ]
    # ESP-IDF's linker-dummy zero-file loads intentionally nest. Require an
    # exact one-to-one NOBITS owner instead of rejecting valid range overlap.
    zero_file_loads_by_range: dict[
        tuple[int, int], list[_ELF32_PROGRAM_HEADER]
    ] = {}
    for load in loads:
        if load.file_size != 0:
            continue
        load_range = (
            load.virtual_address,
            load.virtual_address + load.memory_size,
        )
        zero_file_loads_by_range.setdefault(load_range, []).append(load)
    nobits_sections_by_range: dict[
        tuple[int, int], list[_ELF32_SECTION]
    ] = {}
    for section in nobits_sections:
        section_range = (
            section.address,
            section.address + section.size,
        )
        nobits_sections_by_range.setdefault(
            section_range, []
        ).append(section)
    for load_range, range_loads in zero_file_loads_by_range.items():
        range_sections = nobits_sections_by_range.get(load_range, [])
        if not range_sections:
            errors.append(
                f"{label}: zero-file PT_LOAD must exactly match one "
                "allocatable SHT_NOBITS section"
            )
        elif len(range_loads) != 1 or len(range_sections) != 1:
            errors.append(
                f"{label}: zero-file PT_LOAD ownership is not bijective"
            )
    for section in file_backed_sections:
        if section.section_type not in (1, 14, 15, 16):
            errors.append(
                f"{label}: allocatable ELF section {section.name!r} "
                "has an unsupported initialized section type"
            )

    ordered_file_sections = sorted(
        file_backed_sections,
        key=lambda item: (item.address, item.index),
    )
    for previous, current in zip(
        ordered_file_sections, ordered_file_sections[1:]
    ):
        if current.address < previous.address + previous.size:
            errors.append(
                f"{label}: overlapping initialized allocatable ELF sections"
            )
            break

    def merged_ranges(
        ranges: list[tuple[int, int]],
    ) -> list[tuple[int, int]]:
        merged: list[list[int]] = []
        for start, end in sorted(ranges):
            if start >= end:
                continue
            if not merged or start > merged[-1][1]:
                merged.append([start, end])
            elif end > merged[-1][1]:
                merged[-1][1] = end
        return [(start, end) for start, end in merged]

    def range_is_covered(
        start: int,
        end: int,
        ranges: list[tuple[int, int]],
    ) -> bool:
        cursor = start
        for range_start, range_end in ranges:
            if range_end <= cursor:
                continue
            if range_start > cursor:
                return False
            cursor = max(cursor, range_end)
            if cursor >= end:
                return True
        return cursor >= end

    def uncovered_ranges(
        start: int,
        end: int,
        covered: list[tuple[int, int]],
    ) -> list[tuple[int, int]]:
        result: list[tuple[int, int]] = []
        cursor = start
        for range_start, range_end in covered:
            if range_end <= cursor or range_start >= end:
                continue
            if range_start > cursor:
                result.append((cursor, min(range_start, end)))
            cursor = max(cursor, min(range_end, end))
            if cursor >= end:
                break
        if cursor < end:
            result.append((cursor, end))
        return result

    app_ranges = merged_ranges([
        (segment.address, segment.address + len(segment.data))
        for segment in app_segments
    ])
    memory_section_ranges = merged_ranges([
        (section.address, section.address + section.size)
        for section in allocatable_sections
    ])
    initialized_section_ranges = merged_ranges([
        (section.address, section.address + section.size)
        for section in file_backed_sections
    ])
    for section in file_backed_sections:
        section_end = section.address + section.size
        matching_loads = [
            load for load in loads
            if (
                load.virtual_address <= section.address and
                section_end <= load.virtual_address + load.file_size and
                section.file_offset == (
                    load.file_offset +
                    section.address -
                    load.virtual_address
                )
            )
        ]
        if len(matching_loads) != 1:
            errors.append(
                f"{label}: initialized allocatable ELF section "
                f"{section.name!r} is not uniquely file-backed by PT_LOAD"
            )
        if not range_is_covered(
            section.address, section_end, app_ranges
        ):
            errors.append(
                f"{label}: initialized allocatable ELF section "
                f"{section.name!r} is missing from application segments"
            )

    for section in nobits_sections:
        section_end = section.address + section.size
        if not any(
            load.virtual_address <= section.address and
            section_end <= load.virtual_address + load.memory_size
            for load in loads
        ):
            errors.append(
                f"{label}: SHT_NOBITS allocatable ELF section "
                f"{section.name!r} is not accounted by PT_LOAD memory"
            )

    for load in loads:
        load_start = load.virtual_address
        load_memory_end = load_start + load.memory_size
        load_file_end = load_start + load.file_size
        for gap_start, gap_end in uncovered_ranges(
            load_start,
            load_memory_end,
            memory_section_ranges,
        ):
            following_sections = [
                section for section in allocatable_sections
                if section.address == gap_end
            ]
            reviewed_alignment_gap = (
                gap_start >= load_file_end and
                any(
                    section.alignment > 1 and
                    gap_end % section.alignment == 0 and
                    gap_end - gap_start < section.alignment
                    for section in following_sections
                )
            )
            if not reviewed_alignment_gap:
                errors.append(
                    f"{label}: ELF PT_LOAD {load.index} memory range is "
                    "not fully exposed by allocatable sections"
                )
        if load.file_size == 0:
            continue
        load_nobits_ranges = merged_ranges([
            (
                max(section.address, load_start),
                min(section.address + section.size, load_file_end),
            )
            for section in nobits_sections
            if (
                section.file_offset == (
                    load.file_offset +
                    section.address -
                    load.virtual_address
                ) and
                section.address < load_file_end and
                section.address + section.size > load_start
            )
        ])
        for gap_start, gap_end in uncovered_ranges(
            load_start,
            load_file_end,
            initialized_section_ranges,
        ):
            if not range_is_covered(
                gap_start, gap_end, load_nobits_ranges
            ):
                errors.append(
                    f"{label}: ELF PT_LOAD {load.index} contains hidden "
                    "file-backed bytes"
                )
                continue
            gap_file_offset = load.file_offset + gap_start - load_start
            gap_bytes = elf[
                gap_file_offset:gap_file_offset + gap_end - gap_start
            ]
            if any(gap_bytes):
                errors.append(
                    f"{label}: ELF PT_LOAD {load.index} NOBITS file gap "
                    "contains nonzero bytes"
                )
    if errors:
        return errors

    ordered_file_loads = sorted(
        (load for load in loads if load.file_size > 0),
        key=lambda item: item.virtual_address,
    )
    load_starts = [
        load.virtual_address for load in ordered_file_loads
    ]
    section_starts = [
        section.address for section in ordered_file_sections
    ]

    def containing_load(address: int) -> _ELF32_PROGRAM_HEADER | None:
        index = bisect_right(load_starts, address) - 1
        if index < 0:
            return None
        candidate = ordered_file_loads[index]
        if address < candidate.virtual_address + candidate.file_size:
            return candidate
        return None

    def containing_section(address: int) -> _ELF32_SECTION | None:
        index = bisect_right(section_starts, address) - 1
        if index < 0:
            return None
        candidate = ordered_file_sections[index]
        if address < candidate.address + candidate.size:
            return candidate
        return None

    for segment in app_segments:
        cursor = segment.address
        data_index = 0
        segment_end = segment.address + len(segment.data)
        while cursor < segment_end:
            load = containing_load(cursor)
            section = containing_section(cursor)
            if load is None or section is None:
                remaining = segment_end - cursor
                if (
                    remaining <= 3 and
                    segment.data[data_index:] == b"\x00" * remaining
                ):
                    cursor = segment_end
                    data_index += remaining
                    continue
                errors.append(
                    f"{label}: application load segment {segment.index} "
                    "has bytes outside a unique ELF PT_LOAD/section mapping"
                )
                break
            count = min(
                segment_end - cursor,
                load.virtual_address + load.file_size - cursor,
                section.address + section.size - cursor,
            )
            elf_offset = (
                load.file_offset + cursor - load.virtual_address
            )
            section_offset = (
                section.file_offset + cursor - section.address
            )
            if elf_offset != section_offset:
                errors.append(
                    f"{label}: application load segment {segment.index} "
                    "has inconsistent ELF PT_LOAD/section offsets"
                )
                break
            expected = bytearray(elf[elf_offset:elf_offset + count])
            binary_start = segment.file_data_offset + data_index
            binary_end = binary_start + count
            patch_start = max(
                binary_start, UPLINK_ELF_SHA256_IMAGE_OFFSET
            )
            patch_end = min(binary_end, digest_end)
            if patch_start < patch_end:
                expected_start = patch_start - binary_start
                digest_start = (
                    patch_start - UPLINK_ELF_SHA256_IMAGE_OFFSET
                )
                expected[
                    expected_start:expected_start + patch_end - patch_start
                ] = elf_digest[
                    digest_start:digest_start + patch_end - patch_start
                ]
            actual = segment.data[data_index:data_index + count]
            if not hmac.compare_digest(actual, bytes(expected)):
                errors.append(
                    f"{label}: application ELF load image bytes mismatch "
                    f"in segment {segment.index} at {cursor:#010x}"
                )
                break
            cursor += count
            data_index += count
    return errors


def verify_frozen_badge_uplink_attestation(
    frozen: FrozenArtifactSet,
    *,
    label: str,
    expected_rtc_size: int,
) -> list[str]:
    """Attest only immutable ELF/bin bytes from the exact flash authority."""
    if (
        type(frozen) is not FrozenArtifactSet or
        type(label) is not str or
        not label or
        type(expected_rtc_size) is not int or
        isinstance(expected_rtc_size, bool) or
        expected_rtc_size <= 0
    ):
        return ["frozen badge uplink attestation arguments are malformed"]
    try:
        elf = frozen.member_bytes("artifact.elf")
        firmware = frozen.member_bytes("artifact.firmware")
    except SecureArtifactError as exc:
        return [f"{label}: required frozen ELF/bin member is missing: {exc}"]
    if not 0 < len(elf) <= UPLINK_MAX_FROZEN_ELF_BYTES:
        return [f"{label}: frozen ELF size is invalid"]
    if not 0 < len(firmware) <= UPLINK_MAX_FROZEN_APP_BYTES:
        return [f"{label}: frozen firmware.bin size is invalid"]
    errors = _verify_uplink_rtc_elf_payload(
        elf,
        label=f"{label} ELF",
        expected_size=expected_rtc_size,
    )
    errors.extend(_verify_elf_app_image_provenance(
        elf,
        firmware,
        label=label,
    ))
    return errors


def verify_frozen_badge_uplink_flash_authority(
    frozen: FrozenArtifactSet,
    *,
    environment: str,
    target: str,
    project: str,
    hardware: str,
    version: str,
    expected_rtc_size: int,
) -> list[str]:
    """Bind descriptor-rooted uplink bytes to one selected release platform."""
    scalar_values = (environment, target, project, hardware, version)
    if (
        type(frozen) is not FrozenArtifactSet or
        any(type(value) is not str or not value for value in scalar_values) or
        type(expected_rtc_size) is not int or
        isinstance(expected_rtc_size, bool) or
        expected_rtc_size <= 0
    ):
        return ["frozen badge uplink flash authority arguments are malformed"]
    errors = verify_descriptor_rooted_frozen_role_authority(
        frozen,
        role=UPLINK_ROLE,
        environment=environment,
    )
    if errors:
        return errors
    try:
        expected = expected_identity_for_env(VERSION_HEADER, environment)
        runtime_target = runtime_target_for_env(environment)
    except (OSError, ValueError) as exc:
        return [
            "frozen uplink platform identity cannot be established: "
            f"{exc}"
        ]
    if (
        target != runtime_target or
        project != expected.project or
        hardware != expected.hardware or
        version != expected.version
    ):
        return [
            "frozen uplink selected platform identity does not match "
            "the canonical environment"
        ]
    try:
        firmware = frozen.member_bytes("artifact.firmware")
        sdkconfig = frozen.member_bytes("build.sdkconfig")
    except SecureArtifactError as exc:
        return [f"frozen uplink authority member is unavailable: {exc}"]
    embedded = parse_firmware_identity(firmware)
    if (
        embedded is None or
        embedded.project != project or
        embedded.version != version
    ):
        errors.append(
            "frozen uplink firmware descriptor does not match the "
            "selected project/version"
        )
    for marker, marker_label in (
        (target, "runtime target"),
        (hardware, "hardware"),
    ):
        try:
            encoded = marker.encode("ascii")
        except UnicodeEncodeError:
            errors.append(
                f"frozen uplink {marker_label} marker is not ASCII"
            )
            continue
        if encoded not in firmware:
            errors.append(
                f"frozen uplink firmware lacks selected {marker_label} "
                "marker"
            )
    sdk_values, sdk_errors = _parse_frozen_sdkconfig(
        sdkconfig,
        label="frozen uplink",
    )
    errors.extend(sdk_errors)
    expected_sdk = dict(UPLINK_FROZEN_COMMON_SDKCONFIG)
    if environment == UPLINK_CANARY_ENV:
        expected_sdk.update(CANARY_CONTROLLER_ONLY_SDKCONFIG)
    elif environment == UPLINK_PRODUCTION_ENV:
        expected_sdk["CONFIG_BT_ENABLED"] = "n"
    else:
        errors.append("frozen uplink environment is not a release target")
    for key, wanted in expected_sdk.items():
        actual = sdk_values.get(key)
        if actual != wanted:
            errors.append(
                f"frozen uplink sdkconfig {key} must be {wanted!r}; "
                f"got {actual!r}"
            )
    if errors:
        return errors
    return verify_frozen_badge_uplink_attestation(
        frozen,
        label=f"frozen {environment}",
        expected_rtc_size=expected_rtc_size,
    )


def _verify_canary_final_elf(
    elf_path: Path,
    *,
    required_symbols: tuple[str, ...],
    required_source_files: tuple[str, ...],
    forbidden_symbols: tuple[str, ...],
    forbidden_source_files: tuple[str, ...],
) -> list[str]:
    label = "canary firmware ELF"
    symbols, source_files, errors = _read_elf32_symbol_evidence(
        elf_path, label
    )
    for name in required_symbols:
        if name not in symbols:
            errors.append(
                f"{label}: required linked symbol missing: {name}"
            )
    for name in required_source_files:
        if name not in source_files:
            errors.append(
                f"{label}: required linked source file missing: {name}"
            )
    for name in forbidden_symbols:
        if name in symbols:
            errors.append(
                f"{label}: forbidden linked symbol present: {name}"
            )
    for name in forbidden_source_files:
        if name in source_files:
            errors.append(
                f"{label}: forbidden linked source file present: {name}"
            )
    return errors


def _parse_cmake_cache(path: Path) -> tuple[dict[str, str], list[str]]:
    payload, errors = _read_regular_artifact(
        path, "production CMake cache"
    )
    if payload is None:
        return {}, errors
    try:
        contents = payload.decode("utf-8")
    except UnicodeDecodeError as exc:
        return {}, [f"production CMake cache: invalid UTF-8: {exc}"]
    values: dict[str, str] = {}
    for line in contents.splitlines():
        if not line or line.startswith(("#", "//")) or "=" not in line:
            continue
        raw_key, value = line.split("=", 1)
        key = raw_key.split(":", 1)[0]
        if key in values:
            errors.append(
                f"production CMake cache: duplicate key {key!r}"
            )
            continue
        values[key] = value
    return values, errors


def _verify_production_build_evidence(
    production_build_dir: Path,
    production_env: str,
    production_payload: bytes | None,
) -> list[str]:
    errors: list[str] = []
    try:
        expected_sdkconfig = PRODUCTION_SDKCONFIG_DEFAULTS[production_env]
        expected_identity = expected_identity_for_env(
            VERSION_HEADER, production_env
        )
    except (KeyError, OSError, ValueError) as exc:
        return [
            "production build contract cannot be resolved for "
            f"{production_env!r}: {exc}"
        ]

    cache, cache_errors = _parse_cmake_cache(
        production_build_dir / "CMakeCache.txt"
    )
    errors.extend(cache_errors)
    project = cache.get("CMAKE_PROJECT_NAME")
    if project != expected_identity.project:
        errors.append(
            "production CMake cache: CMAKE_PROJECT_NAME must be "
            f"{expected_identity.project!r}; got {project!r}"
        )
    defaults = cache.get("SDKCONFIG_DEFAULTS")
    if defaults != expected_sdkconfig:
        errors.append(
            "production CMake cache: SDKCONFIG_DEFAULTS must be "
            f"{expected_sdkconfig!r}; got {defaults!r}"
        )
    if "FOF_DC34_GAME_CANARY" in cache:
        errors.append(
            "production CMake cache must not define "
            "FOF_DC34_GAME_CANARY"
        )

    if production_payload is not None:
        embedded = parse_firmware_identity(production_payload)
        if embedded is None:
            errors.append(
                "production firmware identity is invalid or missing"
            )
        elif (
            embedded.project != expected_identity.project or
            embedded.version != expected_identity.version
        ):
            errors.append(
                "production firmware identity must be "
                f"{expected_identity.project} "
                f"{expected_identity.version}; got "
                f"{embedded.project} {embedded.version}"
            )
        runtime_target = runtime_target_for_env(production_env).encode(
            "ascii"
        )
        hardware = expected_identity.hardware.encode("ascii")
        if runtime_target not in production_payload:
            errors.append(
                "production firmware identity is missing runtime target "
                f"{runtime_target.decode('ascii')!r}"
            )
        if hardware not in production_payload:
            errors.append(
                "production firmware identity is missing hardware marker "
                f"{hardware.decode('ascii')!r}"
            )

    symbols, source_files, elf_errors = _read_elf32_symbol_evidence(
        production_build_dir / "firmware.elf",
        "production firmware ELF",
    )
    errors.extend(elf_errors)
    for name in sorted(symbols | source_files):
        if name in PRODUCTION_ELF_ALLOWED_SYMBOLS:
            continue
        if any(
            fragment in name
            for fragment in PRODUCTION_ELF_FORBIDDEN_SYMBOL_FRAGMENTS
        ):
            errors.append(
                "production firmware ELF contains canary-only symbol "
                f"{name!r}"
            )
    return errors


def _verify_canary_artifact_isolation(
    canary_build_dir: Path,
    production_build_dir: Path,
    *,
    canary_env: str,
    production_env: str,
    max_app_bytes: int,
) -> list[str]:
    canary_build_dir = Path(canary_build_dir)
    production_build_dir = Path(production_build_dir)
    errors: list[str] = []

    if canary_build_dir.name != canary_env:
        errors.append(
            "canary build directory must be named exactly "
            f"{canary_env!r}; got {canary_build_dir.name!r}"
        )
    if production_build_dir.name != production_env:
        errors.append(
            "production build directory must be named exactly "
            f"{production_env!r}; got {production_build_dir.name!r}"
        )
    if canary_build_dir.is_symlink():
        errors.append("canary build directory must not be a symlink")
    if production_build_dir.is_symlink():
        errors.append("production build directory must not be a symlink")
    try:
        same_build_directory = (
            canary_build_dir.resolve() == production_build_dir.resolve()
        )
    except (OSError, RuntimeError) as exc:
        errors.append(f"build directories cannot be resolved safely: {exc}")
    else:
        if same_build_directory:
            errors.append(
                "canary and production build directories must be distinct"
            )

    canary_payload, canary_errors = _read_regular_artifact(
        canary_build_dir / "firmware.bin", "canary firmware"
    )
    production_payload, production_errors = _read_regular_artifact(
        production_build_dir / "firmware.bin", "production firmware"
    )
    errors.extend(canary_errors)
    errors.extend(production_errors)
    if (
        canary_payload is not None and
        len(canary_payload) > max_app_bytes
    ):
        errors.append(
            "canary firmware: image size "
            f"{len(canary_payload)} exceeds budget {max_app_bytes} bytes"
        )
    if (
        canary_payload is not None and
        production_payload is not None and
        hashlib.sha256(canary_payload).digest() ==
        hashlib.sha256(production_payload).digest()
    ):
        errors.append(
            "canary firmware must differ from production firmware"
        )
    if production_payload is not None:
        for token in PRODUCTION_FORBIDDEN_RUNTIME_TOKENS:
            if token in production_payload:
                errors.append(
                    "production firmware contains canary-only runtime "
                    f"token {token.decode('ascii')!r}"
                )
    errors.extend(_verify_production_build_evidence(
        production_build_dir,
        production_env,
        production_payload,
    ))
    return errors


def verify_badge_uplink_canary_acceptance(
    canary_build_dir: Path,
    sdkconfig: Path,
    production_build_dir: Path,
) -> list[str]:
    """Fail closed unless canary radio, memory, and isolation evidence passes."""
    canary_build_dir = Path(canary_build_dir)
    errors = verify_badge_uplink_canary_sdkconfig(sdkconfig)
    errors.extend(_verify_canary_linker_map(
        canary_build_dir / UPLINK_CANARY_MAP_FILENAME,
        max_internal_ram_bytes=UPLINK_CANARY_MAX_INTERNAL_RAM_BYTES,
    ))
    errors.extend(_verify_uplink_rtc_final_elf(
        canary_build_dir / "firmware.elf",
        label="canary firmware ELF",
        expected_size=UPLINK_CANARY_RTC_NOINIT_BYTES,
    ))
    errors.extend(_verify_canary_final_elf(
        canary_build_dir / "firmware.elf",
        required_symbols=UPLINK_CANARY_REQUIRED_SYMBOLS,
        required_source_files=UPLINK_CANARY_REQUIRED_SOURCE_FILES,
        forbidden_symbols=UPLINK_CANARY_FORBIDDEN_SYMBOLS,
        forbidden_source_files=UPLINK_CANARY_FORBIDDEN_SOURCE_FILES,
    ))
    errors.extend(_verify_uplink_rtc_final_elf(
        Path(production_build_dir) / "firmware.elf",
        label="production firmware ELF",
        expected_size=UPLINK_PRODUCTION_RTC_NOINIT_BYTES,
    ))
    errors.extend(_verify_canary_artifact_isolation(
        canary_build_dir,
        production_build_dir,
        canary_env=UPLINK_CANARY_ENV,
        production_env=UPLINK_PRODUCTION_ENV,
        max_app_bytes=UPLINK_CANARY_MAX_APP_BYTES,
    ))
    return errors


def prepare_verified_badge_uplink_snapshot(
    build_dir: Path,
    partition_source: Path,
    sdkconfig: Path,
    *,
    private_parent: Path,
    materialize_missing_aliases: bool,
) -> VerifiedBadgeArtifactSnapshot:
    """Return the only live capability for verified uplink artifact bytes."""
    return prepare_verified_role_snapshot(
        UPLINK_ROLE,
        build_dir,
        partition_source,
        sdkconfig,
        private_parent=private_parent,
        materialize_missing_aliases=materialize_missing_aliases,
    )


def _parse_text_manifest(path: Path) -> tuple[dict[int, str], list[str]]:
    entries: dict[int, str] = {}
    errors: list[str] = []
    try:
        lines = path.read_text().splitlines()
    except OSError as exc:
        return entries, [f"{path.name}: cannot read: {exc}"]
    for line_number, line in enumerate(lines, 1):
        try:
            fields = shlex.split(line)
        except ValueError as exc:
            errors.append(f"{path.name}:{line_number}: invalid shell syntax: {exc}")
            continue
        if not fields:
            continue
        index = 0
        while index < len(fields):
            token = fields[index]
            if token in {"--flash_mode", "--flash_freq", "--flash_size"}:
                if index + 1 >= len(fields) or fields[index + 1].startswith("--"):
                    errors.append(
                        f"{path.name}:{line_number}: option {token} needs a value"
                    )
                    break
                index += 2
                continue
            if token.startswith("--"):
                errors.append(
                    f"{path.name}:{line_number}: unexpected option {token!r}"
                )
                index += 1
                continue
            if index + 1 >= len(fields):
                errors.append(
                    f"{path.name}:{line_number}: offset {token!r} has no path"
                )
                break
            try:
                offset = int(token, 0)
            except ValueError:
                errors.append(
                    f"{path.name}:{line_number}: invalid offset {token!r}"
                )
                index += 2
                continue
            if offset in entries:
                errors.append(
                    f"{path.name}:{line_number}: duplicate offset {offset:#x}"
                )
            entries[offset] = fields[index + 1]
            index += 2
    return entries, errors


def _parse_json_manifest(path: Path) -> tuple[dict[int, str], list[str]]:
    errors: list[str] = []
    try:
        payload = json.loads(
            path.read_text(), object_pairs_hook=_json_object_without_duplicates
        )
    except (OSError, json.JSONDecodeError, _DuplicateJsonKeyError) as exc:
        return {}, [f"{path.name}: cannot read JSON: {exc}"]
    files = payload.get("flash_files") if isinstance(payload, dict) else None
    if not isinstance(files, dict):
        return {}, [f"{path.name}: flash_files must be an object"]
    entries: dict[int, str] = {}
    for raw_offset, raw_path in files.items():
        try:
            offset = int(raw_offset, 0)
        except (TypeError, ValueError):
            errors.append(f"{path.name}: invalid flash_files offset {raw_offset!r}")
            continue
        if not isinstance(raw_path, str):
            errors.append(f"{path.name}: path at {raw_offset!r} must be a string")
            continue
        if offset in entries:
            errors.append(
                f"{path.name}: duplicate decoded offset {offset:#x}"
            )
            continue
        entries[offset] = raw_path
    app = payload.get("app") if isinstance(payload, dict) else None
    if not isinstance(app, dict) or app.get("offset") != "0x20000" or \
            app.get("file") != EXPECTED_APP[APP_OFFSET]:
        errors.append(
            f"{path.name}: app must be 0x20000 {EXPECTED_APP[APP_OFFSET]}"
        )
    return entries, errors


def _safe_referenced_file(build_dir: Path, manifest: str,
                          relative: str) -> str | None:
    candidate = Path(relative)
    if candidate.is_absolute() or ".." in candidate.parts:
        return f"{manifest}: unsafe referenced path {relative!r}"
    root = build_dir.resolve()
    resolved = (build_dir / candidate).resolve()
    if resolved != root and root not in resolved.parents:
        return f"{manifest}: referenced path escapes build directory: {relative!r}"
    if not resolved.is_file():
        return f"{manifest}: referenced path missing: {relative}"
    return None


def _decode_ota0(partitions_bin: Path) -> tuple[list[int], list[str]]:
    try:
        payload = partitions_bin.read_bytes()
    except OSError as exc:
        return [], [f"partitions.bin: cannot read: {exc}"]
    offsets: list[int] = []
    errors: list[str] = []
    entry_size = struct.calcsize("<HBBII16sI")
    for index in range(0, len(payload) - entry_size + 1, entry_size):
        entry = payload[index:index + entry_size]
        magic, entry_type, subtype, offset, _size, label, _flags = struct.unpack(
            "<HBBII16sI", entry
        )
        if magic == 0xFFFF:
            break
        if magic == 0xEBEB:  # partition-table MD5 trailer
            break
        if magic != 0x50AA:
            errors.append(f"partitions.bin: invalid entry magic at {index:#x}")
            break
        decoded_label = label.split(b"\x00", 1)[0].decode("ascii", errors="replace")
        if entry_type == 0 and subtype == 0x10 and decoded_label == "ota_0":
            offsets.append(offset)
    return offsets, errors


def _csv_ota0_offset(partition_source: Path) -> tuple[int | None, str | None]:
    try:
        lines = partition_source.read_text().splitlines()
    except OSError as exc:
        return None, f"partition source cannot read: {exc}"
    for line in lines:
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        fields = [field.strip() for field in stripped.split(",")]
        if fields[0] == "ota_0" and len(fields) >= 5:
            try:
                return int(fields[3], 0), None
            except ValueError:
                return None, "partition source ota_0 offset is invalid"
    return None, "partition source has no ota_0 entry"


def _find_partition_generator() -> tuple[Path | None, str | None]:
    override = os.environ.get(PARTITION_GENERATOR_ENV)
    if override:
        candidate = Path(override).expanduser()
        if candidate.is_file():
            return candidate, None
        return None, (
            "ESP-IDF partition generator unavailable: "
            f"{PARTITION_GENERATOR_ENV} does not name a file"
        )

    candidates: list[Path] = []
    idf_path = os.environ.get("IDF_PATH")
    if idf_path:
        candidates.append(Path(idf_path) / PARTITION_GENERATOR_RELATIVE)

    core_dirs: list[Path] = []
    configured_core_dir = os.environ.get("PLATFORMIO_CORE_DIR")
    if configured_core_dir:
        core_dirs.append(Path(configured_core_dir).expanduser())
    core_dirs.append(Path.home() / ".platformio")
    for ancestor in Path(sys.executable).resolve().parents:
        if ancestor.name == ".platformio":
            core_dirs.append(ancestor)
            break

    seen: set[Path] = set()
    for core_dir in core_dirs:
        packages_dir = core_dir / "packages"
        frameworks = [packages_dir / "framework-espidf"]
        frameworks.extend(sorted(packages_dir.glob("framework-espidf@*")))
        for framework in frameworks:
            candidates.append(framework / PARTITION_GENERATOR_RELATIVE)

    for candidate in candidates:
        normalized = candidate.resolve()
        if normalized in seen:
            continue
        seen.add(normalized)
        if candidate.is_file():
            return candidate, None
    return None, (
        "ESP-IDF partition generator unavailable: install framework-espidf "
        f"or set {PARTITION_GENERATOR_ENV}"
    )


def _verify_full_partition_table(
    partition_source: Path, partitions_bin: Path
) -> str | None:
    partition_source = Path(partition_source)
    if not partition_source.is_file():
        return "partition source is not a file"
    try:
        generated_partition_table = partitions_bin.read_bytes()
    except OSError as exc:
        return f"partitions.bin: cannot read for full-table comparison: {exc}"

    generator, generator_error = _find_partition_generator()
    if generator_error:
        return generator_error
    assert generator is not None

    try:
        with tempfile.TemporaryDirectory(
            prefix="fof-badge-partition-verify-"
        ) as temporary_dir:
            regenerated_path = Path(temporary_dir) / "partitions.bin"
            completed = subprocess.run(
                [
                    sys.executable,
                    str(generator),
                    "--quiet",
                    "--flash-size",
                    "8MB",
                    "--offset",
                    "0x8000",
                    str(partition_source),
                    str(regenerated_path),
                ],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                timeout=30,
                check=False,
            )
            if completed.returncode != 0:
                return (
                    "ESP-IDF partition generator failed with exit code "
                    f"{completed.returncode}"
                )
            if not regenerated_path.is_file():
                return "ESP-IDF partition generator produced no output"
            regenerated_partition_table = regenerated_path.read_bytes()
    except subprocess.TimeoutExpired:
        return "ESP-IDF partition generator timed out"
    except OSError as exc:
        return f"ESP-IDF partition generator could not run: {exc}"

    if regenerated_partition_table != generated_partition_table:
        return "partition source does not reproduce partitions.bin"
    return None


def verify_badge_uplink_build(
    build_dir: Path, partition_source: Path | None = None
) -> list[str]:
    build_dir = Path(build_dir)
    errors: list[str] = []
    parsed: dict[str, dict[int, str]] = {}

    for name, expected in TEXT_MANIFESTS.items():
        path = build_dir / name
        if not path.is_file():
            errors.append(f"{name}: required manifest missing")
            continue
        entries, parse_errors = _parse_text_manifest(path)
        errors.extend(parse_errors)
        parsed[name] = entries
        if entries != expected:
            errors.append(f"{name}: mappings differ from required {expected}")

    json_path = build_dir / "flasher_args.json"
    if not json_path.is_file():
        errors.append("flasher_args.json: required manifest missing")
    else:
        entries, parse_errors = _parse_json_manifest(json_path)
        errors.extend(parse_errors)
        parsed[json_path.name] = entries
        if entries != EXPECTED_FULL:
            errors.append(
                f"flasher_args.json: mappings differ from required {EXPECTED_FULL}"
            )

    for manifest, entries in parsed.items():
        if 0x10000 in entries:
            errors.append(f"{manifest}: forbidden 0x10000 application entry")
        if entries.get(APP_OFFSET) != EXPECTED_APP[APP_OFFSET]:
            errors.append(
                f"{manifest}: application must be exactly 0x20000 "
                f"{EXPECTED_APP[APP_OFFSET]}"
            )
        for relative in entries.values():
            path_error = _safe_referenced_file(build_dir, manifest, relative)
            if path_error:
                errors.append(path_error)

    for alias, canonical in ALIASES.items():
        canonical_path = build_dir / canonical
        alias_path = build_dir / alias
        if not canonical_path.is_file():
            errors.append(f"canonical artifact missing: {canonical}")
        elif alias_path.is_file() and alias_path.read_bytes() != canonical_path.read_bytes():
            errors.append(f"alias differs from canonical artifact: {alias}")

    ota0_offsets, partition_errors = _decode_ota0(build_dir / "partitions.bin")
    errors.extend(partition_errors)
    if ota0_offsets != [APP_OFFSET]:
        rendered = ", ".join(hex(offset) for offset in ota0_offsets) or "none"
        errors.append(
            f"partitions.bin: decoded ota_0 must be exactly 0x20000; got {rendered}"
        )
    if partition_source is not None:
        partition_source = Path(partition_source)
        source_offset, source_error = _csv_ota0_offset(partition_source)
        if source_error:
            errors.append(source_error)
        elif source_offset != APP_OFFSET or source_offset not in ota0_offsets:
            errors.append(
                "partition source and decoded partitions.bin ota_0 must match 0x20000"
            )
        full_table_error = _verify_full_partition_table(
            partition_source, build_dir / "partitions.bin"
        )
        if full_table_error:
            errors.append(full_table_error)
    return errors


def _sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--partition-source", type=Path)
    parser.add_argument("--sdkconfig", type=Path)
    parser.add_argument("--canary-production-build-dir", type=Path)
    parser.add_argument("--materialize", action="store_true")
    args = parser.parse_args()
    if args.materialize:
        materialize_badge_uplink_aliases(args.build_dir)
    errors = verify_badge_uplink_build(args.build_dir, args.partition_source)
    is_canary = args.build_dir.name.endswith("-con-crud-canary")
    if is_canary:
        if args.sdkconfig is None:
            errors.append(
                "canary verification requires --sdkconfig"
            )
        if args.canary_production_build_dir is None:
            errors.append(
                "canary verification requires "
                "--canary-production-build-dir"
            )
        if (
            args.sdkconfig is not None and
            args.canary_production_build_dir is not None
        ):
            errors.extend(verify_badge_uplink_canary_acceptance(
                args.build_dir,
                args.sdkconfig,
                args.canary_production_build_dir,
            ))
    elif args.canary_production_build_dir is not None:
        errors.append(
            "--canary-production-build-dir is only valid for a "
            "CON CRUD canary build"
        )
    if errors:
        for error in errors:
            print(f"ERROR: {error}")
        return 1
    print("badge uplink manifests: strict verification passed")
    for name in ("firmware.bin", "partitions.bin", *ALIASES):
        path = args.build_dir / name
        print(f"sha256 {name} {_sha256(path)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
