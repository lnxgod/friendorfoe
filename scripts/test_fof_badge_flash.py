#!/usr/bin/env python3
"""Small stdlib tests for badge-only flasher guardrails."""

from __future__ import annotations

import binascii
import copy
import contextlib
import dataclasses
import hashlib
import inspect
import io
import json
import os
import stat
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from types import ModuleType, SimpleNamespace
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fof_badge_flash as flash
import secure_artifact_tree as artifact_tree


_THEME_ACCENT_ORDER = (
    "drone", "meta", "tracker", "flock", "wifi_attack", "clear",
)
_DEFAULT_THEME_ACCENTS = {
    "drone": 0xFEA0,
    "meta": 0xF833,
    "tracker": 0xF81F,
    "flock": 0xA81F,
    "wifi_attack": 0x07FF,
    "clear": 0x2F65,
}
_BADGE_RELAY_CHUNK_BYTES = 1024


def _usb_record(
    device: str,
    serial_number: str = "e0:72:a1:f9:47:fc",
    *,
    location: str | None = None,
) -> flash.UsbDescriptorRecord:
    """Synthetic immutable descriptor for host-only protocol tests."""
    path_suffix = hashlib.sha256(device.encode("utf-8")).hexdigest()[:8]
    location_suffix = hashlib.sha256(
        serial_number.lower().encode("ascii")
    ).hexdigest()[:8]
    return flash.UsbDescriptorRecord(
        device=device,
        vid=flash.ESPRESSIF_USB_SERIAL_JTAG_VID,
        pid=flash.ESPRESSIF_USB_SERIAL_JTAG_PID,
        serial_number=serial_number.lower(),
        location=location or f"test-{location_suffix}",
        stat_device=1,
        stat_inode=int(path_suffix, 16),
        stat_rdev=int(path_suffix, 16),
    )


def _usb_census(
    paths: list[str] | tuple[str, ...],
    descriptor_ids: dict[str, str] | None = None,
) -> tuple[flash.UsbDescriptorRecord, ...]:
    identities = descriptor_ids or {}
    return tuple(sorted(
        (
            _usb_record(path, identities.get(
                path, "e0:72:a1:f9:47:fc"
            ))
            for path in paths
        ),
        key=lambda record: record.device,
    ))


def _test_badge_theme_hash(theme: dict) -> int:
    value = 2166136261

    def add(byte: int) -> None:
        nonlocal value
        value = ((value ^ byte) * 16777619) & 0xFFFFFFFF

    add(theme["version"])
    add(theme["brightness"])
    for byte in theme["palette"].encode("ascii"):
        add(byte)
    add(0)
    for byte in theme["background"].encode("ascii"):
        add(byte)
    add(0)
    for key in _THEME_ACCENT_ORDER:
        color = theme["accents"][key]
        add(color >> 8)
        add(color & 0xFF)
    return value


def _test_badge_theme(*, brightness: int = 100,
                      palette: str = "field",
                      background: str = "dark",
                      accents: dict | None = None) -> dict:
    return {
        "version": 1,
        "palette": palette,
        "background": background,
        "brightness": brightness,
        "accents": dict(accents or _DEFAULT_THEME_ACCENTS),
    }


def _firmware_image(project: str, version: str, *markers: str) -> bytes:
    image = bytearray(0x20 + 112)
    image[0] = 0xE9
    struct.pack_into("<I", image, 0x20, 0xABCD5432)
    image[0x30:0x50] = version.encode("ascii").ljust(32, b"\x00")
    image[0x50:0x70] = project.encode("ascii").ljust(32, b"\x00")
    for marker in markers:
        image.extend(b"\x00" + marker.encode("ascii") + b"\x00")
    return bytes(image)


def _frozen_firmware_set(data: bytes) -> artifact_tree.FrozenArtifactSet:
    receipt_sha256 = "0" * 64
    member = artifact_tree.FrozenArtifactMember(
        logical_name="artifact.firmware",
        size=len(data),
        sha256=hashlib.sha256(data).hexdigest(),
        content=data,
    )
    aggregate = hashlib.sha256()
    aggregate.update(b"FOF-FROZEN-ARTIFACT-SET-v1\x00")
    aggregate.update(bytes.fromhex(receipt_sha256))
    aggregate.update((1).to_bytes(4, "big"))
    logical = member.logical_name.encode("utf-8")
    aggregate.update(len(logical).to_bytes(4, "big"))
    aggregate.update(logical)
    aggregate.update(member.size.to_bytes(8, "big"))
    aggregate.update(bytes.fromhex(member.sha256))
    aggregate.update(member.content)
    return artifact_tree.FrozenArtifactSet(
        receipt_sha256=receipt_sha256,
        members=(member,),
        aggregate_sha256=aggregate.hexdigest(),
    )


def _frozen_artifact_set(
    contents: dict[str, bytes],
    *,
    source_bindings: dict[str, str] | None = None,
) -> artifact_tree.FrozenArtifactSet:
    members = tuple(
        artifact_tree.FrozenArtifactMember(
            logical_name=logical_name,
            size=len(content),
            sha256=hashlib.sha256(content).hexdigest(),
            content=content,
        )
        for logical_name, content in sorted(contents.items())
    )
    authority = None
    if source_bindings is None:
        receipt_sha256 = "1" * 64
    else:
        if set(source_bindings) != set(contents):
            raise ValueError("test descriptor bindings must be exact")
        verified_files: list[artifact_tree.VerifiedSnapshotFile] = []
        for index, member in enumerate(members, 1):
            source = artifact_tree.RegularFileIdentity(
                relative=source_bindings[member.logical_name],
                device=1,
                inode=index,
                mode=stat.S_IFREG | 0o644,
                nlink=1,
                size=member.size,
                ctime_ns=index,
                mtime_ns=index,
                sha256=member.sha256,
            )
            private = artifact_tree.RegularFileIdentity(
                relative=f"{index:03d}.artifact",
                device=2,
                inode=index,
                mode=stat.S_IFREG | 0o600,
                nlink=1,
                size=member.size,
                ctime_ns=index,
                mtime_ns=index,
                sha256=member.sha256,
            )
            verified_files.append(artifact_tree.VerifiedSnapshotFile(
                logical_name=member.logical_name,
                private_relative=private.relative,
                source=source,
                private=private,
            ))
        files = tuple(verified_files)
        receipt = artifact_tree._canonical_receipt(files)
        receipt_sha256 = hashlib.sha256(receipt).hexdigest()
        authority = artifact_tree.DescriptorRootedFreezeAuthority(
            token=artifact_tree._FROZEN_AUTHORITY_TOKEN,
            receipt_bytes=receipt,
            files=files,
        )
    return artifact_tree.FrozenArtifactSet(
        receipt_sha256=receipt_sha256,
        members=members,
        aggregate_sha256=artifact_tree._aggregate_sha256(
            receipt_sha256, members
        ),
        authority=authority,
    )


def _attested_uplink_elf_and_image(
    version: str,
) -> tuple[bytes, bytes]:
    """Minimal ESP32-S3 ELF/image pair satisfying the release attestation."""
    appdesc_address = 0x3C000020
    text_address = 0x40374000
    rtc_address = 0x50000000
    appdesc = bytearray(0x140)
    struct.pack_into("<I", appdesc, 0, 0xABCD5432)
    appdesc[16:48] = version.encode("ascii").ljust(32, b"\x00")
    appdesc[48:80] = b"fof_badge_uplink".ljust(32, b"\x00")
    appdesc[0xB0:0xD0] = (
        b"uplink-s3-fof_badge".ljust(32, b"\x00")
    )
    appdesc[0xD0:0xF0] = (
        b"seeed_xiao_esp32s3".ljust(32, b"\x00")
    )
    text = b"\x11\x22\x33\x44"
    rtc_size = 0x14

    symbol_records = (
        ("g_fof_badge_rtc_state", rtc_address, rtc_size, 0x11),
        ("fof_badge_rtc_usb_recovery_once_magic", rtc_address, 0, 0x10),
        (
            "fof_badge_rtc_expected_reboot_generation",
            rtc_address + 4,
            0,
            0x10,
        ),
        (
            "fof_badge_rtc_expected_reboot_magic",
            rtc_address + 8,
            0,
            0x10,
        ),
    )
    strings = bytearray(b"\x00")
    string_offsets: dict[str, int] = {}
    for name, *_rest in symbol_records:
        string_offsets[name] = len(strings)
        strings.extend(name.encode("ascii"))
        strings.append(0)
    symbols = bytearray(b"\x00" * 16)
    for name, value, size, info in symbol_records:
        symbols.extend(struct.pack(
            "<IIIBBH",
            string_offsets[name],
            value,
            size,
            info,
            0,
            3,
        ))

    section_names = (
        ".flash.appdesc",
        ".text",
        ".rtc_noinit",
        ".strtab",
        ".symtab",
        ".shstrtab",
    )
    shstrings = bytearray(b"\x00")
    shoffsets: dict[str, int] = {}
    for name in section_names:
        shoffsets[name] = len(shstrings)
        shstrings.extend(name.encode("ascii"))
        shstrings.append(0)

    program_header_offset = 52
    payload = bytearray(b"\x00" * (52 + 3 * 32))

    def append(data: bytes, alignment: int = 4) -> tuple[int, int]:
        while len(payload) % alignment:
            payload.append(0)
        offset = len(payload)
        payload.extend(data)
        return offset, len(data)

    appdesc_offset, appdesc_size = append(bytes(appdesc), 16)
    text_offset, text_size = append(text)
    strtab_offset, strtab_size = append(bytes(strings))
    symtab_offset, symtab_size = append(bytes(symbols))
    shstrtab_offset, shstrtab_size = append(bytes(shstrings))
    section_header_offset = (len(payload) + 3) & ~3
    payload.extend(b"\x00" * (section_header_offset - len(payload)))
    section_headers = (
        (0, 0, 0, 0, 0, 0, 0, 0, 0, 0),
        (
            shoffsets[".flash.appdesc"], 1, 0x2, appdesc_address,
            appdesc_offset, appdesc_size, 0, 0, 16, 0,
        ),
        (
            shoffsets[".text"], 1, 0x6, text_address,
            text_offset, text_size, 0, 0, 4, 0,
        ),
        (
            shoffsets[".rtc_noinit"], 8, 0x3, rtc_address,
            0, rtc_size, 0, 0, 4, 0,
        ),
        (
            shoffsets[".strtab"], 3, 0, 0,
            strtab_offset, strtab_size, 0, 0, 1, 0,
        ),
        (
            shoffsets[".symtab"], 2, 0, 0,
            symtab_offset, symtab_size, 4, 1, 4, 16,
        ),
        (
            shoffsets[".shstrtab"], 3, 0, 0,
            shstrtab_offset, shstrtab_size, 0, 0, 1, 0,
        ),
    )
    for header in section_headers:
        payload.extend(struct.pack("<IIIIIIIIII", *header))
    for index, program_header in enumerate((
        (
            1, appdesc_offset, appdesc_address, appdesc_address,
            appdesc_size, appdesc_size, 4, 4,
        ),
        (
            1, text_offset, text_address, text_address,
            text_size, text_size, 5, 4,
        ),
        (
            1, 0, rtc_address, rtc_address,
            0, rtc_size, 6, 4,
        ),
    )):
        struct.pack_into(
            "<IIIIIIII",
            payload,
            program_header_offset + index * 32,
            *program_header,
        )
    payload[:16] = (
        b"\x7fELF\x01\x01\x01\x00"
        b"\x00\x00\x00\x00\x00\x00\x00\x00"
    )
    struct.pack_into(
        "<HHIIIIIHHHHHH",
        payload,
        16,
        2,
        94,
        1,
        text_address,
        program_header_offset,
        section_header_offset,
        0,
        52,
        32,
        3,
        40,
        len(section_headers),
        6,
    )
    elf = bytes(payload)

    patched_appdesc = bytearray(appdesc)
    patched_appdesc[0x90:0xB0] = hashlib.sha256(elf).digest()
    image = bytearray(
        struct.pack("<BBBBI", 0xE9, 2, 2, 0x3F, text_address)
    )
    image.extend(struct.pack(
        "<BBBBHBHH4sB",
        0xEE,
        0,
        0,
        0,
        9,
        0,
        0,
        0xFFFF,
        b"\x00" * 4,
        1,
    ))
    image.extend(struct.pack(
        "<II", appdesc_address, len(patched_appdesc)
    ))
    image.extend(patched_appdesc)
    image.extend(struct.pack("<II", text_address, len(text)))
    image.extend(text)
    checksum = 0xEF
    for value in (*patched_appdesc, *text):
        checksum ^= value
    image.extend(b"\x00" * (15 - len(image) % 16))
    image.append(checksum)
    image.extend(hashlib.sha256(image).digest())
    return elf, bytes(image)


def _frozen_uplink_set(
    version: str,
) -> artifact_tree.FrozenArtifactSet:
    elf, firmware = _attested_uplink_elf_and_image(version)
    partition = _rom_partition_table()
    bootloader = b"\xe9\x03\x02\x3f" + b"\0" * 28
    ota_data = b"\xff" * 32
    full_manifest = (
        b"--flash_mode dio --flash_freq 80m --flash_size 8MB\n"
        b"0x0 bootloader/bootloader.bin\n"
        b"0x8000 partition_table/partition-table.bin\n"
        b"0xf000 ota_data_initial.bin\n"
        b"0x20000 fof_badge_uplink.bin\n"
    )
    app_manifest = b"--flash_mode dio\n0x20000 fof_badge_uplink.bin\n"
    flasher_json = json.dumps({
        "flash_files": {
            "0x0": "bootloader/bootloader.bin",
            "0x8000": "partition_table/partition-table.bin",
            "0xf000": "ota_data_initial.bin",
            "0x20000": "fof_badge_uplink.bin",
        },
        "bootloader": {
            "offset": "0x0",
            "file": "bootloader/bootloader.bin",
        },
        "app": {
            "offset": "0x20000",
            "file": "fof_badge_uplink.bin",
        },
        "partition-table": {
            "offset": "0x8000",
            "file": "partition_table/partition-table.bin",
        },
    }, sort_keys=True).encode("utf-8")
    sdkconfig = (
        b"CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y\n"
        b"CONFIG_ESPTOOLPY_FLASHMODE_DIO=y\n"
        b"CONFIG_ESPTOOLPY_FLASHFREQ_80M=y\n"
        b"CONFIG_ESPTOOLPY_FLASHSIZE_8MB=y\n"
        b"CONFIG_PARTITION_TABLE_CUSTOM=y\n"
        b'CONFIG_PARTITION_TABLE_CUSTOM_FILENAME='
        b'"partitions_s3_fof_badge_8mb.csv"\n'
        b"# CONFIG_BT_ENABLED is not set\n"
    )
    contents = {
        "alias.application": firmware,
        "alias.bootloader": bootloader,
        "alias.partitions": partition,
        "artifact.bootloader": bootloader,
        "artifact.elf": elf,
        "artifact.firmware": firmware,
        "artifact.ota_data_initial": ota_data,
        "artifact.partitions": partition,
        "build.sdkconfig": sdkconfig,
        "manifest.flash_app_args": app_manifest,
        "manifest.flash_args": full_manifest,
        "manifest.flash_project_args": full_manifest,
        "manifest.flasher_args.json": flasher_json,
        "partition.csv": (
            b"ota_0,app,ota_0,0x20000,0x200000,\n"
        ),
        "partition.generated": partition,
        "partition.generator": b"#!/usr/bin/env python3\n",
    }
    environment = "uplink-s3-fof_badge"
    build_prefix = f"fixture/uplink/.pio/build/{environment}/"
    build_relatives = {
        "alias.application": "fof_badge_uplink.bin",
        "alias.bootloader": "bootloader/bootloader.bin",
        "alias.partitions": "partition_table/partition-table.bin",
        "artifact.bootloader": "bootloader.bin",
        "artifact.elf": "firmware.elf",
        "artifact.firmware": "firmware.bin",
        "artifact.ota_data_initial": "ota_data_initial.bin",
        "artifact.partitions": "partitions.bin",
        "manifest.flash_app_args": "flash_app_args",
        "manifest.flash_args": "flash_args",
        "manifest.flash_project_args": "flash_project_args",
        "manifest.flasher_args.json": "flasher_args.json",
    }
    source_bindings = {
        logical_name: build_prefix + relative
        for logical_name, relative in build_relatives.items()
    }
    source_bindings.update({
        "build.sdkconfig": f"fixture/uplink/sdkconfig.{environment}",
        "partition.csv": (
            "fixture/uplink/partitions_s3_fof_badge_8mb.csv"
        ),
        "partition.generated": "generated/partition.generated",
        "partition.generator": (
            "fixture/components/partition_table/gen_esp32part.py"
        ),
    })
    return _frozen_artifact_set(
        contents,
        source_bindings=source_bindings,
    )


def _bound_rom_stage(
    descriptor: flash.UsbDescriptorRecord,
    artifacts: artifact_tree.FrozenArtifactSet,
    version: str,
) -> flash.RomFlashStageEvidence:
    hashes_by_name = {
        member.logical_name: member.sha256 for member in artifacts.members
    }
    member_hashes = tuple(
        (offset, logical_name, hashes_by_name[logical_name])
        for offset, logical_name in (
            (0x00000, "artifact.bootloader"),
            (0x08000, "artifact.partitions"),
            (0x0F000, "artifact.ota_data_initial"),
            (0x20000, "artifact.firmware"),
        )
    )
    return flash.RomFlashStageEvidence(
        descriptor=descriptor,
        base_mac=descriptor.serial_number,
        layout_version=version,
        aggregate_sha256=artifacts.aggregate_sha256,
        probe=flash.RomIdentityEvidence(
            descriptor_serial=descriptor.serial_number,
            base_mac=descriptor.serial_number,
            chip_name="ESP32-S3",
            revision="v0.2",
            flash_size="8MB",
            psram_size="8MB",
        ),
        write=flash.RomOperationEvidence(
            operation="write",
            base_mac=descriptor.serial_number,
            aggregate_sha256=artifacts.aggregate_sha256,
            member_sha256=member_hashes,
        ),
        verify=flash.RomOperationEvidence(
            operation="verify",
            base_mac=descriptor.serial_number,
            aggregate_sha256=artifacts.aggregate_sha256,
            member_sha256=member_hashes,
        ),
        run=flash.RomOperationEvidence(
            operation="run",
            base_mac=descriptor.serial_number,
            aggregate_sha256=artifacts.aggregate_sha256,
            member_sha256=(),
        ),
    )


def _test_frozen_usb_artifacts(
    version: str = "0.64.78-badge-defcon34",
) -> flash.FrozenUsbFirmwareArtifacts:
    platform = flash.PLATFORMS["badge-trio-xiao-s3"]
    return flash.FrozenUsbFirmwareArtifacts(
        uplink=_frozen_uplink_set(version),
        scanner=_frozen_firmware_set(_firmware_image(
            platform["scanner_project"],
            version,
            platform["scanner_name"],
            platform["hardware_type"],
        )),
    )


def _scanner_status(platform: dict, version: str, *,
                    hardware_id: str = "E0:72:A1:F9:48:58",
                    slot: str = "ble") -> dict:
    ble_primary = slot == "ble"
    expected_profile = "ble_primary" if ble_primary else "wifi_primary"
    return {
        "recovery_mode": "normal",
        "safe_mode": False,
        "usb_control_alive": True,
        "scanner_uart_alive": True,
        "scanners": [{
            "uart": slot,
            "connected": True,
            "board": platform["scanner_name"],
            "firmware_name": platform["scanner_name"],
            "app_project": platform["scanner_project"],
            "hardware_type": platform["hardware_type"],
            "hardware_id": hardware_id,
            "ver": version,
            "rollback_pending": False,
            "recovery_mode": "normal",
            "health": "ok",
            "ota_state": "idle",
            "slot_role": expected_profile,
            "expected_scan_profile": expected_profile,
            "scan_profile": expected_profile,
            "role_acked": True,
            "ble_initialized": ble_primary,
            "ble_scanning": ble_primary,
            "ble_host_active": ble_primary,
            "ble_host_synced": ble_primary,
            "wifi_paused": ble_primary,
            "wifi_initialized": not ble_primary,
            "wifi_init_rc": 0,
            "wifi_active": not ble_primary,
            "full_scan_ok": 0 if ble_primary else 1,
        }],
    }


def _stage_receipt(platform: dict, version: str, data: bytes, slot_mask: int,
                   *, generation: int | None = None) -> dict:
    receipt = {
        "ok": True,
        "target": platform["scanner_name"],
        "name": platform["scanner_name"],
        "app_project": platform["scanner_project"],
        "project": platform["scanner_project"],
        "hardware_type": platform["hardware_type"],
        "hardware": platform["hardware_type"],
        "version": version,
        "size": len(data),
        "crc32": binascii.crc32(data) & 0xFFFFFFFF,
        "sha256": hashlib.sha256(data).hexdigest(),
        "slot_mask": slot_mask,
    }
    if generation is not None:
        receipt["generation"] = generation
    return receipt


def _credit_stage_receipt(
    platform: dict,
    version: str,
    data: bytes,
    slot_mask: int,
    *,
    phase: str,
    received: int,
    credit: int,
    generation: int | None = None,
) -> dict:
    receipt = _stage_receipt(
        platform, version, data, slot_mask, generation=generation
    )
    receipt.update({
        "flow_control": "credit-v1",
        "phase": phase,
        "received": received,
        "total": len(data),
        "credit_bytes": credit,
    })
    return receipt


def _bound_relay_receipt(
    slot: str,
    generation: int,
    hardware_id: str,
    size: int,
) -> dict:
    return {
        "ok": True,
        "phase": "final",
        "slot": slot,
        "uart": slot,
        "generation": generation,
        "hardware_id": hardware_id.lower(),
        "size": size,
        "bytes": size,
        "chunks": (
            size + _BADGE_RELAY_CHUNK_BYTES - 1
        ) // _BADGE_RELAY_CHUNK_BYTES,
        "stage": "done",
        "done": True,
        "error": "",
    }


def _uplink_status(version: str, *,
                   hardware_id: str = "E0:72:A1:F9:47:FC",
                   partition: str = "ota_0",
                   responses: int = 20,
                   pending_verify: bool = False,
                   recovery_mode: str | None = None) -> dict:
    if recovery_mode is None:
        recovery_mode = "startup_dependency" if pending_verify else "normal"
    theme = _test_badge_theme()
    return {
        "version": version,
        "target": "uplink-s3-fof_badge",
        "firmware_name": "uplink-s3-fof_badge",
        "project": "fof_badge_uplink",
        "app_project": "fof_badge_uplink",
        "hardware_type": "seeed_xiao_esp32s3",
        "hardware_id": hardware_id,
        "running_partition": partition,
        "pending_verify": pending_verify,
        "rollback_state": "pending_verify" if pending_verify else "clear",
        "recovery_mode": recovery_mode,
        "usb_health": {"responses_completed": responses},
        "theme_hash": _test_badge_theme_hash(theme),
        "theme": theme,
    }


def _update_maintenance_status(
    version: str,
    *,
    session: str,
    hardware_id: str = "E0:72:A1:F9:47:FC",
    partition: str = "ota_0",
    responses: int = 20,
) -> dict:
    status = _uplink_status(
        version,
        hardware_id=hardware_id,
        partition=partition,
        responses=responses,
        recovery_mode="update_maintenance",
    )
    status.update({
        "update_session": session,
        "ble_initialized": False,
        "stack_main_free": 4096,
        "stack_uart_ble_free": 4096,
        "stack_uart_wifi_free": 4096,
        "update_uplink": {
            "phase": "idle",
            "session": session,
            "version": "",
            "sha256": "",
            "size": 0,
            "partition": "",
            "received": 0,
        },
        "update_scanner": {
            "phase": "idle",
            "session": session,
            "target": "",
            "sha256": "",
            "size": 0,
            "slot_mask": 0,
            "received": 0,
            "generation": 0,
        },
    })
    return status


def _update_preparing_status(
    version: str,
    *,
    session: str,
    hardware_id: str = "E0:72:A1:F9:47:FC",
    partition: str = "ota_0",
    responses: int = 20,
) -> dict:
    status = _uplink_status(
        version,
        hardware_id=hardware_id,
        partition=partition,
        responses=responses,
        recovery_mode="update_preparing",
    )
    status["update_session"] = session
    return status


def _legacy_uplink_status(
    version: str = "0.64.76-badge-defcon34",
) -> dict:
    return {
        "version": version,
        "firmware_name": "uplink-s3-fof_badge",
        "app_project": "fof_badge_uplink",
        "hardware_type": "seeed_xiao_esp32s3",
        "pending_verify": False,
        "recovery_mode": "normal",
        "safe_mode": False,
        "scanner_uart_alive": False,
        "usb_control_alive": True,
    }


def _uplink_receipt(phase: str, *, partition: str = "ota_1",
                    received: int = 0, total: int = 5000,
                    credit: int = 0, ok: bool = True,
                    retryable: bool = False,
                    reboot_required: bool = False,
                    error: str = "") -> dict:
    return {
        "ok": ok,
        "phase": phase,
        "partition": partition,
        "received": received,
        "total": total,
        "credit_bytes": credit,
        "retryable": retryable,
        "reboot_required": reboot_required,
        "error": error,
    }


def _badge_status_with_scanners(version: str, scanner_status: dict, *,
                                responses: int = 21) -> dict:
    status = _uplink_status(version, responses=responses)
    status.update(scanner_status)
    return status


def _post_uplink_evidence(version: str, *, responses: int = 20,
                          partition: str = "ota_0") -> \
        flash.PostUplinkApplicationEvidence:
    status = _uplink_status(
        version, responses=responses, partition=partition
    )
    return flash.verify_post_uplink_application(
        status,
        expected_hardware_id=status["hardware_id"],
        expected_version=version,
        expected_partition=partition,
    )


def _complete_mocked_theme_control(
    _badge, *, initial_status: dict,
    expectation: flash._PostUplinkExpectation,
    initial_evidence: flash.PostUplinkApplicationEvidence,
    restored_status_validator=None,
) -> flash.PostUplinkApplicationEvidence:
    restored = json.loads(json.dumps(initial_status))
    restored["usb_health"]["responses_completed"] = (
        initial_evidence.responses_completed + 1
    )
    if restored_status_validator is not None:
        restored_status_validator(restored)
    return flash.verify_post_uplink_application(
        restored,
        expected_hardware_id=expectation.expected_hardware_id,
        expected_version=expectation.expected_version,
        expected_partition=expectation.expected_partition,
    )


class _ScriptedRawSerial:
    def __init__(self, reads: list[bytes] | None = None) -> None:
        self.reads = list(reads or [])
        self.writes: list[bytes] = []
        self.read_calls = 0

    def read(self, _size: int) -> bytes:
        self.read_calls += 1
        return self.reads.pop(0) if self.reads else b""

    def write(self, payload: bytes) -> int:
        self.writes.append(bytes(payload))
        return len(payload)

    def flush(self) -> None:
        return None

    def reset_input_buffer(self) -> None:
        return None

    def close(self) -> None:
        return None


class BadgeFlashGuardrailTests(unittest.TestCase):
    def setUp(self) -> None:
        self.real_prepare_frozen_usb_artifacts = (
            flash._prepare_frozen_usb_firmware_artifacts
        )
        self.frozen_usb_artifacts = _test_frozen_usb_artifacts()
        artifact_patcher = mock.patch.object(
            flash,
            "_prepare_frozen_usb_firmware_artifacts",
            return_value=self.frozen_usb_artifacts,
        )
        artifact_patcher.start()
        self.addCleanup(artifact_patcher.stop)

        # Legacy flow tests below predate the explicit three-cable role
        # acknowledgement.  Keep their downstream protocol fixtures focused;
        # C1-C3 binding behavior has dedicated tests in
        # test_usb_descriptor_binding.py.
        def selected_descriptor(
            *,
            selected_port: str | None,
            operator_acknowledged: bool,
            trusted_binding=None,
        ):
            del operator_acknowledged, trusted_binding
            port = selected_port or flash.detect_usb_port()
            record = _usb_record(port)
            return record, flash.TrustedUplinkBinding(
                serial_number=record.serial_number,
                location=None,
                source="operator-selection",
            )

        patcher = mock.patch.object(
            flash,
            "select_trusted_uplink_descriptor",
            side_effect=selected_descriptor,
        )
        patcher.start()
        self.addCleanup(patcher.stop)

    def test_usb_flow_freezes_all_selected_artifacts_before_descriptor_open(
        self,
    ) -> None:
        class DescriptorReached(Exception):
            pass

        events: list[str] = []
        private_parents: list[Path] = []
        real_attest = flash._attest_frozen_uplink_flash_authority

        class Snapshot:
            def __init__(self, role: str) -> None:
                self.role = role

            def freeze_for_mutation(self):
                events.append(f"{self.role}.freeze")
                return (
                    _frozen_uplink_set("0.64.78-badge-defcon34")
                    if self.role == "uplink"
                    else _frozen_firmware_set(
                        f"{self.role}-firmware".encode("ascii")
                    )
                )

            def close(self):
                events.append(f"{self.role}.close")

        def prepare(role: str):
            def create(*_args, private_parent: Path, **_kwargs):
                self.assertTrue(private_parent.is_dir())
                self.assertEqual(
                    os.path.realpath(private_parent),
                    os.fspath(private_parent),
                )
                private_parents.append(private_parent)
                events.append(f"{role}.prepare")
                return Snapshot(role)
            return create

        def select(**_kwargs):
            self.assertEqual(events, [
                "uplink.prepare",
                "uplink.freeze",
                "uplink.close",
                "scanner.prepare",
                "scanner.freeze",
                "scanner.close",
                "uplink.attest",
            ])
            self.assertTrue(all(
                not private_parent.exists()
                for private_parent in private_parents
            ))
            events.append("descriptor")
            raise DescriptorReached

        def attest(
            platform: dict,
            frozen: artifact_tree.FrozenArtifactSet,
            version: str,
        ) -> None:
            events.append("uplink.attest")
            real_attest(platform, frozen, version)

        args = SimpleNamespace(
            port="/dev/fake-uplink",
            platform="badge-trio-xiao-s3",
            dry_run=False,
            skip_command_probe=False,
            recovery_rewrite_same_version=False,
        )
        with mock.patch.object(
            flash,
            "_prepare_frozen_usb_firmware_artifacts",
            wraps=self.real_prepare_frozen_usb_artifacts,
        ), mock.patch.object(
            flash,
            "prepare_verified_badge_uplink_snapshot",
            side_effect=prepare("uplink"),
        ), mock.patch.object(
            flash,
            "prepare_verified_badge_scanner_snapshot",
            side_effect=prepare("scanner"),
        ), mock.patch.object(
            flash,
            "select_trusted_uplink_descriptor",
            side_effect=select,
        ), mock.patch.object(
            flash,
            "_attest_frozen_uplink_flash_authority",
            side_effect=attest,
        ), self.assertRaises(DescriptorReached):
            flash.usb_flow(
                args,
                flash.PLATFORMS["badge-trio-xiao-s3"],
                True,
                ["ble", "wifi"],
                "0.64.78-badge-defcon34",
            )

        self.assertEqual(events[-1], "descriptor")

    def test_supplied_frozen_uplink_without_elf_fails_before_usb_census(
        self,
    ) -> None:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        invalid = flash.FrozenUsbFirmwareArtifacts(
            uplink=_frozen_firmware_set(_firmware_image(
                platform["uplink_project"],
                "0.64.78-badge-defcon34",
                platform["uplink_name"],
                platform["hardware_type"],
            )),
            scanner=None,
        )
        args = SimpleNamespace(
            port="/dev/fake-uplink",
            platform="badge-trio-xiao-s3",
            dry_run=False,
            skip_command_probe=False,
            recovery_rewrite_same_version=False,
        )
        with mock.patch.object(
            flash,
            "select_trusted_uplink_descriptor",
            side_effect=AssertionError("USB census was reached"),
        ) as selector, self.assertRaisesRegex(
            flash.FlashError,
            "frozen uplink ELF/bin attestation failed",
        ):
            flash.usb_flow(
                args,
                platform,
                True,
                [],
                "0.64.78-badge-defcon34",
                frozen_artifacts=invalid,
            )

        selector.assert_not_called()

    def test_self_consistent_public_uplink_set_cannot_bypass_authority(
        self,
    ) -> None:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        issued = _frozen_uplink_set("0.64.78-badge-defcon34")
        public_copy = artifact_tree.FrozenArtifactSet(
            receipt_sha256=issued.receipt_sha256,
            members=issued.members,
            aggregate_sha256=issued.aggregate_sha256,
        )
        supplied = flash.FrozenUsbFirmwareArtifacts(
            uplink=public_copy,
            scanner=None,
        )
        args = SimpleNamespace(
            port="/dev/fake-uplink",
            platform="badge-trio-xiao-s3",
            dry_run=False,
            skip_command_probe=False,
            recovery_rewrite_same_version=False,
        )
        with mock.patch.object(
            flash,
            "select_trusted_uplink_descriptor",
            side_effect=AssertionError("USB census was reached"),
        ) as selector, self.assertRaisesRegex(
            flash.FlashError,
            "lacks descriptor-rooted authority",
        ):
            flash.usb_flow(
                args,
                platform,
                True,
                [],
                "0.64.78-badge-defcon34",
                frozen_artifacts=supplied,
            )

        selector.assert_not_called()

    def test_supplied_uplink_version_drift_fails_before_usb_census(
        self,
    ) -> None:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        supplied = flash.FrozenUsbFirmwareArtifacts(
            uplink=_frozen_uplink_set("0.64.78-badge-defcon34"),
            scanner=None,
        )
        args = SimpleNamespace(
            port="/dev/fake-uplink",
            platform="badge-trio-xiao-s3",
            dry_run=False,
            skip_command_probe=False,
            recovery_rewrite_same_version=False,
        )
        with mock.patch.object(
            flash,
            "select_trusted_uplink_descriptor",
            side_effect=AssertionError("USB census was reached"),
        ) as selector, self.assertRaisesRegex(
            flash.FlashError,
            "selected platform identity",
        ):
            flash.usb_flow(
                args,
                platform,
                True,
                [],
                "0.64.77-badge-defcon34",
                frozen_artifacts=supplied,
            )

        selector.assert_not_called()

    def test_usb_flow_validator_blocks_all_mutation_after_application_probe(
        self,
    ) -> None:
        class MutationBlocked(RuntimeError):
            pass

        version = "0.64.78-badge-defcon34"
        initial = _uplink_status(version)
        args = SimpleNamespace(
            port="/dev/fake-uplink",
            platform="badge-trio-xiao-s3",
            dry_run=False,
            skip_command_probe=False,
            recovery_rewrite_same_version=False,
        )
        observed: list[tuple[dict, flash.FrozenUsbFirmwareArtifacts]] = []

        def reject_mutation(
            application_status: dict | None,
            artifacts: flash.FrozenUsbFirmwareArtifacts,
        ) -> None:
            self.assertIs(application_status, initial)
            self.assertIs(artifacts, self.frozen_usb_artifacts)
            observed.append((application_status, artifacts))
            raise MutationBlocked

        with mock.patch.object(
            flash, "probe_application", return_value=initial
        ), mock.patch.object(
            flash, "_prepare_frozen_usb_firmware_artifacts"
        ) as prepare_artifacts, mock.patch.object(
            flash, "BadgeSerial"
        ) as badge_serial, self.assertRaises(MutationBlocked):
            flash.usb_flow(
                args,
                flash.PLATFORMS["badge-trio-xiao-s3"],
                True,
                ["ble", "wifi"],
                version,
                pre_mutation_validator=reject_mutation,
                frozen_artifacts=self.frozen_usb_artifacts,
            )

        self.assertEqual(len(observed), 1)
        prepare_artifacts.assert_not_called()
        badge_serial.assert_not_called()

    def test_usb_artifact_freeze_failure_closes_every_snapshot_and_root(
        self,
    ) -> None:
        events: list[str] = []
        private_parents: list[Path] = []

        class Snapshot:
            def __init__(self, role: str, fail: bool) -> None:
                self.role = role
                self.fail = fail

            def freeze_for_mutation(self):
                events.append(f"{self.role}.freeze")
                if self.fail:
                    raise artifact_tree.SecureArtifactError(
                        "scheduled scanner freeze failure"
                    )
                return _frozen_firmware_set(b"uplink")

            def close(self):
                events.append(f"{self.role}.close")

        def prepare(role: str, fail: bool):
            def create(*_args, private_parent: Path, **_kwargs):
                private_parents.append(private_parent)
                return Snapshot(role, fail)
            return create

        with mock.patch.object(
            flash,
            "prepare_verified_badge_uplink_snapshot",
            side_effect=prepare("uplink", False),
        ), mock.patch.object(
            flash,
            "prepare_verified_badge_scanner_snapshot",
            side_effect=prepare("scanner", True),
        ), self.assertRaisesRegex(
            flash.FlashError, "artifact freeze failed"
        ):
            self.real_prepare_frozen_usb_artifacts(
                flash.PLATFORMS["badge-trio-xiao-s3"],
                True,
                ["ble", "wifi"],
            )

        self.assertEqual(events, [
            "uplink.freeze",
            "uplink.close",
            "scanner.freeze",
            "scanner.close",
        ])
        self.assertTrue(private_parents)
        self.assertTrue(all(
            not private_parent.exists()
            for private_parent in private_parents
        ))

    def test_usb_scanner_flow_result_cannot_be_caller_constructed(self) -> None:
        self.assertTrue(
            flash.UsbScannerFlowResult.__dataclass_params__.frozen
        )
        with self.assertRaisesRegex(TypeError, "production-issued"):
            flash.UsbScannerFlowResult(
                pre_stage_status={},
                final_restored_status={},
                stage_receipt={},
                preflight_older_slots=frozenset(),
                recovery_slots=frozenset(),
                stage_count=1,
                theme_restored=True,
                fresh_usb_proven=True,
            )
        self.assertFalse(
            hasattr(flash, "_issue_usb_scanner_flow_result")
        )

        fabricated = object.__new__(flash.UsbScannerFlowResult)
        with self.assertRaises(flash.FlashError):
            flash._revalidate_usb_scanner_flow_result(fabricated)
        with self.assertRaises(flash.FlashError):
            _ = fabricated.stage_count
        with self.assertRaises(flash.FlashError):
            _ = fabricated.stage_receipts
        with self.assertRaises(flash.FlashError):
            _ = fabricated.attempt_history
        with self.assertRaises(AttributeError):
            object.__setattr__(fabricated, "stage_count", 1)

    def test_badge_serial_exposes_safe_reconnect(self) -> None:
        self.assertTrue(hasattr(flash.BadgeSerial, "reconnect"))

    def test_badge_serial_reconnect_binds_by_hardware_id_not_old_path(self) -> None:
        badge = flash.BadgeSerial(
            _usb_record(
                "/dev/old-uplink", location="test-uplink-location"
            ), dry_run=False,
            expected_hardware_id="E0:72:A1:F9:47:FC",
        )
        events: list[str] = []
        badge._close_serial = lambda: events.append("close")  # type: ignore[method-assign]
        badge._open_serial = lambda: events.append("open")  # type: ignore[method-assign]
        badge.status = lambda *_args, **_kwargs: _uplink_status(  # type: ignore[method-assign]
            "0.64.76-badge-defcon34"
        )

        with mock.patch.object(
            flash,
            "wait_for_application_port",
            return_value=(
                _usb_record(
                    "/dev/new-uplink", location="test-uplink-location"
                ),
                _uplink_status("0.64.76-badge-defcon34"),
            ),
        ) as wait_port:
            badge.reconnect(timeout_s=12)

        self.assertEqual(events, ["close", "open"])
        self.assertEqual(badge.port, "/dev/new-uplink")
        wait_port.assert_called_once()
        wait_args, wait_kwargs = wait_port.call_args
        self.assertEqual(wait_args, ("e0:72:a1:f9:47:fc",))
        self.assertGreater(wait_kwargs["timeout_s"], 0)
        self.assertLessEqual(wait_kwargs["timeout_s"], 12)

    def test_badge_serial_reconnect_reduces_one_budget_after_each_phase(
        self,
    ) -> None:
        clock = SimpleNamespace(now=0.0)
        badge = flash.BadgeSerial(
            _usb_record(
                "/dev/old-uplink", location="test-uplink-location"
            ), dry_run=False,
            expected_hardware_id="E0:72:A1:F9:47:FC",
        )
        discovery_budgets: list[float] = []
        status_budgets: list[float] = []

        def close() -> None:
            clock.now += 0.5

        def discover(
            _hardware_id: str, timeout_s: float
        ) -> tuple[flash.UsbDescriptorRecord, dict]:
            discovery_budgets.append(timeout_s)
            clock.now += 3.0
            return (
                _usb_record(
                    "/dev/new-uplink", location="test-uplink-location"
                ),
                _uplink_status("0.64.76-badge-defcon34"),
            )

        def open_serial() -> None:
            clock.now += 2.0

        def status(timeout_s: float) -> dict:
            status_budgets.append(timeout_s)
            return _uplink_status("0.64.76-badge-defcon34")

        badge._close_serial = close  # type: ignore[method-assign]
        badge._open_serial = open_serial  # type: ignore[method-assign]
        badge.status = status  # type: ignore[method-assign]
        with mock.patch.object(
            flash.time, "monotonic", side_effect=lambda: clock.now
        ), mock.patch.object(
            flash, "wait_for_application_port", side_effect=discover
        ):
            badge.reconnect(timeout_s=10)

        self.assertEqual(discovery_budgets, [9.5])
        self.assertEqual(status_budgets, [4.5])

    def test_badge_serial_reconnect_rejects_location_drift_before_open(
        self,
    ) -> None:
        badge = flash.BadgeSerial(
            _usb_record(
                "/dev/old-uplink", location="trusted-location"
            ),
            dry_run=False,
        )
        events: list[str] = []
        badge._close_serial = lambda: events.append("close")  # type: ignore[method-assign]
        badge._open_serial = lambda: events.append("open")  # type: ignore[method-assign]
        with mock.patch.object(
            flash,
            "wait_for_application_port",
            return_value=(
                _usb_record(
                    "/dev/new-uplink", location="wrong-location"
                ),
                _uplink_status("0.64.76-badge-defcon34"),
            ),
        ), self.assertRaisesRegex(flash.FlashError, "location"):
            badge.reconnect(timeout_s=12)
        self.assertEqual(events, ["close"])

    def test_badge_serial_reconnect_rejects_late_status_completion(
        self,
    ) -> None:
        clock = SimpleNamespace(now=0.0)
        badge = flash.BadgeSerial(
            _usb_record(
                "/dev/old-uplink", location="test-uplink-location"
            ), dry_run=False,
            expected_hardware_id="E0:72:A1:F9:47:FC",
        )
        close_calls = 0

        def close() -> None:
            nonlocal close_calls
            close_calls += 1

        def discover(
            _hardware_id: str, timeout_s: float
        ) -> tuple[flash.UsbDescriptorRecord, dict]:
            clock.now += 0.25
            return (
                _usb_record(
                    "/dev/new-uplink", location="test-uplink-location"
                ),
                _uplink_status("0.64.76-badge-defcon34"),
            )

        def status(timeout_s: float) -> dict:
            del timeout_s
            clock.now = 1.01
            return _uplink_status("0.64.76-badge-defcon34")

        badge._close_serial = close  # type: ignore[method-assign]
        badge._open_serial = lambda: None  # type: ignore[method-assign]
        badge.status = status  # type: ignore[method-assign]
        with mock.patch.object(
            flash.time, "monotonic", side_effect=lambda: clock.now
        ), mock.patch.object(
            flash, "wait_for_application_port", side_effect=discover
        ), self.assertRaises(flash.SerialReadTimeout):
            badge.reconnect(timeout_s=1)

        self.assertEqual(close_calls, 2)

    def test_badge_platform_declares_exact_target_project_and_hardware(self) -> None:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]

        self.assertEqual(platform["scanner_name"], "scanner-s3-combo-fof_badge")
        self.assertEqual(platform["scanner_project"], "fof_badge_scanner")
        self.assertEqual(platform["uplink_name"], "uplink-s3-fof_badge")
        self.assertEqual(platform["uplink_project"], "fof_badge_uplink")
        self.assertEqual(platform["hardware_type"], "seeed_xiao_esp32s3")
        self.assertEqual(platform["version_macro"], "FOF_VERSION_BADGE")

    def test_con_crud_canary_platform_is_explicit_and_default_stays_production(
        self,
    ) -> None:
        canary = flash.PLATFORMS[
            "badge-trio-xiao-s3-con-crud-canary"
        ]
        self.assertEqual(
            canary["uplink_env"],
            "uplink-s3-fof_badge-con-crud-canary",
        )
        self.assertEqual(
            canary["scanner_env"],
            "scanner-s3-combo-fof_badge-con-crud-canary",
        )
        self.assertEqual(canary["version_macro"], "FOF_VERSION_BADGE_CANARY")
        with mock.patch.object(
            sys, "argv", ["fof_badge_flash.py"]
        ):
            self.assertEqual(
                flash.parse_args().platform,
                "badge-trio-xiao-s3",
            )

    def test_repo_version_reads_only_selected_platform_macro(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            esp32_dir = Path(temp_dir)
            (esp32_dir / "shared").mkdir()
            (esp32_dir / "shared" / "version.h").write_text(
                '#define FOF_VERSION_BADGE "0.64.78-badge-defcon34"\n'
                '#define FOF_VERSION_BADGE_CANARY '
                '"0.64.79-badge-defcon34"\n'
            )
            with mock.patch.object(flash, "ESP32_DIR", esp32_dir):
                self.assertEqual(
                    flash.repo_version(
                        flash.PLATFORMS["badge-trio-xiao-s3"]
                    ),
                    "0.64.78-badge-defcon34",
                )
                self.assertEqual(
                    flash.repo_version(
                        flash.PLATFORMS[
                            "badge-trio-xiao-s3-con-crud-canary"
                        ]
                    ),
                    "0.64.79-badge-defcon34",
                )

    def test_badge_scanner_image_identity_is_verified_before_flash(self) -> None:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        version = "0.64.68-badge-live-follow"
        with tempfile.TemporaryDirectory() as temp_dir:
            image = Path(temp_dir) / "scanner.bin"
            image.write_bytes(_firmware_image(
                "fof_badge_scanner",
                version,
                "scanner-s3-combo-fof_badge",
                "seeed_xiao_esp32s3",
            ))

            flash.validate_firmware_artifact(
                image,
                target=platform["scanner_name"],
                project=platform["scanner_project"],
                hardware=platform["hardware_type"],
                version=version,
            )

    def test_badge_scanner_image_rejects_wrong_project_or_missing_hardware(self) -> None:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        version = "0.64.68-badge-live-follow"
        with tempfile.TemporaryDirectory() as temp_dir:
            image = Path(temp_dir) / "scanner.bin"
            image.write_bytes(_firmware_image(
                "fof_scanner",
                version,
                "scanner-s3-combo-fof_badge",
            ))

            with self.assertRaisesRegex(flash.FlashError, "project"):
                flash.validate_firmware_artifact(
                    image,
                    target=platform["scanner_name"],
                    project=platform["scanner_project"],
                    hardware=platform["hardware_type"],
                    version=version,
                )

            image.write_bytes(_firmware_image(
                "fof_badge_scanner",
                version,
                "scanner-s3-combo-fof_badge",
            ))
            with self.assertRaisesRegex(flash.FlashError, "hardware"):
                flash.validate_firmware_artifact(
                    image,
                    target=platform["scanner_name"],
                    project=platform["scanner_project"],
                    hardware=platform["hardware_type"],
                    version=version,
                )

    def test_manual_scanner_guard_precedes_all_platform_setup(self) -> None:
        source = Path(flash.__file__).read_text()
        main_body = source[source.index("def main()") :]

        self.assertLess(
            main_body.index('getattr(args, "manual_scanner", None)'),
            main_body.index("platform = PLATFORMS[args.platform]"),
        )

    def test_cli_manual_scanner_is_rejected_before_any_mutation_setup(
        self,
    ) -> None:
        args = SimpleNamespace(manual_scanner="wifi")
        forbidden_names = (
            "repo_version",
            "selected_targets",
            "require_usb_firmware_transport",
            "build_scanner_firmware",
            "build_firmware",
            "require_artifacts",
            "manual_scanner_flow",
            "flash_scanner_usb",
            "usb_flow",
            "network_flow",
        )
        patches = [
            mock.patch.object(
                flash,
                name,
                side_effect=AssertionError(f"{name} reached"),
            )
            for name in forbidden_names
        ]
        stderr = io.StringIO()
        with contextlib.ExitStack() as stack:
            stack.enter_context(
                mock.patch.object(flash, "parse_args", return_value=args)
            )
            spies = [stack.enter_context(patcher) for patcher in patches]
            with contextlib.redirect_stderr(stderr):
                result = flash.main()
        self.assertEqual(result, 1)
        self.assertIn("scanner", stderr.getvalue().lower())
        self.assertIn("disabled", stderr.getvalue().lower())
        for spy in spies:
            spy.assert_not_called()

    def test_require_artifacts_rejects_badge_scanner_layout_mismatch(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            build_dir = Path(temp_dir) / "scanner-build"
            build_dir.mkdir()
            scanner_bin = build_dir / "firmware.bin"
            scanner_bin.write_bytes(b"scanner")
            platform = dict(flash.PLATFORMS["badge-trio-xiao-s3"])
            platform["scanner_bin"] = scanner_bin
            verifier = mock.Mock(return_value=[
                "flash_app_args: application must be exactly 0x20000"
            ])

            with mock.patch.object(
                flash, "verify_badge_scanner_build", verifier, create=True
            ), mock.patch.object(
                flash, "repo_version", return_value="0.64.78-badge-defcon34"
            ), mock.patch.object(
                flash, "validate_firmware_artifact"
            ), self.assertRaisesRegex(
                flash.FlashError, "scanner build layout"
            ):
                flash.require_artifacts(platform, False, ["ble"])

            verifier.assert_called_once()

    def test_scanner_usb_upload_rechecks_strict_artifacts_before_pio(
        self,
    ) -> None:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        with mock.patch.object(
            flash,
            "require_artifacts",
            side_effect=flash.FlashError("strict scanner layout rejected"),
        ) as guard, mock.patch.object(
            flash, "find_pio", return_value="/fake/pio"
        ), mock.patch.object(
            flash, "run"
        ) as runner:
            with self.assertRaisesRegex(
                flash.FlashError, "strict scanner layout rejected"
            ):
                flash.flash_scanner_usb(
                    platform, "/dev/fake-scanner", False, "ble"
                )

        guard.assert_called_once_with(platform, False, ["ble"])
        runner.assert_not_called()

    def test_version_parser_accepts_v_prefix(self) -> None:
        self.assertTrue(flash.versions_match("v0.64.32", "0.64.32"))
        self.assertTrue(flash.versions_match("0.64.32", "v0.64.32"))

    def test_version_match_rejects_truncated_release_identity(self) -> None:
        self.assertFalse(
            flash.versions_match(
                "0.64.68-badge", "0.64.68-badge-live-follow"
            )
        )

    def test_badge_network_firmware_transport_is_refused(self) -> None:
        with self.assertRaisesRegex(flash.FlashError, "USB.*UART"):
            flash.require_usb_firmware_transport("ap")
        with self.assertRaisesRegex(flash.FlashError, "USB.*UART"):
            flash.require_usb_firmware_transport("lan")
        flash.require_usb_firmware_transport("usb")

    def test_direct_badge_network_mutators_reject_before_any_access(
        self,
    ) -> None:
        class ForbiddenArtifact:
            def __init__(self, events: list[str]) -> None:
                self.events = events

            def _forbid(self, operation: str):
                self.events.append(operation)
                raise AssertionError(f"artifact {operation} reached")

            def exists(self):
                return self._forbid("artifact.exists")

            def stat(self):
                return self._forbid("artifact.stat")

            def read_bytes(self):
                return self._forbid("artifact.read")

            def open(self, *_args, **_kwargs):
                return self._forbid("artifact.open")

            def __fspath__(self):
                return self._forbid("artifact.fspath")

        cases = (
            (
                "upload_scanner_network",
                lambda platform, dry_run: flash.upload_scanner_network(
                    platform,
                    "http://forbidden.invalid",
                    "0.64.76-badge-defcon34",
                    dry_run,
                ),
            ),
            (
                "relay_scanner_network",
                lambda _platform, dry_run: flash.relay_scanner_network(
                    "http://forbidden.invalid",
                    "wifi",
                    dry_run,
                    False,
                    False,
                ),
            ),
            (
                "flash_uplink_network",
                lambda platform, dry_run: flash.flash_uplink_network(
                    platform,
                    "http://forbidden.invalid",
                    dry_run,
                ),
            ),
        )

        for name, invoke in cases:
            for dry_run in (False, True):
                with self.subTest(entry=name, dry_run=dry_run):
                    events: list[str] = []
                    platform = dict(
                        flash.PLATFORMS["badge-trio-xiao-s3"]
                    )
                    forbidden = ForbiddenArtifact(events)
                    platform["scanner_bin"] = forbidden
                    platform["uplink_bin"] = forbidden

                    def forbid(operation: str):
                        def reached(*_args, **_kwargs):
                            events.append(operation)
                            raise AssertionError(f"{operation} reached")

                        return reached

                    with mock.patch.object(
                        flash,
                        "urlencode",
                        side_effect=forbid("urlencode"),
                    ), mock.patch.object(
                        flash,
                        "http_json",
                        side_effect=forbid("http_json"),
                    ), mock.patch.object(
                        flash,
                        "urlopen",
                        side_effect=forbid("urlopen"),
                    ), mock.patch(
                        "builtins.open",
                        side_effect=forbid("builtins.open"),
                    ), mock.patch.object(
                        flash,
                        "log",
                        side_effect=forbid("log"),
                    ):
                        with self.assertRaisesRegex(
                            flash.FlashError,
                            r"USB.*UART.*HTTP/AP/LAN firmware mutation "
                            r"is disabled",
                        ):
                            invoke(platform, dry_run)

                    self.assertEqual(events, [])

    def test_same_version_relay_is_skipped_by_default(self) -> None:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        status = _scanner_status(platform, "v0.64.32")

        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(
                flash.choose_relay_slots(status, platform, ["ble"], "0.64.32",
                                         recovery_rewrite_same_version=False,
                                         label="test"),
                [],
            )

    def test_same_version_relay_requires_explicit_recovery_flag(self) -> None:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        status = _scanner_status(platform, "v0.64.32")

        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(
                flash.choose_relay_slots(status, platform, ["ble"], "0.64.32",
                                         recovery_rewrite_same_version=True,
                                         label="test"),
                ["ble"],
            )

    def test_downgrade_is_rejected_before_staging(self) -> None:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        status = _scanner_status(platform, "0.64.69", slot="wifi")

        with self.assertRaisesRegex(flash.FlashError, "downgrade"):
            flash.reject_scanner_downgrades(status, ["wifi"], "0.64.68")

    def test_automatic_preflight_classifies_newer_peer_without_aborting_older(self) -> None:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        status = _scanner_status(
            platform, "0.64.70-badge-next", slot="wifi",
            hardware_id="14:C1:9F:52:CA:B0",
        )
        status["scanners"].extend(
            _scanner_status(
                platform, "0.64.68-badge-live-follow", slot="ble"
            )["scanners"]
        )

        self.assertEqual(
            flash.scanner_update_newer_slots(
                status,
                ["ble", "wifi"],
                "0.64.69-badge-defcon34",
                require_connected=False,
            ),
            {"wifi"},
        )

    def test_automatic_preflight_can_stage_with_temporarily_offline_slot(self) -> None:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        status = _scanner_status(
            platform, "0.64.68-badge-live-follow", slot="wifi"
        )

        self.assertEqual(
            flash.scanner_update_newer_slots(
                status,
                ["ble", "wifi"],
                "0.64.69-badge-defcon34",
                require_connected=False,
            ),
            set(),
        )

    def test_equal_numeric_core_with_different_suffix_fails_closed(self) -> None:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        status = _scanner_status(
            platform, "0.64.68-field-hotfix", slot="wifi"
        )

        with self.assertRaisesRegex(flash.FlashError, "unordered"):
            flash.reject_scanner_downgrades(
                status, ["wifi"], "0.64.68-badge-live-follow"
            )

    def test_usb_stage_includes_and_verifies_sha256(self) -> None:
        platform = dict(flash.PLATFORMS["badge-trio-xiao-s3"])
        version = "0.64.68-badge-live-follow"
        with tempfile.TemporaryDirectory() as temp_dir:
            image = Path(temp_dir) / "scanner.bin"
            data = _firmware_image(
                platform["scanner_project"],
                version,
                platform["scanner_name"],
                platform["hardware_type"],
            )
            image.write_bytes(data)
            platform["scanner_bin"] = image
            frozen = _frozen_firmware_set(data)
            image.unlink()
            sha256 = hashlib.sha256(data).hexdigest()
            crc32 = binascii.crc32(data) & 0xFFFFFFFF

            class FakeRawSerial:
                def write(self, data: bytes) -> int:
                    return len(data)

                def flush(self) -> None:
                    return None

            badge = flash.BadgeSerial(
                _usb_record("/dev/null"), dry_run=False
            )
            badge.ser = FakeRawSerial()
            lines: list[str] = []
            replies = [
                _credit_stage_receipt(
                    platform, version, data, 1,
                    phase="ready", received=0, credit=len(data),
                ),
                _credit_stage_receipt(
                    platform, version, data, 1,
                    phase="final", received=len(data), credit=0,
                    generation=17,
                ),
            ]
            badge.write_line = lines.append  # type: ignore[method-assign]
            badge.read_prefixed_json = (  # type: ignore[method-assign]
                lambda *_args, **_kwargs: replies.pop(0)
            )

            with contextlib.redirect_stdout(io.StringIO()), mock.patch.object(
                Path,
                "read_bytes",
                side_effect=AssertionError(
                    "scanner staging reopened a build pathname"
                ),
            ) as read_bytes:
                badge.stage_scanner_firmware(
                    platform, frozen, version, ["ble"]
                )

            read_bytes.assert_not_called()
            begin = json.loads(lines[0].removeprefix("FOF_CTL:"))
            self.assertEqual(begin["sha256"], sha256)
            self.assertEqual(begin["slot_mask"], 1)
            self.assertEqual(begin["flow_control"], "credit-v1")

    def test_maintenance_stage_waits_for_supervisor_and_uart_workers(
        self,
    ) -> None:
        platform = dict(flash.PLATFORMS["badge-trio-xiao-s3"])
        version = "0.64.69-badge-defcon34"
        data = _firmware_image(
            platform["scanner_project"],
            version,
            platform["scanner_name"],
            platform["hardware_type"],
        )
        session = "0123456789ABCDEF"
        unready = _update_maintenance_status(
            version,
            session=session,
            responses=20,
        )
        unready["stack_main_free"] = 0
        unready["stack_uart_ble_free"] = 0
        unready["stack_uart_wifi_free"] = 0
        ready = copy.deepcopy(unready)
        ready["usb_health"]["responses_completed"] = 21
        ready["stack_main_free"] = 4096
        ready["stack_uart_ble_free"] = 4096
        ready["stack_uart_wifi_free"] = 4096

        badge = flash.BadgeSerial(
            _usb_record("/dev/fake"),
            False,
            expected_hardware_id="e0:72:a1:f9:47:fc",
        )
        badge._update_session = session
        badge.ser = _ScriptedRawSerial()
        statuses = [unready, ready]
        events: list[str] = []

        def status(*, timeout_s: float) -> dict:
            self.assertGreater(timeout_s, 0)
            events.append("status")
            return statuses.pop(0)

        badge.status = status  # type: ignore[method-assign]
        badge.write_line = lambda _line: events.append("manifest")  # type: ignore[method-assign]
        replies = [
            _credit_stage_receipt(
                platform,
                version,
                data,
                1,
                phase="ready",
                received=0,
                credit=len(data),
            ),
            _credit_stage_receipt(
                platform,
                version,
                data,
                1,
                phase="final",
                received=len(data),
                credit=0,
                generation=17,
            ),
        ]
        badge.read_prefixed_json = lambda *_args, **_kwargs: replies.pop(0)  # type: ignore[method-assign]

        with mock.patch.object(flash.time, "sleep"):
            result = badge.stage_scanner_firmware(
                platform,
                _frozen_firmware_set(data),
                version,
                ["ble"],
            )

        self.assertEqual(result["generation"], 17)
        self.assertEqual(events, ["status", "status", "manifest"])
        self.assertEqual(statuses, [])
        self.assertEqual(b"".join(badge.ser.writes), data)

    def test_usb_stage_credit_v1_waits_for_each_durable_window(
        self,
    ) -> None:
        platform = dict(flash.PLATFORMS["badge-trio-xiao-s3"])
        version = "0.64.69-badge-defcon34"
        with tempfile.TemporaryDirectory() as temp_dir:
            image = Path(temp_dir) / "scanner.bin"
            data = _firmware_image(
                platform["scanner_project"],
                version,
                platform["scanner_name"],
                platform["hardware_type"],
            ) + b"X" * 9001
            self.assertNotEqual(len(data) % 4096, 0)
            image.write_bytes(data)
            platform["scanner_bin"] = image

            class FakeRawSerial:
                def __init__(self) -> None:
                    self.writes: list[bytes] = []

                def write(self, payload: bytes) -> int:
                    self.writes.append(payload)
                    return len(payload)

                def flush(self) -> None:
                    raise AssertionError(
                        "credited scanner staging must not call flush"
                    )

            raw = FakeRawSerial()
            badge = flash.BadgeSerial(
                _usb_record("/dev/null"), dry_run=False
            )
            badge.ser = raw
            lines: list[str] = []
            badge.write_line = lines.append  # type: ignore[method-assign]
            replies = [
                _credit_stage_receipt(
                    platform, version, data, 3,
                    phase="ready", received=0, credit=4096,
                ),
                _credit_stage_receipt(
                    platform, version, data, 3,
                    phase="credit", received=4096, credit=4096,
                ),
                _credit_stage_receipt(
                    platform, version, data, 3,
                    phase="credit", received=8192,
                    credit=len(data) - 8192,
                ),
                _credit_stage_receipt(
                    platform, version, data, 3,
                    phase="final", received=len(data), credit=0,
                    generation=29,
                ),
            ]
            reads_at: list[int] = []
            timeouts: list[float] = []
            clock = SimpleNamespace(now=100.0)

            def read_receipt(
                _prefix: str, timeout_s: float, **_kwargs: object
            ) -> dict:
                reads_at.append(sum(map(len, raw.writes)))
                timeouts.append(timeout_s)
                clock.now += 7.0
                return replies.pop(0)

            badge.read_prefixed_json = read_receipt  # type: ignore[method-assign]

            with contextlib.redirect_stdout(io.StringIO()), \
                 mock.patch.object(
                     flash.time, "monotonic", side_effect=lambda: clock.now
                 ), mock.patch.object(
                     flash.time, "sleep",
                     side_effect=AssertionError(
                         "credited scanner staging must not pace with sleeps"
                     ),
                 ):
                got = badge.stage_scanner_firmware(
                    platform, _frozen_firmware_set(data),
                    version, ["ble", "wifi"]
                )

            manifest = json.loads(lines[0].removeprefix("FOF_CTL:"))
            self.assertEqual(manifest["flow_control"], "credit-v1")
            self.assertEqual(reads_at, [0, 4096, 8192, len(data)])
            self.assertEqual(b"".join(raw.writes), data)
            self.assertTrue(all(
                0 < len(chunk) <= flash.SCANNER_STAGE_WRITE_BYTES
                for chunk in raw.writes
            ))
            self.assertTrue(all(
                0 < timeout <= flash.UPDATE_KEEPALIVE_MAX_S
                for timeout in timeouts
            ))
            self.assertEqual(got["generation"], 29)
            self.assertEqual(replies, [])

    def test_usb_stage_allows_healthy_badge_sized_transfer_past_120_seconds(
        self,
    ) -> None:
        platform = dict(flash.PLATFORMS["badge-trio-xiao-s3"])
        version = "0.64.79-badge-defcon34"
        data = _firmware_image(
            platform["scanner_project"],
            version,
            platform["scanner_name"],
            platform["hardware_type"],
        ) + b"X" * 1_216_000
        clock = SimpleNamespace(now=100.0)

        class SlowRawSerial:
            def __init__(self) -> None:
                self.writes: list[bytes] = []

            def write(self, payload: bytes) -> int:
                self.writes.append(payload)
                clock.now += 0.11
                return len(payload)

            def flush(self) -> None:
                raise AssertionError(
                    "credited scanner staging must not call flush"
                )

        replies = [
            _credit_stage_receipt(
                platform, version, data, 3,
                phase="ready", received=0,
                credit=min(flash.SCANNER_STAGE_CREDIT_BYTES, len(data)),
            )
        ]
        received = 0
        generation = 73
        while received < len(data):
            received += min(
                flash.SCANNER_STAGE_CREDIT_BYTES,
                len(data) - received,
            )
            if received == len(data):
                replies.append(_credit_stage_receipt(
                    platform, version, data, 3,
                    phase="final", received=received, credit=0,
                    generation=generation,
                ))
            else:
                replies.append(_credit_stage_receipt(
                    platform, version, data, 3,
                    phase="credit", received=received,
                    credit=min(
                        flash.SCANNER_STAGE_CREDIT_BYTES,
                        len(data) - received,
                    ),
                ))

        raw = SlowRawSerial()
        badge = flash.BadgeSerial(
            _usb_record("/dev/null"), dry_run=False
        )
        badge.ser = raw
        badge.write_line = lambda _line: None  # type: ignore[method-assign]
        badge.read_prefixed_json = (  # type: ignore[method-assign]
            lambda *_args, **_kwargs: replies.pop(0)
        )

        with contextlib.redirect_stdout(io.StringIO()), \
             mock.patch.object(
                 flash.time, "monotonic", side_effect=lambda: clock.now
             ):
            got = badge.stage_scanner_firmware(
                platform, _frozen_firmware_set(data),
                version, ["ble", "wifi"],
            )

        self.assertGreater(clock.now - 100.0, 120.0)
        self.assertEqual(got["generation"], generation)
        self.assertEqual(b"".join(raw.writes), data)
        self.assertEqual(replies, [])

    def test_usb_stage_credit_v1_rejects_bad_credit_before_more_bytes(
        self,
    ) -> None:
        platform = dict(flash.PLATFORMS["badge-trio-xiao-s3"])
        version = "0.64.69-badge-defcon34"
        with tempfile.TemporaryDirectory() as temp_dir:
            image = Path(temp_dir) / "scanner.bin"
            data = _firmware_image(
                platform["scanner_project"],
                version,
                platform["scanner_name"],
                platform["hardware_type"],
            ) + b"X" * 5000
            image.write_bytes(data)
            platform["scanner_bin"] = image
            ready = _credit_stage_receipt(
                platform, version, data, 1,
                phase="ready", received=0, credit=4096,
            )
            bad_receipts: dict[str, dict] = {
                "stale durable count": _credit_stage_receipt(
                    platform, version, data, 1,
                    phase="credit", received=0,
                    credit=len(data) - 4096,
                ),
                "wrong phase": _credit_stage_receipt(
                    platform, version, data, 1,
                    phase="ready", received=4096,
                    credit=len(data) - 4096,
                ),
                "misaligned credit": _credit_stage_receipt(
                    platform, version, data, 1,
                    phase="credit", received=4096,
                    credit=len(data) - 4097,
                ),
                "wrong identity": {
                    **_credit_stage_receipt(
                        platform, version, data, 1,
                        phase="credit", received=4096,
                        credit=len(data) - 4096,
                    ),
                    "target": "wrong-scanner",
                },
            }

            for name, bad_receipt in bad_receipts.items():
                class FakeRawSerial:
                    def __init__(self) -> None:
                        self.writes: list[bytes] = []

                    def write(self, payload: bytes) -> int:
                        self.writes.append(payload)
                        return len(payload)

                    def flush(self) -> None:
                        raise AssertionError(
                            "credited scanner staging must not call flush"
                        )

                raw = FakeRawSerial()
                badge = flash.BadgeSerial(
                    _usb_record("/dev/null"), dry_run=False
                )
                badge.ser = raw
                badge.write_line = (  # type: ignore[method-assign]
                    lambda _line: None
                )
                replies = [dict(ready), bad_receipt]
                badge.read_prefixed_json = (  # type: ignore[method-assign]
                    lambda *_args, **_kwargs: replies.pop(0)
                )

                with self.subTest(name=name), contextlib.redirect_stdout(
                    io.StringIO()
                ), self.assertRaises(flash.FlashError):
                    badge.stage_scanner_firmware(
                        platform, _frozen_firmware_set(data),
                        version, ["ble"]
                    )

                self.assertEqual(sum(map(len, raw.writes)), 4096)

    def test_usb_stage_rejects_partial_credit_ready_before_first_byte(
        self,
    ) -> None:
        platform = dict(flash.PLATFORMS["badge-trio-xiao-s3"])
        version = "0.64.69-badge-defcon34"
        with tempfile.TemporaryDirectory() as temp_dir:
            image = Path(temp_dir) / "scanner.bin"
            data = _firmware_image(
                platform["scanner_project"],
                version,
                platform["scanner_name"],
                platform["hardware_type"],
            )
            image.write_bytes(data)
            platform["scanner_bin"] = image
            writes: list[bytes] = []
            badge = flash.BadgeSerial(
                _usb_record("/dev/null"), dry_run=False
            )
            badge.ser = SimpleNamespace(
                write=lambda payload: writes.append(payload) or len(payload),
                flush=lambda: None,
            )
            badge.write_line = lambda _line: None  # type: ignore[method-assign]
            partial = _stage_receipt(platform, version, data, 1)
            partial["flow_control"] = "credit-v1"
            badge.read_prefixed_json = (  # type: ignore[method-assign]
                lambda *_args, **_kwargs: partial
            )

            with self.assertRaisesRegex(
                flash.FlashError, "ready.*phase"
            ):
                badge.stage_scanner_firmware(
                    platform, _frozen_firmware_set(data),
                    version, ["ble"]
                )

            self.assertEqual(writes, [])

    def test_usb_stage_uncredited_receipt_is_rejected_before_first_byte(
        self,
    ) -> None:
        platform = dict(flash.PLATFORMS["badge-trio-xiao-s3"])
        version = "0.64.68-badge-live-follow"
        with tempfile.TemporaryDirectory() as temp_dir:
            image = Path(temp_dir) / "scanner.bin"
            data = _firmware_image(
                platform["scanner_project"],
                version,
                platform["scanner_name"],
                platform["hardware_type"],
            ) + b"X" * 10000
            image.write_bytes(data)
            platform["scanner_bin"] = image

            class FakeRawSerial:
                def __init__(self) -> None:
                    self.writes: list[bytes] = []
                    self.flush_calls = 0

                def write(self, payload: bytes) -> int:
                    self.writes.append(payload)
                    return len(payload)

                def flush(self) -> None:
                    self.flush_calls += 1

            raw = FakeRawSerial()
            badge = flash.BadgeSerial(
                _usb_record("/dev/null"), dry_run=False
            )
            badge.ser = raw
            lines: list[str] = []
            badge.write_line = lines.append  # type: ignore[method-assign]
            replies = [
                _stage_receipt(platform, version, data, 3),
                _stage_receipt(
                    platform, version, data, 3, generation=19
                ),
            ]
            badge.read_prefixed_json = (  # type: ignore[method-assign]
                lambda *_args, **_kwargs: replies.pop(0)
            )

            with contextlib.redirect_stdout(io.StringIO()), \
                 mock.patch.object(flash.time, "sleep"), \
                 self.assertRaisesRegex(flash.FlashError, "credit-v1"):
                badge.stage_scanner_firmware(
                    platform, _frozen_firmware_set(data),
                    version, ["ble", "wifi"]
                )

            self.assertEqual(raw.writes, [])
            self.assertEqual(raw.flush_calls, 0)
            manifest = json.loads(lines[0].removeprefix("FOF_CTL:"))
            self.assertEqual(manifest["flow_control"], "credit-v1")

    def test_usb_stage_ready_schema_is_rejected_as_final_receipt(
        self,
    ) -> None:
        platform = dict(flash.PLATFORMS["badge-trio-xiao-s3"])
        version = "0.64.68-badge-live-follow"
        with tempfile.TemporaryDirectory() as temp_dir:
            image = Path(temp_dir) / "scanner.bin"
            data = _firmware_image(
                platform["scanner_project"],
                version,
                platform["scanner_name"],
                platform["hardware_type"],
            )
            image.write_bytes(data)
            platform["scanner_bin"] = image
            ready = _credit_stage_receipt(
                platform,
                version,
                data,
                3,
                phase="ready",
                received=0,
                credit=len(data),
            )
            wrong_final = _credit_stage_receipt(
                platform,
                version,
                data,
                3,
                phase="credit",
                received=len(data),
                credit=0,
            )

            badge = flash.BadgeSerial(
                _usb_record("/dev/null"), dry_run=False
            )
            badge.ser = SimpleNamespace(
                write=lambda payload: len(payload),
                flush=lambda: None,
            )
            badge.write_line = (  # type: ignore[method-assign]
                lambda _line: None
            )
            replies = [ready, wrong_final]
            badge.read_prefixed_json = (  # type: ignore[method-assign]
                lambda *_args, **_kwargs: replies.pop(0)
            )

            with contextlib.redirect_stdout(io.StringIO()), \
                 self.assertRaises(flash.FlashError):
                badge.stage_scanner_firmware(
                    platform, _frozen_firmware_set(data),
                    version, ["ble", "wifi"]
                )

    def test_usb_stage_rejects_incomplete_begin_receipt_before_first_byte(self) -> None:
        platform = dict(flash.PLATFORMS["badge-trio-xiao-s3"])
        version = "0.64.69-badge-defcon34"
        with tempfile.TemporaryDirectory() as temp_dir:
            image = Path(temp_dir) / "scanner.bin"
            data = _firmware_image(
                platform["scanner_project"],
                version,
                platform["scanner_name"],
                platform["hardware_type"],
            )
            image.write_bytes(data)
            platform["scanner_bin"] = image

            class FakeRawSerial:
                def __init__(self) -> None:
                    self.writes: list[bytes] = []

                def write(self, payload: bytes) -> None:
                    self.writes.append(payload)

                def flush(self) -> None:
                    return None

            raw = FakeRawSerial()
            badge = flash.BadgeSerial(
                _usb_record("/dev/null"), dry_run=False
            )
            badge.ser = raw
            badge.write_line = lambda _line: None  # type: ignore[method-assign]
            incomplete = _stage_receipt(platform, version, data, 1)
            del incomplete["slot_mask"]
            replies = [
                incomplete,
                _stage_receipt(
                    platform, version, data, 1, generation=18
                ),
            ]
            badge.read_prefixed_json = (  # type: ignore[method-assign]
                lambda *_args, **_kwargs: replies.pop(0)
            )

            with self.assertRaisesRegex(
                flash.FlashError, "ready.*slot_mask"
            ):
                badge.stage_scanner_firmware(
                    platform, _frozen_firmware_set(data),
                    version, ["ble"]
                )

            self.assertEqual(raw.writes, [])

    def test_usb_stage_returns_generation_bound_final_receipt(self) -> None:
        platform = dict(flash.PLATFORMS["badge-trio-xiao-s3"])
        version = "0.64.69-badge-defcon34"
        with tempfile.TemporaryDirectory() as temp_dir:
            image = Path(temp_dir) / "scanner.bin"
            data = _firmware_image(
                platform["scanner_project"],
                version,
                platform["scanner_name"],
                platform["hardware_type"],
            )
            image.write_bytes(data)
            platform["scanner_bin"] = image
            final_receipt = _credit_stage_receipt(
                platform, version, data, 3,
                phase="final", received=len(data), credit=0,
                generation=44,
            )
            badge = flash.BadgeSerial(
                _usb_record("/dev/null"), dry_run=False
            )
            badge.ser = SimpleNamespace(
                write=lambda data: len(data), flush=lambda: None
            )
            badge.write_line = lambda _line: None  # type: ignore[method-assign]
            replies = [
                _credit_stage_receipt(
                    platform, version, data, 3,
                    phase="ready", received=0, credit=len(data),
                ),
                final_receipt,
            ]
            badge.read_prefixed_json = (  # type: ignore[method-assign]
                lambda *_args, **_kwargs: replies.pop(0)
            )

            with contextlib.redirect_stdout(io.StringIO()):
                got = badge.stage_scanner_firmware(
                    platform, _frozen_firmware_set(data),
                    version, ["ble", "wifi"]
                )

            self.assertEqual(got, final_receipt)

    def test_usb_stage_revalidates_target_identity_before_first_byte(self) -> None:
        platform = dict(flash.PLATFORMS["badge-trio-xiao-s3"])
        version = "0.64.68-badge-live-follow"
        with tempfile.TemporaryDirectory() as temp_dir:
            image = Path(temp_dir) / "scanner.bin"
            image.write_bytes(_firmware_image(
                "fof_scanner",
                version,
                platform["scanner_name"],
                platform["hardware_type"],
            ))
            platform["scanner_bin"] = image
            badge = flash.BadgeSerial(
                _usb_record("/dev/null"), dry_run=False
            )
            lines: list[str] = []
            badge.write_line = lines.append  # type: ignore[method-assign]
            data = image.read_bytes()
            badge.ser = SimpleNamespace(write=lambda _data: None, flush=lambda: None)
            replies = [
                {"ok": True},
                {
                    "ok": True,
                    "name": platform["scanner_name"],
                    "version": version,
                    "size": len(data),
                    "crc32": binascii.crc32(data) & 0xFFFFFFFF,
                },
            ]
            badge.read_prefixed_json = (  # type: ignore[method-assign]
                lambda *_args, **_kwargs: replies.pop(0)
            )

            with self.assertRaisesRegex(flash.FlashError, "project"):
                badge.stage_scanner_firmware(
                    platform, _frozen_firmware_set(data),
                    version, ["ble", "wifi"]
                )

            self.assertEqual(lines, [])

    def test_usb_flow_stages_once_without_manual_relay(self) -> None:
        platform = dict(flash.PLATFORMS["badge-trio-xiao-s3"])
        target_version = "0.64.68-badge-live-follow"
        before_scanners = _scanner_status(
            platform,
            "0.64.67",
            hardware_id="E0:72:A1:F9:48:58",
            slot="ble",
        )
        before_scanners["scanners"].extend(
            _scanner_status(
                platform,
                "0.64.67",
                hardware_id="E0:72:A1:F9:47:FC",
                slot="wifi",
            )["scanners"]
        )
        before = _badge_status_with_scanners(
            target_version,
            before_scanners,
        )
        final_scanners = _scanner_status(
            platform,
            target_version,
            hardware_id="E0:72:A1:F9:48:58",
            slot="ble",
        )
        final_scanners["scanners"].extend(
            _scanner_status(
                platform,
                target_version,
                hardware_id="E0:72:A1:F9:47:FC",
                slot="wifi",
            )["scanners"]
        )
        final = _badge_status_with_scanners(
            target_version,
            final_scanners,
            responses=22,
        )
        initial = _uplink_status(target_version, responses=10)
        post = _post_uplink_evidence(target_version, responses=20)
        args = SimpleNamespace(
            port="/dev/fake-uplink",
            platform="badge-trio-xiao-s3",
            dry_run=False,
            skip_command_probe=False,
            recovery_rewrite_same_version=False,
        )
        scanner_image = b"scanner-image"
        stage_proof = _stage_receipt(
            platform, target_version, scanner_image, 3, generation=1
        )
        restored = json.loads(json.dumps(final))
        restored["usb_health"]["responses_completed"] = 23
        restored_evidence = _post_uplink_evidence(
            target_version, responses=23
        )
        expected_before = json.loads(json.dumps(before))
        expected_restored = json.loads(json.dumps(restored))
        expected_stage_proof = json.loads(json.dumps(stage_proof))

        class FakeBadge:
            staged = 0
            relayed = 0
            staged_slots: list[str] = []

            def __init__(self, *_args, **_kwargs) -> None:
                pass

            def __enter__(self):
                return self

            def __exit__(self, *_args) -> None:
                return None

            def wait_ping(self) -> None:
                return None

            def status(self, timeout_s: float = 5) -> dict:
                return final

            def stage_scanner_firmware(
                self, _platform, _artifacts, _version, slots
            ) -> dict:
                FakeBadge.staged += 1
                FakeBadge.staged_slots = list(slots)
                before["scanners"][0]["ver"] = "mutated-during-stage"
                return stage_proof

            def relay_scanner(self, *_args, **_kwargs) -> None:
                FakeBadge.relayed += 1

        wait_calls: list[
            tuple[dict[str, str], dict | None, set[str] | None]
        ] = []
        auto_calls: list[tuple[list[str], set[str] | None]] = []

        def fake_wait(_badge, _platform, _slots, _version, *,
                      expected_hardware_ids,
                      expected_stage_receipt=None,
                      required_converged_slots=None, **_kwargs) -> None:
            wait_calls.append((
                expected_hardware_ids,
                expected_stage_receipt,
                required_converged_slots,
            ))

        def fake_verify_auto(_status, slots, *,
                             required_converged_slots=None,
                             **_kwargs) -> None:
            auto_calls.append((
                list(slots),
                required_converged_slots,
            ))

        def fake_theme_control(
            _badge, *, initial_status, initial_evidence,
            restored_status_validator=None, **_kwargs
        ):
            self.assertIs(initial_status, final)
            self.assertIsNotNone(restored_status_validator)
            restored_status_validator(restored)
            return restored_evidence

        with mock.patch.object(flash, "probe_rom_device", return_value=None), \
             mock.patch.object(flash, "probe_application", return_value=initial), \
             mock.patch.object(
                 flash, "wait_for_post_uplink_application",
                 return_value=(_usb_record("/dev/fake-uplink-rebound"), post),
             ), \
             mock.patch.object(flash, "BadgeSerial", FakeBadge), \
             mock.patch.object(flash, "wait_for_scanner_status_usb",
                               return_value=before), \
             mock.patch.object(flash, "wait_for_scanners_usb",
                               side_effect=fake_wait), \
             mock.patch.object(
                 flash, "verify_auto_update_convergence",
                 side_effect=fake_verify_auto,
             ), mock.patch.object(
                 flash, "_prove_reversible_usb_theme_control",
                 side_effect=fake_theme_control,
             ):
            with contextlib.redirect_stdout(io.StringIO()):
                result = flash.usb_flow(
                    args,
                    platform,
                    False,
                    ["ble", "wifi"],
                    target_version,
                )

        self.assertEqual(FakeBadge.staged, 1)
        self.assertEqual(FakeBadge.relayed, 0)
        self.assertEqual(FakeBadge.staged_slots, ["ble", "wifi"])
        self.assertEqual(
            wait_calls,
            [(
                {
                    "ble": "e0:72:a1:f9:48:58",
                    "wifi": "e0:72:a1:f9:47:fc",
                },
                stage_proof,
                {"ble", "wifi"},
            )],
        )
        self.assertEqual(
            auto_calls,
            [
                (["ble", "wifi"], {"ble", "wifi"}),
                (["ble", "wifi"], {"ble", "wifi"}),
            ],
        )
        self.assertIs(type(result), flash.UsbScannerFlowResult)
        self.assertEqual(result.pre_stage_status, expected_before)
        self.assertIsNot(result.pre_stage_status, before)
        self.assertEqual(result.final_restored_status, expected_restored)
        self.assertIsNot(result.final_restored_status, restored)
        self.assertEqual(result.stage_receipt, expected_stage_proof)
        self.assertIsNot(result.stage_receipt, stage_proof)
        self.assertEqual(
            result.stage_receipts, (expected_stage_proof,)
        )
        self.assertEqual(result.attempt_history, ())
        self.assertEqual(
            result.preflight_older_slots, frozenset({"ble", "wifi"})
        )
        self.assertEqual(result.recovery_slots, frozenset())
        self.assertIs(type(result.stage_count), int)
        self.assertEqual(result.stage_count, 1)
        self.assertIs(result.theme_restored, True)
        self.assertIs(result.fresh_usb_proven, True)
        self.assertIs(
            flash._revalidate_usb_scanner_flow_result(result), result
        )
        self.assertFalse(hasattr(result, "__dict__"))
        with self.assertRaises(AttributeError):
            object.__setattr__(result, "stage_count", 2)

        before["scanners"][0]["ver"] = "source-mutated"
        restored["scanners"][0]["ver"] = "source-mutated"
        stage_proof["generation"] = 99
        exposed_pre_stage = result.pre_stage_status
        exposed_pre_stage["scanners"][0]["ver"] = "caller-mutated"
        exposed_restored = result.final_restored_status
        exposed_restored["scanners"][0]["ver"] = "caller-mutated"
        exposed_receipt = result.stage_receipt
        exposed_receipt["generation"] = 100
        exposed_receipts = result.stage_receipts
        exposed_receipts[0]["generation"] = 101
        self.assertEqual(result.pre_stage_status, expected_before)
        self.assertEqual(result.final_restored_status, expected_restored)
        self.assertEqual(result.stage_receipt, expected_stage_proof)
        self.assertEqual(
            result.stage_receipts, (expected_stage_proof,)
        )
        with self.assertRaises(TypeError):
            dataclasses.replace(result, stage_count=2)

    def test_usb_flow_classifies_only_identity_captured_slots(self) -> None:
        platform = dict(flash.PLATFORMS["badge-trio-xiao-s3"])
        target_version = "0.64.68-badge-live-follow"
        preflight_scanners = _scanner_status(
            platform,
            "0.64.67",
            hardware_id="E0:72:A1:F9:48:58",
            slot="ble",
        )
        offline_wifi = _scanner_status(
            platform,
            "0.64.67",
            hardware_id="14:C1:9F:52:CA:B0",
            slot="wifi",
        )["scanners"][0]
        offline_wifi["connected"] = False
        preflight_scanners["scanners"].append(offline_wifi)
        preflight = _badge_status_with_scanners(
            target_version,
            preflight_scanners,
        )
        initial = _uplink_status(target_version, responses=10)
        post = _post_uplink_evidence(target_version, responses=20)
        args = SimpleNamespace(
            port="/dev/fake-uplink",
            platform="badge-trio-xiao-s3",
            dry_run=False,
            skip_command_probe=False,
            recovery_rewrite_same_version=False,
        )
        observed_slots: list[str] = []

        class ClassificationObserved(Exception):
            pass

        class FakeBadge:
            def __init__(self, *_args, **_kwargs) -> None:
                pass

            def __enter__(self):
                return self

            def __exit__(self, *_args) -> None:
                return None

        def observe_classification(
            _status, slots, _version, **_kwargs
        ) -> set[str]:
            observed_slots.extend(slots)
            raise ClassificationObserved

        with mock.patch.object(flash, "probe_rom_device", return_value=None), \
             mock.patch.object(flash, "probe_application", return_value=initial), \
             mock.patch.object(
                 flash, "wait_for_post_uplink_application",
                 return_value=(_usb_record("/dev/fake-uplink-rebound"), post),
             ), \
             mock.patch.object(flash, "BadgeSerial", FakeBadge), \
             mock.patch.object(
                 flash, "wait_for_scanner_status_usb", return_value=preflight
             ), \
             mock.patch.object(
                 flash,
                 "scanner_strictly_older_slots",
                 side_effect=observe_classification,
             ):
            with self.assertRaises(ClassificationObserved):
                with contextlib.redirect_stdout(io.StringIO()):
                    flash.usb_flow(
                        args,
                        platform,
                        False,
                        ["ble", "wifi"],
                        target_version,
                    )

        self.assertEqual(observed_slots, ["ble"])

    def test_same_version_recovery_waits_for_strict_readiness_before_relay(self) -> None:
        platform = dict(flash.PLATFORMS["badge-trio-xiao-s3"])
        target_version = "0.64.69-badge-defcon34"
        before = _badge_status_with_scanners(
            target_version,
            _scanner_status(platform, target_version, slot="ble"),
        )
        before["scanners"][0]["role_acked"] = False
        before["scanners"][0]["health"] = "cmd_wait"
        final = _badge_status_with_scanners(
            target_version,
            _scanner_status(platform, target_version, slot="ble"),
            responses=22,
        )
        initial = _uplink_status(target_version, responses=10)
        post = _post_uplink_evidence(target_version, responses=20)
        args = SimpleNamespace(
            port="/dev/fake-uplink",
            platform="badge-trio-xiao-s3",
            dry_run=False,
            skip_command_probe=False,
            recovery_rewrite_same_version=True,
        )
        stage_proof = _stage_receipt(
            platform, target_version, b"scanner-image", 1, generation=1
        )
        restored = json.loads(json.dumps(final))
        restored["usb_health"]["responses_completed"] = 23
        restored_evidence = _post_uplink_evidence(
            target_version, responses=23
        )
        events: list[tuple] = []
        relay_calls: list[tuple[str, tuple, dict]] = []

        class FakeBadge:
            def __init__(self, *_args, **_kwargs) -> None:
                pass

            def __enter__(self):
                return self

            def __exit__(self, *_args) -> None:
                return None

            def wait_ping(self) -> None:
                return None

            def status(self, timeout_s: float = 5) -> dict:
                return final

            def stage_scanner_firmware(
                self, _platform, _artifacts, _version, slots
            ) -> dict:
                events.append(("stage", tuple(slots)))
                return stage_proof

            def relay_scanner(self, slot, *args, **kwargs) -> None:
                events.append(("relay", slot))
                relay_calls.append((slot, args, dict(kwargs)))

        def fake_wait(_badge, _platform, slots, _version, *,
                      expected_hardware_ids,
                      expected_stage_receipt=None,
                      require_auto_update=True,
                      require_radio_health=True,
                      **_kwargs) -> None:
            events.append((
                "wait",
                tuple(slots),
                require_auto_update,
                require_radio_health,
                expected_stage_receipt,
            ))

        def fake_theme_control(
            _badge, *, initial_evidence, restored_status_validator=None,
            **_kwargs
        ):
            self.assertIsNotNone(restored_status_validator)
            restored_status_validator(restored)
            return restored_evidence

        with mock.patch.object(flash, "probe_rom_device", return_value=None), \
             mock.patch.object(flash, "probe_application", return_value=initial), \
             mock.patch.object(
                 flash, "wait_for_post_uplink_application",
                 return_value=(_usb_record("/dev/fake-uplink-rebound"), post),
             ), \
             mock.patch.object(flash, "BadgeSerial", FakeBadge), \
             mock.patch.object(flash, "wait_for_scanner_status_usb",
                               return_value=before), \
             mock.patch.object(flash, "wait_for_scanners_usb",
                               side_effect=fake_wait), \
             mock.patch.object(
                 flash, "verify_auto_update_convergence"
             ), mock.patch.object(
                 flash, "_prove_reversible_usb_theme_control",
                 side_effect=fake_theme_control,
             ):
            with contextlib.redirect_stdout(io.StringIO()):
                result = flash.usb_flow(
                    args, platform, False, ["ble"], target_version
                )

        self.assertEqual(events, [
            ("stage", ("ble",)),
            ("wait", ("ble",), False, False, None),
            ("relay", "ble"),
            ("wait", ("ble",), True, True, stage_proof),
        ])
        self.assertEqual(result.preflight_older_slots, frozenset())
        self.assertEqual(result.recovery_slots, frozenset({"ble"}))
        self.assertEqual(len(relay_calls), 1)
        self.assertEqual(relay_calls[0][0], "ble")
        self.assertEqual(relay_calls[0][1][0:2], (False, True))
        self.assertEqual(
            relay_calls[0][1][2],
            len(self.frozen_usb_artifacts.scanner.member_bytes(
                "artifact.firmware"
            )),
        )
        self.assertEqual(
            relay_calls[0][2],
            {
                "expected_generation": stage_proof["generation"],
                "expected_hardware_id": "e0:72:a1:f9:48:58",
            },
        )

    def test_recovery_transport_proof_can_repair_radio_off_but_not_wrong_role(self) -> None:
        platform = dict(flash.PLATFORMS["badge-trio-xiao-s3"])
        version = "0.64.69-badge-defcon34"
        status = _scanner_status(platform, version, slot="ble")
        scanner = status["scanners"][0]
        scanner.update({
            "health": "ble_off",
            "ble_initialized": True,
            "ble_scanning": False,
            "ble_host_active": False,
            "ble_host_synced": False,
        })

        # Explicit same-version recovery exists to repair a broken radio.
        # Identity, UART, OTA-idle, rollback, and role proof remain mandatory;
        # only the condition the new image is meant to repair is deferred.
        flash.verify_scanners(
            status,
            platform,
            ["ble"],
            version,
            require_radio_health=False,
        )

        scanner["role_acked"] = False
        with self.assertRaisesRegex(flash.FlashError, "role convergence"):
            flash.verify_scanners(
                status,
                platform,
                ["ble"],
                version,
                require_radio_health=False,
            )

    def test_same_version_recovery_fails_closed_before_unhealthy_relay(self) -> None:
        platform = dict(flash.PLATFORMS["badge-trio-xiao-s3"])
        target_version = "0.64.69-badge-defcon34"
        before = _badge_status_with_scanners(
            target_version,
            _scanner_status(platform, target_version, slot="ble"),
        )
        before["scanners"][0]["role_acked"] = False
        before["scanners"][0]["health"] = "cmd_wait"
        final = _badge_status_with_scanners(
            target_version,
            _scanner_status(platform, target_version, slot="ble"),
            responses=22,
        )
        initial = _uplink_status(target_version, responses=10)
        post = _post_uplink_evidence(target_version, responses=20)
        args = SimpleNamespace(
            port="/dev/fake-uplink",
            platform="badge-trio-xiao-s3",
            dry_run=False,
            skip_command_probe=False,
            recovery_rewrite_same_version=True,
        )

        class FakeBadge:
            staged = 0
            relayed = 0

            def __init__(self, *_args, **_kwargs) -> None:
                pass

            def __enter__(self):
                return self

            def __exit__(self, *_args) -> None:
                return None

            def wait_ping(self) -> None:
                return None

            def status(self, timeout_s: float = 5) -> dict:
                return final

            def stage_scanner_firmware(self, *_args) -> dict:
                FakeBadge.staged += 1
                return {"generation": 1, "slot_mask": 1}

            def relay_scanner(self, *_args, **_kwargs) -> None:
                FakeBadge.relayed += 1

        def fail_unhealthy_recovery_wait(
            _badge, _platform, _slots, _version, *,
            require_auto_update=True, **_kwargs
        ) -> None:
            if not require_auto_update:
                raise flash.FlashError(
                    "scanner verification failed: ble scanner health is not normal"
                )

        with mock.patch.object(flash, "probe_rom_device", return_value=None), \
             mock.patch.object(flash, "probe_application", return_value=initial), \
             mock.patch.object(
                 flash, "wait_for_post_uplink_application",
                 return_value=(_usb_record("/dev/fake-uplink-rebound"), post),
             ), \
             mock.patch.object(flash, "BadgeSerial", FakeBadge), \
             mock.patch.object(flash, "wait_for_scanner_status_usb",
                               return_value=before), \
             mock.patch.object(
                 flash,
                 "wait_for_scanners_usb",
                 side_effect=fail_unhealthy_recovery_wait,
             ):
            with self.assertRaisesRegex(flash.FlashError, "health"):
                with contextlib.redirect_stdout(io.StringIO()):
                    flash.usb_flow(
                        args, platform, False, ["ble"], target_version
                    )

        self.assertEqual(FakeBadge.staged, 1)
        self.assertEqual(FakeBadge.relayed, 0)

    def test_legacy_68_bootstrap_skips_same_version_recovery_readiness(self) -> None:
        platform = dict(flash.PLATFORMS["badge-trio-xiao-s3"])
        target_version = "0.64.69-badge-defcon34"
        before = _badge_status_with_scanners(
            target_version,
            _scanner_status(
                platform, "0.64.68-badge-live-follow", slot="ble"
            ),
        )
        before["scanners"][0]["role_acked"] = False
        before["scanners"][0]["health"] = "cmd_wait"
        final = _badge_status_with_scanners(
            target_version,
            _scanner_status(platform, target_version, slot="ble"),
            responses=22,
        )
        initial = _uplink_status(target_version, responses=10)
        post = _post_uplink_evidence(target_version, responses=20)
        args = SimpleNamespace(
            port="/dev/fake-uplink",
            platform="badge-trio-xiao-s3",
            dry_run=False,
            skip_command_probe=False,
            recovery_rewrite_same_version=True,
        )

        class FakeBadge:
            staged = 0
            relayed = 0

            def __init__(self, *_args, **_kwargs) -> None:
                pass

            def __enter__(self):
                return self

            def __exit__(self, *_args) -> None:
                return None

            def wait_ping(self) -> None:
                return None

            def status(self, timeout_s: float = 5) -> dict:
                return final

            def stage_scanner_firmware(self, *_args) -> dict:
                FakeBadge.staged += 1
                return {"generation": 1, "slot_mask": 1}

            def relay_scanner(self, *_args, **_kwargs) -> None:
                FakeBadge.relayed += 1

        wait_modes: list[bool] = []

        def record_wait(_badge, _platform, _slots, _version, *,
                        require_auto_update=True, **_kwargs) -> None:
            wait_modes.append(require_auto_update)

        with mock.patch.object(flash, "probe_rom_device", return_value=None), \
             mock.patch.object(flash, "probe_application", return_value=initial), \
             mock.patch.object(
                 flash, "wait_for_post_uplink_application",
                 return_value=(_usb_record("/dev/fake-uplink-rebound"), post),
             ), \
             mock.patch.object(flash, "BadgeSerial", FakeBadge), \
             mock.patch.object(flash, "wait_for_scanner_status_usb",
                               return_value=before), \
             mock.patch.object(flash, "wait_for_scanners_usb",
                               side_effect=record_wait), \
             mock.patch.object(
                 flash, "verify_auto_update_convergence"
             ), mock.patch.object(
                 flash, "_prove_reversible_usb_theme_control",
                 side_effect=_complete_mocked_theme_control,
             ):
            with contextlib.redirect_stdout(io.StringIO()):
                flash.usb_flow(
                    args, platform, False, ["ble"], target_version
                )

        self.assertEqual(FakeBadge.staged, 1)
        self.assertEqual(FakeBadge.relayed, 0)
        self.assertEqual(wait_modes, [True])

    def test_scanner_only_usb_flow_requires_current_uplink_before_staging(self) -> None:
        platform = dict(flash.PLATFORMS["badge-trio-xiao-s3"])
        target_version = "0.64.69-badge-defcon34"
        scanner_status = _scanner_status(platform, "0.64.68", slot="ble")
        initial = _uplink_status(
            "0.64.68-badge-live-follow", responses=10
        )
        args = SimpleNamespace(
            port="/dev/fake-uplink",
            platform="badge-trio-xiao-s3",
            dry_run=False,
            skip_command_probe=False,
            recovery_rewrite_same_version=False,
        )

        class FakeBadge:
            staged = False

            def __init__(self, *_args, **_kwargs) -> None:
                pass

            def __enter__(self):
                return self

            def __exit__(self, *_args) -> None:
                return None

            def wait_ping(self) -> None:
                return None

            def status(self, timeout_s: float = 5) -> dict:
                return {
                    "version": "0.64.68-badge-live-follow",
                    "firmware_name": platform["uplink_name"],
                    "app_project": platform["uplink_project"],
                    "hardware_type": platform["hardware_type"],
                }

            def stage_scanner_firmware(self, *_args) -> dict:
                FakeBadge.staged = True
                return {"generation": 1}

        with mock.patch.object(flash, "probe_rom_device", return_value=None), \
             mock.patch.object(flash, "probe_application", return_value=initial), \
             mock.patch.object(flash, "BadgeSerial", FakeBadge), \
             mock.patch.object(
                 flash, "wait_for_scanner_status_usb",
                 return_value=scanner_status,
             ), \
             mock.patch.object(flash, "wait_for_scanners_usb"):
            with self.assertRaisesRegex(flash.FlashError, "current|older|uplink"):
                flash.usb_flow(
                    args, platform, False, ["ble"], target_version
                )

        self.assertFalse(FakeBadge.staged)

    def test_usb_uplink_flash_refuses_downgrade_before_uart_flash(self) -> None:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        args = SimpleNamespace(
            port="/dev/fake-uplink",
            platform="badge-trio-xiao-s3",
            dry_run=False,
            recovery_rewrite_same_version=False,
        )
        status = _uplink_status("0.64.69", responses=10)

        class FakeBadge:
            uploaded = 0

            def __init__(self, *_args, **_kwargs) -> None:
                pass

            def __enter__(self):
                return self

            def __exit__(self, *_args) -> None:
                return None

            def upload_uplink_firmware(self, *_args, **_kwargs) -> dict:
                FakeBadge.uploaded += 1
                raise flash.FlashError("uplink downgrade refused")

        with mock.patch.object(flash, "probe_rom_device", return_value=None), \
             mock.patch.object(flash, "probe_application", return_value=status), \
             mock.patch.object(
                 flash, "_attest_frozen_uplink_flash_authority"
             ), \
             mock.patch.object(flash, "BadgeSerial", FakeBadge), \
             mock.patch.object(flash, "flash_complete_uplink_layout") as rom_flash:
            with self.assertRaisesRegex(flash.FlashError, "downgrade"):
                flash.usb_flow(args, platform, True, [], "0.64.68")

        self.assertEqual(FakeBadge.uploaded, 1)
        rom_flash.assert_not_called()

    def test_network_path_is_refused_before_scanner_status_or_upload(self) -> None:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        status = _scanner_status(platform, "0.64.69", slot="wifi")
        args = SimpleNamespace(
            transport="ap",
            host=None,
            node=None,
            backend="http://127.0.0.1:8000",
            port=None,
            platform="badge-trio-xiao-s3",
            dry_run=False,
            network_ttl_s=900,
            skip_command_probe=False,
            skip_current=False,
            recovery_rewrite_same_version=False,
        )

        with mock.patch.object(flash, "wait_http_status", return_value={}), \
             mock.patch.object(flash, "wait_for_scanner_status_network",
                               return_value=status), \
             mock.patch.object(flash, "wait_for_scanners_network"):
            with self.assertRaisesRegex(flash.FlashError, "USB.*UART"):
                flash.network_flow(
                    args, platform, False, ["wifi"], "0.64.68"
                )

    def test_network_uplink_path_is_refused_before_http_ota(self) -> None:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        args = SimpleNamespace(
            transport="ap",
            host=None,
            node=None,
            backend="http://127.0.0.1:8000",
            port=None,
            platform="badge-trio-xiao-s3",
            dry_run=False,
            network_ttl_s=900,
            recovery_rewrite_same_version=False,
        )
        status = {
            "version": "0.64.69",
            "firmware_name": platform["uplink_name"],
            "app_project": platform["uplink_project"],
            "hardware_type": platform["hardware_type"],
        }

        with mock.patch.object(flash, "wait_http_status",
                               return_value=status), \
             mock.patch.object(flash, "flash_uplink_network") as uplink_ota:
            with self.assertRaisesRegex(flash.FlashError, "USB.*UART"):
                flash.network_flow(
                    args, platform, True, [], "0.64.68"
                )

        uplink_ota.assert_not_called()

    def test_network_path_cannot_bypass_usb_uart_transport_on_raw_uart(self) -> None:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        status = {
            "scanners": [{
                "uart": "wifi",
                "connected": False,
                "uart_raw_seen": True,
                "uart_raw_bytes": 2048,
            }]
        }
        args = SimpleNamespace(
            transport="ap",
            host=None,
            node=None,
            backend="http://127.0.0.1:8000",
            port=None,
            platform="badge-trio-xiao-s3",
            dry_run=False,
            network_ttl_s=900,
            skip_command_probe=False,
            skip_current=False,
            recovery_rewrite_same_version=False,
        )

        with mock.patch.object(flash, "wait_http_status", return_value={}), \
             mock.patch.object(flash, "wait_for_scanner_status_network",
                               return_value=status), \
             mock.patch.object(flash, "upload_scanner_network"), \
             mock.patch.object(flash, "relay_scanner_network"), \
             mock.patch.object(flash, "wait_for_scanners_network"):
            with self.assertRaisesRegex(flash.FlashError, "USB.*UART"):
                flash.network_flow(
                    args, platform, False, ["wifi"], "0.64.68"
                )

    def test_network_upgrade_cannot_reach_relay(self) -> None:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        status = _scanner_status(platform, "0.64.67", slot="wifi")
        args = SimpleNamespace(
            transport="ap",
            host=None,
            node=None,
            backend="http://127.0.0.1:8000",
            port=None,
            platform="badge-trio-xiao-s3",
            dry_run=False,
            network_ttl_s=900,
            skip_command_probe=False,
            recovery_rewrite_same_version=False,
        )

        with mock.patch.object(flash, "wait_http_status", return_value={}), \
             mock.patch.object(flash, "wait_for_scanner_status_network",
                               return_value=status), \
             mock.patch.object(flash, "upload_scanner_network"), \
             mock.patch.object(flash, "relay_scanner_network") as relay, \
             mock.patch.object(flash, "wait_for_scanners_network"):
            with self.assertRaisesRegex(flash.FlashError, "USB.*UART"):
                flash.network_flow(
                    args, platform, False, ["wifi"], "0.64.68"
                )

        relay.assert_not_called()

    def test_direct_scanner_flash_requires_uplink_preflight(self) -> None:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        args = SimpleNamespace(
            manual_scanner="wifi",
            port="/dev/fake-scanner",
            verify_port=None,
            platform="badge-trio-xiao-s3",
            dry_run=False,
            recovery_rewrite_same_version=False,
        )

        with mock.patch.object(flash, "flash_scanner_usb") as direct_flash:
            with self.assertRaisesRegex(
                flash.FlashError, "cannot prove downgrade safety"
            ):
                flash.manual_scanner_flow(args, platform, "0.64.68")

        direct_flash.assert_not_called()

    def test_direct_scanner_flash_rejects_downgrade_before_flash(self) -> None:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        status = _scanner_status(platform, "0.64.69", slot="wifi")
        args = SimpleNamespace(
            manual_scanner="wifi",
            port="/dev/fake-scanner",
            verify_port="/dev/fake-uplink",
            platform="badge-trio-xiao-s3",
            dry_run=False,
            recovery_rewrite_same_version=False,
        )

        class FakeBadge:
            def __init__(self, *_args, **_kwargs) -> None:
                pass

            def __enter__(self):
                return self

            def __exit__(self, *_args) -> None:
                return None

            def wait_ping(self) -> None:
                return None

            def status(self, timeout_s: float = 5) -> dict:
                return status

        with mock.patch.object(flash, "BadgeSerial", FakeBadge), \
             mock.patch.object(
                 flash, "select_trusted_uplink_descriptor",
                 return_value=(
                     _usb_record("/dev/fake-uplink"),
                     flash.TrustedUplinkBinding(
                         serial_number="e0:72:a1:f9:47:fc",
                         location=None,
                         source="operator-selection",
                     ),
                 ),
             ), \
             mock.patch.object(flash, "flash_scanner_usb") as direct_flash, \
             mock.patch.object(flash, "wait_for_scanners_usb"):
            with self.assertRaisesRegex(flash.FlashError, "downgrade"):
                flash.manual_scanner_flow(args, platform, "0.64.68")

        direct_flash.assert_not_called()

    def test_direct_scanner_verification_does_not_require_auto_coordinator_generation(self) -> None:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        version = "0.64.69-badge-defcon34"

        class FakeBadge:
            def status(self, timeout_s: float = 5) -> dict:
                return _scanner_status(platform, version, slot="ble")

        with mock.patch.object(flash, "verify_auto_update_convergence") as auto_verify:
            flash.wait_for_scanners_usb(
                FakeBadge(),
                platform,
                ["ble"],
                version,
                timeout_s=1,
                require_auto_update=False,
            )

        auto_verify.assert_not_called()

    def test_scanner_wait_accepts_exact_stage_receipt_proof(self) -> None:
        parameters = inspect.signature(flash.wait_for_scanners_usb).parameters

        self.assertIn("expected_stage_receipt", parameters)

    def test_scanner_wait_refuses_auto_proof_without_stage_receipt(self) -> None:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]

        with self.assertRaisesRegex(flash.FlashError, "stage receipt"):
            flash.wait_for_scanners_usb(
                SimpleNamespace(),
                platform,
                ["ble"],
                "0.64.69-badge-defcon34",
                timeout_s=0,
            )

    def test_scanner_wait_passes_stage_receipt_to_auto_verifier(self) -> None:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        version = "0.64.69-badge-defcon34"
        status = _scanner_status(platform, version, slot="ble")
        receipt = _stage_receipt(
            platform, version, b"scanner-image", 1, generation=8
        )

        class FakeBadge:
            def status(self, timeout_s: float = 5) -> dict:
                return status

        with mock.patch.object(
            flash, "verify_auto_update_convergence"
        ) as auto_verify:
            flash.wait_for_scanners_usb(
                FakeBadge(),
                platform,
                ["ble"],
                version,
                timeout_s=1,
                expected_stage_receipt=receipt,
            )

        auto_verify.assert_called_once_with(
            status,
            ["ble"],
            expected_stage_receipt=receipt,
        )

    def test_scanner_wait_accepts_newer_only_from_bound_coordinator_state(self) -> None:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        version = "0.64.69-badge-defcon34"
        status = _scanner_status(platform, "0.64.70-badge-next", slot="wifi")
        receipt = _stage_receipt(
            platform, version, b"scanner-image", 2, generation=8
        )
        status["firmware_store"] = {
            "stored": True,
            "target": receipt["target"],
            "app_project": receipt["app_project"],
            "hardware_type": receipt["hardware_type"],
            "version": receipt["version"],
            "size": receipt["size"],
            "crc32": receipt["crc32"],
            "sha256": receipt["sha256"],
            "generation": 8,
            "auto_update": {
                "generation": 8,
                "target_slot_mask": 2,
                "pending_mask": 0,
                "worker_running": False,
                "readiness_probes": [0, 1],
                "scanners": [
                    {"slot": 0, "attempts": 0, "state": "excluded"},
                    {"slot": 1, "attempts": 0, "state": "current"},
                ],
            },
        }

        class FakeBadge:
            def status(self, timeout_s: float = 5) -> dict:
                return status

        clock = [0.0]

        def advance_clock(seconds: float) -> None:
            clock[0] += seconds

        with mock.patch.object(
            flash.time, "monotonic", side_effect=lambda: clock[0]
        ), mock.patch.object(
            flash.time, "sleep", side_effect=advance_clock
        ):
            with self.assertRaisesRegex(flash.FlashError, "version"):
                flash.wait_for_scanners_usb(
                    FakeBadge(),
                    platform,
                    ["wifi"],
                    version,
                    timeout_s=1,
                    expected_stage_receipt=receipt,
                    allowed_newer_slots={"wifi"},
                )

        status["firmware_store"]["auto_update"]["scanners"][1]["state"] = (
            "newer_skipped"
        )
        flash.wait_for_scanners_usb(
            FakeBadge(),
            platform,
            ["wifi"],
            version,
            timeout_s=1,
            expected_stage_receipt=receipt,
        )

    def test_scanner_wait_reconnects_after_transient_status_failure(self) -> None:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        version = "0.64.69-badge-defcon34"
        converged = _scanner_status(platform, version, slot="ble")

        class FakeBadge:
            def __init__(self) -> None:
                self.status_calls = 0
                self.reconnect_calls = 0

            def status(self, timeout_s: float = 5) -> dict:
                self.status_calls += 1
                if self.status_calls == 1:
                    raise flash.FlashError("USB serial disconnected")
                return converged

            def reconnect(self, timeout_s: int = 15) -> None:
                self.reconnect_calls += 1

        badge = FakeBadge()
        with mock.patch.object(flash.time, "sleep"):
            flash.wait_for_scanners_usb(
                badge,
                platform,
                ["ble"],
                version,
                timeout_s=1,
                require_auto_update=False,
            )

        self.assertEqual(badge.reconnect_calls, 1)

    def test_scanner_convergence_deadline_ignores_wall_clock_changes(
        self,
    ) -> None:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        clock = SimpleNamespace(now=0.0)

        class UnavailableBadge:
            def status(self, timeout_s: float = 5) -> dict:
                raise flash.FlashError("USB serial disconnected")

            def reconnect(self, timeout_s: int = 15) -> None:
                raise flash.FlashError("still disconnected")

        def advance(seconds: float) -> None:
            clock.now += seconds

        with mock.patch.object(
            flash.time, "time",
            side_effect=AssertionError(
                "scanner convergence must not read wall clock"
            ),
        ), mock.patch.object(
            flash.time, "monotonic", side_effect=lambda: clock.now
        ), mock.patch.object(
            flash.time, "sleep", side_effect=advance
        ), self.assertRaisesRegex(
            flash.FlashError, "scanner verification failed"
        ):
            flash.wait_for_scanners_usb(
                UnavailableBadge(),
                platform,
                ["ble"],
                "0.64.69-badge-defcon34",
                timeout_s=1,
                require_auto_update=False,
            )

        self.assertGreaterEqual(clock.now, 1.0)

    def test_scanner_convergence_rejects_success_after_deadline(
        self,
    ) -> None:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        version = "0.64.69-badge-defcon34"
        converged = _scanner_status(platform, version, slot="ble")
        clock = SimpleNamespace(now=0.0)

        class LateBadge:
            def status(self, timeout_s: float = 5) -> dict:
                clock.now += 1.1
                return converged

        with mock.patch.object(
            flash.time, "monotonic", side_effect=lambda: clock.now
        ), mock.patch.object(
            flash.time, "sleep",
            side_effect=AssertionError("late success must not sleep"),
        ), self.assertRaisesRegex(
            flash.FlashError, "scanner verification failed"
        ):
            flash.wait_for_scanners_usb(
                LateBadge(),
                platform,
                ["ble"],
                version,
                timeout_s=1,
                require_auto_update=False,
            )

    def test_scanner_convergence_bounds_each_wait_by_remaining_time(
        self,
    ) -> None:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        clock = SimpleNamespace(now=0.0)
        status_budgets: list[float] = []
        reconnect_budgets: list[float] = []
        sleep_budgets: list[float] = []

        class UnavailableBadge:
            def status(self, timeout_s: float = 5) -> dict:
                status_budgets.append(timeout_s)
                clock.now += 0.6
                raise flash.FlashError("USB serial disconnected")

            def reconnect(self, timeout_s: float = 15) -> None:
                reconnect_budgets.append(timeout_s)
                clock.now += 0.1
                raise flash.FlashError("still disconnected")

        def advance(seconds: float) -> None:
            sleep_budgets.append(seconds)
            clock.now += seconds

        with mock.patch.object(
            flash.time, "monotonic", side_effect=lambda: clock.now
        ), mock.patch.object(
            flash.time, "sleep", side_effect=advance
        ), self.assertRaisesRegex(
            flash.FlashError, "scanner verification failed"
        ):
            flash.wait_for_scanners_usb(
                UnavailableBadge(),
                platform,
                ["ble"],
                "0.64.69-badge-defcon34",
                timeout_s=1,
                require_auto_update=False,
            )

        self.assertTrue(status_budgets)
        self.assertTrue(reconnect_budgets)
        self.assertTrue(sleep_budgets)
        self.assertLessEqual(status_budgets[0], 1.0)
        self.assertLessEqual(reconnect_budgets[0], 0.4)
        self.assertLessEqual(sleep_budgets[0], 0.300_001)

    def test_scanner_preflight_reconnects_after_transient_status_failure(self) -> None:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        ready = _scanner_status(
            platform, "0.64.68-badge-live-follow", slot="ble"
        )

        class FakeBadge:
            def __init__(self) -> None:
                self.status_calls = 0
                self.reconnect_calls = 0

            def status(self, timeout_s: float = 5) -> dict:
                self.status_calls += 1
                if self.status_calls == 1:
                    raise flash.FlashError("USB serial disconnected")
                return ready

            def reconnect(self, timeout_s: int = 15) -> None:
                self.reconnect_calls += 1

        badge = FakeBadge()
        with mock.patch.object(flash.time, "sleep"):
            got = flash.wait_for_scanner_status_usb(
                badge, ["ble"], timeout_s=1
            )

        self.assertEqual(got, ready)
        self.assertEqual(badge.reconnect_calls, 1)

    def test_scanner_preflight_waits_past_connected_boot_placeholder(self) -> None:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        booting = {
            "scanners": [{
                "uart": "ble",
                "connected": True,
                "slot_role": "ble_primary",
                "role_acked": False,
                "firmware_name": "",
                "app_project": "",
                "hardware_type": "",
                "hardware_id": "",
                "ver": "",
            }]
        }
        ready = _scanner_status(
            platform, "0.64.68-badge-live-follow", slot="ble"
        )
        self.assertFalse(
            flash.scanner_status_has_relay_path(booting, ["ble"])
        )

        class FakeBadge:
            def __init__(self) -> None:
                self.statuses = [booting, ready]
                self.status_calls = 0

            def status(self, timeout_s: float = 5) -> dict:
                self.status_calls += 1
                return self.statuses.pop(0)

        badge = FakeBadge()
        with mock.patch.object(flash.time, "sleep"):
            got = flash.wait_for_scanner_status_usb(
                badge, ["ble"], timeout_s=1
            )

        self.assertEqual(got, ready)
        self.assertEqual(badge.status_calls, 2)

    def test_scanner_convergence_requires_exact_identity_and_mac(self) -> None:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        version = "0.64.68-badge-live-follow"
        status = _scanner_status(platform, version)

        flash.verify_scanners(
            status,
            platform,
            ["ble"],
            version,
            expected_hardware_ids={"ble": "e0:72:a1:f9:48:58"},
        )

        status["scanners"][0]["app_project"] = "fof_scanner"
        with self.assertRaisesRegex(flash.FlashError, "project"):
            flash.verify_scanners(
                status,
                platform,
                ["ble"],
                version,
                expected_hardware_ids={"ble": "e0:72:a1:f9:48:58"},
            )

    def test_scanner_convergence_rejects_mac_swap(self) -> None:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        version = "0.64.68-badge-live-follow"
        status = _scanner_status(platform, version)

        with self.assertRaisesRegex(flash.FlashError, "hardware id"):
            flash.verify_scanners(
                status,
                platform,
                ["ble"],
                version,
                expected_hardware_ids={"ble": "14:c1:9f:52:ca:b0"},
            )

    def test_scanner_convergence_rejects_duplicate_requested_macs(self) -> None:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        version = "0.64.69-badge-defcon34"
        status = _scanner_status(platform, version, slot="ble")
        status["scanners"].extend(
            _scanner_status(
                platform,
                version,
                slot="wifi",
                hardware_id="E0:72:A1:F9:48:58",
            )["scanners"]
        )

        with self.assertRaisesRegex(flash.FlashError, "unique"):
            flash.verify_scanners(status, platform, ["ble", "wifi"], version)

    def test_scanner_convergence_requires_exact_untruncated_version(self) -> None:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        version = "0.64.68-badge-live-follow"
        status = _scanner_status(platform, "0.64.68-badge")

        with self.assertRaisesRegex(flash.FlashError, "version"):
            flash.verify_scanners(status, platform, ["ble"], version)

    def test_scanner_convergence_allows_only_a_proven_still_newer_skip(self) -> None:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        target = "0.64.69-badge-defcon34"
        status = _scanner_status(platform, "0.64.70-badge-next", slot="wifi")

        flash.verify_scanners(
            status,
            platform,
            ["wifi"],
            target,
            allowed_newer_slots={"wifi"},
        )
        with self.assertRaisesRegex(flash.FlashError, "version"):
            flash.verify_scanners(status, platform, ["wifi"], target)

        status["scanners"][0]["ver"] = "0.64.68-badge-live-follow"
        with self.assertRaisesRegex(flash.FlashError, "newer"):
            flash.verify_scanners(
                status,
                platform,
                ["wifi"],
                target,
                allowed_newer_slots={"wifi"},
            )

    def test_scanner_convergence_requires_rollback_clear_and_normal_health(self) -> None:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        version = "0.64.68-badge-live-follow"
        status = _scanner_status(platform, version)
        status["scanners"][0]["rollback_pending"] = True

        with self.assertRaisesRegex(flash.FlashError, "rollback"):
            flash.verify_scanners(status, platform, ["ble"], version)

        status["scanners"][0]["rollback_pending"] = False
        status["scanners"][0]["health"] = "cmd_wait"
        with self.assertRaisesRegex(flash.FlashError, "health"):
            flash.verify_scanners(status, platform, ["ble"], version)

    def test_scanner_convergence_requires_exact_slot_role_and_live_radios(self) -> None:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        version = "0.64.69-badge-defcon34"

        for slot in ("ble", "wifi"):
            status = _scanner_status(platform, version, slot=slot)
            flash.verify_scanners(status, platform, [slot], version)

            status["scanners"][0]["role_acked"] = False
            with self.assertRaisesRegex(flash.FlashError, "role"):
                flash.verify_scanners(status, platform, [slot], version)

        ble = _scanner_status(platform, version, slot="ble")
        ble["scanners"][0]["ble_scanning"] = False
        with self.assertRaisesRegex(flash.FlashError, "radio"):
            flash.verify_scanners(ble, platform, ["ble"], version)

        wifi = _scanner_status(platform, version, slot="wifi")
        wifi["scanners"][0]["wifi_paused"] = True
        with self.assertRaisesRegex(flash.FlashError, "radio"):
            flash.verify_scanners(wifi, platform, ["wifi"], version)

    def test_wifi_primary_rejects_logical_health_without_physical_scanner_health(self) -> None:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        version = "0.64.69-badge-defcon34"
        status = _scanner_status(platform, version, slot="wifi")
        scanner = status["scanners"][0]
        scanner.update({
            "health": "ok",
            "wifi_paused": False,
            "wifi_initialized": False,
            "wifi_init_rc": 257,
            "wifi_active": False,
            "full_scan_ok": 0,
        })

        with self.assertRaisesRegex(flash.FlashError, "physical radio"):
            flash.verify_scanners(status, platform, ["wifi"], version)

    def test_primary_profiles_require_each_physical_radio_signal(self) -> None:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        version = "0.64.69-badge-defcon34"
        failures = {
            "ble": {
                "ble_initialized": False,
                "ble_host_active": False,
                "ble_host_synced": False,
                "ble_scanning": False,
            },
            "wifi": {
                "wifi_initialized": False,
                "wifi_init_rc": 257,
                "wifi_active": False,
                "full_scan_ok": 0,
            },
        }

        for slot, field_failures in failures.items():
            for field, failed_value in field_failures.items():
                with self.subTest(slot=slot, field=field):
                    status = _scanner_status(platform, version, slot=slot)
                    status["scanners"][0][field] = failed_value
                    with self.assertRaisesRegex(
                        flash.FlashError, "physical radio"
                    ):
                        flash.verify_scanners(
                            status, platform, [slot], version
                        )

    def test_fixed_slots_reject_hybrid_profile_in_role_contract(self) -> None:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        version = "0.64.69-badge-defcon34"

        for slot in ("ble", "wifi"):
            scenarios = {
                "slot_role": {"slot_role": "hybrid_failover"},
                "expected_scan_profile": {
                    "expected_scan_profile": "hybrid_failover",
                    "scan_profile": "hybrid_failover",
                },
                "scan_profile": {"scan_profile": "hybrid_failover"},
            }
            for field, overrides in scenarios.items():
                with self.subTest(slot=slot, field=field):
                    status = _scanner_status(platform, version, slot=slot)
                    scanner = status["scanners"][0]
                    scanner.update({
                        "ble_initialized": True,
                        "ble_scanning": True,
                        "ble_host_active": True,
                        "ble_host_synced": True,
                        "wifi_paused": False,
                        "wifi_initialized": True,
                        "wifi_init_rc": 0,
                        "wifi_active": True,
                        "full_scan_ok": 1,
                    })
                    scanner.update(overrides)
                    with self.assertRaisesRegex(
                        flash.FlashError, "role mismatch"
                    ):
                        flash.verify_scanners(
                            status, platform, [slot], version
                        )

    def test_wifi_full_scan_counter_accepts_only_safe_legacy_alias(self) -> None:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        version = "0.64.69-badge-defcon34"
        status = _scanner_status(platform, version, slot="wifi")
        scanner = status["scanners"][0]
        scanner["wifi_full_scan_ok"] = scanner.pop("full_scan_ok")

        flash.verify_scanners(status, platform, ["wifi"], version)

        scanner.pop("wifi_full_scan_ok")
        with self.assertRaisesRegex(flash.FlashError, "physical radio"):
            flash.verify_scanners(status, platform, ["wifi"], version)

    def test_auto_update_convergence_binds_terminal_state_to_staged_generation(self) -> None:
        status = {
            "firmware_store": {
                "stored": True,
                "generation": 42,
                "auto_update": {
                    "generation": 42,
                    "target_slot_mask": 3,
                    "pending_mask": 0,
                    "worker_running": False,
                    "readiness_probes": [1, 2],
                    "scanners": [
                        {"slot": 0, "attempts": 1, "state": "converged"},
                        {"slot": 1, "attempts": 0, "state": "current"},
                    ],
                },
            },
        }

        flash.verify_auto_update_convergence(
            status,
            ["ble", "wifi"],
            required_converged_slots={"ble"},
        )

        status["firmware_store"]["auto_update"]["generation"] = 41
        with self.assertRaisesRegex(flash.FlashError, "generation"):
            flash.verify_auto_update_convergence(
                status,
                ["ble", "wifi"],
                required_converged_slots={"ble"},
            )

    def test_preflight_older_slots_require_attempted_convergence(self) -> None:
        status = {
            "firmware_store": {
                "stored": True,
                "generation": 42,
                "auto_update": {
                    "generation": 42,
                    "target_slot_mask": 3,
                    "pending_mask": 0,
                    "worker_running": False,
                    "readiness_probes": [1, 1],
                    "scanners": [
                        {"slot": 0, "attempts": 1, "state": "converged"},
                        {"slot": 1, "attempts": 0, "state": "current"},
                    ],
                },
            },
        }

        with self.assertRaisesRegex(
            flash.FlashError, "preflight-proven older"
        ):
            flash.verify_auto_update_convergence(
                status,
                ["ble", "wifi"],
                required_converged_slots={"ble", "wifi"},
            )

        status["firmware_store"]["auto_update"]["scanners"][1].update({
            "state": "converged",
            "attempts": 0,
        })
        with self.assertRaisesRegex(
            flash.FlashError, "preflight-proven older"
        ):
            flash.verify_auto_update_convergence(
                status,
                ["ble", "wifi"],
                required_converged_slots={"ble", "wifi"},
            )

        status["firmware_store"]["auto_update"]["scanners"][1][
            "attempts"
        ] = 1
        flash.verify_auto_update_convergence(
            status,
            ["ble", "wifi"],
            required_converged_slots={"ble", "wifi"},
        )

    def test_preflight_classifies_exact_strictly_older_scanner_slots(self) -> None:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        candidate = "0.64.76-badge-defcon34"
        status = _scanner_status(
            platform, "0.64.75-badge-defcon34", slot="ble"
        )
        for wifi_version in (
            candidate,
            "0.64.77-badge-next",
        ):
            with self.subTest(wifi_version=wifi_version):
                case_status = json.loads(json.dumps(status))
                case_status["scanners"].extend(
                    _scanner_status(
                        platform, wifi_version, slot="wifi"
                    )["scanners"]
                )
                self.assertEqual(
                    flash.scanner_strictly_older_slots(
                        case_status, ["ble", "wifi"], candidate
                    ),
                    {"ble"},
                )

    def test_auto_update_convergence_accepts_newer_skipped_terminal(self) -> None:
        status = {
            "firmware_store": {
                "stored": True,
                "generation": 42,
                "auto_update": {
                    "generation": 42,
                    "target_slot_mask": 3,
                    "pending_mask": 0,
                    "worker_running": False,
                    "readiness_probes": [1, 1],
                    "scanners": [
                        {"slot": 0, "attempts": 1, "state": "converged"},
                        {"slot": 1, "attempts": 0, "state": "newer_skipped"},
                    ],
                },
            },
        }

        flash.verify_auto_update_convergence(
            status,
            ["ble", "wifi"],
            required_converged_slots={"ble"},
        )

    def test_auto_update_verifier_accepts_exact_stage_receipt_proof(self) -> None:
        parameters = inspect.signature(
            flash.verify_auto_update_convergence
        ).parameters

        self.assertIn("expected_stage_receipt", parameters)

    def test_auto_update_convergence_binds_store_to_exact_stage_receipt(self) -> None:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        version = "0.64.69-badge-defcon34"
        receipt = _stage_receipt(
            platform, version, b"scanner-image", 1, generation=42
        )
        status = {
            "firmware_store": {
                "stored": True,
                "target": receipt["target"],
                "app_project": receipt["app_project"],
                "hardware_type": receipt["hardware_type"],
                "version": receipt["version"],
                "sha256": receipt["sha256"],
                "size": receipt["size"],
                "crc32": receipt["crc32"],
                "generation": receipt["generation"],
                "auto_update": {
                    "generation": receipt["generation"],
                    "target_slot_mask": 1,
                    "pending_mask": 0,
                    "worker_running": False,
                    "readiness_probes": [1, 0],
                    "scanners": [
                        {"slot": 0, "attempts": 1, "state": "converged"},
                        {"slot": 1, "attempts": 0, "state": "excluded"},
                    ],
                },
            },
        }

        flash.verify_auto_update_convergence(
            status, ["ble"], expected_stage_receipt=receipt
        )

        for field, replacement in (
            ("generation", 43),
            ("sha256", "0" * 64),
            ("target", "scanner-s3-combo"),
        ):
            with self.subTest(field=field):
                changed = json.loads(json.dumps(status))
                changed["firmware_store"][field] = replacement
                if field == "generation":
                    changed["firmware_store"]["auto_update"]["generation"] = replacement
                with self.assertRaisesRegex(
                    flash.FlashError, f"stage receipt.*{field}"
                ):
                    flash.verify_auto_update_convergence(
                        changed,
                        ["ble"],
                        expected_stage_receipt=receipt,
                    )

        wrong_mask_receipt = dict(receipt)
        wrong_mask_receipt["slot_mask"] = 2
        with self.assertRaisesRegex(flash.FlashError, "stage receipt.*slot_mask"):
            flash.verify_auto_update_convergence(
                status,
                ["ble"],
                expected_stage_receipt=wrong_mask_receipt,
            )

    def test_auto_update_convergence_proves_exact_requested_slot_mask(self) -> None:
        status = {
            "firmware_store": {
                "stored": True,
                "generation": 9,
                "auto_update": {
                    "generation": 9,
                    "target_slot_mask": 1,
                    "pending_mask": 0,
                    "worker_running": False,
                    "readiness_probes": [1, 0],
                    "scanners": [
                        {"slot": 0, "attempts": 1, "state": "converged"},
                        {"slot": 1, "attempts": 0, "state": "excluded"},
                    ],
                },
            },
        }

        flash.verify_auto_update_convergence(status, ["ble"])

        auto_update = status["firmware_store"]["auto_update"]
        auto_update["target_slot_mask"] = 3
        with self.assertRaisesRegex(flash.FlashError, "slot mask"):
            flash.verify_auto_update_convergence(status, ["ble"])

        auto_update["target_slot_mask"] = 1
        auto_update["scanners"][1]["state"] = "current"
        with self.assertRaisesRegex(flash.FlashError, "excluded"):
            flash.verify_auto_update_convergence(status, ["ble"])

    def test_auto_update_convergence_rejects_pending_or_nonterminal_work(self) -> None:
        status = {
            "firmware_store": {
                "stored": True,
                "generation": 7,
                "auto_update": {
                    "generation": 7,
                    "target_slot_mask": 2,
                    "pending_mask": 0,
                    "worker_running": False,
                    "readiness_probes": [0, 1],
                    "scanners": [
                        {"slot": 0, "attempts": 0, "state": "excluded"},
                        {"slot": 1, "attempts": 1, "state": "converged"},
                    ],
                },
            },
        }

        auto_update = status["firmware_store"]["auto_update"]
        auto_update["pending_mask"] = 2
        with self.assertRaisesRegex(flash.FlashError, "pending"):
            flash.verify_auto_update_convergence(status, ["wifi"])

        auto_update["pending_mask"] = 0
        auto_update["worker_running"] = True
        with self.assertRaisesRegex(flash.FlashError, "worker"):
            flash.verify_auto_update_convergence(status, ["wifi"])

        auto_update["worker_running"] = False
        auto_update["scanners"][1]["state"] = "ready_queued"
        with self.assertRaisesRegex(flash.FlashError, "terminal"):
            flash.verify_auto_update_convergence(status, ["wifi"])

    def test_raw_uart_bad_status_is_recoverable(self) -> None:
        status = {
            "scanners": [{
                "uart": "wifi",
                "connected": False,
                "uart_raw_seen": True,
                "uart_raw_bytes": 2048,
                "uart_json_err": 12,
            }]
        }
        self.assertTrue(flash.scanner_status_has_relay_path(status, ["wifi"]))

    def test_missing_uart_path_is_physical_offline(self) -> None:
        status = {"scanners": [{"uart": "wifi", "connected": False}]}
        self.assertFalse(flash.scanner_status_has_relay_path(status, ["wifi"]))

    def test_relay_timeout_scales_with_firmware_size(self) -> None:
        self.assertGreaterEqual(flash.scanner_relay_timeout_s(1_200_000), 240)
        self.assertLessEqual(flash.scanner_relay_timeout_s(1_200_000), 900)

    def test_same_version_relay_requires_exact_bound_terminal_receipt_table(
        self,
    ) -> None:
        slot = "ble"
        generation = 37
        hardware_id = "e0:72:a1:f9:48:58"
        firmware_size = 4100
        exact = _bound_relay_receipt(
            slot, generation, hardware_id, firmware_size
        )
        wrong_values = {
            "ok": False,
            "phase": "progress",
            "slot": "wifi",
            "uart": "wifi",
            "generation": generation + 1,
            "hardware_id": "e0:72:a1:f9:48:59",
            "size": firmware_size - 1,
            "bytes": firmware_size - 1,
            "chunks": exact["chunks"] - 1,
            "stage": "health",
            "done": False,
            "error": "not-empty",
        }
        cases: list[tuple[str, dict]] = []
        for field, wrong in wrong_values.items():
            missing = dict(exact)
            missing.pop(field)
            cases.append((f"missing {field}", missing))
            mismatched = dict(exact)
            mismatched[field] = wrong
            cases.append((f"mismatched {field}", mismatched))
        extra = dict(exact)
        extra["unbound"] = True
        cases.append(("extra field", extra))

        for name, receipt in cases:
            badge = flash.BadgeSerial(
                _usb_record("/dev/null"), dry_run=False
            )
            badge.write_line = (  # type: ignore[method-assign]
                lambda *_args, **_kwargs: None
            )
            badge.read_prefixed_json = (  # type: ignore[method-assign]
                lambda *_args, **_kwargs: dict(receipt)
            )
            with self.subTest(name=name), self.assertRaises(
                flash.FlashError
            ):
                badge.relay_scanner(
                    slot,
                    False,
                    True,
                    firmware_size,
                    expected_generation=generation,
                    expected_hardware_id=hardware_id,
                )

    def test_same_version_relay_sends_bound_values_without_logging_mac(
        self,
    ) -> None:
        slot = "wifi"
        generation = 41
        hardware_id = "e0:72:a1:f9:48:59"
        firmware_size = 4097
        receipt = _bound_relay_receipt(
            slot, generation, hardware_id, firmware_size
        )
        written: list[tuple[str, str | None]] = []
        badge = flash.BadgeSerial(
            _usb_record("/dev/null"), dry_run=False
        )

        def write_line(
            line: str, *, log_override: str | None = None
        ) -> None:
            written.append((line, log_override))

        badge.write_line = write_line  # type: ignore[method-assign]
        badge.read_prefixed_json = (  # type: ignore[method-assign]
            lambda *_args, **_kwargs: dict(receipt)
        )
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            got = badge.relay_scanner(
                slot,
                False,
                True,
                firmware_size,
                expected_generation=generation,
                expected_hardware_id=hardware_id,
            )

        payload = json.loads(written[0][0].removeprefix("FOF_CTL:"))
        self.assertEqual(payload["expected_generation"], generation)
        self.assertEqual(payload["expected_hardware_id"], hardware_id)
        self.assertNotIn(hardware_id, written[0][1] or "")
        self.assertNotIn(hardware_id, output.getvalue())
        self.assertEqual(got, receipt)

    def test_same_version_relay_identity_mismatch_error_redacts_both_macs(
        self,
    ) -> None:
        expected_hardware_id = "e0:72:a1:f9:48:58"
        wrong_hardware_id = "e0:72:a1:f9:48:59"
        receipt = _bound_relay_receipt(
            "ble", 52, wrong_hardware_id, 4096
        )
        badge = flash.BadgeSerial(
            _usb_record("/dev/null"), dry_run=False
        )
        badge.write_line = (  # type: ignore[method-assign]
            lambda *_args, **_kwargs: None
        )
        badge.read_prefixed_json = (  # type: ignore[method-assign]
            lambda *_args, **_kwargs: dict(receipt)
        )

        with self.assertRaises(flash.FlashError) as caught:
            badge.relay_scanner(
                "ble",
                False,
                True,
                4096,
                expected_generation=52,
                expected_hardware_id=expected_hardware_id,
            )

        rendered = str(caught.exception)
        self.assertNotIn(expected_hardware_id, rendered)
        self.assertNotIn(wrong_hardware_id, rendered)

    def test_progress_lines_are_logged_while_waiting_for_final_relay(self) -> None:
        progress = {
            "uart": "ble",
            "stage": "chunks",
            "bytes": 600000,
            "size": 1200000,
            "percent": 50,
            "chunks": 586,
            "nacks": 0,
            "retries": 0,
            "elapsed_s": 22,
            "error": "",
        }
        terminal = _bound_relay_receipt(
            "ble", 17, "e0:72:a1:f9:48:58", 1200000
        )

        class FakeSerial:
            def __init__(self) -> None:
                self.lines = [
                    (
                        "FOF_FW_RELAY_PROGRESS:" +
                        json.dumps(progress, separators=(",", ":")) + "\n"
                    ).encode(),
                    (
                        "FOF_FW_RELAY:" +
                        json.dumps(terminal, separators=(",", ":")) + "\n"
                    ).encode(),
                ]

            def read(self, _n: int) -> bytes:
                return self.lines.pop(0) if self.lines else b""

        badge = flash.BadgeSerial(
            _usb_record("/dev/null"), dry_run=False
        )
        badge.ser = FakeSerial()
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            body = badge.read_prefixed_json(
                "FOF_FW_RELAY:",
                2,
                progress_prefix="FOF_FW_RELAY_PROGRESS:",
                allowed_schema_ids=(
                    flash.HostJsonSchemaId.RELAY_TERMINAL,
                ),
                progress_allowed_schema_ids=(
                    flash.HostJsonSchemaId.RELAY_PROGRESS,
                ),
                progress_validator=lambda value: (
                    flash.validate_scanner_relay_progress(
                        value,
                        slot="ble",
                        firmware_size=1200000,
                    )
                ),
            )
        self.assertTrue(body["ok"])
        self.assertIn("[relay] ble chunks 50%", out.getvalue())

    def test_updater_diagnostic_lines_are_visible_while_waiting_for_status(self) -> None:
        class FakeSerial:
            def __init__(self) -> None:
                self.lines = [
                    b'I fw_store: Auto scanner relay[0] generation=42 attempt=1 ok=0 stage=begin error=ota_ack_timeout chunks=0\n',
                    b'FOF_STATUS:{"version":"0.64.76-badge-defcon34"}\n',
                ]

            def read(self, _n: int) -> bytes:
                return self.lines.pop(0) if self.lines else b""

        badge = flash.BadgeSerial(
            _usb_record("/dev/null"), dry_run=False
        )
        badge.ser = FakeSerial()
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            body = badge.read_prefixed_json("FOF_STATUS:", 2)
        self.assertEqual(body["version"], "0.64.76-badge-defcon34")
        self.assertIn("[device] Auto scanner relay[0]", out.getvalue())
        self.assertIn("error=ota_ack_timeout", out.getvalue())


_ROM_PARTITION_SPECS = [
    ("nvs", 1, 2, 0x9000, 0x6000, 0),
    ("otadata", 1, 0, 0xF000, 0x2000, 0),
    ("phy_init", 1, 1, 0x11000, 0x1000, 0),
    ("ota_0", 0, 0x10, 0x20000, 0x200000, 0),
    ("ota_1", 0, 0x11, 0x220000, 0x200000, 0),
    ("fw_scanner_s3", 1, 0x40, 0x420000, 0x200000, 0),
    ("storage", 1, 0x82, 0x620000, 0x100000, 0),
    ("reserved", 1, 0x81, 0x720000, 0xE0000, 0),
]


def _rom_partition_entry(spec: tuple) -> bytes:
    label, entry_type, subtype, offset, size, flags = spec
    encoded = label if isinstance(label, bytes) else label.encode("ascii")
    if len(encoded) > 16:
        raise ValueError("test partition label is too long")
    return struct.pack(
        "<HBBII16sI", 0x50AA, entry_type, subtype, offset, size,
        encoded.ljust(16, b"\x00"), flags,
    )


def _rom_partition_table(specs: list[tuple] | None = None, *,
                         total_size: int = 0x1000) -> bytes:
    entries = b"".join(
        _rom_partition_entry(spec) for spec in (specs or _ROM_PARTITION_SPECS)
    )
    trailer = (
        struct.pack("<H", 0xEBEB) + b"\xFF" * 14 +
        hashlib.md5(entries).digest()
    )
    payload = entries + trailer
    if total_size < len(payload):
        raise ValueError("test partition table size is too small")
    return payload + b"\xFF" * (total_size - len(payload))


def _s3_image(segments: list[tuple[int, bytes]], *,
              entry_point: int = 0x40378000) -> bytes:
    header = bytearray(24)
    header[0] = 0xE9
    header[1] = len(segments)
    header[2] = 2
    header[3] = 0x3F
    struct.pack_into("<I", header, 4, entry_point)
    header[8] = 0xEE
    struct.pack_into("<H", header, 12, 9)
    struct.pack_into("<H", header, 17, 0xFFFF)
    header[23] = 1
    image = bytearray(header)
    checksum = 0xEF
    for address, data in segments:
        if not data and address not in (0, 4):
            raise ValueError("only format-reserved S3 segments may be empty")
        if len(data) % 4:
            raise ValueError("synthetic S3 segment must be nonempty and aligned")
        image.extend(struct.pack("<II", address, len(data)))
        image.extend(data)
        for byte in data:
            checksum ^= byte
    checksum_at = ((len(image) + 1 + 15) & ~15) - 1
    image.extend(b"\x00" * (checksum_at - len(image)))
    image.append(checksum)
    image.extend(hashlib.sha256(image).digest())
    return bytes(image)


def _rom_firmware_image(project: str, version: str, target: str,
                        hardware: str) -> bytes:
    descriptor_and_markers = _firmware_image(
        project, version, target, hardware
    )[0x20:]
    aligned = descriptor_and_markers.ljust(
        (len(descriptor_and_markers) + 3) & ~3, b"\x00"
    )
    return _s3_image([
        (0x3C0E0020, aligned),
        (0x40377900, b"\x00" * 0x100),
    ], entry_point=0x40377940)


_ROM_VERIFIER_BUILD_PATHS = (
    "bootloader.bin",
    "partitions.bin",
    "ota_data_initial.bin",
    "firmware.bin",
    "flash_args",
    "flash_app_args",
    "flash_project_args",
    "flasher_args.json",
    "bootloader/bootloader.bin",
    "partition_table/partition-table.bin",
    "fof_badge_uplink.bin",
)


class BadgeRomLayoutTests(unittest.TestCase):
    VERSION = "0.64.78-badge-defcon34"

    def _fixture(self, root: str, *, partition_table: bytes | None = None,
                 bootloader: bytes | None = None,
                 ota_data: bytes | None = None,
                 firmware: bytes | None = None) -> tuple[Path, Path, dict, dict[str, bytes]]:
        uplink_dir = Path(root) / "esp32" / "uplink"
        build_dir = (
            uplink_dir / ".pio" / "build" / "uplink-s3-fof_badge"
        )
        build_dir.mkdir(parents=True)
        (uplink_dir / "partitions_s3_fof_badge_8mb.csv").write_text(
            "# exact source path used by the verifier fixture\n"
        )
        platform = dict(flash.PLATFORMS["badge-trio-xiao-s3"])
        platform["uplink_bin"] = Path(root) / "forbidden-decoy.bin"
        if firmware is None:
            firmware = _rom_firmware_image(
                platform["uplink_project"], self.VERSION,
                platform["uplink_name"], platform["hardware_type"],
            )
        artifacts = {
            "bootloader.bin": bootloader if bootloader is not None else _s3_image([
                (0x3FCE0000, b"B" * 64),
                (0x40378000, b"C" * 64),
            ]),
            "partitions.bin": partition_table if partition_table is not None else _rom_partition_table(),
            "ota_data_initial.bin": ota_data if ota_data is not None else b"\xFF" * 0x2000,
            "firmware.bin": firmware,
        }
        full_flash_args = (
            b"--flash_mode dio --flash_freq 80m --flash_size 8MB\n"
            b"0x0 bootloader/bootloader.bin\n"
            b"0x20000 fof_badge_uplink.bin\n"
            b"0x8000 partition_table/partition-table.bin\n"
            b"0xf000 ota_data_initial.bin\n"
        )
        verifier_inputs = {
            **artifacts,
            "flash_args": full_flash_args,
            "flash_app_args": (
                b"--flash_mode dio --flash_freq 80m --flash_size 8MB\n"
                b"0x20000 fof_badge_uplink.bin\n"
            ),
            "flash_project_args": full_flash_args,
            "flasher_args.json": json.dumps({
                "flash_files": {
                    "0x0": "bootloader/bootloader.bin",
                    "0x20000": "fof_badge_uplink.bin",
                    "0x8000": "partition_table/partition-table.bin",
                    "0xf000": "ota_data_initial.bin",
                },
                "app": {
                    "offset": "0x20000",
                    "file": "fof_badge_uplink.bin",
                },
            }).encode(),
            "bootloader/bootloader.bin": artifacts["bootloader.bin"],
            "partition_table/partition-table.bin": artifacts["partitions.bin"],
            "fof_badge_uplink.bin": artifacts["firmware.bin"],
        }
        for name, payload in verifier_inputs.items():
            path = build_dir / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(payload)
        return uplink_dir, build_dir, platform, artifacts

    def _validate(self, uplink_dir: Path, platform: dict,
                  verifier: mock.Mock | None = None):
        verifier = verifier or mock.Mock(return_value=[])
        with mock.patch.object(flash, "UPLINK_DIR", uplink_dir), \
             mock.patch.object(
                 flash, "verify_badge_uplink_build", verifier
             ):
            return flash.validate_current_uplink_rom_layout(
                platform, self.VERSION
            )

    @staticmethod
    def _required_snapshot_paths(
        uplink_dir: Path, build_dir: Path,
    ) -> list[Path]:
        return [build_dir / name for name in _ROM_VERIFIER_BUILD_PATHS] + [
            uplink_dir / "partitions_s3_fof_badge_8mb.csv"
        ]

    def test_exact_current_layout_returns_immutable_regions_and_partitions(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            uplink_dir, build_dir, platform, artifacts = self._fixture(temp_dir)
            verifier = mock.Mock(return_value=[])

            layout = self._validate(uplink_dir, platform, verifier)

            verifier.assert_called_once()
            self.assertTrue(flash.PartitionEntry.__dataclass_params__.frozen)
            self.assertTrue(flash.RomFlashRegion.__dataclass_params__.frozen)
            self.assertTrue(flash.UplinkRomLayout.__dataclass_params__.frozen)
            self.assertEqual(layout.build_dir, build_dir)
            self.assertEqual(layout.version, self.VERSION)
            self.assertIsInstance(layout.regions, tuple)
            self.assertIsInstance(layout.partitions, tuple)
            self.assertEqual(
                [tuple((p.label, p.type, p.subtype, p.offset, p.size, p.flags))
                 for p in layout.partitions],
                _ROM_PARTITION_SPECS,
            )
            self.assertEqual(
                [region.offset for region in layout.regions],
                [0, 0x8000, 0xF000, 0x20000],
            )
            expected_names = [
                "bootloader.bin", "partitions.bin", "ota_data_initial.bin",
                "firmware.bin",
            ]
            self.assertEqual(
                [region.path for region in layout.regions],
                [build_dir / name for name in expected_names],
            )
            for region, name in zip(layout.regions, expected_names):
                self.assertEqual(region.data, artifacts[name])
                self.assertEqual(region.size, len(artifacts[name]))
                self.assertEqual(
                    region.sha256,
                    hashlib.sha256(artifacts[name]).hexdigest(),
                )

    def test_layout_rejects_platform_drift_even_when_firmware_matches_drift(self) -> None:
        drift_fields = (
            "uplink_name", "uplink_project", "hardware_type", "uplink_env"
        )
        for field in drift_fields:
            with self.subTest(field=field), \
                 tempfile.TemporaryDirectory() as temp_dir:
                uplink_dir, _build, platform, _artifacts = self._fixture(
                    temp_dir
                )
                platform[field] = f"attacker-{field}"
                with self.assertRaisesRegex(flash.FlashError, "canonical"):
                    self._validate(uplink_dir, platform)

        with tempfile.TemporaryDirectory() as temp_dir:
            malicious = _rom_firmware_image(
                "attacker-project", self.VERSION, "attacker-target",
                "attacker-hardware",
            )
            uplink_dir, _build, platform, _artifacts = self._fixture(
                temp_dir, firmware=malicious
            )
            platform.update({
                "uplink_name": "attacker-target",
                "uplink_env": "attacker-target",
                "uplink_project": "attacker-project",
                "hardware_type": "attacker-hardware",
            })
            with self.assertRaisesRegex(flash.FlashError, "canonical"):
                self._validate(uplink_dir, platform)

    def test_layout_rejects_missing_required_canonical_platform_fields(self) -> None:
        required_fields = (
            "uplink_name", "uplink_project", "hardware_type", "uplink_env"
        )
        for field in required_fields:
            with self.subTest(field=field), \
                 tempfile.TemporaryDirectory() as temp_dir:
                uplink_dir, _build, platform, _artifacts = self._fixture(
                    temp_dir
                )
                del platform[field]
                with self.assertRaisesRegex(flash.FlashError, "canonical"):
                    self._validate(uplink_dir, platform)

    def test_layout_rejects_stale_caller_and_matching_stale_image_before_reads(self) -> None:
        stale_version = "0.64.75-badge-defcon34"
        with tempfile.TemporaryDirectory() as temp_dir:
            base = flash.PLATFORMS["badge-trio-xiao-s3"]
            stale_firmware = _rom_firmware_image(
                base["uplink_project"], stale_version,
                base["uplink_name"], base["hardware_type"],
            )
            uplink_dir, _build, platform, _artifacts = self._fixture(
                temp_dir, firmware=stale_firmware
            )
            snapshot = mock.Mock(side_effect=AssertionError("artifact read"))
            verifier = mock.Mock(side_effect=AssertionError("verifier called"))
            with mock.patch.object(flash, "UPLINK_DIR", uplink_dir), \
                 mock.patch.object(
                     flash, "repo_version", return_value=self.VERSION
                 ), mock.patch.object(
                     flash, "_read_regular_file_snapshot", snapshot,
                     create=True,
                 ), mock.patch.object(
                     flash, "verify_badge_uplink_build", verifier
                 ):
                with self.assertRaisesRegex(flash.FlashError, "version"):
                    flash.validate_current_uplink_rom_layout(
                        platform, stale_version
                    )
            snapshot.assert_not_called()
            verifier.assert_not_called()

    def test_layout_rejects_invalid_internal_repo_version_before_reads(self) -> None:
        for derived in (None, "", "unknown", "not-a-version"):
            with self.subTest(derived=derived), \
                 tempfile.TemporaryDirectory() as temp_dir:
                uplink_dir, _build, platform, _artifacts = self._fixture(
                    temp_dir
                )
                snapshot = mock.Mock(
                    side_effect=AssertionError("artifact read")
                )
                verifier = mock.Mock(
                    side_effect=AssertionError("verifier called")
                )
                with mock.patch.object(flash, "UPLINK_DIR", uplink_dir), \
                     mock.patch.object(
                         flash, "repo_version", return_value=derived
                     ), mock.patch.object(
                         flash, "_read_regular_file_snapshot", snapshot,
                         create=True,
                     ), mock.patch.object(
                         flash, "verify_badge_uplink_build", verifier
                     ):
                    with self.assertRaisesRegex(
                        flash.FlashError, "canonical.*version"
                    ):
                        flash.validate_current_uplink_rom_layout(
                            platform, self.VERSION
                        )
                snapshot.assert_not_called()
                verifier.assert_not_called()

    def test_partition_csv_must_be_exact_regular_non_symlink_before_verifier(self) -> None:
        for mutation in ("missing", "directory", "symlink"):
            with self.subTest(mutation=mutation), \
                 tempfile.TemporaryDirectory() as temp_dir:
                uplink_dir, _build, platform, _artifacts = self._fixture(
                    temp_dir
                )
                source = uplink_dir / "partitions_s3_fof_badge_8mb.csv"
                source.unlink()
                if mutation == "directory":
                    source.mkdir()
                elif mutation == "symlink":
                    target = Path(temp_dir) / "other.csv"
                    target.write_text("not canonical\n")
                    source.symlink_to(target)
                verifier = mock.Mock(return_value=[])
                with mock.patch.object(flash, "UPLINK_DIR", uplink_dir), \
                     mock.patch.object(
                         flash, "verify_badge_uplink_build", verifier
                     ):
                    with self.assertRaisesRegex(
                        flash.FlashError, "partition CSV"
                    ):
                        flash.validate_current_uplink_rom_layout(
                            platform, self.VERSION
                        )
                verifier.assert_not_called()

    def test_s3_image_accepts_structurally_exact_fixture(self) -> None:
        image = _s3_image([
            (0x3C000020, b"A" * 16),
            (0x40378000, b"B" * 32),
        ])
        flash._validate_esp32_s3_image(
            image, Path("fixture.bin"), "fixture"
        )

    def test_s3_image_accepts_entry_in_each_executable_memory_range(self) -> None:
        cases = (
            ("IRAM", 0x40378000),
            ("IROM", 0x42000020),
            ("RTC IRAM", 0x600FE000),
        )
        for memory, address in cases:
            with self.subTest(memory=memory):
                image = _s3_image(
                    [(address, b"A" * 16)], entry_point=address
                )
                flash._validate_esp32_s3_image(
                    image, Path("fixture.bin"), "fixture"
                )

    def test_s3_image_rejects_entry_in_nonexecutable_drom_and_dram(self) -> None:
        cases = (
            ("DROM", 0x3C000020),
            ("DRAM", 0x3FC88000),
        )
        for memory, address in cases:
            with self.subTest(memory=memory):
                image = _s3_image(
                    [(address, b"A" * 16)], entry_point=address
                )
                with self.assertRaisesRegex(
                    flash.FlashError, "executable"
                ):
                    flash._validate_esp32_s3_image(
                        image, Path("fixture.bin"), "fixture"
                    )

    def test_s3_image_rejects_empty_header_and_segment_truncations(self) -> None:
        image = _s3_image(
            [(0x40378000, b"A" * 16)], entry_point=0x40378000
        )
        checksum_at = 63
        mutations = (
            ("empty", b"", "header"),
            ("image header", image[:23], "header"),
            ("segment header", image[:28], "segment header"),
            ("segment data", image[:40], "segment data"),
            ("checksum", image[:checksum_at], "checksum"),
            ("digest", image[:-1], "digest"),
        )
        for mutation, payload, expected_error in mutations:
            with self.subTest(mutation=mutation):
                with self.assertRaisesRegex(
                    flash.FlashError, expected_error
                ):
                    flash._validate_esp32_s3_image(
                        payload, Path("fixture.bin"), "fixture"
                    )

    def test_s3_image_rejects_every_required_header_mutation(self) -> None:
        exact = _s3_image([(0x3C000020, b"A" * 16)])
        mutations = {
            "magic": (0, 0x00, "magic"),
            "zero segments": (1, 0, "segment count"),
            "excess segments": (1, 17, "segment count"),
            "mode": (2, 0, "DIO"),
            "size": (3, 0x2F, "size/frequency"),
            "frequency": (3, 0x3E, "size/frequency"),
            "write protect": (8, 0, "write-protect"),
            "chip": (12, 8, "chip"),
            "reserved": (19, 1, "reserved"),
            "append": (23, 0, "append_digest"),
        }
        for mutation, (offset, value, expected_error) in mutations.items():
            damaged = bytearray(exact)
            damaged[offset] = value
            with self.subTest(mutation=mutation):
                with self.assertRaisesRegex(
                    flash.FlashError, expected_error
                ):
                    flash._validate_esp32_s3_image(
                        bytes(damaged), Path("fixture.bin"), "fixture"
                    )

        zero_entry = bytearray(exact)
        struct.pack_into("<I", zero_entry, 4, 0)
        with self.assertRaisesRegex(flash.FlashError, "entry point"):
            flash._validate_esp32_s3_image(
                bytes(zero_entry), Path("fixture.bin"), "fixture"
            )

    def test_s3_image_pins_exact_s3_revision_tuple_with_repaired_digest(self) -> None:
        exact = _s3_image([(0x40378000, b"A" * 16)])
        mutations = (
            ("minimum revision", 14, b"\x01"),
            ("minimum full revision", 15, b"\x01\x00"),
            ("maximum full revision", 17, b"\xFE\xFF"),
        )
        for field, offset, replacement in mutations:
            damaged = bytearray(exact)
            damaged[offset:offset + len(replacement)] = replacement
            digest_at = len(damaged) - hashlib.sha256().digest_size
            damaged[digest_at:] = hashlib.sha256(damaged[:digest_at]).digest()
            with self.subTest(field=field):
                with self.assertRaisesRegex(
                    flash.FlashError, "revision"
                ):
                    flash._validate_esp32_s3_image(
                        bytes(damaged), Path("fixture.bin"), "fixture"
                    )

    def test_s3_image_rejects_invalid_segment_sizes_addresses_and_overlap(self) -> None:
        exact = _s3_image([(0x3C000020, b"A" * 16)])
        zero = bytearray(exact)
        struct.pack_into("<I", zero, 28, 0)
        misaligned = bytearray(exact)
        struct.pack_into("<I", misaligned, 28, 6)
        overflow = bytearray(exact)
        struct.pack_into("<I", overflow, 24, 0xFFFFFFF8)
        cases = (
            ("zero", bytes(zero), "zero"),
            ("misaligned", bytes(misaligned), "4-byte"),
            ("overflow", bytes(overflow), "overflow"),
            ("overlap", _s3_image([
                (0x40378000, b"A" * 16),
                (0x40378008, b"B" * 16),
            ], entry_point=0x40378000), "overlap"),
        )
        for mutation, payload, expected_error in cases:
            with self.subTest(mutation=mutation):
                with self.assertRaisesRegex(
                    flash.FlashError, expected_error
                ):
                    flash._validate_esp32_s3_image(
                        payload, Path("fixture.bin"), "fixture"
                    )

    def test_s3_image_allows_only_reserved_zero_segments_and_excludes_them(self) -> None:
        valid = _s3_image([
            (0, b""),
            (4, b""),
            (0x40378000, b"A" * 16),
        ], entry_point=0x40378000)
        flash._validate_esp32_s3_image(
            valid, Path("fixture.bin"), "fixture", flash_offset=0
        )

        reserved_entry = _s3_image([
            (4, b""),
            (0x40378000, b"A" * 16),
        ], entry_point=4)
        with self.assertRaisesRegex(flash.FlashError, "entry point"):
            flash._validate_esp32_s3_image(
                reserved_entry, Path("fixture.bin"), "fixture",
                flash_offset=0,
            )

    def test_s3_image_requires_entry_inside_nonreserved_segment(self) -> None:
        image = _s3_image(
            [(0x40378000, b"A" * 16)], entry_point=0x40379000
        )
        with self.assertRaisesRegex(flash.FlashError, "entry point"):
            flash._validate_esp32_s3_image(
                image, Path("fixture.bin"), "fixture", flash_offset=0
            )

    def test_s3_image_rejects_invalid_memory_range_and_boundary_crossing(self) -> None:
        cases = (
            ("invalid", _s3_image(
                [(0x20000000, b"A" * 16)], entry_point=0x20000000
            )),
            ("crossing", _s3_image(
                [(0x3FD00000 - 8, b"A" * 16)],
                entry_point=0x3FD00000 - 8,
            )),
        )
        for mutation, image in cases:
            with self.subTest(mutation=mutation):
                with self.assertRaisesRegex(flash.FlashError, "S3 memory"):
                    flash._validate_esp32_s3_image(
                        image, Path("fixture.bin"), "fixture",
                        flash_offset=0,
                    )

    def test_s3_image_enforces_mapped_flash_offset_congruence(self) -> None:
        valid = _s3_image([
            (0x3C001020, b"A" * 16),
            (0x40378000, b"B" * 16),
        ], entry_point=0x40378000)
        flash._validate_esp32_s3_image(
            valid, Path("fixture.bin"), "fixture", flash_offset=0x21000
        )

        wrong = _s3_image([
            (0x3C001024, b"A" * 16),
            (0x40378000, b"B" * 16),
        ], entry_point=0x40378000)
        with self.assertRaisesRegex(flash.FlashError, "mapped flash"):
            flash._validate_esp32_s3_image(
                wrong, Path("fixture.bin"), "fixture",
                flash_offset=0x21000,
            )

    def test_s3_image_rejects_padding_checksum_digest_and_trailing_bytes(self) -> None:
        exact = _s3_image(
            [(0x40378000, b"A" * 16)], entry_point=0x40378000
        )
        checksum_at = 63
        mutations: list[tuple[str, bytes, str]] = []
        padding = bytearray(exact)
        padding[48] = 1
        mutations.append(("padding", bytes(padding), "padding"))
        checksum = bytearray(exact)
        checksum[checksum_at] ^= 1
        mutations.append(("checksum", bytes(checksum), "checksum"))
        digest = bytearray(exact)
        digest[checksum_at + 1] ^= 1
        mutations.append(("digest", bytes(digest), "digest"))
        mutations.append(("trailing", exact + b"\x00", "trailing"))
        for mutation, payload, expected_error in mutations:
            with self.subTest(mutation=mutation):
                with self.assertRaisesRegex(
                    flash.FlashError, expected_error
                ):
                    flash._validate_esp32_s3_image(
                        payload, Path("fixture.bin"), "fixture"
                    )

    def test_layout_deep_validates_bootloader_and_firmware_images(self) -> None:
        for name in ("bootloader.bin", "firmware.bin"):
            with self.subTest(name=name), \
                 tempfile.TemporaryDirectory() as temp_dir:
                uplink_dir, build_dir, platform, _artifacts = self._fixture(
                    temp_dir
                )
                image_path = build_dir / name
                damaged = bytearray(image_path.read_bytes())
                damaged[-1] ^= 1
                image_path.write_bytes(damaged)
                with self.assertRaisesRegex(flash.FlashError, "digest"):
                    self._validate(uplink_dir, platform)

    def test_ota_data_must_be_exact_erased_ff_image(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            uplink_dir, _build, platform, _artifacts = self._fixture(
                temp_dir, ota_data=b"O" * 0x2000
            )
            with self.assertRaisesRegex(flash.FlashError, "all 0xFF"):
                self._validate(uplink_dir, platform)

    def test_layout_regions_keep_original_bytes_after_sources_change(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            uplink_dir, _build, platform, artifacts = self._fixture(temp_dir)
            layout = self._validate(uplink_dir, platform)
            original = {
                region.path.name: region.data for region in layout.regions
            }
            for index, region in enumerate(layout.regions):
                if index < 2:
                    region.path.write_bytes(b"changed")
                else:
                    region.path.unlink()

            for region in layout.regions:
                self.assertIsInstance(region.data, bytes)
                self.assertEqual(region.data, original[region.path.name])
                self.assertEqual(region.data, artifacts[region.path.name])
                self.assertEqual(region.size, len(region.data))
                self.assertEqual(
                    region.sha256, hashlib.sha256(region.data).hexdigest()
                )
                self.assertNotIn(repr(region.data), repr(region))

    def test_layout_verifies_private_snapshots_and_survives_source_swap(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            uplink_dir, build_dir, platform, artifacts = self._fixture(
                temp_dir
            )
            expected_build = {
                name: (build_dir / name).read_bytes()
                for name in _ROM_VERIFIER_BUILD_PATHS
            }
            source = uplink_dir / "partitions_s3_fof_badge_8mb.csv"
            expected_source = source.read_bytes()
            private_roots: list[Path] = []

            def verifier(private_build: Path, private_source: Path) -> list[str]:
                private_roots.append(private_build)
                self.assertNotEqual(private_build, build_dir)
                self.assertNotEqual(private_source, source)
                self.assertEqual(private_build.stat().st_mode & 0o777, 0o700)
                for directory in ("bootloader", "partition_table"):
                    private_directory = private_build / directory
                    self.assertTrue(private_directory.is_dir())
                    self.assertFalse(private_directory.is_symlink())
                    self.assertEqual(
                        private_directory.stat().st_mode & 0o777, 0o700
                    )
                for name, wanted in expected_build.items():
                    private_path = private_build / name
                    self.assertTrue(private_path.is_file())
                    self.assertFalse(private_path.is_symlink())
                    self.assertEqual(private_path.stat().st_mode & 0o777, 0o400)
                    self.assertEqual(private_path.read_bytes(), wanted)
                self.assertTrue(private_source.is_file())
                self.assertFalse(private_source.is_symlink())
                self.assertEqual(private_source.stat().st_mode & 0o777, 0o400)
                self.assertEqual(private_source.read_bytes(), expected_source)

                (build_dir / "firmware.bin").write_bytes(b"attacker")
                (build_dir / "fof_badge_uplink.bin").unlink()
                return []

            with mock.patch.object(flash, "UPLINK_DIR", uplink_dir), \
                 mock.patch.object(
                     flash, "verify_badge_uplink_build", verifier
                 ):
                layout = flash.validate_current_uplink_rom_layout(
                    platform, self.VERSION
                )

            self.assertEqual(len(private_roots), 1)
            self.assertFalse(private_roots[0].exists())
            self.assertEqual(
                {region.path.name: region.data for region in layout.regions},
                artifacts,
            )

    def test_private_verifier_tree_is_cleaned_after_verifier_failure(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            uplink_dir, _build, platform, _artifacts = self._fixture(temp_dir)
            private_roots: list[Path] = []

            def verifier(private_build: Path, _source: Path) -> list[str]:
                private_roots.append(private_build)
                return ["manifest mismatch"]

            with mock.patch.object(flash, "UPLINK_DIR", uplink_dir), \
                 mock.patch.object(
                     flash, "verify_badge_uplink_build", verifier
                 ):
                with self.assertRaisesRegex(
                    flash.FlashError, "manifest mismatch"
                ):
                    flash.validate_current_uplink_rom_layout(
                        platform, self.VERSION
                    )

            self.assertEqual(len(private_roots), 1)
            self.assertFalse(private_roots[0].exists())

    def test_each_verifier_input_rejects_missing_symlink_and_nonregular(self) -> None:
        for mutation in ("missing", "symlink", "directory"):
            for index in range(len(_ROM_VERIFIER_BUILD_PATHS) + 1):
                with self.subTest(mutation=mutation, index=index), \
                     tempfile.TemporaryDirectory() as temp_dir:
                    uplink_dir, build_dir, platform, _artifacts = self._fixture(
                        temp_dir
                    )
                    path = self._required_snapshot_paths(
                        uplink_dir, build_dir
                    )[index]
                    payload = path.read_bytes()
                    path.unlink()
                    if mutation == "symlink":
                        target = Path(temp_dir) / f"replacement-{index}"
                        target.write_bytes(payload)
                        path.symlink_to(target)
                    elif mutation == "directory":
                        path.mkdir()
                    with self.assertRaises(flash.FlashError):
                        self._validate(uplink_dir, platform)

    def test_each_verifier_input_has_a_fail_closed_size_bound(self) -> None:
        limits = {
            "bootloader.bin": 0x8000,
            "partitions.bin": 0x1000,
            "ota_data_initial.bin": 0x2000,
            "firmware.bin": 0x200000,
            "flash_args": 0x10000,
            "flash_app_args": 0x10000,
            "flash_project_args": 0x10000,
            "flasher_args.json": 0x10000,
            "bootloader/bootloader.bin": 0x8000,
            "partition_table/partition-table.bin": 0x1000,
            "fof_badge_uplink.bin": 0x200000,
            "partitions_s3_fof_badge_8mb.csv": 0x10000,
        }
        for name, limit in limits.items():
            with self.subTest(name=name), \
                 tempfile.TemporaryDirectory() as temp_dir:
                uplink_dir, build_dir, platform, _artifacts = self._fixture(
                    temp_dir
                )
                path = (
                    uplink_dir / name
                    if name == "partitions_s3_fof_badge_8mb.csv"
                    else build_dir / name
                )
                path.write_bytes(b"X" * (limit + 1))
                with self.assertRaisesRegex(
                    flash.FlashError, "size|exceed|bytes"
                ):
                    self._validate(uplink_dir, platform)

    def test_snapshot_rejects_file_metadata_change_during_fd_read(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            uplink_dir, _build, platform, _artifacts = self._fixture(temp_dir)
            real_fstat = os.fstat
            regular_calls = 0

            def changed_fstat(fd: int):
                nonlocal regular_calls
                result = real_fstat(fd)
                if not stat.S_ISREG(result.st_mode):
                    return result
                regular_calls += 1
                if regular_calls != 2:
                    return result
                return SimpleNamespace(
                    st_mode=result.st_mode,
                    st_dev=result.st_dev,
                    st_ino=result.st_ino,
                    st_size=result.st_size,
                    st_uid=result.st_uid,
                    st_gid=result.st_gid,
                    st_mtime_ns=result.st_mtime_ns + 1,
                    st_ctime_ns=result.st_ctime_ns,
                    st_nlink=result.st_nlink,
                )

            with mock.patch.object(flash.os, "fstat", changed_fstat):
                with self.assertRaisesRegex(
                    flash.FlashError, "changed during read"
                ):
                    self._validate(uplink_dir, platform)

    def test_fd_nofollow_rejects_symlink_swap_at_open(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            uplink_dir, build_dir, platform, _artifacts = self._fixture(
                temp_dir
            )
            target = build_dir / "firmware.bin"
            attacker = Path(temp_dir) / "attacker-firmware.bin"
            attacker.write_bytes(target.read_bytes())
            real_open = os.open
            observed_flags: list[int] = []

            def racing_open(path, flags, mode=0o777, *, dir_fd=None):
                if path == target.name and dir_fd is not None and \
                        not flags & os.O_DIRECTORY and not observed_flags:
                    observed_flags.append(flags)
                    target.unlink()
                    target.symlink_to(attacker)
                if dir_fd is None:
                    return real_open(path, flags, mode)
                return real_open(path, flags, mode, dir_fd=dir_fd)

            with mock.patch.object(flash.os, "open", racing_open):
                with self.assertRaises(flash.FlashError):
                    self._validate(uplink_dir, platform)
            self.assertEqual(len(observed_flags), 1)
            self.assertTrue(observed_flags[0] & os.O_NOFOLLOW)

    def test_snapshot_rejects_fifo_without_blocking(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            uplink_dir, build_dir, platform, _artifacts = self._fixture(
                temp_dir
            )
            fifo = build_dir / "flash_args"
            fifo.unlink()
            os.mkfifo(fifo)
            with self.assertRaises(flash.FlashError):
                self._validate(uplink_dir, platform)

    def test_snapshot_rejects_hardlinked_source(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            uplink_dir, build_dir, platform, _artifacts = self._fixture(
                temp_dir
            )
            source = build_dir / "flash_args"
            os.link(source, Path(temp_dir) / "second-flash-args-link")
            with self.assertRaisesRegex(flash.FlashError, "link"):
                self._validate(uplink_dir, platform)

    def test_snapshot_rejects_symlinked_nested_verifier_directory(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            uplink_dir, build_dir, platform, _artifacts = self._fixture(
                temp_dir
            )
            nested = build_dir / "bootloader"
            payload = (nested / "bootloader.bin").read_bytes()
            (nested / "bootloader.bin").unlink()
            nested.rmdir()
            replacement = Path(temp_dir) / "attacker-bootloader"
            replacement.mkdir()
            (replacement / "bootloader.bin").write_bytes(payload)
            nested.symlink_to(replacement, target_is_directory=True)
            with self.assertRaises(flash.FlashError):
                self._validate(uplink_dir, platform)

    def test_snapshot_rejects_nested_directory_swap_during_open(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            uplink_dir, build_dir, platform, _artifacts = self._fixture(
                temp_dir
            )
            nested = build_dir / "bootloader"
            replacement = Path(temp_dir) / "attacker-bootloader"
            replacement.mkdir()
            (replacement / "bootloader.bin").write_bytes(
                (nested / "bootloader.bin").read_bytes()
            )
            held = build_dir / "bootloader-held"
            real_open = os.open
            observed: list[int] = []

            def racing_open(path, flags, mode=0o777, *, dir_fd=None):
                if path == "bootloader" and flags & os.O_DIRECTORY and \
                        not observed:
                    observed.append(flags)
                    nested.rename(held)
                    nested.symlink_to(replacement, target_is_directory=True)
                if dir_fd is None:
                    return real_open(path, flags, mode)
                return real_open(path, flags, mode, dir_fd=dir_fd)

            with mock.patch.object(flash.os, "open", racing_open):
                with self.assertRaises(flash.FlashError):
                    self._validate(uplink_dir, platform)
            self.assertEqual(len(observed), 1)
            self.assertTrue(observed[0] & os.O_NOFOLLOW)

    def test_snapshot_closes_all_directory_fds_when_child_fstat_fails(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            uplink_dir, build_dir, _platform, _artifacts = self._fixture(
                temp_dir
            )
            path = build_dir / "bootloader" / "bootloader.bin"
            real_open = os.open
            real_fstat = os.fstat
            directory_fds: list[int] = []
            injected = False

            def tracked_open(path, flags, mode=0o777, *, dir_fd=None):
                if dir_fd is None:
                    fd = real_open(path, flags, mode)
                else:
                    fd = real_open(path, flags, mode, dir_fd=dir_fd)
                if flags & os.O_DIRECTORY:
                    directory_fds.append(fd)
                return fd

            def failing_fstat(fd: int):
                nonlocal injected
                if len(directory_fds) >= 2 and fd == directory_fds[-1] and \
                        not injected:
                    injected = True
                    raise OSError("injected child fstat failure")
                return real_fstat(fd)

            with mock.patch.object(flash.os, "open", tracked_open), \
                 mock.patch.object(flash.os, "fstat", failing_fstat):
                with self.assertRaises(flash.FlashError):
                    flash._read_regular_file_snapshot(
                        path,
                        root=uplink_dir,
                        max_size=0x8000,
                        artifact="nested alias",
                    )

            self.assertTrue(injected)
            self.assertGreaterEqual(len(directory_fds), 2)
            for fd in directory_fds:
                with self.subTest(fd=fd):
                    with self.assertRaises(OSError):
                        real_fstat(fd)

    def test_snapshot_rejects_parent_component_escape_from_root(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir) / "root"
            root.mkdir()
            outside = Path(temp_dir) / "outside.bin"
            outside.write_bytes(b"outside")
            escaped = root / ".." / outside.name
            with self.assertRaisesRegex(flash.FlashError, "escape"):
                flash._read_regular_file_snapshot(
                    escaped,
                    root=root,
                    max_size=64,
                    artifact="escape fixture",
                )

    def test_decode_partition_table_accepts_exact_idf_fixture(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            path = Path(temp_dir) / "partitions.bin"
            path.write_bytes(_rom_partition_table())
            decoded = flash.decode_partition_table(path)
        self.assertEqual(
            [tuple((p.label, p.type, p.subtype, p.offset, p.size, p.flags))
             for p in decoded],
            _ROM_PARTITION_SPECS,
        )

    def test_decode_rejects_entry_magic_md5_trailer_digest_padding_and_trailing(self) -> None:
        exact = _rom_partition_table()
        entry_bytes = len(_ROM_PARTITION_SPECS) * 32
        mutations = {
            "entry magic": bytes([0, 0]) + exact[2:],
            "MD5 trailer": exact[:entry_bytes] + b"\xEA\xEB" + exact[entry_bytes + 2:],
            "MD5 digest": exact[:entry_bytes + 16] + bytes([
                exact[entry_bytes + 16] ^ 1
            ]) + exact[entry_bytes + 17:],
            "padding": exact[:-1] + b"\x00",
            "trailing": exact + b"\xFF",
        }
        for expected_error, payload in mutations.items():
            with self.subTest(expected_error=expected_error), \
                 tempfile.TemporaryDirectory() as temp_dir:
                path = Path(temp_dir) / "partitions.bin"
                path.write_bytes(payload)
                with self.assertRaisesRegex(
                    flash.FlashError, expected_error
                ):
                    flash.decode_partition_table(path)

    def test_decode_rejects_non_ascii_dirty_empty_and_duplicate_labels(self) -> None:
        raw_specs = list(_ROM_PARTITION_SPECS)
        label_mutations = {
            "ASCII": b"\xFFvs",
            "NUL": b"nvs\x00garbage",
            "empty": b"",
        }
        for expected_error, label in label_mutations.items():
            specs = list(raw_specs)
            specs[0] = (label, *specs[0][1:])
            with self.subTest(expected_error=expected_error), \
                 tempfile.TemporaryDirectory() as temp_dir:
                path = Path(temp_dir) / "partitions.bin"
                path.write_bytes(_rom_partition_table(specs))
                with self.assertRaisesRegex(
                    flash.FlashError, expected_error
                ):
                    flash.decode_partition_table(path)

        duplicate = list(raw_specs)
        duplicate[1] = (duplicate[0][0], *duplicate[1][1:])
        with tempfile.TemporaryDirectory() as temp_dir:
            path = Path(temp_dir) / "partitions.bin"
            path.write_bytes(_rom_partition_table(duplicate))
            with self.assertRaisesRegex(flash.FlashError, "duplicate"):
                flash.decode_partition_table(path)

    def test_layout_rejects_missing_extra_reordered_and_each_field_drift(self) -> None:
        mutation_specs: list[tuple[str, list[tuple]]] = []
        mutation_specs.append(("missing", list(_ROM_PARTITION_SPECS[:-1])))
        mutation_specs.append(("extra", list(_ROM_PARTITION_SPECS) + [
            ("extra", 1, 0x99, 0x12000, 0x1000, 0)
        ]))
        reordered = list(_ROM_PARTITION_SPECS)
        reordered[0], reordered[1] = reordered[1], reordered[0]
        mutation_specs.append(("order", reordered))
        field_values = {
            "label": "nvs_changed",
            "type": 0,
            "subtype": 3,
            "offset": 0x8000,
            "size": 0x5000,
            "flags": 1,
        }
        field_names = ["label", "type", "subtype", "offset", "size", "flags"]
        for field, value in field_values.items():
            specs = list(_ROM_PARTITION_SPECS)
            changed = list(specs[0])
            changed[field_names.index(field)] = value
            specs[0] = tuple(changed)
            mutation_specs.append((field, specs))

        for mutation, specs in mutation_specs:
            with self.subTest(mutation=mutation), \
                 tempfile.TemporaryDirectory() as temp_dir:
                uplink_dir, _build, platform, _artifacts = self._fixture(
                    temp_dir, partition_table=_rom_partition_table(specs)
                )
                with self.assertRaises(flash.FlashError):
                    self._validate(uplink_dir, platform)

    def test_layout_rejects_zero_overlap_arithmetic_flash_overflow_and_forbidden_app(self) -> None:
        cases = []
        zero = list(_ROM_PARTITION_SPECS)
        zero[2] = (*zero[2][:4], 0, zero[2][5])
        cases.append(("zero", zero))
        overlap = list(_ROM_PARTITION_SPECS)
        overlap[0] = (*overlap[0][:4], 0x7000, overlap[0][5])
        cases.append(("overlap", overlap))
        arithmetic = list(_ROM_PARTITION_SPECS)
        arithmetic[-1] = (
            *arithmetic[-1][:3], 0xFFFFF000, 0x2000,
            arithmetic[-1][5],
        )
        cases.append(("arithmetic", arithmetic))
        flash_overflow = list(_ROM_PARTITION_SPECS)
        flash_overflow[-1] = (
            *flash_overflow[-1][:3], 0x7F0000, 0x20000,
            flash_overflow[-1][5],
        )
        cases.append(("8 MiB", flash_overflow))
        forbidden = list(_ROM_PARTITION_SPECS)
        forbidden[3] = (
            *forbidden[3][:3], 0x10000, forbidden[3][4], forbidden[3][5]
        )
        cases.append(("0x10000", forbidden))

        for expected_error, specs in cases:
            with self.subTest(expected_error=expected_error), \
                 tempfile.TemporaryDirectory() as temp_dir:
                uplink_dir, _build, platform, _artifacts = self._fixture(
                    temp_dir, partition_table=_rom_partition_table(specs)
                )
                with self.assertRaisesRegex(
                    flash.FlashError, expected_error
                ):
                    self._validate(uplink_dir, platform)

    def test_layout_rejects_symlink_for_each_canonical_region(self) -> None:
        for name in (
            "bootloader.bin", "partitions.bin", "ota_data_initial.bin",
            "firmware.bin",
        ):
            with self.subTest(name=name), \
                 tempfile.TemporaryDirectory() as temp_dir:
                uplink_dir, build_dir, platform, _artifacts = self._fixture(
                    temp_dir
                )
                target = Path(temp_dir) / f"real-{name}"
                canonical = build_dir / name
                target.write_bytes(canonical.read_bytes())
                canonical.unlink()
                canonical.symlink_to(target)
                with self.assertRaisesRegex(flash.FlashError, "symlink"):
                    self._validate(uplink_dir, platform)

    def test_layout_rejects_each_rom_region_bound(self) -> None:
        cases = {
            "bootloader": {"bootloader": b"B" * 0x8001},
            "partitions": {
                "partition_table": _rom_partition_table(total_size=0x1020)
            },
            "OTA data": {"ota_data": b"O" * 0x1FFF},
        }
        for expected_error, kwargs in cases.items():
            with self.subTest(expected_error=expected_error), \
                 tempfile.TemporaryDirectory() as temp_dir:
                uplink_dir, _build, platform, _artifacts = self._fixture(
                    temp_dir, **kwargs
                )
                with self.assertRaisesRegex(
                    flash.FlashError, expected_error
                ):
                    self._validate(uplink_dir, platform)

        with tempfile.TemporaryDirectory() as temp_dir:
            platform = dict(flash.PLATFORMS["badge-trio-xiao-s3"])
            oversized = _firmware_image(
                platform["uplink_project"], self.VERSION,
                platform["uplink_name"], platform["hardware_type"],
            ).ljust(0x200001, b"F")
            uplink_dir, _build, platform, _artifacts = self._fixture(
                temp_dir, firmware=oversized
            )
            with self.assertRaisesRegex(flash.FlashError, "ota_0"):
                self._validate(uplink_dir, platform)

    def test_partition_image_bound_aborts_before_deep_decoding(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            uplink_dir, _build, platform, _artifacts = self._fixture(
                temp_dir, partition_table=b"\x00" * 0x1001
            )
            decoder = mock.Mock(return_value=flash.UPLINK_ROM_PARTITIONS)
            with mock.patch.object(flash, "UPLINK_DIR", uplink_dir), \
                 mock.patch.object(
                     flash, "verify_badge_uplink_build", return_value=[]
                 ), mock.patch.object(
                     flash, "_decode_partition_table_bytes", decoder
                 ):
                with self.assertRaisesRegex(flash.FlashError, "0x9000"):
                    flash.validate_current_uplink_rom_layout(
                        platform, self.VERSION
                    )
            decoder.assert_not_called()

    def test_layout_rejects_wrong_firmware_target_project_hardware_and_version(self) -> None:
        base_platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        images = {
            "target": _rom_firmware_image(
                base_platform["uplink_project"], self.VERSION,
                "wrong-uplink-target", base_platform["hardware_type"],
            ),
            "project": _rom_firmware_image(
                "wrong_project", self.VERSION,
                base_platform["uplink_name"], base_platform["hardware_type"],
            ),
            "hardware": _rom_firmware_image(
                base_platform["uplink_project"], self.VERSION,
                base_platform["uplink_name"], "wrong_hardware",
            ),
            "version": _rom_firmware_image(
                base_platform["uplink_project"], "0.64.75-badge-defcon34",
                base_platform["uplink_name"], base_platform["hardware_type"],
            ),
        }
        for expected_error, image in images.items():
            with self.subTest(expected_error=expected_error), \
                 tempfile.TemporaryDirectory() as temp_dir:
                uplink_dir, _build, platform, _artifacts = self._fixture(
                    temp_dir, firmware=image
                )
                with self.assertRaisesRegex(
                    flash.FlashError, expected_error
                ):
                    self._validate(uplink_dir, platform)

    def test_layout_snapshots_every_input_once_with_fd_nofollow(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            uplink_dir, build_dir, platform, artifacts = self._fixture(temp_dir)
            real_open = os.open
            expected_paths = self._required_snapshot_paths(
                uplink_dir, build_dir
            )
            opens: list[tuple[str, int]] = []

            def tracked_open(path, flags, mode=0o777, *, dir_fd=None):
                if dir_fd is not None and flags & os.O_NONBLOCK and \
                        flags & os.O_NOFOLLOW and \
                        not flags & os.O_DIRECTORY and \
                        not flags & (os.O_WRONLY | os.O_RDWR | os.O_CREAT):
                    opens.append((str(path), flags))
                if dir_fd is None:
                    return real_open(path, flags, mode)
                return real_open(path, flags, mode, dir_fd=dir_fd)

            with mock.patch.object(flash.os, "open", tracked_open):
                layout = self._validate(uplink_dir, platform)

            self.assertEqual(
                [path for path, _flags in opens],
                [path.name for path in expected_paths],
            )
            required_flags = (
                os.O_NOFOLLOW | os.O_NONBLOCK |
                getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOCTTY", 0)
            )
            self.assertTrue(all(
                flags & required_flags == required_flags
                for _path, flags in opens
            ))
            self.assertEqual(
                [region.sha256 for region in layout.regions],
                [hashlib.sha256(artifacts[name]).hexdigest() for name in (
                    "bootloader.bin", "partitions.bin",
                    "ota_data_initial.bin", "firmware.bin",
                )],
            )


class BadgeApplicationOtaTests(unittest.TestCase):
    VERSION = "0.64.76-badge-defcon34"
    OLD_VERSION = "0.64.75-badge-defcon34"

    def _platform_with_image(self, root: str, *, version: str | None = None,
                             size: int = 5000) -> tuple[dict, bytes]:
        platform = dict(flash.PLATFORMS["badge-trio-xiao-s3"])
        version = version or self.VERSION
        identity = _firmware_image(
            platform["uplink_project"],
            version,
            platform["uplink_name"],
            platform["hardware_type"],
        )
        data = identity + bytes((index % 251 for index in range(size - len(identity))))
        image = Path(root) / "uplink.bin"
        image.write_bytes(data)
        platform["uplink_bin"] = image
        return platform, data

    def test_prefixed_reader_preserves_coalesced_ready_and_credit(self) -> None:
        ready = _uplink_receipt("ready", credit=4096)
        credit = _uplink_receipt("credit", received=4096, credit=904)
        raw = _ScriptedRawSerial([
            (
                "FOF_UPLINK_OTA:" + json.dumps(ready) + "\n" +
                "FOF_UPLINK_OTA:" + json.dumps(credit) + "\n"
            ).encode()
        ])
        badge = flash.BadgeSerial(_usb_record("/dev/fake"), False)
        badge.ser = raw

        self.assertEqual(
            badge.read_prefixed_json(
                "FOF_UPLINK_OTA:",
                1,
                allowed_schema_ids=(
                    flash.HostJsonSchemaId.UPLINK_OTA,
                ),
            ),
            ready,
        )
        self.assertEqual(
            badge.read_prefixed_json(
                "FOF_UPLINK_OTA:",
                1,
                allowed_schema_ids=(
                    flash.HostJsonSchemaId.UPLINK_OTA,
                ),
            ),
            credit,
        )
        self.assertEqual(raw.read_calls, 1)

    def test_prefixed_reader_rejects_unbounded_frame(self) -> None:
        raw = _ScriptedRawSerial([
            b"X" * (flash.SERIAL_RX_BUFFER_MAX + 1) + b"\n"
        ])
        badge = flash.BadgeSerial(_usb_record("/dev/fake"), False)
        badge.ser = raw
        with self.assertRaisesRegex(flash.SerialTransportError, "buffer"):
            badge.read_prefixed_json("FOF_STATUS:", 1)

    def test_pong_text_reader_is_exact_but_accepts_crlf_framing(self) -> None:
        raw = _ScriptedRawSerial([
            (
                f"FOF_PONG:{self.VERSION}\r\n"
                f"FOF_PONG:{self.VERSION} \n"
            ).encode()
        ])
        badge = flash.BadgeSerial(_usb_record("/dev/fake"), False)
        badge.ser = raw
        self.assertEqual(
            badge.read_prefixed_text("FOF_PONG:", 1), self.VERSION
        )
        with self.assertRaisesRegex(flash.SerialTransportError, "malformed"):
            badge.read_prefixed_text("FOF_PONG:", 1)

    def test_json_reader_deadline_exit_preserves_seen_frame_activity(self) -> None:
        clock = SimpleNamespace(now=0.0)
        badge = flash.BadgeSerial(_usb_record("/dev/fake"), False)

        def unrelated_line(_timeout_s: float) -> bytes:
            clock.now = 1.0
            return b"scanner boot noise"

        badge._read_line = unrelated_line  # type: ignore[method-assign]
        with mock.patch.object(
            flash.time, "monotonic", side_effect=lambda: clock.now
        ):
            with self.assertRaises(flash.SerialReadTimeout) as caught:
                badge.read_prefixed_json("FOF_STATUS:", 1)

        self.assertTrue(caught.exception.saw_activity)
        self.assertFalse(caught.exception.partial_frame)

    def test_text_reader_deadline_exit_preserves_seen_frame_activity(self) -> None:
        clock = SimpleNamespace(now=0.0)
        badge = flash.BadgeSerial(_usb_record("/dev/fake"), False)

        def unrelated_line(_timeout_s: float) -> bytes:
            clock.now = 1.0
            return b"scanner boot noise"

        badge._read_line = unrelated_line  # type: ignore[method-assign]
        with mock.patch.object(
            flash.time, "monotonic", side_effect=lambda: clock.now
        ):
            with self.assertRaises(flash.SerialReadTimeout) as caught:
                badge.read_prefixed_text("FOF_PONG:", 1)

        self.assertTrue(caught.exception.saw_activity)
        self.assertFalse(caught.exception.partial_frame)

    def test_open_never_issues_dtr_or_rts_line_state_updates(self) -> None:
        descriptor = _usb_record("/dev/fake")
        handle = mock.Mock()
        badge = flash.BadgeSerial(descriptor, False)
        with mock.patch.object(
            flash, "open_bound_application_serial", return_value=handle
        ) as bound_open:
            badge._open_serial()
        bound_open.assert_called_once_with(
            descriptor,
            expected_uplink_serial=descriptor.serial_number,
            baudrate=115200,
            timeout=0.15,
            write_timeout=3,
        )
        self.assertIs(badge.ser, handle)

    def test_context_close_failure_preserves_active_primary(self) -> None:
        primary = RuntimeError("update recovery primary")
        badge = flash.BadgeSerial(_usb_record("/dev/fake"), True)
        handle = mock.Mock()
        handle.close.side_effect = OSError("USB close failed")
        badge.ser = handle

        with self.assertRaises(RuntimeError) as caught:
            with badge:
                raise primary

        self.assertIs(caught.exception, primary)
        self.assertIsNone(badge.ser)
        handle.close.assert_called_once_with()
        self.assertTrue(any(
            "serial transport context cleanup" in note
            and "USB close failed" in note
            for note in getattr(primary, "__notes__", [])
        ))

    def test_context_close_only_failure_is_typed_and_detaches(self) -> None:
        badge = flash.BadgeSerial(_usb_record("/dev/fake"), True)
        handle = mock.Mock()
        handle.close.side_effect = OSError("USB close failed")
        badge.ser = handle

        with self.assertRaises(flash.SerialTransportError) as caught:
            with badge:
                pass

        self.assertIn("close failed", str(caught.exception))
        self.assertIsNone(badge.ser)
        handle.close.assert_called_once_with()

    def test_open_wraps_serial_constructor_failure(self) -> None:
        badge = flash.BadgeSerial(_usb_record("/dev/missing"), False)
        with mock.patch.object(
            flash, "open_bound_application_serial",
            side_effect=flash.UsbDescriptorBindingError("device vanished"),
        ):
            with self.assertRaises(flash.SerialTransportError) as caught:
                badge._open_serial()
        self.assertTrue(caught.exception.terminal_unavailable)
        self.assertIsNone(badge.ser)

    def test_opened_badge_serial_cannot_reopen_or_change_path(self) -> None:
        badge = flash.BadgeSerial(_usb_record("/dev/fake"), False)
        badge.ser = mock.Mock()
        with mock.patch.object(
            flash, "open_bound_application_serial",
            side_effect=AssertionError("reopen reached serial"),
        ) as bound_open, self.assertRaisesRegex(
            flash.SerialTransportError, "cannot reopen"
        ):
            badge._open_serial()
        bound_open.assert_not_called()
        with self.assertRaises(AttributeError):
            badge.port = "/dev/replacement"  # type: ignore[misc]

    def test_open_wraps_configuration_failure_and_cleans_up(self) -> None:
        badge = flash.BadgeSerial(_usb_record("/dev/fake"), False)
        with mock.patch.object(
            flash, "open_bound_application_serial",
            side_effect=flash.UsbDescriptorBindingError("bad baud"),
        ):
            with self.assertRaises(flash.SerialTransportError) as caught:
                badge._open_serial()
        self.assertTrue(caught.exception.terminal_unavailable)
        self.assertIsNone(badge.ser)

    def test_open_wraps_unopenable_transport_and_cleans_up(self) -> None:
        badge = flash.BadgeSerial(_usb_record("/dev/missing"), False)
        with mock.patch.object(
            flash, "open_bound_application_serial",
            side_effect=flash.UsbDescriptorBindingError("no such device"),
        ):
            with self.assertRaises(flash.SerialTransportError) as caught:
                badge._open_serial()
        self.assertTrue(caught.exception.terminal_unavailable)
        self.assertIsNone(badge.ser)

    def test_open_wraps_input_reset_failure_and_cleans_partial_open(self) -> None:
        badge = flash.BadgeSerial(_usb_record("/dev/fake"), False)
        with mock.patch.object(
            flash, "open_bound_application_serial",
            side_effect=flash.UsbDescriptorBindingError(
                "device disconnected"
            ),
        ):
            with self.assertRaises(flash.SerialTransportError) as caught:
                badge._open_serial()
        self.assertTrue(caught.exception.terminal_unavailable)
        self.assertIsNone(badge.ser)

    def test_command_write_and_flush_failures_are_typed_transport_errors(self) -> None:
        class FailingHandle:
            def __init__(self, stage: str) -> None:
                self.stage = stage

            def write(self, payload: bytes) -> int:
                if self.stage == "write":
                    raise OSError("write disconnected")
                return len(payload)

            def flush(self) -> None:
                if self.stage == "flush":
                    raise OSError("flush disconnected")

        for stage in ("write", "flush"):
            badge = flash.BadgeSerial(_usb_record("/dev/fake"), False)
            badge.ser = FailingHandle(stage)
            with self.subTest(stage=stage):
                with self.assertRaises(flash.SerialTransportError) as caught:
                    badge.write_line("FOF_STATUS")
                self.assertTrue(caught.exception.terminal_unavailable)

    def test_command_short_write_is_terminal_and_never_flushes(self) -> None:
        class ShortWriteHandle:
            flushed = False

            def write(self, payload: bytes) -> int:
                return len(payload) - 1

            def flush(self) -> None:
                self.flushed = True

        handle = ShortWriteHandle()
        badge = flash.BadgeSerial(_usb_record("/dev/fake"), False)
        badge.ser = handle
        with self.assertRaisesRegex(
            flash.SerialTransportError, "short"
        ) as caught:
            badge.write_line("FOF_STATUS")
        self.assertTrue(caught.exception.terminal_unavailable)
        self.assertFalse(handle.flushed)

    def test_silent_application_never_invokes_generic_runner(self) -> None:
        badge = flash.BadgeSerial(_usb_record("/dev/fake"), False)
        badge._wait_ping_once = mock.Mock(  # type: ignore[method-assign]
            side_effect=flash.SerialReadTimeout(
                "silent", saw_activity=False, partial_frame=False
            )
        )
        with mock.patch.object(flash, "run") as run:
            with self.assertRaises(flash.SerialReadTimeout):
                badge.wait_ping(timeout_s=1)
        run.assert_not_called()

    def test_wait_ping_retries_dropped_reply_within_one_deadline(self) -> None:
        clock = SimpleNamespace(now=0.0)
        badge = flash.BadgeSerial(_usb_record("/dev/fake"), False)
        attempt_budgets: list[float] = []

        def ping_once(timeout_s: float) -> None:
            attempt_budgets.append(timeout_s)
            if len(attempt_budgets) == 1:
                clock.now += timeout_s
                raise flash.SerialReadTimeout(
                    "first reply dropped", saw_activity=False,
                    partial_frame=False,
                )

        badge._wait_ping_once = ping_once  # type: ignore[method-assign]
        with mock.patch.object(
            flash.time, "monotonic", side_effect=lambda: clock.now
        ), \
             mock.patch.object(flash, "run") as run:
            badge.wait_ping(timeout_s=2)

        self.assertGreaterEqual(len(attempt_budgets), 2)
        self.assertLessEqual(sum(attempt_budgets), 2.0)
        run.assert_not_called()

    def test_wait_ping_final_timeout_accumulates_activity_and_partial(self) -> None:
        clock = SimpleNamespace(now=0.0)
        badge = flash.BadgeSerial(_usb_record("/dev/fake"), False)
        attempt_budgets: list[float] = []

        def ping_once(timeout_s: float) -> None:
            attempt_budgets.append(timeout_s)
            clock.now += timeout_s
            first = len(attempt_budgets) == 1
            raise flash.SerialReadTimeout(
                "reply dropped", saw_activity=first, partial_frame=first
            )

        badge._wait_ping_once = ping_once  # type: ignore[method-assign]
        with mock.patch.object(
            flash.time, "monotonic", side_effect=lambda: clock.now
        ):
            with self.assertRaises(flash.SerialReadTimeout) as caught:
                badge.wait_ping(timeout_s=2)

        self.assertGreaterEqual(len(attempt_budgets), 2)
        self.assertLessEqual(sum(attempt_budgets), 2.0)
        self.assertTrue(caught.exception.saw_activity)
        self.assertTrue(caught.exception.partial_frame)

    def test_application_proof_never_replays_a_timed_out_status(self) -> None:
        clock = SimpleNamespace(now=0.0)
        badge = flash.BadgeSerial(_usb_record("/dev/fake"), False)
        status_calls: list[float] = []
        lines: list[str] = []

        def status(timeout_s: float) -> dict[str, object]:
            status_calls.append(timeout_s)
            clock.now += timeout_s
            raise flash.SerialReadTimeout(
                "status dropped", saw_activity=False, partial_frame=False,
            )

        badge.status = status  # type: ignore[method-assign]
        badge.write_line = lines.append  # type: ignore[method-assign]
        with mock.patch.object(
            flash.time, "monotonic", side_effect=lambda: clock.now
        ), \
             mock.patch.object(flash, "run") as run, \
             self.assertRaises(flash.SerialReadTimeout):
            flash._prove_badge_application(badge, timeout_s=4)

        self.assertEqual(len(status_calls), 1)
        self.assertEqual(lines, [])
        self.assertLessEqual(clock.now, 4.0)
        run.assert_not_called()

    def test_probe_application_requires_exact_pong_counter_and_identity(self) -> None:
        baseline = _uplink_status(self.VERSION, responses=40)
        final = _uplink_status(self.VERSION, responses=42)
        expected_version = self.VERSION
        events: list[str] = []

        class FakeBadge:
            def __init__(self, *_args, **_kwargs):
                self.statuses = [baseline, final]
            def __enter__(self): return self
            def __exit__(self, *_args): return None
            def status(self, *_args, **_kwargs):
                events.append("status")
                return self.statuses.pop(0)
            def write_line(self, line): events.append(line)
            def read_prefixed_text(self, prefix, _timeout):
                events.append(prefix)
                return expected_version

        with mock.patch.object(flash, "BadgeSerial", FakeBadge):
            got = flash.probe_application(
                _usb_record("/dev/renamed"), timeout_s=1
            )
        self.assertEqual(got, final)
        self.assertEqual(
            events, ["status", "FOF_PING", "FOF_PONG:", "status"]
        )

    def test_probe_returns_none_only_for_bounded_silence(self) -> None:
        class SilentBadge:
            def __init__(self, *_args, **_kwargs): pass
            def __enter__(self): return self
            def __exit__(self, *_args): return None
            def status(self, *_args, **_kwargs):
                raise flash.SerialReadTimeout(
                    "silent", saw_activity=False, partial_frame=False
                )

        with mock.patch.object(flash, "BadgeSerial", SilentBadge):
            self.assertIsNone(flash.probe_application(
                _usb_record("/dev/silent"), 1
            ))

        class MalformedBadge(SilentBadge):
            def status(self, *_args, **_kwargs):
                raise flash.SerialTransportError("malformed FOF_STATUS")

        with mock.patch.object(flash, "BadgeSerial", MalformedBadge):
            with self.assertRaisesRegex(flash.FlashError, "malformed"):
                flash.probe_application(_usb_record("/dev/bad"), 1)

    def test_probe_propagates_silent_pong_after_valid_baseline_activity(self) -> None:
        baseline = _uplink_status(self.VERSION, responses=40)
        attempt_errors: list[flash.SerialReadTimeout] = []

        class BaselineThenSilentPong:
            def __init__(self, *_args, **_kwargs): pass
            def __enter__(self): return self
            def __exit__(self, *_args): return None
            def status(self, *_args, **_kwargs): return baseline
            def write_line(self, _line): return None
            def read_prefixed_text(self, *_args, **_kwargs):
                error = flash.SerialReadTimeout(
                    "pong attempt silent", saw_activity=False,
                    partial_frame=False,
                )
                attempt_errors.append(error)
                raise error

        with mock.patch.object(flash, "BadgeSerial", BaselineThenSilentPong):
            with self.assertRaises(flash.SerialReadTimeout) as caught:
                flash.probe_application(_usb_record("/dev/uplink"), 1)

        self.assertTrue(caught.exception.saw_activity)
        self.assertFalse(caught.exception.partial_frame)
        self.assertEqual(str(caught.exception), "timed out waiting for FOF_PONG")
        self.assertIs(caught.exception.__cause__, attempt_errors[-1])
        self.assertEqual(str(caught.exception.__cause__), "pong attempt silent")
        self.assertEqual(len(attempt_errors), 1)

    def test_probe_propagates_silent_final_status_after_valid_pong_activity(self) -> None:
        baseline = _uplink_status(self.VERSION, responses=40)
        status_calls = 0
        attempt_errors: list[flash.SerialReadTimeout] = []

        class BaselinePongThenSilentStatus:
            def __init__(self, *_args, **_kwargs): pass
            def __enter__(self): return self
            def __exit__(self, *_args): return None
            def status(self, *_args, **_kwargs):
                nonlocal status_calls
                status_calls += 1
                if status_calls == 1:
                    return baseline
                error = flash.SerialReadTimeout(
                    "final status attempt silent", saw_activity=False,
                    partial_frame=False,
                )
                attempt_errors.append(error)
                raise error
            def write_line(self, _line): return None
            def read_prefixed_text(self, *_args, **_kwargs):
                return self_version

        self_version = self.VERSION
        with mock.patch.object(
            flash, "BadgeSerial", BaselinePongThenSilentStatus
        ):
            with self.assertRaises(flash.SerialReadTimeout) as caught:
                flash.probe_application(_usb_record("/dev/uplink"), 1)

        self.assertTrue(caught.exception.saw_activity)
        self.assertFalse(caught.exception.partial_frame)
        self.assertEqual(
            str(caught.exception), "timed out waiting for final FOF_STATUS"
        )
        self.assertIs(caught.exception.__cause__, attempt_errors[-1])
        self.assertEqual(
            str(caught.exception.__cause__), "final status attempt silent"
        )
        self.assertEqual(status_calls, 2)
        self.assertEqual(len(attempt_errors), 1)

    def test_probe_rejects_wrong_identity_pong_or_stale_counter(self) -> None:
        baseline = _uplink_status(self.VERSION, responses=10)
        cases = []
        wrong_identity = _uplink_status(self.VERSION, responses=12)
        wrong_identity["project"] = "fof_uplink"
        cases.append((wrong_identity, self.VERSION, "project"))
        cases.append((_uplink_status(self.VERSION, responses=12), "0.64.75", "PONG"))
        cases.append((_uplink_status(self.VERSION, responses=11), self.VERSION, "counter"))

        for final, pong, error in cases:
            class FakeBadge:
                def __init__(self, *_args, **_kwargs): self.statuses = [baseline, final]
                def __enter__(self): return self
                def __exit__(self, *_args): return None
                def status(self, *_args, **_kwargs): return self.statuses.pop(0)
                def write_line(self, _line): return None
                def read_prefixed_text(self, _prefix, _timeout): return pong
            with self.subTest(error=error), mock.patch.object(
                flash, "BadgeSerial", FakeBadge
            ):
                with self.assertRaisesRegex(flash.FlashError, error):
                    flash.probe_application(_usb_record("/dev/fake"), 1)

    def test_wait_for_application_port_follows_mac_across_renamed_path(self) -> None:
        wanted = _uplink_status(self.VERSION)
        other = _uplink_status(
            self.VERSION, hardware_id="14:C1:9F:52:CA:B0"
        )
        statuses = {"/dev/cu.old": other, "/dev/cu.new": wanted}
        descriptor_ids = {
            "/dev/cu.old": "14:c1:9f:52:ca:b0",
            "/dev/cu.new": "e0:72:a1:f9:47:fc",
        }
        with mock.patch.object(
            flash, "_take_badge_usb_descriptor_census",
            return_value=_usb_census(tuple(statuses), descriptor_ids),
        ), mock.patch.object(
            flash, "probe_application",
            side_effect=lambda descriptor, _timeout: statuses[
                descriptor.device
            ],
        ):
            descriptor, status = flash.wait_for_application_port(
                "e0:72:a1:f9:47:fc", timeout_s=0
            )
        self.assertEqual(descriptor.device, "/dev/cu.new")
        self.assertEqual(status, wanted)

    def test_discovery_skips_noisy_scanner_and_typed_port_errors(self) -> None:
        wanted = _uplink_status(self.VERSION)
        scanner = _uplink_status(
            self.VERSION, hardware_id="14:C1:9F:52:CA:B0"
        )
        scanner["project"] = "fof_badge_scanner"
        ports = [
            "/dev/cu.scanner", "/dev/cu.noisy", "/dev/cu.vanished",
            "/dev/cu.uplink",
        ]
        outcomes = {
            "/dev/cu.scanner": scanner,
            "/dev/cu.noisy": flash.SerialReadTimeout(
                "boot chatter", saw_activity=True, partial_frame=True
            ),
            "/dev/cu.vanished": flash.SerialTransportError(
                "open failed", terminal_unavailable=True
            ),
            "/dev/cu.uplink": wanted,
        }

        descriptor_ids = {
            port: (
                "14:c1:9f:52:ca:b0"
                if port == "/dev/cu.scanner"
                else "e0:72:a1:f9:47:fc"
            )
            for port in ports
        }

        def probe(descriptor: flash.UsbDescriptorRecord, _timeout_s: float):
            outcome = outcomes[descriptor.device]
            if isinstance(outcome, Exception):
                raise outcome
            return outcome

        output = io.StringIO()
        with mock.patch.object(
            flash, "_take_badge_usb_descriptor_census",
            return_value=_usb_census(ports, descriptor_ids),
        ), \
             mock.patch.object(flash, "probe_application", side_effect=probe), \
             contextlib.redirect_stdout(output):
            descriptor, status = flash.wait_for_application_port(
                "e0:72:a1:f9:47:fc", timeout_s=1
            )

        self.assertEqual(descriptor.device, "/dev/cu.uplink")
        self.assertEqual(status, wanted)
        diagnostics = output.getvalue()
        for skipped in ports[:-1]:
            self.assertIn(skipped, diagnostics)

    def test_application_discovery_descriptor_filters_before_any_port_probe(
        self,
    ) -> None:
        expected = "e0:72:a1:f9:47:fc"
        ports = [
            "/dev/cu.uplink",
            "/dev/cu.scanner-ble",
            "/dev/cu.scanner-wifi",
        ]
        descriptor_ids = {
            "/dev/cu.uplink": expected,
            "/dev/cu.scanner-ble": "e0:72:a1:f9:48:58",
            "/dev/cu.scanner-wifi": "e0:72:a1:f9:48:59",
        }
        probed: list[str] = []

        def probe(
            descriptor: flash.UsbDescriptorRecord,
            _timeout_s: float,
        ) -> dict:
            probed.append(descriptor.device)
            if descriptor.device != "/dev/cu.uplink":
                raise AssertionError("nonmatching scanner USB port was opened")
            return _uplink_status(self.VERSION, hardware_id=expected)

        with mock.patch.object(
            flash, "_take_badge_usb_descriptor_census",
            return_value=_usb_census(ports, descriptor_ids),
        ), mock.patch.object(
            flash, "probe_application", side_effect=probe
        ):
            descriptor, status = flash.wait_for_application_port(
                expected, timeout_s=1
            )

        self.assertEqual(descriptor.device, "/dev/cu.uplink")
        self.assertEqual(status["hardware_id"].lower(), expected)
        self.assertEqual(probed, ["/dev/cu.uplink"])

    def test_discovery_uses_one_global_timeout_budget_across_ports(self) -> None:
        clock = SimpleNamespace(now=10.0)
        budgets: list[float] = []
        ports = ["/dev/cu.a", "/dev/cu.b", "/dev/cu.c"]

        def silent_probe(
            _descriptor: flash.UsbDescriptorRecord,
            timeout_s: float,
        ):
            budgets.append(timeout_s)
            clock.now += timeout_s
            return None

        with mock.patch.object(
            flash.time, "monotonic", side_effect=lambda: clock.now
        ), \
             mock.patch.object(
                 flash, "_take_badge_usb_descriptor_census",
                 return_value=_usb_census(ports),
             ), \
             mock.patch.object(
                 flash, "probe_application", side_effect=silent_probe
             ):
            with self.assertRaisesRegex(flash.FlashError, "timed out"):
                flash.wait_for_application_port(
                    "e0:72:a1:f9:47:fc", timeout_s=0.3
                )

        self.assertGreaterEqual(len(budgets), 1)
        self.assertTrue(all(budget >= 0 for budget in budgets))
        self.assertLessEqual(sum(budgets), 0.3 + 1e-9)

    def test_discovery_reopens_a_stale_post_reboot_descriptor(self) -> None:
        clock = SimpleNamespace(now=0.0)
        wanted = _uplink_status(self.VERSION)
        budgets: list[float] = []

        def probe(
            _descriptor: flash.UsbDescriptorRecord,
            timeout_s: float,
        ):
            budgets.append(timeout_s)
            if len(budgets) == 1:
                # The pathname survives the USB reboot, but this first open is
                # still bound to the disappearing pre-reboot interface.
                clock.now += timeout_s
                return None
            clock.now += 0.1
            return wanted

        def sleep(seconds: float) -> None:
            clock.now += seconds

        timeout_s = 2 * flash.APPLICATION_DISCOVERY_PROBE_SLICE_S + 0.5
        with mock.patch.object(
            flash.time, "monotonic", side_effect=lambda: clock.now
        ), mock.patch.object(
            flash.time, "sleep", side_effect=sleep
        ), mock.patch.object(
            flash, "_take_badge_usb_descriptor_census",
            return_value=_usb_census(["/dev/cu.uplink"]),
        ), mock.patch.object(
            flash, "probe_application", side_effect=probe
        ):
            descriptor, status = flash.wait_for_application_port(
                "e0:72:a1:f9:47:fc", timeout_s=timeout_s
            )

        self.assertEqual(descriptor.device, "/dev/cu.uplink")
        self.assertEqual(status, wanted)
        self.assertGreaterEqual(len(budgets), 2)
        self.assertLessEqual(
            budgets[0], flash.APPLICATION_DISCOVERY_PROBE_SLICE_S
        )
        self.assertLessEqual(clock.now, timeout_s)

    def test_discovery_silent_scanner_cannot_starve_later_uplink(self) -> None:
        clock = SimpleNamespace(now=20.0)
        wanted = _uplink_status(self.VERSION)
        budgets: list[tuple[str, float]] = []

        def probe(
            descriptor: flash.UsbDescriptorRecord,
            timeout_s: float,
        ):
            budgets.append((descriptor.device, timeout_s))
            if descriptor.device == "/dev/cu.scanner":
                clock.now += timeout_s
                raise flash.SerialReadTimeout(
                    "silent scanner", saw_activity=False,
                    partial_frame=False,
                )
            if timeout_s <= 0:
                return None
            return wanted

        with mock.patch.object(
            flash.time, "monotonic", side_effect=lambda: clock.now
        ), \
             mock.patch.object(
                 flash, "_take_badge_usb_descriptor_census",
                 return_value=_usb_census(
                     ["/dev/cu.scanner", "/dev/cu.uplink"],
                     {
                         "/dev/cu.scanner": "14:c1:9f:52:ca:b0",
                         "/dev/cu.uplink": "e0:72:a1:f9:47:fc",
                     },
                 ),
             ), mock.patch.object(
                 flash, "probe_application", side_effect=probe
             ):
            descriptor, status = flash.wait_for_application_port(
                "e0:72:a1:f9:47:fc", timeout_s=1.0
            )

        self.assertEqual(descriptor.device, "/dev/cu.uplink")
        self.assertEqual(status, wanted)
        self.assertEqual([port for port, _budget in budgets], [
            "/dev/cu.uplink"
        ])
        self.assertGreater(budgets[0][1], 0)
        self.assertLessEqual(sum(budget for _port, budget in budgets), 1.0)

    def test_application_discovery_deadline_ignores_regressed_wall_clock(self) -> None:
        clock = SimpleNamespace(monotonic=0.0, wall_calls=0, rounds=0)

        def regressed_wall_time() -> float:
            clock.wall_calls += 1
            return 100.0 if clock.wall_calls == 1 else -100.0

        def census() -> tuple[flash.UsbDescriptorRecord, ...]:
            clock.rounds += 1
            if clock.rounds > 3:
                raise AssertionError("application discovery overran deadline")
            return ()

        def sleep(seconds: float) -> None:
            clock.monotonic += seconds

        with mock.patch.object(
            flash.time, "time", side_effect=regressed_wall_time
        ), mock.patch.object(
            flash.time, "monotonic", side_effect=lambda: clock.monotonic
        ), mock.patch.object(
            flash.time, "sleep", side_effect=sleep
        ), mock.patch.object(
            flash, "_take_badge_usb_descriptor_census", side_effect=census
        ), mock.patch.object(
            flash, "probe_application",
            side_effect=AssertionError("no ports should be probed"),
        ), self.assertRaisesRegex(flash.FlashError, "timed out"):
            flash.wait_for_application_port(
                "e0:72:a1:f9:47:fc", timeout_s=0.5
            )

        self.assertLessEqual(clock.monotonic, 0.5)
        self.assertLessEqual(clock.rounds, 3)

    def test_application_discovery_never_accepts_after_monotonic_deadline(self) -> None:
        clock = SimpleNamespace(monotonic=0.0)
        wanted = _uplink_status(self.VERSION)
        calls: list[tuple[str, float]] = []

        def probe(
            descriptor: flash.UsbDescriptorRecord,
            timeout_s: float,
        ):
            calls.append((descriptor.device, timeout_s))
            if descriptor.device == "/dev/cu.a-slow":
                clock.monotonic = 0.31
                return None
            return wanted

        with mock.patch.object(
            flash.time, "time", return_value=50.0
        ), mock.patch.object(
            flash.time, "monotonic", side_effect=lambda: clock.monotonic
        ), mock.patch.object(
            flash, "_take_badge_usb_descriptor_census",
            return_value=_usb_census(
                ["/dev/cu.a-slow", "/dev/cu.z-late"]
            ),
        ), mock.patch.object(
            flash, "probe_application", side_effect=probe
        ), self.assertRaisesRegex(flash.FlashError, "timed out"):
            flash.wait_for_application_port(
                "e0:72:a1:f9:47:fc", timeout_s=0.3
            )

        self.assertEqual(
            [port for port, _budget in calls], ["/dev/cu.a-slow"]
        )
        self.assertLessEqual(sum(budget for _port, budget in calls), 0.3)

    def test_nested_passive_serial_probe_shares_monotonic_budget(self) -> None:
        clock = SimpleNamespace(monotonic=0.0, wall_calls=0)

        class SilentSerial:
            def __init__(self) -> None:
                self.read_calls = 0

            def open(self) -> None:
                return None

            def reset_input_buffer(self) -> None:
                return None

            def read(self, _size: int) -> bytes:
                self.read_calls += 1
                return b""

            def write(self, payload: bytes) -> int:
                return len(payload)

            def flush(self) -> None:
                return None

            def close(self) -> None:
                return None

        def regressed_wall_time() -> float:
            clock.wall_calls += 1
            return 100.0 if clock.wall_calls == 1 else -100.0

        def advance_monotonic(seconds: float) -> None:
            clock.monotonic += seconds
            if clock.monotonic > 0.35:
                raise AssertionError("nested passive read overran deadline")

        with mock.patch.object(
            flash.time, "time", side_effect=regressed_wall_time
        ), mock.patch.object(
            flash.time, "monotonic", side_effect=lambda: clock.monotonic
        ), mock.patch.object(
            flash.time, "sleep", side_effect=advance_monotonic
        ), mock.patch.object(
            flash, "_take_badge_usb_descriptor_census",
            return_value=_usb_census(["/dev/cu.silent"]),
        ), mock.patch.object(
            flash, "open_bound_application_serial",
            return_value=SilentSerial(),
        ), self.assertRaisesRegex(flash.FlashError, "timed out"):
            flash.wait_for_application_port(
                "e0:72:a1:f9:47:fc", timeout_s=0.3
            )

        self.assertLessEqual(clock.monotonic, 0.3 + 1e-9)
        self.assertEqual(clock.wall_calls, 0)

    def test_application_discovery_rejects_unbounded_timeouts_before_io(self) -> None:
        for timeout_s in (float("nan"), float("inf"), 601, -1, True):
            with self.subTest(timeout_s=timeout_s), mock.patch.object(
                flash, "_take_badge_usb_descriptor_census",
                side_effect=AssertionError("invalid timeout reached USB"),
            ) as census, self.assertRaises(flash.FlashError):
                flash.wait_for_application_port(
                    "e0:72:a1:f9:47:fc", timeout_s=timeout_s
                )
            census.assert_not_called()

    def test_wait_for_application_port_rejects_duplicate_mac(self) -> None:
        wanted = _uplink_status(self.VERSION)
        with mock.patch.object(
            flash, "_take_badge_usb_descriptor_census",
            return_value=_usb_census(["/dev/cu.a", "/dev/cu.b"]),
        ), mock.patch.object(flash, "probe_application", return_value=wanted):
            with self.assertRaisesRegex(flash.FlashError, "duplicate"):
                flash.wait_for_application_port(
                    "e0:72:a1:f9:47:fc", timeout_s=0
                )

    def test_status_reduces_read_budget_after_command_write(self) -> None:
        clock = SimpleNamespace(now=0.0)
        badge = flash.BadgeSerial(_usb_record("/dev/fake"), False)
        read_budgets: list[float] = []

        def write_line(_line: str) -> None:
            clock.now += 0.25

        def read_status(_prefix: str, timeout_s: float) -> dict:
            read_budgets.append(timeout_s)
            return _uplink_status(self.VERSION)

        badge.write_line = write_line  # type: ignore[method-assign]
        badge.read_prefixed_json = read_status  # type: ignore[method-assign]
        with mock.patch.object(
            flash.time, "monotonic", side_effect=lambda: clock.now
        ):
            got = badge.status(timeout_s=1)

        self.assertEqual(got["version"], self.VERSION)
        self.assertEqual(read_budgets, [0.75])

    def test_status_rejects_reply_completed_after_global_deadline(self) -> None:
        clock = SimpleNamespace(now=0.0)
        badge = flash.BadgeSerial(_usb_record("/dev/fake"), False)

        def write_line(_line: str) -> None:
            clock.now += 0.25

        def late_status(_prefix: str, _timeout_s: float) -> dict:
            clock.now = 1.01
            return _uplink_status(self.VERSION)

        badge.write_line = write_line  # type: ignore[method-assign]
        badge.read_prefixed_json = late_status  # type: ignore[method-assign]
        with mock.patch.object(
            flash.time, "monotonic", side_effect=lambda: clock.now
        ), self.assertRaises(flash.SerialReadTimeout):
            badge.status(timeout_s=1)

    def test_status_binds_first_hardware_id_and_rejects_later_change(self) -> None:
        badge = flash.BadgeSerial(_usb_record("/dev/fake"), False)
        badge.write_line = lambda _line: None  # type: ignore[method-assign]
        statuses = [
            _uplink_status(self.VERSION),
            _uplink_status(
                self.VERSION, hardware_id="14:C1:9F:52:CA:B0"
            ),
        ]
        badge.read_prefixed_json = lambda *_args, **_kwargs: statuses.pop(0)  # type: ignore[method-assign]
        badge.status()
        self.assertEqual(
            badge.expected_hardware_id, "e0:72:a1:f9:47:fc"
        )
        with self.assertRaisesRegex(flash.FlashError, "changed"):
            badge.status()

    def test_upload_manifest_credit_pacing_short_final_and_exact_commit(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            platform, data = self._platform_with_image(temp_dir)
            badge = flash.BadgeSerial(_usb_record("/dev/fake"), False)
            badge.ser = _ScriptedRawSerial()
            badge._prove_open_application = lambda _timeout: _uplink_status(  # type: ignore[attr-defined]
                self.OLD_VERSION, responses=20
            )
            lines: list[str] = []
            events: list[str] = []
            badge.write_line = lines.append  # type: ignore[method-assign]
            replies = [
                _uplink_receipt("ready", total=len(data), credit=4096),
                _uplink_receipt(
                    "credit", total=len(data), received=4096,
                    credit=len(data) - 4096,
                ),
                _uplink_receipt(
                    "committed", total=len(data), received=len(data),
                    reboot_required=True,
                ),
            ]
            original_write = badge.ser.write
            def record_write(payload: bytes) -> int:
                events.append(f"write:{len(payload)}")
                return original_write(payload)
            badge.ser.write = record_write
            def read_receipt(*_args, **_kwargs):
                receipt = replies.pop(0)
                events.append(f"receipt:{receipt['phase']}")
                return receipt
            badge.read_prefixed_json = read_receipt  # type: ignore[method-assign]

            result = badge.upload_uplink_firmware(
                platform, _frozen_firmware_set(data), self.VERSION
            )

            manifest = json.loads(lines[0].removeprefix("FOF_CTL:"))
            self.assertEqual(list(manifest), [
                "cmd", "target", "project", "hardware_type", "version",
                "size", "crc32", "sha256", "flow_control",
                "recovery_rewrite_same_version",
            ])
            self.assertEqual(manifest["cmd"], "uplink_ota_begin")
            self.assertEqual(manifest["flow_control"], "credit-v1")
            self.assertEqual(manifest["size"], len(data))
            self.assertEqual(manifest["crc32"], binascii.crc32(data) & 0xFFFFFFFF)
            self.assertEqual(manifest["sha256"], hashlib.sha256(data).hexdigest())
            raw_writes = badge.ser.writes
            self.assertTrue(all(0 < len(chunk) <= 1024 for chunk in raw_writes))
            self.assertEqual(b"".join(raw_writes), data)
            self.assertEqual(sum(map(len, raw_writes[:4])), 4096)
            self.assertEqual(sum(map(len, raw_writes[4:])), len(data) - 4096)
            self.assertEqual(result["phase"], "committed")
            self.assertEqual(events[0], "receipt:ready")
            credit_at = events.index("receipt:credit")
            self.assertEqual(
                sum(int(item.split(":")[1]) for item in events[1:credit_at]),
                4096,
            )
            self.assertEqual(events[-1], "receipt:committed")

    def test_maintenance_upload_waits_for_supervisor_and_uart_workers(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            platform, data = self._platform_with_image(
                temp_dir, size=flash.UPLINK_OTA_CREDIT_BYTES
            )
            session = "0123456789ABCDEF"
            unready = _update_maintenance_status(
                self.OLD_VERSION,
                session=session,
                responses=20,
            )
            unready["stack_main_free"] = 0
            unready["stack_uart_ble_free"] = 0
            unready["stack_uart_wifi_free"] = 0
            ready = copy.deepcopy(unready)
            ready["usb_health"]["responses_completed"] = 21
            ready["stack_main_free"] = 4096
            ready["stack_uart_ble_free"] = 4096
            ready["stack_uart_wifi_free"] = 4096

            badge = flash.BadgeSerial(
                _usb_record("/dev/fake"),
                False,
                expected_hardware_id="e0:72:a1:f9:47:fc",
            )
            badge._update_session = session
            badge.ser = _ScriptedRawSerial()
            statuses = [unready, ready]
            events: list[str] = []

            def status(*, timeout_s: float) -> dict:
                self.assertGreater(timeout_s, 0)
                events.append("status")
                return statuses.pop(0)

            badge.status = status  # type: ignore[method-assign]
            badge.write_line = lambda _line: events.append("manifest")  # type: ignore[method-assign]
            replies = [
                _uplink_receipt(
                    "ready",
                    total=len(data),
                    credit=min(flash.UPLINK_OTA_CREDIT_BYTES, len(data)),
                ),
                _uplink_receipt(
                    "committed",
                    total=len(data),
                    received=len(data),
                    reboot_required=True,
                ),
            ]
            badge.read_prefixed_json = lambda *_args, **_kwargs: replies.pop(0)  # type: ignore[method-assign]

            with mock.patch.object(flash.time, "sleep"):
                result = badge.upload_uplink_firmware(
                    platform,
                    _frozen_firmware_set(data),
                    self.VERSION,
                )

            self.assertEqual(result["phase"], "committed")
            self.assertEqual(events, ["status", "status", "manifest"])
            self.assertEqual(statuses, [])
            self.assertEqual(b"".join(badge.ser.writes), data)

    def test_upload_streams_frozen_member_after_build_path_disappears(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            platform, original = self._platform_with_image(
                temp_dir, size=4096
            )
            frozen = _frozen_firmware_set(original)
            platform["uplink_bin"].unlink()
            badge = flash.BadgeSerial(_usb_record("/dev/fake"), False)
            badge.ser = _ScriptedRawSerial()
            badge._prove_open_application = lambda _timeout: _uplink_status(  # type: ignore[attr-defined]
                self.OLD_VERSION
            )
            manifest_lines: list[str] = []
            badge.write_line = manifest_lines.append  # type: ignore[method-assign]
            replies = [
                _uplink_receipt(
                    "ready", total=len(original), credit=len(original)
                ),
                _uplink_receipt(
                    "committed", total=len(original), received=len(original),
                    reboot_required=True,
                ),
            ]
            badge.read_prefixed_json = lambda *_args, **_kwargs: replies.pop(0)  # type: ignore[method-assign]

            with mock.patch.object(
                Path,
                "read_bytes",
                side_effect=AssertionError(
                    "routine USB upload reopened a build pathname"
                ),
            ) as read_bytes:
                result = badge.upload_uplink_firmware(
                    platform, frozen, self.VERSION
                )

            self.assertEqual(result["phase"], "committed")
            read_bytes.assert_not_called()
            self.assertEqual(b"".join(badge.ser.writes), original)
            manifest = json.loads(
                manifest_lines[0].removeprefix("FOF_CTL:")
            )
            self.assertEqual(
                manifest["sha256"], hashlib.sha256(original).hexdigest()
            )
            self.assertEqual(
                manifest["crc32"], binascii.crc32(original) & 0xFFFFFFFF
            )

    def test_upload_revalidates_frozen_set_identity_before_usb_control(
        self,
    ) -> None:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        data = _firmware_image(
            platform["uplink_project"],
            self.VERSION,
            platform["uplink_name"],
            platform["hardware_type"],
        )
        frozen = _frozen_firmware_set(data)
        object.__setattr__(frozen, "aggregate_sha256", "f" * 64)
        badge = flash.BadgeSerial(_usb_record("/dev/fake"), False)
        badge._prove_open_application = mock.Mock(  # type: ignore[method-assign]
            side_effect=AssertionError(
                "invalid frozen set reached USB application control"
            )
        )

        with self.assertRaises(flash.FlashError):
            badge.upload_uplink_firmware(
                platform,
                frozen,
                self.VERSION,
            )
        badge._prove_open_application.assert_not_called()

    def test_upload_writes_zero_bytes_before_fully_valid_ready(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            platform, data = self._platform_with_image(temp_dir)
            badge = flash.BadgeSerial(_usb_record("/dev/fake"), False)
            badge.ser = _ScriptedRawSerial()
            badge._prove_open_application = lambda _timeout: _uplink_status(  # type: ignore[attr-defined]
                self.OLD_VERSION
            )
            badge.write_line = lambda _line: None  # type: ignore[method-assign]
            invalid = _uplink_receipt("ready", total=len(data), credit=4096)
            invalid["partition"] = "ota_0"
            badge.read_prefixed_json = lambda *_args, **_kwargs: invalid  # type: ignore[method-assign]
            with self.assertRaisesRegex(flash.FlashError, "partition"):
                badge.upload_uplink_firmware(
                    platform, _frozen_firmware_set(data), self.VERSION
                )
            self.assertEqual(badge.ser.writes, [])

            invalid = _uplink_receipt(
                "ready", total=len(data), credit=4096
            )
            invalid["unexpected"] = 1
            badge.read_prefixed_json = lambda *_args, **_kwargs: invalid  # type: ignore[method-assign]
            with self.assertRaisesRegex(flash.FlashError, "schema"):
                badge.upload_uplink_firmware(
                    platform, _frozen_firmware_set(data), self.VERSION
                )
            self.assertEqual(badge.ser.writes, [])

    def test_upload_rejects_stale_credit_before_next_window(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            platform, data = self._platform_with_image(temp_dir)
            badge = flash.BadgeSerial(_usb_record("/dev/fake"), False)
            badge.ser = _ScriptedRawSerial()
            badge._prove_open_application = lambda _timeout: _uplink_status(  # type: ignore[attr-defined]
                self.OLD_VERSION
            )
            badge.write_line = lambda _line: None  # type: ignore[method-assign]
            replies = [
                _uplink_receipt("ready", total=len(data), credit=4096),
                _uplink_receipt(
                    "credit", total=len(data), received=4095,
                    credit=len(data) - 4096,
                ),
            ]
            badge.read_prefixed_json = lambda *_args, **_kwargs: replies.pop(0)  # type: ignore[method-assign]
            with self.assertRaisesRegex(flash.FlashError, "received"):
                badge.upload_uplink_firmware(
                    platform, _frozen_firmware_set(data), self.VERSION
                )
            self.assertEqual(sum(map(len, badge.ser.writes)), 4096)

    def test_upload_returns_uncertain_only_after_all_payload_bytes(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            platform, data = self._platform_with_image(temp_dir, size=4096)
            badge = flash.BadgeSerial(_usb_record("/dev/fake"), False)
            badge.ser = _ScriptedRawSerial()
            badge._prove_open_application = lambda _timeout: _uplink_status(  # type: ignore[attr-defined]
                self.OLD_VERSION
            )
            badge.write_line = lambda _line: None  # type: ignore[method-assign]
            replies = [
                _uplink_receipt("ready", total=len(data), credit=len(data)),
                flash.SerialReadTimeout(
                    "terminal missing", saw_activity=True, partial_frame=True
                ),
            ]
            def read(*_args, **_kwargs):
                value = replies.pop(0)
                if isinstance(value, Exception):
                    raise value
                return value
            badge.read_prefixed_json = read  # type: ignore[method-assign]
            result = badge.upload_uplink_firmware(
                platform, _frozen_firmware_set(data), self.VERSION
            )
            self.assertTrue(result["uncertain"])
            self.assertEqual(result["expected_partition"], "ota_1")
            self.assertEqual(result["hardware_id"], "e0:72:a1:f9:47:fc")
            self.assertEqual(result["version"], self.VERSION)
            self.assertEqual(sum(map(len, badge.ser.writes)), len(data))

    def test_upload_treats_explicit_abort_as_definitive_after_payload(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            platform, data = self._platform_with_image(temp_dir, size=4096)
            badge = flash.BadgeSerial(_usb_record("/dev/fake"), False)
            badge.ser = _ScriptedRawSerial()
            badge._prove_open_application = lambda _timeout: _uplink_status(  # type: ignore[attr-defined]
                self.OLD_VERSION
            )
            badge.write_line = lambda _line: None  # type: ignore[method-assign]
            replies = [
                _uplink_receipt("ready", total=len(data), credit=len(data)),
                _uplink_receipt(
                    "aborted", total=len(data), received=len(data),
                    ok=False, error="sha256_mismatch",
                ),
            ]
            badge.read_prefixed_json = lambda *_args, **_kwargs: replies.pop(0)  # type: ignore[method-assign]
            with self.assertRaisesRegex(flash.FlashError, "definitively"):
                badge.upload_uplink_firmware(
                    platform, _frozen_firmware_set(data), self.VERSION
                )

    def test_upload_version_policy_and_explicit_equal_recovery(self) -> None:
        cases = [
            ("0.64.77-badge-defcon34", "downgrade"),
            ("0.64.76-field", "unordered"),
            ("garbage", "invalid"),
        ]
        for running, error in cases:
            with tempfile.TemporaryDirectory() as temp_dir:
                platform, data = self._platform_with_image(temp_dir)
                badge = flash.BadgeSerial(_usb_record("/dev/fake"), False)
                badge.ser = _ScriptedRawSerial()
                badge._prove_open_application = lambda _timeout, running=running: _uplink_status(  # type: ignore[attr-defined]
                    running
                )
                badge.write_line = mock.Mock()  # type: ignore[method-assign]
                with self.subTest(running=running), self.assertRaisesRegex(
                    flash.FlashError, error
                ):
                    badge.upload_uplink_firmware(
                        platform, _frozen_firmware_set(data), self.VERSION
                    )
                badge.write_line.assert_not_called()

        with tempfile.TemporaryDirectory() as temp_dir:
            platform, data = self._platform_with_image(temp_dir)
            badge = flash.BadgeSerial(_usb_record("/dev/fake"), False)
            badge.ser = _ScriptedRawSerial()
            badge._prove_open_application = lambda _timeout: _uplink_status(  # type: ignore[attr-defined]
                self.VERSION
            )
            badge.write_line = mock.Mock()  # type: ignore[method-assign]
            frozen = _frozen_firmware_set(data)
            skipped = badge.upload_uplink_firmware(
                platform, frozen, self.VERSION
            )
            self.assertTrue(skipped["skipped"])
            badge.write_line.assert_not_called()

            replies = [
                _uplink_receipt("ready", total=len(data), credit=4096),
                _uplink_receipt(
                    "credit", total=len(data), received=4096,
                    credit=len(data) - 4096,
                ),
                _uplink_receipt(
                    "committed", total=len(data), received=len(data),
                    reboot_required=True,
                ),
            ]
            badge.read_prefixed_json = lambda *_args, **_kwargs: replies.pop(0)  # type: ignore[method-assign]
            committed = badge.upload_uplink_firmware(
                platform, frozen, self.VERSION,
                recovery_rewrite_same_version=True
            )
            self.assertEqual(committed["phase"], "committed")
            manifest = json.loads(
                badge.write_line.call_args.args[0].removeprefix("FOF_CTL:")
            )
            self.assertIs(manifest["recovery_rewrite_same_version"], True)

    def test_pending_verify_must_clear_before_begin(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            platform, data = self._platform_with_image(temp_dir)
            pending = _uplink_status(
                self.OLD_VERSION, pending_verify=True
            )
            badge = flash.BadgeSerial(_usb_record("/dev/fake"), False)
            badge.ser = _ScriptedRawSerial()
            badge._prove_open_application = lambda _timeout: pending  # type: ignore[attr-defined]
            badge.status = lambda *_args, **_kwargs: pending  # type: ignore[method-assign]
            badge.write_line = mock.Mock()  # type: ignore[method-assign]
            with mock.patch.object(flash.time, "sleep"):
                with self.assertRaisesRegex(flash.FlashError, "pending_verify"):
                    badge.upload_uplink_firmware(
                        platform, _frozen_firmware_set(data), self.VERSION
                    )
            badge.write_line.assert_not_called()

    def test_pending_verify_can_clear_on_fresh_status_before_begin(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            platform, data = self._platform_with_image(temp_dir, size=4096)
            pending = _uplink_status(
                self.OLD_VERSION, pending_verify=True
            )
            clear = _uplink_status(self.OLD_VERSION, responses=30)
            badge = flash.BadgeSerial(_usb_record("/dev/fake"), False)
            badge.ser = _ScriptedRawSerial()
            badge._prove_open_application = lambda _timeout: pending  # type: ignore[attr-defined]
            badge.status = mock.Mock(return_value=clear)  # type: ignore[method-assign]
            badge.write_line = mock.Mock()  # type: ignore[method-assign]
            replies = [
                _uplink_receipt("ready", total=len(data), credit=len(data)),
                _uplink_receipt(
                    "committed", total=len(data), received=len(data),
                    reboot_required=True,
                ),
            ]
            badge.read_prefixed_json = lambda *_args, **_kwargs: replies.pop(0)  # type: ignore[method-assign]
            with mock.patch.object(flash.time, "sleep"):
                result = badge.upload_uplink_firmware(
                    platform, _frozen_firmware_set(data), self.VERSION
                )
            self.assertEqual(result["phase"], "committed")
            badge.status.assert_called_once()


class GuardedEsptoolRunnerTests(unittest.TestCase):
    MAC = "e0:72:a1:f9:47:fc"

    @staticmethod
    def _regions() -> tuple[flash.RomFlashRegion, ...]:
        regions = []
        for index, (offset, size) in enumerate((
            (0x0, 5), (0x8000, 9), (0xF000, 13), (0x20000, 17),
        )):
            data = bytes([0x41 + index]) * size
            regions.append(flash.RomFlashRegion(
                offset=offset,
                path=Path(f"/must-not-be-read/original-{index}.bin"),
                data=data,
                size=size,
                sha256=hashlib.sha256(data).hexdigest(),
            ))
        return tuple(regions)

    @staticmethod
    def _snapshot_paths(root: Path) -> tuple[Path, ...]:
        return tuple(
            (root / name).resolve() for name in (
                "00 bootloader.bin", "01 partitions.bin",
                "02 ota_data.bin", "03 firmware.bin",
            )
        )

    @classmethod
    def _identity_transcript(cls) -> str:
        return (
            "\x1b[32mesptool.py v4.11.0\x1b[0m\r"
            "Serial port /dev/cu.usbmodem-test\r\n"
            "Connecting...\r"
            "Chip is ESP32-S3 (QFN56) (revision v0.2)\r\n"
            "Features: WiFi, BLE, Embedded PSRAM 8MB (AP_3v3)\n"
            "Crystal is 40MHz\n"
            f"MAC: {cls.MAC.upper()}\n"
            "Manufacturer: c8\nDevice: 4017\n"
            "Detected flash size: 8MB\n"
        )

    @staticmethod
    def _unavailable_transcript(port: str = "/dev/test") -> str:
        return (
            "esptool.py v4.11.0\n"
            f"Serial port {port}\n"
            "WARNING: Pre-connection option \"no_reset\" was selected. "
            "Connection may fail if the chip is not in bootloader or "
            "flasher stub mode.\n"
            "Connecting...\n"
            f"{flash.ESPTOOL_PROBE_UNAVAILABLE_MARKER}\n"
        )

    @classmethod
    def _write_transcript(
        cls, regions: tuple[flash.RomFlashRegion, ...],
        *, mac: str | None = None,
    ) -> str:
        lines = [
            "esptool.py v4.11.0",
            f"MAC: {mac or cls.MAC}",
            "Writing at 0x00000000... (100 %)",
        ]
        for region in regions:
            padded = (region.size + 3) & ~3
            lines.extend((
                f"Wrote {padded} bytes ({max(1, padded - 1)} compressed) "
                f"at 0x{region.offset:08x} in 0.1 seconds "
                "(effective 640.0 kbit/s)...",
                "Hash of data verified.",
            ))
        return "\r\n".join(lines) + "\r\n"

    @classmethod
    def _verify_transcript(
        cls, regions: tuple[flash.RomFlashRegion, ...],
        paths: tuple[Path, ...], *, mac: str | None = None,
    ) -> str:
        lines = ["esptool.py v4.11.0", f"MAC: {mac or cls.MAC}"]
        for region, path in zip(regions, paths):
            padded = (region.size + 3) & ~3
            lines.extend((
                f"Verifying 0x{padded:x} ({padded}) bytes @ "
                f"0x{region.offset:08x} in flash against "
                f"{path.resolve()}...",
                "-- verify OK (digest matched)",
            ))
        return "\n".join(lines) + "\n"

    def test_guard_and_exact_operation_argv(self) -> None:
        regions = self._regions()
        self.assertEqual(
            flash.ESPTOOL_GUARD,
            "import esptool,sys;"
            "getattr(esptool,'__version__',None)=='4.11.0' or "
            "sys.exit('unsupported esptool version');"
            "import esptool.loader;"
            "from esptool.loader import ESPLoader;"
            "from esptool.targets.esp32s3 import "
            "ESP32S3ROM,ESP32S3StubLoader;"
            "esptool.loader.WRITE_BLOCK_ATTEMPTS=1;"
            "ESPLoader.WRITE_FLASH_ATTEMPTS="
            "ESP32S3ROM.WRITE_FLASH_ATTEMPTS="
            "ESP32S3StubLoader.WRITE_FLASH_ATTEMPTS=1;"
            "esptool._main()",
        )
        with tempfile.TemporaryDirectory() as temp_dir, mock.patch.object(
            flash, "find_platformio_python", return_value="/opt/pio-python"
        ):
            paths = self._snapshot_paths(Path(temp_dir))
            guarded_prefix = [
                "/opt/pio-python", "-I", "-c", flash.ESPTOOL_GUARD
            ]
            probe_prefix = [
                "/opt/pio-python", "-I", "-c", flash.ESPTOOL_PROBE_GUARD
            ]
            probe_common = [
                "--chip", "esp32s3", "--port", "/dev/test", "--baud",
                "115200", "--before", "no_reset", "--after", "no_reset",
                "--no-stub", "--connect-attempts", "1",
            ]
            flash_common = [
                "--chip", "esp32s3", "--port", "/dev/test", "--baud",
                "460800", "--before", "no_reset", "--after", "no_reset",
                "--connect-attempts", "1",
            ]
            app_handoff_common = [
                "--chip", "esp32s3", "--port", "/dev/test", "--baud",
                "115200", "--before", "no_reset", "--after",
                "watchdog_reset", "--no-stub", "--connect-attempts", "1",
            ]
            pairs = []
            for region, path in zip(regions, paths):
                pairs.extend((f"0x{region.offset:x}", str(path.resolve())))

            commands = (
                flash.build_esptool_probe_argv("/dev/test"),
                flash.build_esptool_probe_argv(
                    "/dev/test", native_usb_reset=True
                ),
                flash.build_esptool_write_argv(
                    "/dev/test", regions, paths
                ),
                flash.build_esptool_verify_argv(
                    "/dev/test", regions, paths
                ),
                flash.build_esptool_run_argv("/dev/test"),
            )
            self.assertEqual(
                commands[0], probe_prefix + probe_common + ["flash_id"]
            )
            usb_reset_probe_common = list(probe_common)
            usb_reset_probe_common[
                usb_reset_probe_common.index("no_reset")
            ] = "usb_reset"
            self.assertEqual(
                commands[1],
                probe_prefix + usb_reset_probe_common + ["flash_id"],
            )
            self.assertEqual(commands[2], guarded_prefix + flash_common + [
                "write_flash", "--compress", "--verify",
                "--flash_mode", "dio", "--flash_freq", "80m",
                "--flash_size", "8MB", *pairs,
            ])
            self.assertEqual(commands[3], guarded_prefix + flash_common + [
                "verify_flash", "--flash_mode", "dio",
                "--flash_freq", "80m", "--flash_size", "8MB", *pairs,
            ])
            self.assertEqual(
                commands[4], guarded_prefix + app_handoff_common + [
                    "write_mem", "0x6000812c", "0x0", "0x1",
                ]
            )

            forbidden = (
                "hard_reset", "hard-reset", "usb-reset",
                "erase-all", "erase_all", "--force",
                "0x10000", "upload",
            )
            for command in commands:
                rendered = " ".join(command).lower()
                for token in forbidden:
                    self.assertNotIn(token, rendered)
            self.assertEqual(
                [
                    command[command.index("--before") + 1]
                    for command in commands
                ],
                [
                    "no_reset", "usb_reset", "no_reset", "no_reset",
                    "no_reset",
                ],
            )
            self.assertNotIn("-m", commands[0])
            self.assertNotIn(
                "PlatformIO", " ".join(
                    token for command in commands for token in command
                )
            )

    @staticmethod
    def _fake_esptool_modules(version: object = "4.11.0") -> tuple[
        dict[str, ModuleType], mock.Mock, type, type, type,
    ]:
        esptool = ModuleType("esptool")
        esptool.__path__ = []  # type: ignore[attr-defined]
        if version is not None:
            esptool.__version__ = version  # type: ignore[attr-defined]
        main = mock.Mock()
        esptool._main = main  # type: ignore[attr-defined]
        loader = ModuleType("esptool.loader")
        loader.WRITE_BLOCK_ATTEMPTS = 3  # type: ignore[attr-defined]

        class ESPLoader:
            WRITE_FLASH_ATTEMPTS = 2

        loader.ESPLoader = ESPLoader  # type: ignore[attr-defined]
        targets = ModuleType("esptool.targets")
        targets.__path__ = []  # type: ignore[attr-defined]
        s3 = ModuleType("esptool.targets.esp32s3")

        class ESP32S3ROM:
            WRITE_FLASH_ATTEMPTS = 2

        class ESP32S3StubLoader:
            WRITE_FLASH_ATTEMPTS = 2

        s3.ESP32S3ROM = ESP32S3ROM  # type: ignore[attr-defined]
        s3.ESP32S3StubLoader = ESP32S3StubLoader  # type: ignore[attr-defined]
        esptool.loader = loader  # type: ignore[attr-defined]
        esptool.targets = targets  # type: ignore[attr-defined]
        targets.esp32s3 = s3  # type: ignore[attr-defined]
        return ({
            "esptool": esptool,
            "esptool.loader": loader,
            "esptool.targets": targets,
            "esptool.targets.esp32s3": s3,
        }, main, ESPLoader, ESP32S3ROM, ESP32S3StubLoader)

    def test_child_guard_checks_version_before_mutation_or_main(self) -> None:
        for version in ("4.10.0", None):
            modules, main, loader, rom, stub = self._fake_esptool_modules(
                version
            )
            with self.subTest(version=version):
                with mock.patch.dict(
                    sys.modules, modules
                ), self.assertRaises(SystemExit):
                    exec(flash.ESPTOOL_GUARD, {})
                main.assert_not_called()
                self.assertEqual(
                    modules["esptool.loader"].WRITE_BLOCK_ATTEMPTS, 3  # type: ignore[attr-defined]
                )
                self.assertEqual(loader.WRITE_FLASH_ATTEMPTS, 2)
                self.assertEqual(rom.WRITE_FLASH_ATTEMPTS, 2)
                self.assertEqual(stub.WRITE_FLASH_ATTEMPTS, 2)

    def test_child_guard_pins_all_retries_then_calls_main_once(self) -> None:
        modules, main, loader, rom, stub = self._fake_esptool_modules()
        with mock.patch.dict(sys.modules, modules):
            exec(flash.ESPTOOL_GUARD, {})
        self.assertEqual(
            modules["esptool.loader"].WRITE_BLOCK_ATTEMPTS, 1  # type: ignore[attr-defined]
        )
        self.assertEqual(loader.WRITE_FLASH_ATTEMPTS, 1)
        self.assertEqual(rom.WRITE_FLASH_ATTEMPTS, 1)
        self.assertEqual(stub.WRITE_FLASH_ATTEMPTS, 1)
        main.assert_called_once_with()

    @staticmethod
    def _fake_probe_guard_modules(
        *, error: str = "No serial data received.",
        config_path: str | None = None,
        mode: str = "no_reset",
    ) -> tuple[dict[str, ModuleType], mock.Mock, object]:
        class FatalError(RuntimeError):
            pass

        class FakePort:
            def __init__(self) -> None:
                self.flush_output_calls = 0

            def flushOutput(self) -> None:
                self.flush_output_calls += 1

        class ESPLoader:
            def __init__(self) -> None:
                self._port = FakePort()
                self.flush_input_calls = 0
                self.sync_calls = 0
                self.reset_strategy = mock.Mock()

            def flush_input(self) -> None:
                self.flush_input_calls += 1

            def sync(self) -> None:
                self.sync_calls += 1
                raise FatalError(error)

            def _connect_attempt(self, _strategy: object,
                                 _mode: str = "default_reset") -> object:
                last_error: object = None
                for _ in range(5):
                    try:
                        self.flush_input()
                        self._port.flushOutput()
                        self.sync()
                    except FatalError as exc:
                        last_error = exc
                return last_error

        instance = ESPLoader()
        main = mock.Mock(side_effect=lambda: instance._connect_attempt(
            instance.reset_strategy, mode
        ))
        esptool = ModuleType("esptool")
        esptool.__path__ = []  # type: ignore[attr-defined]
        esptool.__version__ = "4.11.0"  # type: ignore[attr-defined]
        esptool._main = main  # type: ignore[attr-defined]
        config = ModuleType("esptool.config")
        config.load_config_file = mock.Mock(  # type: ignore[attr-defined]
            return_value=(object(), config_path)
        )
        loader = ModuleType("esptool.loader")
        loader.ESPLoader = ESPLoader  # type: ignore[attr-defined]
        util = ModuleType("esptool.util")
        util.FatalError = FatalError  # type: ignore[attr-defined]
        esptool.config = config  # type: ignore[attr-defined]
        esptool.loader = loader  # type: ignore[attr-defined]
        esptool.util = util  # type: ignore[attr-defined]
        return ({
            "esptool": esptool,
            "esptool.config": config,
            "esptool.loader": loader,
            "esptool.util": util,
        }, main, instance)

    def test_probe_child_guard_replaces_five_inner_syncs_with_one(self) -> None:
        modules, main, instance = self._fake_probe_guard_modules(
            error="partial or noisy response"
        )
        with mock.patch.dict(sys.modules, modules):
            exec(flash.ESPTOOL_PROBE_GUARD, {})
        main.assert_called_once_with()
        self.assertEqual(instance.flush_input_calls, 1)  # type: ignore[attr-defined]
        self.assertEqual(instance._port.flush_output_calls, 1)  # type: ignore[attr-defined]
        self.assertEqual(instance.sync_calls, 1)  # type: ignore[attr-defined]

    def test_probe_child_guard_performs_one_native_usb_reset_before_sync(
        self,
    ) -> None:
        modules, main, instance = self._fake_probe_guard_modules(
            error="partial or noisy response", mode="usb_reset"
        )
        with mock.patch.dict(sys.modules, modules):
            exec(flash.ESPTOOL_PROBE_GUARD, {})
        main.assert_called_once_with()
        instance.reset_strategy.assert_called_once_with()  # type: ignore[attr-defined]
        self.assertEqual(instance.flush_input_calls, 1)  # type: ignore[attr-defined]
        self.assertEqual(instance._port.flush_output_calls, 1)  # type: ignore[attr-defined]
        self.assertEqual(instance.sync_calls, 1)  # type: ignore[attr-defined]

    def test_probe_child_guard_emits_private_marker_only_for_proven_app_absence(
        self,
    ) -> None:
        for error in (
            "No serial data received.",
            "Invalid head of packet (0x46): Possible serial noise or corruption.",
            "Invalid head of packet (0x49): Possible serial noise or corruption.",
            "Invalid head of packet (0x65): Possible serial noise or corruption.",
        ):
            with self.subTest(error=error):
                modules, main, instance = self._fake_probe_guard_modules(
                    error=error
                )
                output = io.StringIO()
                with mock.patch.dict(sys.modules, modules), \
                     contextlib.redirect_stdout(output), \
                     self.assertRaises(SystemExit) as raised:
                    exec(flash.ESPTOOL_PROBE_GUARD, {})
                self.assertEqual(
                    raised.exception.code,
                    flash.ESPTOOL_PROBE_UNAVAILABLE_EXIT,
                )
                self.assertEqual(
                    [
                        line for line in output.getvalue().splitlines()
                        if line
                    ],
                    [flash.ESPTOOL_PROBE_UNAVAILABLE_MARKER],
                )
                main.assert_called_once_with()
                self.assertEqual(
                    instance.sync_calls, 1  # type: ignore[attr-defined]
                )

        for error in (
            "No serial data received. ",
            "No serial data received",
            "Invalid head of packet (0x47): Possible serial noise or corruption.",
            "Invalid head of packet (0x4): Possible serial noise or corruption.",
            "Invalid head of packet (0xGG): Possible serial noise or corruption.",
            "Invalid head of packet (0x46): Possible serial noise or corruption. ",
            "Invalid head of packet (0x46): serial noise or corruption.",
            "The chip stopped responding.",
            "Wrong boot mode detected (0x13)!",
        ):
            with self.subTest(error=error):
                modules, _main, instance = self._fake_probe_guard_modules(
                    error=error
                )
                with mock.patch.dict(sys.modules, modules):
                    exec(flash.ESPTOOL_PROBE_GUARD, {})
                self.assertEqual(instance.sync_calls, 1)  # type: ignore[attr-defined]

    def test_probe_child_guard_rejects_config_and_wrong_mode_before_sync(self) -> None:
        modules, main, instance = self._fake_probe_guard_modules(
            config_path="/tmp/esptool.cfg"
        )
        with mock.patch.dict(sys.modules, modules), self.assertRaises(
            SystemExit
        ):
            exec(flash.ESPTOOL_PROBE_GUARD, {})
        main.assert_not_called()
        self.assertEqual(instance.sync_calls, 0)  # type: ignore[attr-defined]

        modules, _main, instance = self._fake_probe_guard_modules(
            mode="default_reset"
        )
        with mock.patch.dict(sys.modules, modules), self.assertRaises(
            RuntimeError
        ):
            exec(flash.ESPTOOL_PROBE_GUARD, {})
        self.assertEqual(instance.sync_calls, 0)  # type: ignore[attr-defined]

    def test_probe_child_guard_never_types_pre_sync_failures(self) -> None:
        for stage in ("flush_input", "flush_output"):
            modules, _main, instance = self._fake_probe_guard_modules()
            fatal = modules["esptool.util"].FatalError  # type: ignore[attr-defined]

            def fail() -> None:
                raise fatal("No serial data received.")

            if stage == "flush_input":
                instance.flush_input = fail  # type: ignore[attr-defined]
            else:
                instance._port.flushOutput = fail  # type: ignore[attr-defined]
            output = io.StringIO()
            with self.subTest(stage=stage), \
                 mock.patch.dict(sys.modules, modules), \
                 contextlib.redirect_stdout(output), \
                 self.assertRaises(RuntimeError):
                exec(flash.ESPTOOL_PROBE_GUARD, {})
            self.assertNotIn(
                flash.ESPTOOL_PROBE_UNAVAILABLE_MARKER, output.getvalue()
            )
            self.assertEqual(instance.sync_calls, 0)  # type: ignore[attr-defined]

    def test_runner_executes_guard_once_with_bounded_time_combined_capture(self) -> None:
        runner = mock.Mock(return_value=SimpleNamespace(
            returncode=0, stdout="esptool.py v4.11.0\nprobe evidence\n"
        ))
        with mock.patch.object(
            flash, "find_platformio_python", return_value="/opt/pio-python"
        ), mock.patch.dict(os.environ, {
            "PYTHONPATH": "/tmp/attacker",
            "PYTHONHOME": "/tmp/attacker-home",
            "PYTHONWARNINGS": "error",
            "PYTHONUTF8": "1",
            "ESPTOOL_CFGFILE": "/tmp/attacker.cfg",
            "ESPTOOL_PORT": "/dev/attacker",
            "ESPTOOL_OPEN_PORT_ATTEMPTS": "99",
        }):
            argv = flash.build_esptool_probe_argv("/dev/test")
            output = flash.run_guarded_esptool(
                argv, timeout_s=12.5, runner=runner
            )

        self.assertEqual(output, "esptool.py v4.11.0\nprobe evidence\n")
        runner.assert_called_once()
        called_argv = runner.call_args.args[0]
        kwargs = runner.call_args.kwargs
        self.assertEqual(called_argv, argv)
        self.assertEqual(called_argv[:3], [
            "/opt/pio-python", "-I", "-c",
        ])
        self.assertEqual(called_argv[3], flash.ESPTOOL_PROBE_GUARD)
        self.assertEqual(kwargs["cwd"], str(flash.REPO_ROOT))
        self.assertIs(kwargs["stdout"], flash.subprocess.PIPE)
        self.assertIs(kwargs["stderr"], flash.subprocess.STDOUT)
        self.assertIs(kwargs["text"], True)
        self.assertIs(kwargs["check"], False)
        self.assertIs(kwargs["shell"], False)
        self.assertEqual(kwargs["timeout"], 12.5)
        self.assertEqual(kwargs["env"]["ESPTOOL_OPEN_PORT_ATTEMPTS"], "1")
        self.assertFalse(any(
            key.startswith("PYTHON") for key in kwargs["env"]
        ))
        self.assertEqual(
            {key for key in kwargs["env"] if key.startswith("ESPTOOL_")},
            {"ESPTOOL_OPEN_PORT_ATTEMPTS"},
        )
        self.assertIsNot(kwargs["env"], os.environ)

    def test_runner_never_retries_failures_or_retry_marker(self) -> None:
        cases = (
            SimpleNamespace(returncode=2, stdout="fatal failure\n"),
            SimpleNamespace(
                returncode=0,
                stdout="Lost connection, retrying...\nthen success\n",
            ),
            subprocess.TimeoutExpired(["esptool"], 3),
            OSError("spawn failed"),
        )
        for outcome in cases:
            runner = mock.Mock()
            if isinstance(outcome, BaseException):
                runner.side_effect = outcome
            else:
                runner.return_value = outcome
            with self.subTest(outcome=type(outcome).__name__), \
                 mock.patch.object(
                     flash, "find_platformio_python",
                     return_value="/opt/pio-python",
                 ):
                argv = flash.build_esptool_probe_argv("/dev/test")
                with self.assertRaises(flash.FlashError):
                    flash.run_guarded_esptool(
                        argv, timeout_s=3, runner=runner
                    )
            runner.assert_called_once()

    def test_runner_types_only_guard_attested_exact_probe_silence(self) -> None:
        runner = mock.Mock(return_value=SimpleNamespace(
            returncode=flash.ESPTOOL_PROBE_UNAVAILABLE_EXIT,
            stdout=self._unavailable_transcript(),
        ))
        with mock.patch.object(
            flash, "find_platformio_python", return_value="/opt/pio-python"
        ):
            argv = flash.build_esptool_probe_argv("/dev/test")
            with self.assertRaises(flash.RomProbeUnavailable):
                flash.run_guarded_esptool(argv, timeout_s=1, runner=runner)
        runner.assert_called_once()

    def test_runner_rejects_probe_marker_attacks_as_plain_flash_errors(self) -> None:
        good = self._unavailable_transcript()
        mutations = {
            "zero exit": (0, good),
            "ordinary exit": (2, good),
            "non-integer reserved exit": (
                float(flash.ESPTOOL_PROBE_UNAVAILABLE_EXIT), good
            ),
            "duplicate marker": (
                flash.ESPTOOL_PROBE_UNAVAILABLE_EXIT,
                good + flash.ESPTOOL_PROBE_UNAVAILABLE_MARKER + "\n",
            ),
            "extra noise": (
                flash.ESPTOOL_PROBE_UNAVAILABLE_EXIT, good + "noise\n"
            ),
            "missing version": (
                flash.ESPTOOL_PROBE_UNAVAILABLE_EXIT,
                good.replace("esptool.py v4.11.0\n", ""),
            ),
            "wrong version": (
                flash.ESPTOOL_PROBE_UNAVAILABLE_EXIT,
                good.replace("v4.11.0", "v4.10.0"),
            ),
            "wrong port": (
                flash.ESPTOOL_PROBE_UNAVAILABLE_EXIT,
                good.replace("Serial port /dev/test", "Serial port /dev/other"),
            ),
            "missing warning": (
                flash.ESPTOOL_PROBE_UNAVAILABLE_EXIT,
                good.replace(
                    "WARNING: Pre-connection option \"no_reset\" was selected. "
                    "Connection may fail if the chip is not in bootloader or "
                    "flasher stub mode.\n", "",
                ),
            ),
            "missing connecting": (
                flash.ESPTOOL_PROBE_UNAVAILABLE_EXIT,
                good.replace("Connecting...\n", ""),
            ),
            "identity": (
                flash.ESPTOOL_PROBE_UNAVAILABLE_EXIT,
                good + "Chip is ESP32-S3 (revision v0.2)\n",
            ),
            "mac": (
                flash.ESPTOOL_PROBE_UNAVAILABLE_EXIT,
                good + f"MAC: {self.MAC}\n",
            ),
            "write": (
                flash.ESPTOOL_PROBE_UNAVAILABLE_EXIT,
                good + "Wrote 4 bytes (3 compressed) at 0x0 in 0.1 seconds...\n",
            ),
            "hash": (
                flash.ESPTOOL_PROBE_UNAVAILABLE_EXIT,
                good + "Hash of data verified.\n",
            ),
            "retry": (
                flash.ESPTOOL_PROBE_UNAVAILABLE_EXIT,
                good + flash.ESPTOOL_RETRY_MARKER + "\n",
            ),
        }
        with mock.patch.object(
            flash, "find_platformio_python", return_value="/opt/pio-python"
        ):
            argv = flash.build_esptool_probe_argv("/dev/test")
            for name, (returncode, output) in mutations.items():
                runner = mock.Mock(return_value=SimpleNamespace(
                    returncode=returncode, stdout=output
                ))
                with self.subTest(name=name):
                    try:
                        flash.run_guarded_esptool(
                            argv, timeout_s=1, runner=runner
                        )
                    except flash.FlashError as exc:
                        self.assertNotIsInstance(exc,
                                                 flash.RomProbeUnavailable)
                    else:
                        self.fail("marker attack was accepted")

    def test_probe_guard_is_required_only_for_flash_id(self) -> None:
        with mock.patch.object(
            flash, "find_platformio_python", return_value="/opt/pio-python"
        ):
            probe = flash.build_esptool_probe_argv("/dev/test")
            run = flash.build_esptool_run_argv("/dev/test")
            tampered_probe = list(probe)
            tampered_probe[3] = flash.ESPTOOL_GUARD
            tampered_run = list(run)
            tampered_run[3] = flash.ESPTOOL_PROBE_GUARD
            runner = mock.Mock()
            for argv in (tampered_probe, tampered_run):
                with self.assertRaises(flash.FlashError):
                    flash.run_guarded_esptool(
                        argv, timeout_s=1, runner=runner
                    )
            runner.assert_not_called()

            with tempfile.TemporaryDirectory() as temp_dir:
                regions = self._regions()
                paths = self._snapshot_paths(Path(temp_dir))
                for builder in (
                    flash.build_esptool_write_argv,
                    flash.build_esptool_verify_argv,
                ):
                    argv = builder("/dev/test", regions, paths)
                    argv[3] = flash.ESPTOOL_PROBE_GUARD
                    with self.assertRaises(flash.FlashError):
                        flash.run_guarded_esptool(
                            argv, timeout_s=1, runner=runner
                        )
            runner.assert_not_called()

    def test_builders_require_exact_normalized_dev_port(self) -> None:
        with mock.patch.object(
            flash, "find_platformio_python", return_value="/opt/pio-python"
        ):
            for port in (
                "relative", "/tmp/tty", "/dev/../tmp/tty", "/dev/test\n",
                Path("/dev/test"),
            ):
                with self.subTest(port=port), self.assertRaises(
                    flash.FlashError
                ):
                    flash.build_esptool_probe_argv(port)  # type: ignore[arg-type]

    def test_runner_rejects_mutable_or_subclassed_argv_before_execution(self) -> None:
        class FlipList(list[str]):
            def __init__(self, safe: list[str], swapped: list[str]) -> None:
                super().__init__(safe)
                self.swapped = swapped
                self.iterations = 0

            def __iter__(self):  # type: ignore[no-untyped-def]
                self.iterations += 1
                chosen = list.__iter__(self) if self.iterations == 1 \
                    else iter(self.swapped)
                yield from chosen

        class StrSubclass(str):
            pass

        runner = mock.Mock(return_value=SimpleNamespace(
            returncode=0, stdout="esptool.py v4.11.0\n"
        ))
        with mock.patch.object(
            flash, "find_platformio_python", return_value="/opt/pio-python"
        ):
            safe = flash.build_esptool_probe_argv("/dev/test")
            swapped = list(safe)
            swapped[-1] = "erase_flash"
            flip = FlipList(safe, swapped)
            subclassed = list(safe)
            subclassed[subclassed.index("/dev/test")] = StrSubclass(
                "/dev/test"
            )
            for name, argv in (
                ("list subclass", flip),
                ("str subclass", subclassed),
            ):
                with self.subTest(name=name), self.assertRaises(
                    flash.FlashError
                ):
                    flash.run_guarded_esptool(
                        argv, timeout_s=1, runner=runner
                    )
        runner.assert_not_called()

    def test_runner_rejects_version_drift_before_returning_exit_zero(self) -> None:
        outputs = (
            "MAC: e0:72:a1:f9:47:fc\n",
            "esptool.py v4.11.0\nesptool.py v4.11.0\n",
            "esptool.py v4.10.0\n",
        )
        for output in outputs:
            runner = mock.Mock(return_value=SimpleNamespace(
                returncode=0, stdout=output
            ))
            with self.subTest(output=output), mock.patch.object(
                flash, "find_platformio_python", return_value="/opt/pio-python"
            ):
                argv = flash.build_esptool_probe_argv("/dev/test")
                with self.assertRaises(flash.FlashError):
                    flash.run_guarded_esptool(
                        argv, timeout_s=1, runner=runner
                    )
            runner.assert_called_once()

    def test_runner_rejects_unbounded_timeout_and_non_guarded_argv(self) -> None:
        runner = mock.Mock()
        for timeout in (0, -1, flash.ESPTOOL_TIMEOUT_MAX_S + 1):
            with self.subTest(timeout=timeout), self.assertRaises(
                flash.FlashError
            ):
                flash.run_guarded_esptool(
                    ["python", "-c", flash.ESPTOOL_GUARD],
                    timeout_s=timeout, runner=runner,
                )
        with self.assertRaises(flash.FlashError):
            flash.run_guarded_esptool(
                ["python", "-m", "esptool", "flash_id"],
                timeout_s=1, runner=runner,
            )
        runner.assert_not_called()

    def test_runner_rejects_tampered_or_unsafe_prefixed_argv(self) -> None:
        runner = mock.Mock(return_value=SimpleNamespace(
            returncode=0, stdout="must not run"
        ))
        with mock.patch.object(
            flash, "find_platformio_python", return_value="/opt/pio-python"
        ):
            safe = flash.build_esptool_probe_argv("/dev/test")
            chip = safe.index("--chip")
            legacy_nonisolated = [
                safe[0], "-c", flash.ESPTOOL_GUARD, *safe[chip:]
            ]
            before = safe.index("--before") + 1
            after = safe.index("--after") + 1
            connect = safe.index("--connect-attempts")
            cases = {
                "default reset":
                    safe[:before] + ["default_reset"] + safe[before + 1:],
                "hard reset": safe[:after] + ["hard_reset"] + safe[after + 1:],
                "erase all": safe + ["--erase-all"],
                "force": safe + ["--force"],
                "wrong offset": safe + ["0x10000"],
                "missing attempts": safe[:connect] + safe[connect + 2:],
                "duplicate attempts": safe[:connect] +
                    ["--connect-attempts", "1"] + safe[connect:],
                "platformio upload": safe[:-1] + ["upload"],
                "missing isolated mode": legacy_nonisolated,
            }
            for name, argv in cases.items():
                with self.subTest(name=name), self.assertRaises(
                    flash.FlashError
                ):
                    flash.run_guarded_esptool(
                        argv, timeout_s=1, runner=runner
                    )
            with tempfile.TemporaryDirectory() as temp_dir:
                regions = self._regions()
                paths = self._snapshot_paths(Path(temp_dir))
                duplicate_path = flash.build_esptool_write_argv(
                    "/dev/test", regions, paths
                )
                duplicate_path[-1] = duplicate_path[-7]
                with self.assertRaises(flash.FlashError):
                    flash.run_guarded_esptool(
                        duplicate_path, timeout_s=1, runner=runner
                    )
        runner.assert_not_called()

    def test_identity_parser_accepts_only_exact_s3_8mb_device(self) -> None:
        identity = flash.parse_esptool_rom_identity(
            self._identity_transcript(), "/dev/cu.usbmodem-test"
        )

        self.assertTrue(flash.RomDeviceIdentity.__dataclass_params__.frozen)
        self.assertEqual(identity.base_mac, self.MAC)
        self.assertEqual(identity.port, "/dev/cu.usbmodem-test")
        self.assertEqual(identity.chip, "ESP32-S3")
        self.assertEqual(identity.revision, "v0.2")
        self.assertEqual(identity.flash_size, "8MB")
        self.assertEqual(identity.psram_size, "8MB")

    def test_identity_parser_rejects_missing_duplicate_conflicting_or_retry(self) -> None:
        good = self._identity_transcript()
        mutations = {
            "missing version": good.replace(
                "\x1b[32mesptool.py v4.11.0\x1b[0m\r", ""
            ),
            "duplicate version": good + "esptool.py v4.11.0\n",
            "wrong version": good.replace(
                "esptool.py v4.11.0", "esptool.py v4.10.0"
            ),
            "missing chip": good.replace(
                "Chip is ESP32-S3 (QFN56) (revision v0.2)\r\n", ""
            ),
            "duplicate chip": good +
                "Chip is ESP32-S3 (QFN56) (revision v0.2)\n",
            "wrong chip": good.replace("ESP32-S3", "ESP32-C3"),
            "missing mac": good.replace(
                f"MAC: {self.MAC.upper()}\n", ""
            ),
            "duplicate mac": good + f"MAC: {self.MAC}\n",
            "malformed mac": good.replace(
                self.MAC.upper(), "E0:72:A1:F9:47"
            ),
            "wrong flash": good.replace(
                "Detected flash size: 8MB", "Detected flash size: 4MB"
            ),
            "missing flash": good.replace(
                "Detected flash size: 8MB\n", ""
            ),
            "duplicate flash": good + "Detected flash size: 8MB\n",
            "missing psram": good.replace("Embedded PSRAM 8MB", "No PSRAM"),
            "wrong psram": good.replace(
                "Embedded PSRAM 8MB", "Embedded PSRAM 2MB"
            ),
            "conflicting psram in one line": good.replace(
                "Embedded PSRAM 8MB (AP_3v3)",
                "Embedded PSRAM 8MB (AP_3v3), Embedded PSRAM 4MB",
            ),
            "negated psram": good.replace(
                "Embedded PSRAM 8MB", "Not Embedded PSRAM 8MB"
            ),
            "duplicate psram": good +
                "Features: Embedded PSRAM 8MB\n",
            "missing serial port": good.replace(
                "Serial port /dev/cu.usbmodem-test\r\n", ""
            ),
            "duplicate serial port": good +
                "Serial port /dev/cu.usbmodem-test\n",
            "wrong serial port": good.replace(
                "Serial port /dev/cu.usbmodem-test",
                "Serial port /dev/cu.usbmodem-other",
            ),
            "retry": good + "Lost connection, retrying...\n",
        }
        for name, transcript in mutations.items():
            with self.subTest(name=name), self.assertRaises(
                flash.FlashError
            ):
                flash.parse_esptool_rom_identity(
                    transcript, "/dev/cu.usbmodem-test"
                )

    def test_every_operation_requires_one_exact_mac(self) -> None:
        version = "esptool.py v4.11.0\n"
        good = version + f"Connecting...\nMAC: {self.MAC.upper()}\n"
        self.assertEqual(
            flash.verify_esptool_operation_mac(good, self.MAC), self.MAC
        )
        bad = (
            version + "Connecting...\n",
            version + f"MAC: {self.MAC}\nMAC: {self.MAC}\n",
            version + "MAC: e0:72:a1:f9:47:fd\n",
            version + "MAC: not-a-mac\n",
            version + f"MAC: {self.MAC}\nLost connection, retrying...\n",
        )
        for transcript in bad:
            with self.subTest(transcript=transcript), self.assertRaises(
                flash.FlashError
            ):
                flash.verify_esptool_operation_mac(transcript, self.MAC)

    def test_run_result_requires_force_download_clear_and_watchdog_handoff(
        self,
    ) -> None:
        good = (
            "esptool.py v4.11.0\n"
            f"MAC: {self.MAC.upper()}\n"
            "Wrote 00000000, mask 00000001 to 6000812c\n"
            "Hard resetting with a watchdog...\n"
        )
        self.assertEqual(
            flash.parse_esptool_run_result(good, self.MAC), self.MAC
        )
        cases = (
            good.replace(f"MAC: {self.MAC.upper()}\n", ""),
            good.replace(
                f"MAC: {self.MAC.upper()}\n",
                f"MAC: {self.MAC}\nMAC: {self.MAC}\n",
            ),
            good.replace(self.MAC.upper(), "e0:72:a1:f9:47:fd"),
            good.replace("v4.11.0", "v4.10.0"),
            good.replace("esptool.py v4.11.0\n", ""),
            good.replace(
                "Wrote 00000000, mask 00000001 to 6000812c\n", ""
            ),
            good.replace("00000000, mask", "00000001, mask"),
            good.replace("mask 00000001", "mask ffffffff"),
            good.replace("to 6000812c", "to 60008128"),
            good.replace(
                "Wrote 00000000, mask 00000001 to 6000812c\n",
                "Wrote 00000000, mask 00000001 to 6000812c\n" * 2,
            ),
            good.replace("Hard resetting with a watchdog...\n", ""),
            good + "Staying in bootloader.\n",
        )
        for transcript in cases:
            with self.subTest(transcript=transcript), self.assertRaises(
                flash.FlashError
            ):
                flash.parse_esptool_run_result(transcript, self.MAC)

    def test_bound_nonwriting_reset_uses_only_probe_and_app_handoff(
        self,
    ) -> None:
        descriptor = _usb_record("/dev/cu.bound-reset", self.MAC)
        badge = flash.BadgeSerial(
            descriptor,
            False,
            expected_hardware_id=self.MAC,
        )
        badge.ser = object()
        badge._close_serial = mock.Mock(  # type: ignore[method-assign]
            side_effect=lambda: setattr(badge, "ser", None)
        )
        normal = _uplink_status(
            flash.UPDATE_MAINTENANCE_MIN_VERSION,
            hardware_id=self.MAC,
            responses=50,
        )
        badge.reconnect_same_uplink_normal = mock.Mock(  # type: ignore[method-assign]
            return_value=normal
        )
        identity = flash.RomDeviceIdentity(
            base_mac=self.MAC,
            port=descriptor.device,
            chip="ESP32-S3",
            revision="v0.2",
            flash_size="8MB",
            psram_size="8MB",
        )
        run_transcript = (
            "esptool.py v4.11.0\n"
            f"MAC: {self.MAC.upper()}\n"
            "Wrote 00000000, mask 00000001 to 6000812c\n"
            "Hard resetting with a watchdog...\n"
        )
        reset = mock.Mock(return_value=identity)
        runner = mock.Mock(return_value=run_transcript)

        with mock.patch.object(
            flash,
            "_take_badge_usb_descriptor_census",
            return_value=(descriptor,),
        ), mock.patch.object(
            flash,
            "reset_uplink_usb_to_rom",
            reset,
        ), mock.patch.object(
            flash,
            "run_guarded_esptool",
            runner,
        ):
            rebound, status = flash._reset_bound_uplink_without_write(
                badge,
                deadline=flash.time.monotonic() + 60,
            )

        self.assertIs(rebound, descriptor)
        self.assertEqual(status, normal)
        reset.assert_called_once()
        self.assertEqual(reset.call_args.args[0], descriptor.device)
        argv = runner.call_args.args[0]
        self.assertEqual(argv, flash.build_esptool_run_argv(
            descriptor.device
        ))
        self.assertNotIn("write_flash", argv)
        self.assertNotIn("verify_flash", argv)
        badge.reconnect_same_uplink_normal.assert_called_once()

    def test_bound_nonwriting_reset_refuses_moved_identity(
        self,
    ) -> None:
        descriptor = _usb_record(
            "/dev/cu.bound-reset",
            self.MAC,
            location="1-2",
        )
        moved = _usb_record(
            "/dev/cu.moved-reset",
            self.MAC,
            location="1-3",
        )
        badge = flash.BadgeSerial(
            descriptor,
            False,
            expected_hardware_id=self.MAC,
        )
        badge.ser = object()
        badge._close_serial = mock.Mock(  # type: ignore[method-assign]
            side_effect=lambda: setattr(badge, "ser", None)
        )
        reset = mock.Mock()
        runner = mock.Mock()

        with mock.patch.object(
            flash,
            "_take_badge_usb_descriptor_census",
            return_value=(moved,),
        ), mock.patch.object(
            flash,
            "reset_uplink_usb_to_rom",
            reset,
        ), mock.patch.object(
            flash,
            "run_guarded_esptool",
            runner,
        ), self.assertRaisesRegex(
            flash.FlashError, "another location"
        ):
            flash._reset_bound_uplink_without_write(
                badge,
                deadline=flash.time.monotonic() + 60,
            )

        reset.assert_not_called()
        runner.assert_not_called()

    def test_write_receipts_require_exact_order_size_address_and_hash_pair(self) -> None:
        regions = self._regions()
        transcript = self._write_transcript(regions)

        receipts = flash.parse_esptool_write_receipts(
            transcript, regions, self.MAC
        )

        self.assertEqual(
            [(receipt.offset, receipt.size) for receipt in receipts],
            [(region.offset, (region.size + 3) & ~3)
             for region in regions],
        )
        larger = transcript.replace("(7 compressed)", "(9 compressed)", 1)
        receipts = flash.parse_esptool_write_receipts(
            larger, regions, self.MAC
        )
        self.assertEqual(receipts[0].compressed_size, 9)

    def test_write_receipts_reject_weak_or_malformed_evidence(self) -> None:
        regions = self._regions()
        good = self._write_transcript(regions)
        blocks = good.splitlines()
        first_wrote = next(
            index for index, line in enumerate(blocks)
            if line.startswith("Wrote ")
        )
        second_wrote = next(
            index for index, line in enumerate(blocks[first_wrote + 2:],
                                                first_wrote + 2)
            if line.startswith("Wrote ")
        )
        swapped = list(blocks)
        swapped[first_wrote:first_wrote + 2], swapped[second_wrote:second_wrote + 2] = (
            swapped[second_wrote:second_wrote + 2],
            swapped[first_wrote:first_wrote + 2],
        )
        mutations = {
            "missing": "\n".join(blocks[:-2]),
            "extra": good + "Wrote 4 bytes (3 compressed) at 0x00030000 "
                     "in 0.1 seconds...\nHash of data verified.\n",
            "reordered": "\n".join(swapped),
            "wrong size": good.replace("Wrote 8 bytes", "Wrote 12 bytes", 1),
            "wrong address": good.replace(
                "at 0x00000000 in 0.1 seconds",
                "at 0x00010000 in 0.1 seconds", 1,
            ),
            "no compress": good.replace(" (7 compressed)", "", 1),
            "zero compressed": good.replace(
                "(7 compressed)", "(0 compressed)", 1
            ),
            "failed": good.replace("Hash of data verified.", "FAILED", 1),
            "unpaired": good.replace(
                "Hash of data verified.", "progress noise\nHash of data verified.", 1
            ),
            "duplicate": good + "\n".join(blocks[first_wrote:first_wrote + 2]),
            "count only": f"MAC: {self.MAC}\nWrote 4 regions\n",
            "wrong mac": self._write_transcript(
                regions, mac="e0:72:a1:f9:47:fd"
            ),
        }
        for name, transcript in mutations.items():
            with self.subTest(name=name), self.assertRaises(
                flash.FlashError
            ):
                flash.parse_esptool_write_receipts(
                    transcript, regions, self.MAC
                )

    def test_receipt_parsers_validate_immutable_region_metadata(self) -> None:
        regions = self._regions()
        bad_size = list(regions)
        bad_size[0] = flash.RomFlashRegion(
            offset=regions[0].offset, path=regions[0].path,
            data=regions[0].data, size=regions[0].size + 1,
            sha256=regions[0].sha256,
        )
        bad_hash = list(regions)
        bad_hash[0] = flash.RomFlashRegion(
            offset=regions[0].offset, path=regions[0].path,
            data=regions[0].data, size=regions[0].size,
            sha256="0" * 64,
        )
        for name, malformed in (("size", bad_size), ("hash", bad_hash)):
            with self.subTest(name=name), self.assertRaises(
                flash.FlashError
            ):
                flash.parse_esptool_write_receipts(
                    self._write_transcript(regions), tuple(malformed), self.MAC
                )

    def test_verify_receipts_require_exact_paths_order_and_digest_pair(self) -> None:
        regions = self._regions()
        with tempfile.TemporaryDirectory() as temp_dir:
            paths = self._snapshot_paths(Path(temp_dir))
            transcript = self._verify_transcript(regions, paths)

            receipts = flash.parse_esptool_verify_receipts(
                transcript, regions, paths, self.MAC
            )

        self.assertEqual(
            [(receipt.offset, receipt.size, receipt.path)
             for receipt in receipts],
            [(region.offset, (region.size + 3) & ~3, path.resolve())
             for region, path in zip(regions, paths)],
        )

    def test_parsers_accept_ansi_on_evidence_lines_and_bare_cr(self) -> None:
        identity = self._identity_transcript().replace(
            "Chip is ESP32-S3 (QFN56) (revision v0.2)",
            "\x1b[36mChip is ESP32-S3 (QFN56) (revision v0.2)\x1b[0m",
        ).replace(
            f"MAC: {self.MAC.upper()}",
            f"\x1b[33mMAC: {self.MAC.upper()}\x1b[0m",
        ).replace("\r\n", "\r").replace("\n", "\r")
        parsed = flash.parse_esptool_rom_identity(
            identity, "/dev/cu.usbmodem-test"
        )
        self.assertEqual(parsed.base_mac, self.MAC)

        regions = self._regions()
        write = self._write_transcript(regions)
        wrote_line = next(
            line for line in write.splitlines() if line.startswith("Wrote ")
        )
        write = write.replace(
            wrote_line, f"\x1b[35m{wrote_line}\x1b[0m", 1
        ).replace(
            "Hash of data verified.",
            "\x1b[32mHash of data verified.\x1b[0m", 1,
        ).replace("\r\n", "\r")
        flash.parse_esptool_write_receipts(write, regions, self.MAC)

        with tempfile.TemporaryDirectory() as temp_dir:
            paths = self._snapshot_paths(Path(temp_dir))
            verify = self._verify_transcript(regions, paths)
            verify_line = next(
                line for line in verify.splitlines()
                if line.startswith("Verifying ")
            )
            verify = verify.replace(
                verify_line, f"\x1b[34m{verify_line}\x1b[0m", 1
            ).replace(
                "-- verify OK (digest matched)",
                "\x1b[32m-- verify OK (digest matched)\x1b[0m", 1,
            ).replace("\n", "\r")
            flash.parse_esptool_verify_receipts(
                verify, regions, paths, self.MAC
            )

    def test_verify_parser_validates_immutable_region_metadata(self) -> None:
        regions = self._regions()
        with tempfile.TemporaryDirectory() as temp_dir:
            paths = self._snapshot_paths(Path(temp_dir))
            transcript = self._verify_transcript(regions, paths)
            malformed = (
                flash.RomFlashRegion(
                    offset=regions[0].offset, path=regions[0].path,
                    data=regions[0].data, size=regions[0].size + 1,
                    sha256=regions[0].sha256,
                ),
                *regions[1:],
            )
            with self.assertRaises(flash.FlashError):
                flash.parse_esptool_verify_receipts(
                    transcript, malformed, paths, self.MAC
                )
            malformed = (
                flash.RomFlashRegion(
                    offset=regions[0].offset, path=regions[0].path,
                    data=regions[0].data, size=regions[0].size,
                    sha256="0" * 64,
                ),
                *regions[1:],
            )
            with self.assertRaises(flash.FlashError):
                flash.parse_esptool_verify_receipts(
                    transcript, malformed, paths, self.MAC
                )

    def test_receipt_parsers_wrap_or_reject_giant_numeric_tokens(self) -> None:
        regions = self._regions()
        giant = "9" * 5000
        write = self._write_transcript(regions).replace(
            "Wrote 8 bytes", f"Wrote {giant} bytes", 1
        )
        with self.assertRaises(flash.FlashError):
            flash.parse_esptool_write_receipts(write, regions, self.MAC)

        with tempfile.TemporaryDirectory() as temp_dir:
            paths = self._snapshot_paths(Path(temp_dir))
            verify = self._verify_transcript(regions, paths).replace(
                "0x8 (8)", f"0x{'f' * 5000} ({giant})", 1
            )
            with self.assertRaises(flash.FlashError):
                flash.parse_esptool_verify_receipts(
                    verify, regions, paths, self.MAC
                )

    def test_verify_receipts_reject_weak_or_malformed_evidence(self) -> None:
        regions = self._regions()
        with tempfile.TemporaryDirectory() as temp_dir:
            paths = self._snapshot_paths(Path(temp_dir))
            good = self._verify_transcript(regions, paths)
            blocks = good.splitlines()
            first = next(
                i for i, line in enumerate(blocks)
                if line.startswith("Verifying ")
            )
            second = next(
                i for i, line in enumerate(blocks[first + 2:], first + 2)
                if line.startswith("Verifying ")
            )
            swapped = list(blocks)
            swapped[first:first + 2], swapped[second:second + 2] = (
                swapped[second:second + 2], swapped[first:first + 2],
            )
            mutations = {
                "missing": "\n".join(blocks[:-2]),
                "extra": good + blocks[first] + "\n" + blocks[first + 1] + "\n",
                "reordered": "\n".join(swapped),
                "path": good.replace(str(paths[0].resolve()), "/tmp/wrong.bin", 1),
                "wrong size": good.replace("0x8 (8)", "0xc (12)", 1),
                "wrong address": good.replace("0x00000000", "0x00010000", 1),
                "failed": good.replace(
                    "-- verify OK (digest matched)", "FAILED", 1
                ),
                "unpaired": good.replace(
                    "-- verify OK (digest matched)",
                    "progress noise\n-- verify OK (digest matched)", 1,
                ),
                "count only": f"MAC: {self.MAC}\nVerified 4 regions\n",
                "wrong mac": self._verify_transcript(
                    regions, paths, mac="e0:72:a1:f9:47:fd"
                ),
            }
            for name, transcript in mutations.items():
                with self.subTest(name=name), self.assertRaises(
                    flash.FlashError
                ):
                    flash.parse_esptool_verify_receipts(
                        transcript, regions, paths, self.MAC
                    )

    def test_builders_reject_region_or_path_drift_without_reading_originals(self) -> None:
        regions = self._regions()
        with tempfile.TemporaryDirectory() as temp_dir:
            paths = self._snapshot_paths(Path(temp_dir))
            with self.assertRaises(flash.FlashError):
                flash.build_esptool_write_argv(
                    "/dev/test", regions[:-1], paths[:-1]
                )
            wrong_offset = list(regions)
            region = wrong_offset[-1]
            wrong_offset[-1] = flash.RomFlashRegion(
                offset=0x10000, path=region.path, data=region.data,
                size=region.size, sha256=region.sha256,
            )
            with self.assertRaises(flash.FlashError):
                flash.build_esptool_verify_argv(
                    "/dev/test", tuple(wrong_offset), paths
                )
            boolean_size = list(regions)
            data = b"A"
            boolean_size[0] = flash.RomFlashRegion(
                offset=0, path=regions[0].path, data=data, size=True,
                sha256=hashlib.sha256(data).hexdigest(),
            )
            with self.assertRaises(flash.FlashError):
                flash.build_esptool_write_argv(
                    "/dev/test", tuple(boolean_size), paths
                )
            with self.assertRaises(flash.FlashError):
                flash.build_esptool_write_argv(
                    "/dev/test", regions,
                    (Path("relative.bin"), *paths[1:]),
                )
            with self.assertRaises(flash.FlashError):
                flash.build_esptool_write_argv(
                    "/dev/test", regions,
                    (paths[0].parent / "nested" / ".." / paths[0].name,
                     *paths[1:]),
                )


class RomDiscoveryEvidenceTests(unittest.TestCase):
    VERSION = "0.64.76-badge-defcon34"
    MAC = "e0:72:a1:f9:47:fc"

    class _Clock:
        def __init__(self) -> None:
            self.now = 0.0

        def monotonic(self) -> float:
            return self.now

        def sleep(self, duration: float) -> None:
            self.now += duration

    @classmethod
    def _identity(cls, port: str, mac: str | None = None) -> flash.RomDeviceIdentity:
        return flash.RomDeviceIdentity(
            base_mac=mac or cls.MAC,
            port=port,
            chip="ESP32-S3",
            revision="v0.2",
            flash_size="8MB",
            psram_size="8MB",
        )

    @classmethod
    def _transcript(cls, port: str, mac: str | None = None) -> str:
        return (
            "esptool.py v4.11.0\n"
            f"Serial port {port}\n"
            "Connecting...\n"
            "Chip is ESP32-S3 (QFN56) (revision v0.2)\n"
            "Features: WiFi, BLE, Embedded PSRAM 8MB (AP_3v3)\n"
            f"MAC: {mac or cls.MAC}\n"
            "Detected flash size: 8MB\n"
        )

    def test_probe_rom_device_returns_exact_identity_or_typed_absence(self) -> None:
        port = "/dev/cu.probe"
        calls: list[tuple[list[str], float]] = []

        def success(argv: list[str], *, timeout_s: float) -> str:
            calls.append((argv, timeout_s))
            return self._transcript(port)

        identity = flash.probe_rom_device(
            port, 4.5, esptool_runner=success
        )
        self.assertEqual(identity, self._identity(port))
        self.assertEqual(calls[0][1], 4.5)
        self.assertEqual(calls[0][0][-1], "flash_id")
        self.assertEqual(calls[0][0][3], flash.ESPTOOL_PROBE_GUARD)

        def unavailable(_argv: list[str], *, timeout_s: float) -> str:
            raise flash.RomProbeUnavailable(f"silent after {timeout_s}")

        self.assertIsNone(flash.probe_rom_device(
            port, 2, esptool_runner=unavailable
        ))

    def test_usb_port_hardware_id_requires_one_exact_descriptor(self) -> None:
        port = "/dev/cu.selected"
        with mock.patch.object(
            flash, "_take_badge_usb_descriptor_census",
            return_value=_usb_census(
                ["/dev/cu.other", port],
                {
                    "/dev/cu.other": "11:22:33:44:55:66",
                    port: self.MAC,
                },
            ),
        ):
            self.assertEqual(flash.usb_port_hardware_id(port), self.MAC)

    def test_usb_port_hardware_id_fails_closed_on_uncertain_descriptor(
        self,
    ) -> None:
        port = "/dev/cu.selected"
        record = _usb_record(port, self.MAC)
        cases = {
            "missing port": (),
            "duplicate port": (record, record),
        }
        for name, census in cases.items():
            with self.subTest(name=name), mock.patch.object(
                flash, "_take_badge_usb_descriptor_census",
                return_value=census,
            ), self.assertRaises(flash.FlashError):
                flash.usb_port_hardware_id(port)

        with mock.patch.object(
            flash, "_take_badge_usb_descriptor_census",
            side_effect=flash.FlashError("malformed supported descriptor"),
        ), self.assertRaises(flash.FlashError):
            flash.usb_port_hardware_id(port)

    def test_selected_port_rom_absence_uses_repeated_bound_proofs(
        self,
    ) -> None:
        port = "/dev/cu.selected"
        probe = mock.Mock(return_value=None)
        descriptor = mock.Mock(return_value=self.MAC)
        with mock.patch.object(
            flash, "usb_port_hardware_id", descriptor
        ), mock.patch.object(
            flash, "probe_rom_device", probe
        ):
            flash.require_selected_port_not_rom(
                port, self.MAC, timeout_s=9
            )
        self.assertEqual(descriptor.call_args_list, [
            mock.call(port),
            mock.call(port),
        ])
        self.assertEqual(probe.call_count, flash.ROM_CENSUS_ABSENCE_PROOFS)
        self.assertTrue(all(
            len(call.args) == 2 and call.args[0] == port
            and 0 < call.args[1] <= 9
            for call in probe.call_args_list
        ))

    def test_selected_port_rom_absence_rejects_identity_or_path_change(
        self,
    ) -> None:
        port = "/dev/cu.selected"
        other_mac = "11:22:33:44:55:66"
        with mock.patch.object(
            flash, "usb_port_hardware_id",
            side_effect=[self.MAC, other_mac],
        ), mock.patch.object(
            flash, "probe_rom_device", return_value=None
        ), self.assertRaisesRegex(flash.FlashError, "descriptor.*changed"):
            flash.require_selected_port_not_rom(
                port, self.MAC, timeout_s=9
            )

        with mock.patch.object(
            flash, "usb_port_hardware_id", return_value=self.MAC
        ), mock.patch.object(
            flash, "probe_rom_device",
            return_value=self._identity(port),
        ) as probe, self.assertRaisesRegex(
            flash.FlashError, "pre-existing.*ROM"
        ):
            flash.require_selected_port_not_rom(
                port, self.MAC, timeout_s=9
            )
        probe.assert_called_once()

    def test_native_usb_reset_requires_and_returns_exact_rom_identity(
        self,
    ) -> None:
        port = "/dev/cu.native-reset"
        calls: list[tuple[list[str], float]] = []

        def success(argv: list[str], *, timeout_s: float) -> str:
            calls.append((argv, timeout_s))
            return self._transcript(port)

        identity = flash.reset_uplink_usb_to_rom(
            port, 7.5, esptool_runner=success
        )
        self.assertEqual(identity, self._identity(port))
        self.assertEqual(calls[0][1], 7.5)
        self.assertEqual(
            calls[0][0][calls[0][0].index("--before") + 1],
            "usb_reset",
        )
        self.assertEqual(calls[0][0][-1], "flash_id")

        def unavailable(_argv: list[str], *, timeout_s: float) -> str:
            raise flash.RomProbeUnavailable(f"silent after {timeout_s}")

        with self.assertRaises(flash.RomProbeUnavailable):
            flash.reset_uplink_usb_to_rom(
                port, 2, esptool_runner=unavailable
            )

    def test_probe_rom_device_propagates_every_non_typed_failure(self) -> None:
        port = "/dev/cu.probe"
        for outcome in (
            flash.FlashError("open failed"),
            "esptool.py v4.11.0\nnoise\n",
        ):
            def runner(_argv: list[str], *, timeout_s: float,
                       result: object = outcome) -> str:
                if isinstance(result, BaseException):
                    raise result
                return str(result)

            with self.subTest(outcome=type(outcome).__name__), \
                 self.assertRaises(flash.FlashError):
                flash.probe_rom_device(
                    port, 1, esptool_runner=runner
                )

    def test_wait_for_rom_device_splits_one_global_budget_without_starvation(self) -> None:
        clock = self._Clock()
        calls: list[tuple[str, float]] = []
        ports = ["/dev/cu.z", "/dev/cu.a", "/dev/cu.a"]

        def probe(port: str, timeout_s: float) -> flash.RomDeviceIdentity | None:
            calls.append((port, timeout_s))
            clock.now += timeout_s
            return self._identity(port) if port == "/dev/cu.z" else None

        with mock.patch.object(flash, "list_usb_ports", return_value=ports), \
             mock.patch.object(flash, "probe_rom_device", side_effect=probe), \
             mock.patch.object(flash.time, "monotonic", side_effect=clock.monotonic), \
             mock.patch.object(flash.time, "sleep", side_effect=clock.sleep):
            found = flash.wait_for_rom_device(self.MAC, timeout_s=10)

        self.assertEqual(found, self._identity("/dev/cu.z"))
        self.assertEqual(calls, [
            ("/dev/cu.a", 5.0),
            ("/dev/cu.z", 5.0),
        ])

    def test_wait_for_rom_device_rebinds_known_mac_across_path_rename(self) -> None:
        clock = self._Clock()
        other = "e0:72:a1:f9:47:fd"
        rounds = [["/dev/cu.old"], ["/dev/cu.renamed"]]

        def probe(port: str, _timeout_s: float) -> flash.RomDeviceIdentity:
            if port.endswith("old"):
                return self._identity(port, other)
            return self._identity(port)

        with mock.patch.object(flash, "list_usb_ports", side_effect=rounds), \
             mock.patch.object(flash, "probe_rom_device", side_effect=probe), \
             mock.patch.object(flash.time, "monotonic", side_effect=clock.monotonic), \
             mock.patch.object(flash.time, "sleep", side_effect=clock.sleep):
            found = flash.wait_for_rom_device(self.MAC.upper(), timeout_s=5)
        self.assertEqual(found.port, "/dev/cu.renamed")
        self.assertEqual(found.base_mac, self.MAC)

    def test_wait_for_rom_device_completes_round_and_rejects_duplicates(self) -> None:
        clock = self._Clock()
        with mock.patch.object(
            flash, "list_usb_ports", return_value=["/dev/cu.a", "/dev/cu.b"]
        ), mock.patch.object(
            flash, "probe_rom_device",
            side_effect=lambda port, _timeout: self._identity(port),
        ), mock.patch.object(
            flash.time, "monotonic", side_effect=clock.monotonic
        ), mock.patch.object(
            flash.time, "sleep", side_effect=clock.sleep
        ), self.assertRaisesRegex(flash.FlashError, "duplicate"):
            flash.wait_for_rom_device(self.MAC, timeout_s=5)

    def test_wait_for_rom_device_unknown_accepts_one_only_after_all_silent(self) -> None:
        clock = self._Clock()
        calls: list[str] = []

        def probe(port: str, _timeout_s: float) -> flash.RomDeviceIdentity | None:
            calls.append(port)
            return self._identity(port) if port.endswith("a") else None

        with mock.patch.object(
            flash, "list_usb_ports", return_value=["/dev/cu.b", "/dev/cu.a"]
        ), mock.patch.object(
            flash, "probe_rom_device", side_effect=probe
        ), mock.patch.object(
            flash.time, "monotonic", side_effect=clock.monotonic
        ), mock.patch.object(
            flash.time, "sleep", side_effect=clock.sleep
        ):
            found = flash.wait_for_rom_device(None, timeout_s=5)
        self.assertEqual(found.port, "/dev/cu.a")
        self.assertEqual(calls, ["/dev/cu.a", "/dev/cu.b"])

        with mock.patch.object(
            flash, "list_usb_ports", return_value=["/dev/cu.a", "/dev/cu.b"]
        ), mock.patch.object(
            flash, "probe_rom_device",
            side_effect=lambda port, _timeout: self._identity(
                port, self.MAC if port.endswith("a") else "e0:72:a1:f9:47:fd"
            ),
        ), mock.patch.object(
            flash.time, "monotonic", side_effect=clock.monotonic
        ), mock.patch.object(
            flash.time, "sleep", side_effect=clock.sleep
        ), self.assertRaisesRegex(flash.FlashError, "multiple"):
            flash.wait_for_rom_device(None, timeout_s=5)

    def test_wait_for_rom_device_validates_before_enumeration_and_propagates(self) -> None:
        enumerate_ports = mock.Mock()
        with mock.patch.object(flash, "list_usb_ports", enumerate_ports), \
             self.assertRaises(flash.FlashError):
            flash.wait_for_rom_device("not-a-mac", timeout_s=5)
        enumerate_ports.assert_not_called()

        with mock.patch.object(
            flash, "list_usb_ports", return_value=["/dev/cu.a"]
        ), mock.patch.object(
            flash, "probe_rom_device", side_effect=flash.FlashError("port open")
        ), self.assertRaisesRegex(flash.FlashError, "port open"):
            flash.wait_for_rom_device(self.MAC, timeout_s=5)

    def test_wait_for_rom_device_timeout_diagnoses_silent_and_other_macs(self) -> None:
        clock = self._Clock()
        other = "e0:72:a1:f9:47:fd"

        def probe(port: str, timeout_s: float) -> flash.RomDeviceIdentity | None:
            clock.now += timeout_s
            return None if port.endswith("a") else self._identity(port, other)

        with mock.patch.object(
            flash, "list_usb_ports", return_value=["/dev/cu.a", "/dev/cu.b"]
        ), mock.patch.object(
            flash, "probe_rom_device", side_effect=probe
        ), mock.patch.object(
            flash.time, "monotonic", side_effect=clock.monotonic
        ), mock.patch.object(
            flash.time, "sleep", side_effect=clock.sleep
        ), self.assertRaises(flash.FlashError) as raised:
            flash.wait_for_rom_device(self.MAC, timeout_s=8)
        self.assertIn("/dev/cu.a=silent", str(raised.exception))
        self.assertIn(f"/dev/cu.b={other}", str(raised.exception))

    def test_wait_for_rom_device_never_accepts_identity_after_deadline(self) -> None:
        clock = self._Clock()

        def late_probe(port: str, timeout_s: float) -> flash.RomDeviceIdentity:
            self.assertEqual(timeout_s, 5.0)
            clock.now = 99.0
            return self._identity(port)

        with mock.patch.object(
            flash, "list_usb_ports", return_value=["/dev/cu.late"]
        ), mock.patch.object(
            flash, "probe_rom_device", side_effect=late_probe
        ), mock.patch.object(
            flash.time, "monotonic", side_effect=clock.monotonic
        ), mock.patch.object(
            flash.time, "sleep", side_effect=clock.sleep
        ), self.assertRaisesRegex(flash.FlashError, "timed out"):
            flash.wait_for_rom_device(self.MAC, timeout_s=5)

    def test_post_uplink_application_evidence_is_strict_pure_and_frozen(self) -> None:
        status = _uplink_status(
            self.VERSION, hardware_id=self.MAC.upper(), responses=42
        )
        with mock.patch.object(flash, "log") as logger, \
             mock.patch.object(flash.time, "monotonic") as monotonic, \
             mock.patch.object(flash.time, "sleep") as sleeper:
            evidence = flash.verify_post_uplink_application(
                status,
                expected_hardware_id=self.MAC,
                expected_version=self.VERSION,
                expected_partition="ota_0",
            )
        logger.assert_not_called()
        monotonic.assert_not_called()
        sleeper.assert_not_called()
        self.assertTrue(
            flash.PostUplinkApplicationEvidence.__dataclass_params__.frozen
        )
        self.assertEqual(evidence.hardware_id, self.MAC)
        self.assertEqual(evidence.version, self.VERSION)
        self.assertEqual(evidence.running_partition, "ota_0")
        self.assertEqual(evidence.responses_completed, 42)
        self.assertIs(evidence.application_health_verified, True)
        self.assertIs(evidence.rollback_cleared, True)
        with self.assertRaises(TypeError):
            flash.PostUplinkApplicationEvidence(
                self.MAC, self.VERSION, "ota_0", 42,
                application_health_verified=False,
            )

    def test_post_uplink_evidence_cannot_be_directly_constructed_or_replaced(self) -> None:
        for values in (
            (self.MAC, self.VERSION, "ota_0", 42),
            ("bad", "", "factory", -1),
        ):
            with self.subTest(values=values), self.assertRaises(TypeError):
                flash.PostUplinkApplicationEvidence(*values)

        evidence = flash.verify_post_uplink_application(
            _uplink_status(self.VERSION),
            expected_hardware_id=self.MAC,
            expected_version=self.VERSION,
            expected_partition="ota_0",
        )
        with self.assertRaises(TypeError):
            dataclasses.replace(
                evidence, hardware_id="e0:72:a1:f9:47:fd"
            )

    def test_post_uplink_evidence_requires_private_attestation(self) -> None:
        evidence = flash.verify_post_uplink_application(
            _uplink_status(self.VERSION),
            expected_hardware_id=self.MAC,
            expected_version=self.VERSION,
            expected_partition="ota_0",
        )
        self.assertIs(
            flash._revalidate_post_uplink_application_evidence(evidence),
            evidence,
        )

        fabricated = object.__new__(flash.PostUplinkApplicationEvidence)
        for name, value in {
            "hardware_id": self.MAC,
            "version": self.VERSION,
            "running_partition": "ota_0",
            "responses_completed": 42,
            "application_health_verified": True,
            "rollback_cleared": True,
        }.items():
            object.__setattr__(fabricated, name, value)
        with self.assertRaises(flash.FlashError):
            flash._revalidate_post_uplink_application_evidence(fabricated)

    def test_post_uplink_evidence_binds_exact_issued_field_snapshot(self) -> None:
        mutations = {
            "hardware_id": "e0:72:a1:f9:47:fd",
            "version": "0.64.77-badge-defcon34",
            "running_partition": "ota_1",
            "responses_completed": 43,
        }
        for field_name, replacement in mutations.items():
            evidence = flash.verify_post_uplink_application(
                _uplink_status(self.VERSION),
                expected_hardware_id=self.MAC,
                expected_version=self.VERSION,
                expected_partition="ota_0",
            )
            object.__setattr__(evidence, field_name, replacement)
            with self.subTest(field=field_name), self.assertRaises(
                flash.FlashError
            ):
                flash._revalidate_post_uplink_application_evidence(evidence)

    def test_post_uplink_evidence_exposes_no_standalone_issuer(self) -> None:
        self.assertFalse(hasattr(
            flash, "_issue_post_uplink_application_evidence"
        ))

    def test_post_uplink_application_rejects_every_unproven_field(self) -> None:
        good = _uplink_status(self.VERSION)
        cases: list[tuple[str, object, dict[str, object]]] = [
            (
                "stage evidence",
                _bound_rom_stage(
                    _usb_record("/dev/cu.a", self.MAC),
                    _frozen_uplink_set(self.VERSION),
                    self.VERSION,
                ),
                {},
            ),
            ("mac continuity", good, {
                "expected_hardware_id": "e0:72:a1:f9:47:fd"
            }),
            ("exact version", good, {
                "expected_version": "v" + self.VERSION
            }),
            ("invalid expected partition", good, {
                "expected_partition": "factory"
            }),
            ("wrong actual partition", good, {
                "expected_partition": "ota_1"
            }),
            ("malformed expected version", good, {"expected_version": b"x"}),
        ]
        pending = _uplink_status(self.VERSION, pending_verify=True)
        cases.append(("pending verify", pending, {}))
        rollback = _uplink_status(self.VERSION)
        rollback["rollback_state"] = "pending_verify"
        cases.append(("rollback", rollback, {}))
        recovery = _uplink_status(self.VERSION)
        recovery["recovery_mode"] = "startup_dependency"
        cases.append(("recovery", recovery, {}))
        boolean_count = _uplink_status(self.VERSION)
        boolean_count["usb_health"]["responses_completed"] = True
        cases.append(("boolean count", boolean_count, {}))
        negative_count = _uplink_status(self.VERSION)
        negative_count["usb_health"]["responses_completed"] = -1
        cases.append(("negative count", negative_count, {}))

        defaults: dict[str, object] = {
            "expected_hardware_id": self.MAC,
            "expected_version": self.VERSION,
            "expected_partition": "ota_0",
        }
        for name, status, changes in cases:
            arguments = dict(defaults)
            arguments.update(changes)
            with self.subTest(name=name), self.assertRaises(flash.FlashError):
                flash.verify_post_uplink_application(
                    status, **arguments  # type: ignore[arg-type]
                )


class LegacyUsbBootstrapTests(unittest.TestCase):
    VERSION = "0.64.76-badge-defcon34"
    PORT = "/dev/cu.legacy-uplink"
    MAC = "e0:72:a1:f9:47:fc"

    @classmethod
    def _device(cls) -> flash.RomDeviceIdentity:
        return flash.RomDeviceIdentity(
            base_mac=cls.MAC,
            port="/dev/cu.rom-uplink",
            chip="ESP32-S3",
            revision="v0.2",
            flash_size="8MB",
            psram_size="8MB",
        )

    def test_exact_legacy_identity_and_health_are_required(self) -> None:
        status = _legacy_uplink_status(self.VERSION)
        self.assertEqual(
            flash.validate_legacy_uplink_bootstrap_status(status),
            self.VERSION,
        )
        status["scanner_uart_alive"] = True
        self.assertEqual(
            flash.validate_legacy_uplink_bootstrap_status(status),
            self.VERSION,
        )
        status["scanner_uart_alive"] = False

        bad_cases = {
            "wrong firmware": ("firmware_name", "scanner-s3-combo-fof_badge"),
            "wrong project": ("app_project", "fof_badge_scanner"),
            "wrong hardware": ("hardware_type", "esp32s3"),
            "pending verify": ("pending_verify", True),
            "unsafe recovery": ("recovery_mode", "startup_dependency"),
            "safe mode": ("safe_mode", True),
            "USB dead": ("usb_control_alive", False),
            "wrong version": ("version", "0.64.75-badge-defcon34"),
            "scanner UART malformed": ("scanner_uart_alive", 0),
        }
        for name, (field, value) in bad_cases.items():
            with self.subTest(name=name):
                candidate = dict(status)
                candidate[field] = value
                with self.assertRaises(flash.FlashError):
                    flash.validate_legacy_uplink_bootstrap_status(candidate)

        for current_only_field, value in (
            ("target", "uplink-s3-fof_badge"),
            ("project", "fof_badge_uplink"),
            ("hardware_id", self.MAC),
            ("running_partition", "ota_0"),
            ("rollback_state", "clear"),
            ("usb_health", {"responses_completed": 1}),
        ):
            with self.subTest(current_only_field=current_only_field):
                candidate = dict(status)
                candidate[current_only_field] = value
                with self.assertRaisesRegex(
                    flash.FlashError, "current-schema|legacy"
                ):
                    flash.validate_legacy_uplink_bootstrap_status(candidate)

    def test_bootloader_request_uses_same_open_badge_and_exactly_one_command(
        self,
    ) -> None:
        events: list[tuple[object, ...]] = []
        outer = self

        class FakeBadge:
            def status(self, timeout_s=5):
                events.append(("status", timeout_s))
                return _legacy_uplink_status(outer.VERSION)

            def write_line(self, line):
                events.append(("write", line))

            def read_prefixed_text(self, prefix, timeout_s):
                events.append(("read", prefix, timeout_s))
                return "OK"

        got = flash.request_legacy_uplink_rom(FakeBadge(), timeout_s=3)
        self.assertEqual(got, self.VERSION)
        self.assertEqual(
            [event for event in events if event[0] == "write"],
            [("write", "FOF_BOOTLOADER")],
        )
        self.assertIn(("read", "FOF_BOOTLOADER:", 3), events)

    def test_wire_contains_one_bootloader_command_and_no_aliases(self) -> None:
        payload = (
            b"FOF_STATUS:" +
            json.dumps(
                _legacy_uplink_status(self.VERSION),
                separators=(",", ":"),
            ).encode("utf-8") +
            b"\nFOF_BOOTLOADER:OK\n"
        )
        raw = _ScriptedRawSerial([payload])
        badge = flash.BadgeSerial(_usb_record(self.PORT), False)
        badge.ser = raw

        got = flash.request_legacy_uplink_rom(badge, timeout_s=3)

        self.assertEqual(got, self.VERSION)
        self.assertEqual(
            raw.writes,
            [b"\nFOF_STATUS\n", b"\nFOF_BOOTLOADER\n"],
        )
        wire = b"".join(raw.writes)
        self.assertEqual(wire.count(b"FOF_BOOTLOADER"), 1)
        for forbidden in (b"FOF_DOWNLOAD", b"FOF_FLASH", b"FOF_CTL:"):
            self.assertNotIn(forbidden, wire)

    def test_invalid_status_or_ack_never_discovers_rom(self) -> None:
        class FakeBadge:
            def __init__(self, status, ack="OK"):
                self._status = status
                self._ack = ack
                self.commands: list[str] = []

            def status(self, timeout_s=5):
                return self._status

            def write_line(self, line):
                self.commands.append(line)

            def read_prefixed_text(self, _prefix, _timeout_s):
                return self._ack

        invalid = _legacy_uplink_status(self.VERSION)
        invalid["usb_control_alive"] = False
        invalid_badge = FakeBadge(invalid)
        with self.assertRaises(flash.FlashError):
            flash.request_legacy_uplink_rom(invalid_badge, timeout_s=3)
        self.assertEqual(invalid_badge.commands, [])

        bad_ack = FakeBadge(_legacy_uplink_status(self.VERSION), ack="NO")
        with self.assertRaisesRegex(flash.FlashError, "ack"):
            flash.request_legacy_uplink_rom(bad_ack, timeout_s=3)
        self.assertEqual(bad_ack.commands, ["FOF_BOOTLOADER"])

    def test_bridge_closes_application_before_bound_rom_session(self) -> None:
        descriptor = _usb_record(self.PORT, self.MAC, location="3-1")
        artifacts = _frozen_uplink_set(self.VERSION)
        stage = _bound_rom_stage(descriptor, artifacts, self.VERSION)
        events: list[tuple[object, ...]] = []
        outer = self

        class FakeBadge:
            def __init__(self, descriptor, dry_run):
                events.append((
                    "construct", descriptor.device, dry_run
                ))

            def __enter__(self):
                events.append(("open",))
                return self

            def __exit__(self, *_args):
                events.append(("close",))

            def status(self, timeout_s=5):
                events.append(("status", timeout_s))
                return _legacy_uplink_status(outer.VERSION)

            def write_line(self, line):
                events.append(("write", line))

            def read_prefixed_text(self, prefix, timeout_s):
                events.append(("read", prefix, timeout_s))
                return "OK"

        def wait_rom(binding, selected_artifacts, version, *, timeout_s):
            events.append((
                "wait_rom",
                binding,
                selected_artifacts,
                version,
                timeout_s,
            ))
            return stage

        with mock.patch.object(flash, "BadgeSerial", FakeBadge), \
             mock.patch.object(
                 flash.time, "sleep",
                 side_effect=lambda duration: events.append(
                     ("settle", duration)
                 ),
             ), \
             mock.patch.object(
                 flash, "_wait_for_bound_rom_flash", side_effect=wait_rom
            ):
            got = flash.legacy_usb_bootstrap_to_rom(
                descriptor,
                artifacts,
                self.VERSION,
                timeout_s=12,
            )

        self.assertEqual(got, stage)
        self.assertLess(events.index(("open",)), next(
            index for index, event in enumerate(events)
            if event[0] == "status"
        ))
        self.assertLess(events.index(("close",)), events.index(
            ("settle", flash.LEGACY_ROM_ENUMERATION_SETTLE_S)
        ))
        self.assertLess(
            events.index(
                ("settle", flash.LEGACY_ROM_ENUMERATION_SETTLE_S)
            ),
            next(
                index for index, event in enumerate(events)
                if event[0] == "wait_rom"
            ),
        )
        rebound = next(
            event for event in events if event[0] == "wait_rom"
        )
        self.assertEqual(rebound[1].serial_number, self.MAC)
        self.assertEqual(rebound[1].location, "3-1")
        self.assertIs(rebound[2], artifacts)
        self.assertEqual(rebound[3:], (self.VERSION, 12))
        self.assertEqual(
            [event for event in events if event[0] == "write"],
            [("write", "FOF_BOOTLOADER")],
        )

    def test_bridge_propagates_bound_rom_failure_without_retrying_command(
        self,
    ) -> None:
        descriptor = _usb_record(self.PORT, self.MAC)
        artifacts = _frozen_uplink_set(self.VERSION)
        badge = mock.MagicMock()
        badge.__enter__.return_value = badge
        badge.read_prefixed_text.return_value = "OK"
        badge.status.return_value = _legacy_uplink_status(self.VERSION)
        failure = flash.FlashError("descriptor location changed")
        with mock.patch.object(
            flash, "BadgeSerial", return_value=badge
        ), mock.patch.object(
            flash.time, "sleep"
        ), mock.patch.object(
            flash, "_wait_for_bound_rom_flash", side_effect=failure
        ) as wait_rom, self.assertRaisesRegex(
            flash.FlashError, "location changed"
        ):
            flash.legacy_usb_bootstrap_to_rom(
                descriptor,
                artifacts,
                self.VERSION,
                timeout_s=12,
            )
        wait_rom.assert_called_once()
        badge.write_line.assert_called_once_with("FOF_BOOTLOADER")

    def test_rom_census_requires_stable_ports_and_zero_rom_devices(
        self,
    ) -> None:
        other = "/dev/cu.other"
        ports = [self.PORT, other]
        silent_probe = mock.Mock(
            side_effect=[None] * (
                len(ports) * flash.ROM_CENSUS_ABSENCE_PROOFS
            )
        )
        with mock.patch.object(
            flash, "list_usb_ports", side_effect=[ports, ports]
        ), mock.patch.object(
            flash, "probe_rom_device", silent_probe
        ):
            self.assertEqual(
                flash.require_no_rom_devices(self.PORT, timeout_s=3),
                tuple(sorted(ports)),
            )
        expected_ports = [
            port
            for port in sorted(ports)
            for _proof in range(flash.ROM_CENSUS_ABSENCE_PROOFS)
        ]
        self.assertEqual(
            [call.args[0] for call in silent_probe.call_args_list],
            expected_ports,
        )
        self.assertTrue(all(
            call.args[1] > 0 for call in silent_probe.call_args_list
        ))

        with mock.patch.object(
            flash, "list_usb_ports", side_effect=[ports, ports]
        ), mock.patch.object(
            flash, "probe_rom_device",
            side_effect=[
                None, self._device(),
                None, None, None,
            ],
        ), self.assertRaisesRegex(
            flash.FlashError, "pre-existing ROM"
        ):
            flash.require_no_rom_devices(self.PORT, timeout_s=3)

        with mock.patch.object(
            flash, "list_usb_ports",
            side_effect=[ports, [self.PORT]],
        ), mock.patch.object(
            flash, "probe_rom_device",
            side_effect=[None] * (
                len(ports) * flash.ROM_CENSUS_ABSENCE_PROOFS
            ),
        ), self.assertRaisesRegex(
            flash.FlashError, "changed"
        ):
            flash.require_no_rom_devices(self.PORT, timeout_s=3)


class GuardedUsbRecoveryFlowTests(unittest.TestCase):
    VERSION = "0.64.76-badge-defcon34"
    OLD_VERSION = "0.64.75-badge-defcon34"
    MAC = "e0:72:a1:f9:47:fc"
    PORT = "/dev/cu.initial"

    def setUp(self) -> None:
        self.frozen_usb_artifacts = _test_frozen_usb_artifacts(self.VERSION)
        artifact_patcher = mock.patch.object(
            flash,
            "_prepare_frozen_usb_firmware_artifacts",
            return_value=self.frozen_usb_artifacts,
        )
        artifact_patcher.start()
        self.addCleanup(artifact_patcher.stop)
        attestation_patcher = mock.patch.object(
            flash,
            "_attest_frozen_uplink_flash_authority",
        )
        attestation_patcher.start()
        self.addCleanup(attestation_patcher.stop)

        def selected_descriptor(
            *,
            selected_port: str | None,
            operator_acknowledged: bool,
            trusted_binding=None,
        ):
            del operator_acknowledged, trusted_binding
            port = selected_port or flash.detect_usb_port()
            record = _usb_record(port, self.MAC)
            return record, flash.TrustedUplinkBinding(
                serial_number=record.serial_number,
                location=None,
                source="operator-selection",
            )

        patcher = mock.patch.object(
            flash,
            "select_trusted_uplink_descriptor",
            side_effect=selected_descriptor,
        )
        patcher.start()
        self.addCleanup(patcher.stop)

    @classmethod
    def _args(cls, *, dry_run: bool = False,
              port: str | None = PORT) -> SimpleNamespace:
        return SimpleNamespace(
            port=port,
            platform="badge-trio-xiao-s3",
            dry_run=dry_run,
            skip_command_probe=False,
            recovery_rewrite_same_version=False,
        )

    @classmethod
    def _device(cls, port: str = PORT) -> flash.RomDeviceIdentity:
        return flash.RomDeviceIdentity(
            base_mac=cls.MAC, port=port, chip="ESP32-S3",
            revision="v0.2", flash_size="8MB", psram_size="8MB",
        )

    @classmethod
    def _full_status(
        cls, version: str | None = None, *, partition: str = "ota_0",
        responses: int = 20, pending: bool = False,
        slots: tuple[str, ...] = (), scanner_version: str | None = None,
    ) -> dict[str, object]:
        status: dict[str, object] = _uplink_status(
            version or cls.VERSION,
            hardware_id=cls.MAC,
            partition=partition,
            responses=responses,
            pending_verify=pending,
        )
        if slots:
            scanners = [
                _scanner_status(
                    flash.PLATFORMS["badge-trio-xiao-s3"],
                    scanner_version or cls.VERSION,
                    slot=slot,
                )["scanners"][0]
                for slot in slots
            ]
            status.update({
                "safe_mode": False,
                "usb_control_alive": True,
                "scanner_uart_alive": True,
                "scanners": scanners,
            })
        return status

    @classmethod
    def _post_evidence(
        cls, *, partition: str = "ota_0", responses: int = 20,
    ) -> flash.PostUplinkApplicationEvidence:
        return flash.verify_post_uplink_application(
            cls._full_status(partition=partition, responses=responses),
            expected_hardware_id=cls.MAC,
            expected_version=cls.VERSION,
            expected_partition=partition,
        )

    @classmethod
    def _rom_stage(
        cls, port: str = PORT,
    ) -> flash.RomFlashStageEvidence:
        return _bound_rom_stage(
            _usb_record(port, cls.MAC),
            _frozen_uplink_set(cls.VERSION),
            cls.VERSION,
        )

    def test_post_uplink_budget_exceeds_firmware_health_window(self) -> None:
        self.assertGreater(
            flash.POST_UPLINK_APPLICATION_TIMEOUT_S,
            60,
        )
        self.assertGreaterEqual(
            flash.POST_UPLINK_TRANSITION_POLL_S,
            flash.APPLICATION_DISCOVERY_PROBE_SLICE_S,
        )

    def test_production_flow_rejects_rebound_location_drift_before_open(
        self,
    ) -> None:
        initial = _usb_record(self.PORT, self.MAC)
        status = self._full_status(responses=10)
        post = self._post_evidence(responses=20)
        moved = _usb_record(
            "/dev/cu.moved",
            self.MAC,
            location="moved-location",
        )
        with mock.patch.object(
            flash, "probe_application", return_value=status
        ), mock.patch.object(
            flash,
            "wait_for_post_uplink_application",
            return_value=(moved, post),
        ), mock.patch.object(
            flash,
            "BadgeSerial",
            side_effect=AssertionError("moved descriptor reached serial open"),
        ), self.assertRaisesRegex(flash.FlashError, "location"):
            flash.usb_flow(
                self._args(),
                flash.PLATFORMS["badge-trio-xiao-s3"],
                False,
                [],
                self.VERSION,
            )
        self.assertNotEqual(initial.location, moved.location)

    def test_required_rom_recovery_rejects_running_application_before_mutation(
        self,
    ) -> None:
        args = self._args()
        args.require_rom_recovery = True
        args.recovery_rewrite_same_version = True
        args.bind_selected_uplink = False
        args.trusted_uplink_binding = flash.TrustedUplinkBinding(
            serial_number=self.MAC,
            location=None,
            source="retained-session",
        )
        with mock.patch.object(
            flash,
            "probe_application",
            return_value=self._full_status(responses=10),
        ), mock.patch.object(
            flash,
            "BadgeSerial",
            side_effect=flash.FlashError("application mutation attempted"),
        ) as badge_serial, self.assertRaisesRegex(
            flash.FlashError,
            "ROM recovery requires the selected uplink to be in ROM",
        ):
            flash.usb_flow(
                args,
                flash.PLATFORMS["badge-trio-xiao-s3"],
                True,
                ["ble", "wifi"],
                self.VERSION,
            )
        badge_serial.assert_not_called()

    def test_required_rom_recovery_refuses_operator_binding_before_device(
        self,
    ) -> None:
        args = self._args()
        args.require_rom_recovery = True
        args.recovery_rewrite_same_version = True
        args.bind_selected_uplink = True
        args.trusted_uplink_binding = None
        with mock.patch.object(
            flash,
            "select_trusted_uplink_descriptor",
            side_effect=flash.FlashError("descriptor binding attempted"),
        ) as select_descriptor, self.assertRaisesRegex(
            flash.FlashError,
            "retained-session binding",
        ):
            flash.usb_flow(
                args,
                flash.PLATFORMS["badge-trio-xiao-s3"],
                True,
                ["ble", "wifi"],
                self.VERSION,
            )
        select_descriptor.assert_not_called()

    def test_dry_run_returns_before_every_device_or_artifact_api(self) -> None:
        args = self._args(dry_run=True, port=None)
        forbidden = (
            "detect_usb_port", "list_usb_ports", "probe_rom_device",
            "probe_application", "validate_current_uplink_rom_layout",
            "flash_complete_uplink_layout", "wait_for_rom_device",
            "wait_for_application_port", "wait_for_post_uplink_application",
            "wait_for_scanner_status_usb", "wait_for_scanners_usb",
        )
        patches = [
            mock.patch.object(
                flash, name, create=True,
                side_effect=AssertionError(f"dry-run called {name}"),
            )
            for name in forbidden
        ]
        with contextlib.ExitStack() as stack:
            for patcher in patches:
                stack.enter_context(patcher)
            stack.enter_context(mock.patch.object(
                flash, "BadgeSerial",
                side_effect=AssertionError("dry-run opened BadgeSerial"),
            ))
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                flash.usb_flow(
                    args, flash.PLATFORMS["badge-trio-xiao-s3"],
                    True, ["ble", "wifi"], self.VERSION,
                )
        self.assertIn("dry", output.getvalue().lower())

    def test_ambiguous_auto_ports_fail_before_probe_or_mutation(self) -> None:
        args = self._args(port=None)
        with mock.patch.object(
            flash, "detect_usb_port",
            side_effect=flash.FlashError("multiple USB serial ports"),
        ), mock.patch.object(
            flash, "probe_rom_device"
        ) as probe, mock.patch.object(
            flash, "flash_complete_uplink_layout"
        ) as mutate, self.assertRaisesRegex(flash.FlashError, "multiple"):
            flash.usb_flow(
                args, flash.PLATFORMS["badge-trio-xiao-s3"],
                True, [], self.VERSION,
            )
        probe.assert_not_called()
        mutate.assert_not_called()

    def test_explicit_legacy_bridge_uses_frozen_bound_rom_path(
        self,
    ) -> None:
        args = self._args()
        args.legacy_usb_bootstrap = True
        stage = self._rom_stage("/dev/cu.rom")
        post = self._post_evidence()
        preflight_status = self._full_status(
            responses=21, slots=("ble", "wifi")
        )
        final_status = self._full_status(
            responses=22, slots=("ble", "wifi")
        )
        preflight_status["scanners"][1]["hardware_id"] = \
            "E0:72:A1:F9:48:59"
        final_status["scanners"][1]["hardware_id"] = \
            "E0:72:A1:F9:48:59"
        stage_receipt = {
            "ok": True,
            "generation": 1,
            "slot_mask": 3,
            "version": self.VERSION,
        }
        events: list[tuple[object, ...]] = []

        class FakeBadge:
            def __init__(self, *_args, **_kwargs):
                events.append(("open_final",))
            def __enter__(self): return self
            def __exit__(self, *_args): return None
            def status(self): return final_status
            def stage_scanner_firmware(self, *_args):
                return stage_receipt

        with mock.patch.object(
            flash, "probe_application",
            side_effect=AssertionError("legacy bridge probed strict app"),
        ), mock.patch.object(
            flash, "validate_current_uplink_rom_layout",
            side_effect=AssertionError("legacy bridge reopened build paths"),
        ), mock.patch.object(
            flash, "legacy_usb_bootstrap_to_rom",
            side_effect=lambda descriptor, artifacts, version, timeout_s: (
                events.append((
                    "bridge",
                    descriptor.device,
                    artifacts,
                    version,
                    timeout_s,
                )) or stage
            ),
        ), mock.patch.object(
            flash, "flash_complete_uplink_layout",
            side_effect=AssertionError(
                "flow must not open a second ROM session after legacy bridge"
            ),
        ), mock.patch.object(
            flash, "wait_for_post_uplink_application",
            return_value=(_usb_record("/dev/cu.app"), post),
        ), mock.patch.object(
            flash, "wait_for_scanner_status_usb",
            return_value=preflight_status,
        ), mock.patch.object(
            flash, "wait_for_scanners_usb",
        ), mock.patch.object(
            flash, "coordinator_newer_skipped_slots",
            return_value=set(),
        ), mock.patch.object(
            flash, "verify_scanners",
        ), mock.patch.object(
            flash, "verify_auto_update_convergence",
        ), mock.patch.object(
            flash, "BadgeSerial", FakeBadge,
        ), mock.patch.object(
            flash, "_prove_reversible_usb_theme_control",
            side_effect=_complete_mocked_theme_control,
        ):
            flash.usb_flow(
                args, flash.PLATFORMS["badge-trio-xiao-s3"],
                True, ["ble", "wifi"], self.VERSION,
            )

        self.assertEqual(events[0][0:2], ("bridge", self.PORT))
        self.assertIs(events[0][2], self.frozen_usb_artifacts.uplink)
        self.assertEqual(events[0][3:], (self.VERSION, 30))

    def test_legacy_bootstrap_application_proof_failure_never_starts_rom_flash(
        self,
    ) -> None:
        args = self._args()
        args.legacy_usb_bootstrap = True
        failure = flash.FlashError("legacy application identity unavailable")
        with mock.patch.object(
            flash,
            "legacy_usb_bootstrap_to_rom",
            side_effect=failure,
        ) as bridge, mock.patch.object(
            flash, "flash_complete_uplink_layout"
        ) as mutate, self.assertRaisesRegex(
            flash.FlashError, "identity unavailable"
        ):
            flash.usb_flow(
                args,
                flash.PLATFORMS["badge-trio-xiao-s3"],
                True,
                ["ble", "wifi"],
                self.VERSION,
            )
        bridge.assert_called_once_with(
            _usb_record(self.PORT, self.MAC),
            self.frozen_usb_artifacts.uplink,
            self.VERSION,
            timeout_s=30,
        )
        mutate.assert_not_called()

    def test_legacy_bridge_requires_exact_full_badge_target(self) -> None:
        for port, need_uplink, slots, message in (
            (None, True, ["ble", "wifi"], "explicit.*port"),
            (self.PORT, False, ["ble", "wifi"], "uplink"),
            (self.PORT, True, [], "both scanner"),
            (self.PORT, True, ["ble"], "both scanner"),
        ):
            args = self._args(port=port)
            args.legacy_usb_bootstrap = True
            with self.subTest(
                port=port, need_uplink=need_uplink, slots=slots
            ), \
                 mock.patch.object(flash, "detect_usb_port") as detect, \
                 mock.patch.object(flash, "probe_rom_device") as probe, \
                 self.assertRaisesRegex(flash.FlashError, message):
                flash.usb_flow(
                    args, flash.PLATFORMS["badge-trio-xiao-s3"],
                    need_uplink, slots, self.VERSION,
                )
            detect.assert_not_called()
            probe.assert_not_called()

    def test_legacy_bridge_rejects_recovery_overrides(self) -> None:
        for field in (
            "recovery_rewrite_same_version", "skip_command_probe",
        ):
            args = self._args()
            args.legacy_usb_bootstrap = True
            setattr(args, field, True)
            with self.subTest(field=field), mock.patch.object(
                flash, "probe_rom_device"
            ) as probe, self.assertRaisesRegex(
                flash.FlashError, "override|recovery"
            ):
                flash.usb_flow(
                    args, flash.PLATFORMS["badge-trio-xiao-s3"],
                    True, ["ble", "wifi"], self.VERSION,
                )
            probe.assert_not_called()

    def test_scanner_only_silence_refuses_without_any_rom_probe(self) -> None:
        args = self._args()
        with mock.patch.object(flash, "detect_usb_port") as detect, \
             mock.patch.object(
                 flash, "probe_rom_device", return_value=None
             ) as rom_probe, mock.patch.object(
                 flash, "probe_application", return_value=None
             ) as app_probe, mock.patch.object(
                 flash, "wait_for_rom_device"
             ) as wait_rom, mock.patch.object(
                 flash, "flash_complete_uplink_layout"
             ) as mutate, self.assertRaisesRegex(
                 flash.FlashError, "scanner-only|silent"
             ):
            flash.usb_flow(
                args, flash.PLATFORMS["badge-trio-xiao-s3"],
                False, ["ble"], self.VERSION,
            )
        detect.assert_not_called()
        rom_probe.assert_not_called()
        app_probe.assert_called_once_with(
            _usb_record(self.PORT, self.MAC), 5
        )
        wait_rom.assert_not_called()
        mutate.assert_not_called()

    def test_silent_already_rom_flashes_but_scanner_only_refuses(
        self,
    ) -> None:
        layout = SimpleNamespace(version=self.VERSION)
        stage = self._rom_stage()
        post = self._post_evidence()
        for need_uplink in (True, False):
            final_status = self._full_status(responses=21)
            final_evidence = self._post_evidence(responses=22)

            class FakeBadge:
                def __init__(self, *_args, **_kwargs): pass
                def __enter__(self): return self
                def __exit__(self, *_args): return None
                def status(self): return final_status

            app_probe = mock.Mock(return_value=None)
            rom_probe = mock.Mock(return_value=self._device())
            rom_absence = mock.Mock()
            wait_rom = mock.Mock(return_value=self._device())
            native_reset = mock.Mock(side_effect=AssertionError(
                "silent application must not trigger native USB reset"
            ))
            mutate = mock.Mock(return_value=stage)
            wait_post = mock.Mock(return_value=(_usb_record("/dev/cu.rebound"), post))
            output = io.StringIO()
            contexts = (
                mock.patch.object(
                    flash, "probe_rom_device", new=rom_probe
                ),
                mock.patch.object(flash, "probe_application", app_probe),
                mock.patch.object(
                    flash, "usb_port_hardware_id", return_value=self.MAC
                ),
                mock.patch.object(
                    flash, "require_selected_port_not_rom",
                    create=True,
                    new=rom_absence,
                ),
                mock.patch.object(
                    flash, "wait_for_rom_device", new=wait_rom
                ),
                mock.patch.object(
                    flash, "reset_uplink_usb_to_rom", new=native_reset
                ),
                mock.patch.object(
                    flash, "validate_current_uplink_rom_layout",
                    return_value=layout,
                ),
                mock.patch.object(
                    flash, "flash_complete_uplink_layout", mutate
                ),
                mock.patch.object(
                    flash, "wait_for_post_uplink_application",
                    create=True, new=wait_post,
                ),
                mock.patch.object(flash, "BadgeSerial", FakeBadge),
                mock.patch.object(
                    flash, "_prove_reversible_usb_theme_control",
                    return_value=final_evidence,
                ),
            )
            with self.subTest(need_uplink=need_uplink), \
                 contextlib.ExitStack() as stack:
                for context in contexts:
                    stack.enter_context(context)
                with contextlib.redirect_stdout(output):
                    if need_uplink:
                        flash.usb_flow(
                            self._args(),
                            flash.PLATFORMS["badge-trio-xiao-s3"],
                            True, [], self.VERSION,
                        )
                    else:
                        with self.assertRaises(flash.FlashError):
                            flash.usb_flow(
                                self._args(),
                                flash.PLATFORMS["badge-trio-xiao-s3"],
                                False, ["ble"], self.VERSION,
                            )
            if need_uplink:
                self.assertEqual(
                    output.getvalue().count(flash.ROM_ENTRY_PROMPT), 0
                )
                app_probe.assert_called_once_with(
                    _usb_record(self.PORT, self.MAC), 5
                )
                rom_probe.assert_not_called()
                rom_absence.assert_not_called()
                wait_rom.assert_not_called()
                mutate.assert_called_once_with(
                    _usb_record(self.PORT, self.MAC),
                    self.frozen_usb_artifacts.uplink,
                    self.VERSION,
                )
                native_reset.assert_not_called()
                wait_post.assert_called_once()
            else:
                app_probe.assert_called_once_with(
                    _usb_record(self.PORT, self.MAC), 5
                )
                rom_probe.assert_not_called()
                rom_absence.assert_not_called()
                wait_rom.assert_not_called()
                native_reset.assert_not_called()
                mutate.assert_not_called()
                wait_post.assert_not_called()

    def test_total_silence_requires_the_physical_chord_without_native_reset(
        self,
    ) -> None:
        stage = self._rom_stage(self.PORT)
        post = self._post_evidence()
        final_status = self._full_status(responses=21)
        final_evidence = self._post_evidence(responses=22)
        events: list[tuple[str, object]] = []
        rebound = _usb_record(
            "/dev/cu.rebound",
            self.MAC,
            location=_usb_record(self.PORT, self.MAC).location,
        )
        mutation_calls = 0

        def mutate(descriptor, artifacts, version):
            nonlocal mutation_calls
            mutation_calls += 1
            events.append((
                "flash",
                (descriptor, artifacts, version),
            ))
            if mutation_calls == 1:
                raise flash.BoundRomUnavailableError("not in ROM")
            return stage

        class FakeBadge:
            def __init__(self, *_args, **_kwargs): pass
            def __enter__(self): return self
            def __exit__(self, *_args): return None
            def status(self): return final_status

        with mock.patch.object(
            flash, "probe_application", return_value=None
        ), mock.patch.object(
            flash,
            "_take_badge_usb_descriptor_census",
            return_value=(rebound,),
        ), mock.patch.object(
            flash, "reset_uplink_usb_to_rom",
            side_effect=AssertionError(
                "silent application must not trigger a native USB reset"
            ),
        ), mock.patch.object(
            flash, "flash_complete_uplink_layout",
            side_effect=mutate,
        ), mock.patch.object(
            flash, "wait_for_post_uplink_application", create=True,
            return_value=(_usb_record("/dev/cu.app"), post),
        ), mock.patch.object(
            flash, "BadgeSerial", FakeBadge,
        ), mock.patch.object(
            flash, "_prove_reversible_usb_theme_control",
            return_value=final_evidence,
        ), mock.patch.object(
            flash, "log", side_effect=lambda message: events.append(
                ("log", message)
            ),
        ):
            flash.usb_flow(
                self._args(), flash.PLATFORMS["badge-trio-xiao-s3"],
                True, [], self.VERSION,
            )
        operational_events = [event for event in events if event[0] != "log"]
        self.assertEqual(
            operational_events,
            [
                (
                    "flash",
                    (
                        _usb_record(self.PORT, self.MAC),
                        self.frozen_usb_artifacts.uplink,
                        self.VERSION,
                    ),
                ),
                (
                    "flash",
                    (
                        rebound,
                        self.frozen_usb_artifacts.uplink,
                        self.VERSION,
                    ),
                ),
            ],
        )
        prompt = ("log", flash.ROM_ENTRY_PROMPT)
        self.assertEqual(events.count(prompt), 1)
        self.assertLess(
            events.index(operational_events[0]),
            events.index(prompt),
        )
        self.assertLess(
            events.index(prompt),
            events.index(operational_events[1]),
        )

    def obsolete_silent_selected_port_already_in_rom_is_never_flashed(
        self,
    ) -> None:
        with mock.patch.object(
            flash, "probe_application", return_value=None
        ), mock.patch.object(
            flash, "usb_port_hardware_id", return_value=self.MAC
        ), mock.patch.object(
            flash, "probe_rom_device", return_value=self._device()
        ) as rom_probe, mock.patch.object(
            flash, "wait_for_rom_device",
            side_effect=AssertionError(
                "a pre-existing ROM must not enter discovery"
            ),
        ) as wait_rom, mock.patch.object(
            flash, "validate_current_uplink_rom_layout"
        ) as layout, mock.patch.object(
            flash, "flash_complete_uplink_layout"
        ) as mutate, self.assertRaisesRegex(
            flash.FlashError, "pre-existing|already.*ROM"
        ):
            flash.usb_flow(
                self._args(),
                flash.PLATFORMS["badge-trio-xiao-s3"],
                True,
                [],
                self.VERSION,
            )
        rom_probe.assert_called_once()
        self.assertEqual(rom_probe.call_args.args[0], self.PORT)
        self.assertGreater(rom_probe.call_args.args[1], 0)
        self.assertLessEqual(
            rom_probe.call_args.args[1],
            flash.SELECTED_ROM_ABSENCE_TIMEOUT_S,
        )
        wait_rom.assert_not_called()
        layout.assert_not_called()
        mutate.assert_not_called()

    def test_incomplete_application_activity_never_enters_rom_or_mutates(
        self,
    ) -> None:
        incomplete = flash.SerialReadTimeout(
            "incomplete application frame",
            saw_activity=True,
            partial_frame=True,
        )
        with mock.patch.object(
            flash, "probe_rom_device",
            side_effect=AssertionError(
                "full-badge recovery probed ROM before the application"
            ),
        ), mock.patch.object(
            flash, "probe_application", side_effect=incomplete
        ), mock.patch.object(
            flash, "wait_for_rom_device"
        ) as wait_rom, mock.patch.object(
            flash, "validate_current_uplink_rom_layout"
        ) as layout, mock.patch.object(
            flash, "reset_uplink_usb_to_rom"
        ) as native_reset, mock.patch.object(
            flash, "flash_complete_uplink_layout"
        ) as mutate, self.assertRaises(
            flash.SerialReadTimeout
        ) as caught:
            flash.usb_flow(
                self._args(), flash.PLATFORMS["badge-trio-xiao-s3"],
                True, [], self.VERSION,
            )
        self.assertIs(caught.exception, incomplete)
        wait_rom.assert_not_called()
        layout.assert_not_called()
        native_reset.assert_not_called()
        mutate.assert_not_called()

    def test_noisy_application_and_rom_uncertainty_never_fallback(self) -> None:
        noisy = flash.SerialTransportError("partial application frame")
        with mock.patch.object(
            flash, "probe_rom_device", return_value=None
        ), mock.patch.object(
            flash, "probe_application", side_effect=noisy
        ), mock.patch.object(
            flash, "validate_current_uplink_rom_layout"
        ) as layout, mock.patch.object(
            flash, "wait_for_rom_device"
        ) as wait_rom, self.assertRaisesRegex(
            flash.SerialTransportError, "partial"
        ):
            flash.usb_flow(
                self._args(), flash.PLATFORMS["badge-trio-xiao-s3"],
                True, [], self.VERSION,
            )
        layout.assert_not_called()
        wait_rom.assert_not_called()

        uncertain = flash.RomFlashUncertainError("mutation uncertain")
        with mock.patch.object(
            flash, "probe_rom_device", return_value=None
        ), mock.patch.object(
            flash, "probe_application", return_value=None
        ), mock.patch.object(
            flash, "usb_port_hardware_id", return_value=self.MAC
        ), mock.patch.object(
            flash, "wait_for_rom_device", return_value=self._device()
        ), mock.patch.object(
            flash, "reset_uplink_usb_to_rom",
            side_effect=AssertionError(
                "physical ROM fallback must not use native USB reset"
            ),
        ), mock.patch.object(
            flash, "validate_current_uplink_rom_layout",
            return_value=SimpleNamespace(version=self.VERSION),
        ), mock.patch.object(
            flash, "flash_complete_uplink_layout", side_effect=uncertain
        ), mock.patch.object(
            flash, "wait_for_post_uplink_application", create=True
        ) as wait_post, self.assertRaises(
            flash.RomFlashUncertainError
        ) as caught:
            flash.usb_flow(
                self._args(), flash.PLATFORMS["badge-trio-xiao-s3"],
                True, ["ble"], self.VERSION,
            )
        self.assertIs(caught.exception, uncertain)
        wait_post.assert_not_called()

    def test_healthy_application_uses_application_ota_once_never_rom(self) -> None:
        baseline = self._full_status(
            self.OLD_VERSION, partition="ota_0", responses=10
        )
        final_status = self._full_status(
            partition="ota_1", responses=21
        )
        final_evidence = self._post_evidence(
            partition="ota_1", responses=22
        )
        expected_size = len(
            flash._frozen_firmware_bytes(
                self.frozen_usb_artifacts.uplink,
                role="uplink",
            )
        )
        receipt = _uplink_receipt(
            "committed",
            partition="ota_1",
            received=expected_size,
            total=expected_size,
            reboot_required=True,
        )
        calls: list[tuple[object, ...]] = []

        class FakeBadge:
            def __init__(
                self, descriptor, dry_run, expected_hardware_id=None
            ):
                calls.append((
                    "open", descriptor.device, dry_run,
                    expected_hardware_id,
                ))
            def __enter__(self): return self
            def __exit__(self, *_args): return None
            def upload_uplink_firmware(
                self, platform, artifacts, version, recovery
            ):
                calls.append((
                    "upload", platform, artifacts, version, recovery
                ))
                return receipt
            def status(self): return final_status

        with mock.patch.object(
            flash, "probe_rom_device", return_value=None
        ), mock.patch.object(
            flash, "probe_application", return_value=baseline
        ), mock.patch.object(
            flash, "BadgeSerial", FakeBadge
        ), mock.patch.object(
            flash, "validate_current_uplink_rom_layout"
        ) as layout, mock.patch.object(
            flash, "flash_complete_uplink_layout"
        ) as rom_flash, mock.patch.object(
            flash, "wait_for_post_uplink_application", create=True,
            return_value=(
                _usb_record("/dev/cu.renamed", self.MAC),
                self._post_evidence(partition="ota_1"),
            ),
        ) as wait_post, mock.patch.object(
            flash, "_prove_reversible_usb_theme_control",
            return_value=final_evidence,
        ):
            flash.usb_flow(
                self._args(), flash.PLATFORMS["badge-trio-xiao-s3"],
                True, [], self.VERSION,
            )
        self.assertEqual(calls[0], ("open", self.PORT, False, self.MAC))
        self.assertEqual(calls[1][0], "upload")
        self.assertIs(calls[1][2], self.frozen_usb_artifacts.uplink)
        self.assertEqual(calls[1][3:], (self.VERSION, False))
        self.assertEqual(
            calls[2], ("open", "/dev/cu.renamed", False, self.MAC)
        )
        self.assertEqual(len(calls), 3)
        self.assertEqual(
            wait_post.call_args.kwargs["timeout_s"],
            flash.POST_UPLINK_APPLICATION_TIMEOUT_S,
        )
        layout.assert_not_called()
        rom_flash.assert_not_called()

    def test_receipt_classifier_accepts_only_three_exact_shapes(self) -> None:
        current = self._full_status(partition="ota_0")
        old = self._full_status(
            self.OLD_VERSION, partition="ota_0", responses=10
        )
        skipped = {
            "ok": True, "skipped": True, "phase": "current",
            "hardware_id": self.MAC, "version": self.VERSION,
            "partition": "ota_0",
        }
        committed = _uplink_receipt(
            "committed", partition="ota_1", received=4096, total=4096,
            reboot_required=True,
        )
        terminal = {
            "ok": False, "uncertain": True,
            "phase": "terminal_unavailable",
            "expected_partition": "ota_1",
            "hardware_id": self.MAC, "version": self.VERSION,
            "received": 4096, "total": 4096,
            "error": "terminal reply missing",
        }
        cases = (
            (skipped, current, False, "ota_0", "current"),
            (committed, old, True, "ota_1", "committed"),
            (terminal, old, True, "ota_1", "terminal_unavailable"),
        )
        for receipt, pre, mutated, partition, source in cases:
            with self.subTest(source=source):
                expectation = flash._classify_uplink_update_receipt(
                    receipt,
                    pre_status=pre,
                    target_version=self.VERSION,
                    expected_sha256="a" * 64,
                    expected_size=4096,
                    update_session="0123456789ABCDEF",
                )
                self.assertTrue(
                    flash._PostUplinkExpectation.__dataclass_params__.frozen
                )
                self.assertEqual(expectation.expected_hardware_id, self.MAC)
                self.assertEqual(expectation.expected_version, self.VERSION)
                self.assertEqual(expectation.expected_partition, partition)
                self.assertIs(expectation.mutation_expected, mutated)
                self.assertEqual(expectation.source, source)

        malformed = []
        for receipt in (skipped, committed, terminal):
            extra = dict(receipt)
            extra["extra"] = True
            malformed.append(extra)
        for receipt in (skipped, terminal):
            noncanonical_mac = dict(receipt)
            noncanonical_mac["hardware_id"] = self.MAC.upper()
            malformed.append(noncanonical_mac)
        wrong_commit = dict(committed)
        wrong_commit["credit_bytes"] = 1
        malformed.append(wrong_commit)
        wrong_terminal = dict(terminal)
        wrong_terminal["received"] = 4095
        malformed.append(wrong_terminal)
        for receipt in malformed:
            with self.subTest(receipt=receipt), self.assertRaises(
                flash.FlashError
            ):
                flash._classify_uplink_update_receipt(
                    receipt,
                    pre_status=old,
                    target_version=self.VERSION,
                    expected_sha256="a" * 64,
                    expected_size=4096,
                    update_session="0123456789ABCDEF",
                )

    def test_rom_stage_evidence_is_not_application_proof(self) -> None:
        stage = self._rom_stage()
        expectation = flash._expectation_from_rom_flash(
            stage,
            layout_version=self.VERSION,
            artifacts=self.frozen_usb_artifacts.uplink,
            update_session="0123456789ABCDEF",
        )
        self.assertEqual(expectation.expected_partition, "ota_0")
        self.assertIsNone(expectation.pre_version)
        self.assertIs(stage.application_health_verified, False)
        self.assertIs(stage.rollback_cleared, False)
        with self.assertRaises(flash.FlashError):
            flash._revalidate_post_uplink_application_evidence(stage)

    def test_wait_for_post_application_follows_mac_and_safe_transitions(self) -> None:
        expectation = flash._PostUplinkExpectation(
            expected_hardware_id=self.MAC,
            expected_version=self.VERSION,
            expected_partition="ota_1",
            expected_sha256="a" * 64,
            expected_size=4096,
            pre_version=self.OLD_VERSION,
            pre_partition="ota_0",
            mutation_expected=True,
            source="committed",
            update_session="0123456789ABCDEF",
        )
        statuses = [
            self._full_status(self.OLD_VERSION, partition="ota_0", responses=10),
            self._full_status(
                self.VERSION, partition="ota_1", responses=11, pending=True
            ),
            self._full_status(self.VERSION, partition="ota_1", responses=12),
        ]
        ports = ["/dev/cu.old", "/dev/cu.mid", "/dev/cu.renamed"]
        calls: list[tuple[str, float]] = []

        def wait(expected_mac: str, timeout_s: float):
            calls.append((expected_mac, timeout_s))
            return _usb_record(ports.pop(0), self.MAC), statuses.pop(0)

        with mock.patch.object(
            flash, "wait_for_application_port", side_effect=wait
        ), mock.patch.object(
            flash, "probe_application", return_value=None
        ) as bound_probe, mock.patch.object(
            flash.time, "monotonic", return_value=0.0
        ), mock.patch.object(flash.time, "sleep") as paced_sleep:
            descriptor, evidence = flash.wait_for_post_uplink_application(
                expectation, timeout_s=60
            )
        self.assertEqual(descriptor.device, "/dev/cu.renamed")
        self.assertEqual(evidence.running_partition, "ota_1")
        self.assertEqual(len(calls), 3)
        self.assertTrue(all(call[0] == self.MAC for call in calls))
        self.assertEqual(bound_probe.call_count, 2)
        self.assertEqual(paced_sleep.call_count, 4)
        self.assertTrue(all(
            0 < call.args[0] <= flash.POST_UPLINK_TRANSITION_POLL_S
            for call in paced_sleep.call_args_list
        ))

    def test_wait_for_post_reuses_proven_port_while_rollback_clears(self) -> None:
        expectation = flash._PostUplinkExpectation(
            expected_hardware_id=self.MAC,
            expected_version=self.VERSION,
            expected_partition="ota_1",
            expected_sha256="a" * 64,
            expected_size=4096,
            pre_version=self.OLD_VERSION,
            pre_partition="ota_0",
            mutation_expected=True,
            source="committed",
            update_session="0123456789ABCDEF",
        )
        pending = self._full_status(
            self.VERSION, partition="ota_1", responses=11, pending=True
        )
        final = self._full_status(
            self.VERSION, partition="ota_1", responses=12
        )
        discovery_calls = 0

        def discover(_expected_mac: str, _timeout_s: float):
            nonlocal discovery_calls
            discovery_calls += 1
            if discovery_calls > 1:
                raise AssertionError(
                    "rollback polling rescanned unrelated USB ports"
                )
            return _usb_record("/dev/cu.uplink", self.MAC), pending

        with mock.patch.object(
            flash, "wait_for_application_port", side_effect=discover
        ), mock.patch.object(
            flash, "probe_application", return_value=final
        ) as probe, mock.patch.object(
            flash.time, "monotonic", return_value=0.0
        ), mock.patch.object(flash.time, "sleep"):
            descriptor, evidence = flash.wait_for_post_uplink_application(
                expectation, timeout_s=60
            )

        self.assertEqual(descriptor.device, "/dev/cu.uplink")
        self.assertEqual(evidence.running_partition, "ota_1")
        self.assertEqual(discovery_calls, 1)
        probe.assert_called_once_with(
            _usb_record("/dev/cu.uplink", self.MAC),
            flash.APPLICATION_DISCOVERY_PROBE_SLICE_S,
        )

    def test_wait_for_post_application_hard_fails_wrong_state_and_deadline(self) -> None:
        expectation = flash._PostUplinkExpectation(
            expected_hardware_id=self.MAC,
            expected_version=self.VERSION,
            expected_partition="ota_1",
            expected_sha256="a" * 64,
            expected_size=4096,
            pre_version=self.OLD_VERSION,
            pre_partition="ota_0",
            mutation_expected=True,
            source="terminal_unavailable",
            update_session="0123456789ABCDEF",
        )
        for invalid_timeout in (float("nan"), float("inf"), 601, 0, -1, True):
            wait_invalid = mock.Mock(
                side_effect=AssertionError("invalid timeout reached USB")
            )
            with self.subTest(timeout=invalid_timeout), mock.patch.object(
                flash, "wait_for_application_port", wait_invalid
            ), self.assertRaises(flash.FlashError):
                flash.wait_for_post_uplink_application(
                    expectation, timeout_s=invalid_timeout
                )
            wait_invalid.assert_not_called()

        class NonExactPartition(str):
            pass

        forged_partition = dataclasses.replace(
            expectation, expected_partition=NonExactPartition("ota_1")
        )
        wait_forged = mock.Mock(
            side_effect=AssertionError("forged partition reached USB")
        )
        with mock.patch.object(
            flash, "wait_for_application_port", wait_forged
        ), self.assertRaises(flash.FlashError):
            flash.wait_for_post_uplink_application(forged_partition)
        wait_forged.assert_not_called()

        wrong = self._full_status(
            "0.64.74-badge-defcon34", partition="ota_1"
        )
        wait = mock.Mock(return_value=(
            _usb_record("/dev/cu.wrong", self.MAC), wrong
        ))
        with mock.patch.object(
            flash, "wait_for_application_port", wait
        ), mock.patch.object(
            flash.time, "monotonic", return_value=0.0
        ), self.assertRaises(flash.FlashError):
            flash.wait_for_post_uplink_application(expectation, timeout_s=60)
        wait.assert_called_once()

        clock = SimpleNamespace(now=0.0)
        target = self._full_status(self.VERSION, partition="ota_1")
        def late(_mac: str, _timeout: float):
            clock.now = 99.0
            return _usb_record("/dev/cu.late", self.MAC), target
        with mock.patch.object(
            flash, "wait_for_application_port", side_effect=late
        ), mock.patch.object(
            flash.time, "monotonic", side_effect=lambda: clock.now
        ), self.assertRaisesRegex(flash.FlashError, "timed out|deadline"):
            flash.wait_for_post_uplink_application(expectation, timeout_s=60)

    def test_scanner_reopen_requires_newer_uplink_proof_before_one_stage(self) -> None:
        args = self._args()
        initial = self._full_status(
            slots=("ble",), scanner_version=self.OLD_VERSION,
            responses=10,
        )
        post = self._post_evidence(responses=20)
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]

        for latest_count, should_stage in ((20, False), (21, True)):
            latest = self._full_status(
                slots=("ble",), scanner_version=self.OLD_VERSION,
                responses=latest_count,
            )
            final = self._full_status(
                slots=("ble",), scanner_version=self.VERSION,
                responses=latest_count + 1,
            )
            events: list[str] = []

            class FakeBadge:
                def __init__(
                    self, descriptor, dry_run,
                    expected_hardware_id=None,
                ):
                    events.append(
                        f"open:{descriptor.device}:{dry_run}:"
                        f"{expected_hardware_id}"
                    )
                def __enter__(self): return self
                def __exit__(self, *_args): return None
                def stage_scanner_firmware(self, *_args):
                    events.append("stage")
                    return {"generation": 1, "slot_mask": 1}
                def relay_scanner(self, *_args, **_kwargs):
                    events.append("relay")
                def status(self): return final

            with self.subTest(latest_count=latest_count), \
                 mock.patch.object(flash, "probe_rom_device", return_value=None), \
                 mock.patch.object(flash, "probe_application", return_value=initial), \
                 mock.patch.object(
                     flash, "wait_for_post_uplink_application", create=True,
                     return_value=(_usb_record("/dev/cu.rebound"), post),
                 ), mock.patch.object(flash, "BadgeSerial", FakeBadge), \
                 mock.patch.object(
                     flash, "wait_for_scanner_status_usb", return_value=latest
                 ), mock.patch.object(
                     flash, "wait_for_scanners_usb"
                 ) as final_wait, mock.patch.object(
                     flash, "verify_auto_update_convergence"
                 ), mock.patch.object(
                     flash, "_prove_reversible_usb_theme_control",
                     side_effect=_complete_mocked_theme_control,
                 ):
                if should_stage:
                    flash.usb_flow(
                        args, platform, False, ["ble"], self.VERSION
                    )
                else:
                    with self.assertRaises(flash.FlashError):
                        flash.usb_flow(
                            args, platform, False, ["ble"], self.VERSION
                        )
            self.assertEqual(events.count("stage"), 1 if should_stage else 0)
            self.assertEqual(events.count("relay"), 0)
            self.assertIn(f"open:/dev/cu.rebound:False:{self.MAC}", events)
            if should_stage:
                final_wait.assert_called_once()
            else:
                final_wait.assert_not_called()

    def test_legacy_uplink_helpers_are_deleted(self) -> None:
        for name in (
            "wait_for_port", "reset_uplink_from_bootloader",
            "flash_uplink_usb",
        ):
            self.assertFalse(hasattr(flash, name), name)
        flow_source = inspect.getsource(flash.usb_flow)
        for forbidden in (
            "wait_for_port", "reset_uplink_from_bootloader",
            "flash_uplink_usb", "find_pio", "run(",
        ):
            self.assertNotIn(forbidden, flow_source)


class UpdateMaintenanceTransactionTests(unittest.TestCase):
    VERSION = flash.UPDATE_MAINTENANCE_MIN_VERSION
    SESSION = "0123456789ABCDEF"
    MAC = "e0:72:a1:f9:47:fc"

    @classmethod
    def _normal_status(cls, *, responses: int) -> dict:
        status = _uplink_status(
            cls.VERSION,
            hardware_id=cls.MAC,
            responses=responses,
        )
        status.update({
            "game_seed": "immune",
            "game_state": "human",
            "game_active": False,
            "game_shield": 0,
        })
        return status

    @classmethod
    def _maintenance_status(cls, *, responses: int) -> dict:
        status = _update_maintenance_status(
            cls.VERSION,
            session=cls.SESSION,
            hardware_id=cls.MAC,
            responses=responses,
        )
        status.update({
            "game_seed": "immune",
            "game_state": "human",
            "game_active": False,
            "game_shield": 0,
        })
        return status

    @staticmethod
    def _args() -> SimpleNamespace:
        return SimpleNamespace(
            recovery_rewrite_same_version=False,
            skip_command_probe=False,
        )

    def test_post_rom_validator_rejects_before_maintenance_or_serial_bytes(
        self,
    ) -> None:
        descriptor = _usb_record("/dev/cu.rom", self.MAC)
        binding = flash.TrustedUplinkBinding(
            serial_number=self.MAC,
            location=descriptor.location,
            source="retained-session",
        )
        artifacts = _test_frozen_usb_artifacts(self.VERSION)
        stage = _bound_rom_stage(
            descriptor,
            artifacts.uplink,
            self.VERSION,
        )
        post_status = self._normal_status(responses=20)
        post_evidence = flash.verify_post_uplink_application(
            post_status,
            expected_hardware_id=self.MAC,
            expected_version=self.VERSION,
            expected_partition="ota_0",
        )
        observed: list[dict] = []

        class RejectPostRom(RuntimeError):
            pass

        def reject(status: dict) -> None:
            self.assertEqual(status, post_status)
            self.assertIsNot(status, post_status)
            observed.append(status)
            raise RejectPostRom("post-ROM memory gate rejected")

        maintenance_validator = mock.Mock(
            side_effect=AssertionError(
                "maintenance validator ran after rejected ROM proof"
            )
        )
        capture_state = mock.Mock(
            side_effect=AssertionError(
                "post-ROM state capture ran after rejected ROM proof"
            )
        )
        badge_serial = mock.Mock(
            side_effect=AssertionError(
                "rejected ROM proof reached serial commands or bytes"
            )
        )

        with mock.patch.object(
            flash,
            "_flash_silent_uplink_with_chord_fallback",
            return_value=stage,
        ), mock.patch.object(
            flash,
            "wait_for_post_uplink_application",
            return_value=(descriptor, post_evidence),
        ), mock.patch.object(
            flash,
            "probe_application",
            return_value=post_status,
        ), mock.patch.object(
            flash,
            "_capture_persisted_game_state",
            capture_state,
        ), mock.patch.object(
            flash,
            "BadgeSerial",
            badge_serial,
        ), self.assertRaisesRegex(
            RejectPostRom,
            "post-ROM memory gate rejected",
        ):
            flash._usb_update_maintenance_flow(
                self._args(),
                flash.PLATFORMS["badge-trio-xiao-s3"],
                True,
                ["ble", "wifi"],
                self.VERSION,
                initial_descriptor=descriptor,
                trusted_uplink_binding=binding,
                application_status=None,
                frozen_artifacts=artifacts,
                scanner_image_size=len(
                    flash._frozen_firmware_bytes(
                        artifacts.scanner,
                        role="scanner",
                    )
                ),
                legacy_bootstrap=False,
                _issue_scanner_flow_result=mock.Mock(),
                maintenance_status_validator=maintenance_validator,
                post_rom_bootstrap_status_validator=reject,
            )

        self.assertEqual(len(observed), 1)
        capture_state.assert_not_called()
        badge_serial.assert_not_called()
        maintenance_validator.assert_not_called()

    def test_supported_prepare_binds_session_before_uncertain_reconnect(
        self,
    ) -> None:
        badge = flash.BadgeSerial(
            _usb_record("/dev/fake", self.MAC),
            False,
            expected_hardware_id=self.MAC,
        )
        flush_uncertainty = flash.SerialTransportError(
            "prepare flush result lost",
            terminal_unavailable=True,
        )
        badge.write_line = mock.Mock(  # type: ignore[method-assign]
            side_effect=flush_uncertainty
        )
        badge._read_update_mode_or_control_error = mock.Mock(  # type: ignore[method-assign]
            side_effect=AssertionError(
                "flush uncertainty attempted to read a receipt"
            )
        )
        observed: list[tuple[str | None, str | None]] = []
        uncertainty = flash.SerialTransportError(
            "maintenance reconnect remained uncertain",
            terminal_unavailable=True,
        )

        def reconnect(
            *,
            deadline: float,
            maintenance_session: str | None,
        ) -> dict:
            self.assertGreater(deadline, flash.time.monotonic())
            observed.append((
                badge._update_session,
                maintenance_session,
            ))
            raise uncertainty

        badge._reconnect_same_uplink_mode = reconnect  # type: ignore[method-assign]
        with self.assertRaises(flash.SerialTransportError) as caught:
            badge.prepare_update_maintenance(
                self.SESSION,
                deadline=flash.time.monotonic() + 10,
                source_supports_update_maintenance=True,
            )

        self.assertIs(caught.exception, uncertainty)
        self.assertEqual(observed, [
            (self.SESSION, self.SESSION),
        ])
        self.assertEqual(badge._update_session, self.SESSION)
        badge._read_update_mode_or_control_error.assert_not_called()

    def test_same_uplink_reconnect_proves_small_ping_before_large_status(
        self,
    ) -> None:
        descriptor = _usb_record("/dev/cu.rebound", self.MAC)
        badge = flash.BadgeSerial(
            descriptor,
            False,
            expected_hardware_id=self.MAC,
        )
        badge.ser = object()
        events: list[str] = []

        def close_serial() -> None:
            events.append("close")
            badge.ser = None

        def open_serial() -> None:
            events.append("open")
            badge.ser = object()

        def wait_ping_once(timeout_s: float) -> None:
            self.assertGreater(timeout_s, 0)
            events.append("ping")

        def status(*, timeout_s: float) -> dict:
            self.assertGreater(timeout_s, 0)
            events.append("status")
            return self._normal_status(responses=20)

        badge._close_serial = close_serial  # type: ignore[method-assign]
        badge._open_serial = open_serial  # type: ignore[method-assign]
        badge._wait_ping_once = wait_ping_once  # type: ignore[method-assign]
        badge.status = status  # type: ignore[method-assign]

        with mock.patch.object(
            flash,
            "_take_badge_usb_descriptor_census",
            return_value=(descriptor,),
        ):
            got = badge._reconnect_same_uplink_mode(
                deadline=flash.time.monotonic() + 10,
                maintenance_session=None,
            )

        self.assertEqual(got["hardware_id"].lower(), self.MAC)
        self.assertEqual(events, ["close", "open", "ping", "status"])

    def test_normal_reconnect_accepts_legacy_same_descriptor_after_ping_proof(
        self,
    ) -> None:
        old = _usb_record("/dev/cu.same", self.MAC)
        badge = flash.BadgeSerial(
            old,
            False,
            expected_hardware_id=self.MAC,
        )
        badge.ser = object()
        opened: list[int] = []
        commands: list[str] = []
        clock = SimpleNamespace(now=0.0)

        def close_serial() -> None:
            badge.ser = None

        def open_serial() -> None:
            opened.append(badge._descriptor.stat_inode)
            badge.ser = object()

        def wait_ping_once(_timeout_s: float) -> None:
            commands.append("ping")

        def status(*, timeout_s: float) -> dict:
            self.assertGreater(timeout_s, 0)
            commands.append("status")
            return self._normal_status(responses=20)

        badge._close_serial = close_serial  # type: ignore[method-assign]
        badge._open_serial = open_serial  # type: ignore[method-assign]
        badge._wait_ping_once = wait_ping_once  # type: ignore[method-assign]
        badge.status = status  # type: ignore[method-assign]

        with mock.patch.object(
            flash,
            "_take_badge_usb_descriptor_census",
            return_value=(old,),
        ), mock.patch.object(
            flash.time, "monotonic", side_effect=lambda: clock.now
        ), mock.patch.object(
            flash.time, "sleep",
            side_effect=lambda delay: setattr(
                clock, "now", clock.now + delay
            ),
        ):
            got = badge.reconnect_same_uplink_normal(
                deadline=1.0,
            )

        self.assertEqual(got["hardware_id"].lower(), self.MAC)
        self.assertEqual(opened, [old.stat_inode])
        self.assertEqual(commands, ["ping", "status"])

    def test_maintenance_uplink_target_retries_dropped_status(
        self,
    ) -> None:
        ready = self._maintenance_status(responses=22)
        dropped = flash.SerialReadTimeout(
            "scheduled FOF_STATUS loss",
            saw_activity=False,
            partial_frame=False,
        )

        class FakeBadge:
            expected_hardware_id = self.MAC

            def __init__(inner_self) -> None:
                inner_self.status_calls = 0
                inner_self.reconnect_calls = 0

            def status(
                inner_self, *, timeout_s: float = 5
            ) -> dict:
                self.assertGreater(timeout_s, 0)
                inner_self.status_calls += 1
                if inner_self.status_calls == 1:
                    raise dropped
                return copy.deepcopy(ready)

            def reconnect_same_uplink(
                inner_self, *, deadline: float
            ) -> dict:
                self.assertGreater(deadline, flash.time.monotonic())
                inner_self.reconnect_calls += 1
                return copy.deepcopy(ready)

        badge = FakeBadge()
        got = flash._wait_for_maintenance_uplink_target(
            badge,
            session=self.SESSION,
            expected_version=self.VERSION,
            expected_partition="ota_0",
            deadline=flash.time.monotonic() + 10,
        )

        self.assertEqual(got, ready)
        self.assertEqual(badge.status_calls, 1)
        self.assertEqual(badge.reconnect_calls, 1)

    def test_update_transfer_budget_covers_slow_two_lane_convergence(
        self,
    ) -> None:
        self.assertGreaterEqual(
            flash.UPDATE_TRANSFER_TIMEOUT_S,
            20 * 60,
        )

    def test_maintenance_uplink_target_rejects_protocol_errors(
        self,
    ) -> None:
        protocol_error = flash.SerialTransportError(
            "malformed maintenance response",
            terminal_unavailable=False,
        )

        class FakeBadge:
            expected_hardware_id = self.MAC

            def status(
                inner_self, *, timeout_s: float = 5
            ) -> dict:
                self.assertGreater(timeout_s, 0)
                raise protocol_error

            def reconnect_same_uplink(
                inner_self, *, deadline: float
            ) -> dict:
                raise AssertionError(
                    "protocol errors must fail closed without reconnect"
                )

        with self.assertRaises(
            flash.SerialTransportError
        ) as caught:
            flash._wait_for_maintenance_uplink_target(
                FakeBadge(),
                session=self.SESSION,
                expected_version=self.VERSION,
                expected_partition="ota_0",
                deadline=flash.time.monotonic() + 10,
            )

        self.assertIs(caught.exception, protocol_error)

    def test_maintenance_uplink_target_retries_truncated_status_frame(
        self,
    ) -> None:
        ready = self._maintenance_status(responses=24)
        truncated = flash.SerialTransportError(
            "malformed FOF_STATUS: frame on /dev/fake: "
            "Expecting value: line 1 column 502",
            terminal_unavailable=False,
        )

        class FakeBadge:
            expected_hardware_id = self.MAC

            def __init__(inner_self) -> None:
                inner_self.reconnect_calls = 0

            def status(
                inner_self, *, timeout_s: float = 5
            ) -> dict:
                self.assertGreater(timeout_s, 0)
                raise truncated

            def reconnect_same_uplink(
                inner_self, *, deadline: float
            ) -> dict:
                self.assertGreater(deadline, flash.time.monotonic())
                inner_self.reconnect_calls += 1
                return copy.deepcopy(ready)

        badge = FakeBadge()
        got = flash._wait_for_maintenance_uplink_target(
            badge,
            session=self.SESSION,
            expected_version=self.VERSION,
            expected_partition="ota_0",
            deadline=flash.time.monotonic() + 10,
        )

        self.assertEqual(got, ready)
        self.assertEqual(badge.reconnect_calls, 1)

    def test_uplink_transfer_loss_validates_reconnect_before_retry_bytes(
        self,
    ) -> None:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        frozen = _test_frozen_usb_artifacts(self.VERSION).uplink
        data = flash._frozen_firmware_bytes(frozen, role="uplink")
        ready = _uplink_receipt(
            "ready",
            total=len(data),
            credit=min(flash.UPLINK_OTA_CREDIT_BYTES, len(data)),
        )
        committed = _uplink_receipt(
            "committed",
            received=len(data),
            total=len(data),
            reboot_required=True,
        )

        def run(
            validator,
            observed: dict[str, list],
        ) -> tuple[list[str], list[bytes], list[str]]:
            class LostTransport:
                def write(self, _payload: bytes) -> int:
                    raise OSError("scheduled uplink transport loss")

            retry_serial = _ScriptedRawSerial()
            badge = flash.BadgeSerial(
                _usb_record("/dev/fake"), False,
                expected_hardware_id=self.MAC,
            )
            badge._update_session = self.SESSION
            badge.ser = LostTransport()
            badge.status = mock.Mock(  # type: ignore[method-assign]
                return_value=self._maintenance_status(responses=20)
            )
            manifests: list[str] = []
            badge.write_line = manifests.append  # type: ignore[method-assign]
            replies = [dict(ready), dict(ready), dict(committed)]
            badge.read_prefixed_json = (  # type: ignore[method-assign]
                lambda *_args, **_kwargs: replies.pop(0)
            )
            events: list[str] = []
            observed.update({
                "manifests": manifests,
                "writes": retry_serial.writes,
                "events": events,
            })

            def reconnect(*, deadline: float) -> dict:
                self.assertGreater(deadline, 0)
                events.append("reconnect")
                badge.ser = retry_serial
                return self._maintenance_status(responses=21)

            def reconcile(_expected: dict) -> str:
                events.append("reconcile")
                return "restart_from_zero"

            badge.reconnect_same_uplink = reconnect  # type: ignore[method-assign]
            badge.reconcile_uplink_ota = reconcile  # type: ignore[method-assign]
            with contextlib.redirect_stdout(io.StringIO()):
                badge.upload_uplink_firmware(
                    platform,
                    frozen,
                    self.VERSION,
                    recovery_rewrite_same_version=True,
                    maintenance_status_validator=validator(events),
                )
            return manifests, retry_serial.writes, events

        def degraded(events: list[str]):
            def validate(_status: dict, session: str) -> None:
                self.assertEqual(session, self.SESSION)
                events.append("validate")
                raise flash.FlashError("degraded reconnect metrics")
            return validate

        with self.assertRaisesRegex(
            flash.FlashError, "degraded reconnect metrics"
        ):
            bad: dict[str, list] = {}
            run(degraded, bad)
        self.assertEqual(len(bad["manifests"]), 1)
        self.assertEqual(bad["writes"], [])
        self.assertEqual(bad["events"], ["reconnect", "validate"])

        def healthy(events: list[str]):
            def validate(_status: dict, session: str) -> None:
                self.assertEqual(session, self.SESSION)
                events.append("validate")
            return validate

        manifests, retry_writes, events = run(healthy, {})
        self.assertEqual(events, ["reconnect", "validate", "reconcile"])
        self.assertEqual(len(manifests), 2)
        self.assertEqual(b"".join(retry_writes), data)

    def test_scanner_transfer_loss_validates_reconnect_before_retry_bytes(
        self,
    ) -> None:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        frozen = _test_frozen_usb_artifacts(self.VERSION).scanner
        data = flash._frozen_firmware_bytes(frozen, role="scanner")
        ready = _credit_stage_receipt(
            platform,
            self.VERSION,
            data,
            3,
            phase="ready",
            received=0,
            credit=min(flash.SCANNER_STAGE_CREDIT_BYTES, len(data)),
        )
        final = _credit_stage_receipt(
            platform,
            self.VERSION,
            data,
            3,
            phase="final",
            received=len(data),
            credit=0,
            generation=9,
        )

        def run(
            validator,
            observed: dict[str, list],
        ) -> tuple[list[str], list[bytes], list[str]]:
            class LostTransport:
                def write(self, _payload: bytes) -> int:
                    raise OSError("scheduled scanner transport loss")

            retry_serial = _ScriptedRawSerial()
            badge = flash.BadgeSerial(
                _usb_record("/dev/fake"), False,
                expected_hardware_id=self.MAC,
            )
            badge._update_session = self.SESSION
            badge.ser = LostTransport()
            badge.status = mock.Mock(  # type: ignore[method-assign]
                return_value=self._maintenance_status(responses=20)
            )
            manifests: list[str] = []
            badge.write_line = manifests.append  # type: ignore[method-assign]
            replies = [dict(ready), dict(ready), dict(final)]
            badge.read_prefixed_json = (  # type: ignore[method-assign]
                lambda *_args, **_kwargs: replies.pop(0)
            )
            events: list[str] = []
            observed.update({
                "manifests": manifests,
                "writes": retry_serial.writes,
                "events": events,
            })

            def reconnect(*, deadline: float) -> dict:
                self.assertGreater(deadline, 0)
                events.append("reconnect")
                badge.ser = retry_serial
                return self._maintenance_status(responses=21)

            def reconcile(_expected: dict) -> str:
                events.append("reconcile")
                return "restart_from_zero"

            badge.reconnect_same_uplink = reconnect  # type: ignore[method-assign]
            badge.reconcile_scanner_stage = reconcile  # type: ignore[method-assign]
            with contextlib.redirect_stdout(io.StringIO()):
                badge.stage_scanner_firmware(
                    platform,
                    frozen,
                    self.VERSION,
                    ["ble", "wifi"],
                    maintenance_status_validator=validator(events),
                )
            return manifests, retry_serial.writes, events

        def degraded(events: list[str]):
            def validate(_status: dict, session: str) -> None:
                self.assertEqual(session, self.SESSION)
                events.append("validate")
                raise flash.FlashError("degraded reconnect metrics")
            return validate

        with self.assertRaisesRegex(
            flash.FlashError, "degraded reconnect metrics"
        ):
            bad: dict[str, list] = {}
            run(degraded, bad)
        self.assertEqual(len(bad["manifests"]), 1)
        self.assertEqual(bad["writes"], [])
        self.assertEqual(bad["events"], ["reconnect", "validate"])

        def healthy(events: list[str]):
            def validate(_status: dict, session: str) -> None:
                self.assertEqual(session, self.SESSION)
                events.append("validate")
            return validate

        manifests, retry_writes, events = run(healthy, {})
        self.assertEqual(events, ["reconnect", "validate", "reconcile"])
        self.assertEqual(len(manifests), 2)
        self.assertEqual(b"".join(retry_writes), data)

    def test_one_session_spans_uplink_and_both_scanner_lanes_before_finish(
        self,
    ) -> None:
        initial = self._normal_status(responses=20)
        maintenance = self._maintenance_status(responses=21)
        final = self._normal_status(responses=30)
        preflight = copy.deepcopy(initial)
        preflight.update({
            "safe_mode": False,
            "usb_control_alive": True,
            "scanner_uart_alive": True,
            "scanners": [
                _scanner_status(
                    flash.PLATFORMS["badge-trio-xiao-s3"],
                    self.VERSION,
                    hardware_id="e0:72:a1:f9:48:58",
                    slot="ble",
                )["scanners"][0],
                _scanner_status(
                    flash.PLATFORMS["badge-trio-xiao-s3"],
                    self.VERSION,
                    hardware_id="e0:72:a1:f9:48:59",
                    slot="wifi",
                )["scanners"][0],
            ],
        })
        artifacts = _test_frozen_usb_artifacts(self.VERSION)
        scanner_data = flash._frozen_firmware_bytes(
            artifacts.scanner, role="scanner"
        )
        stage_receipt = _stage_receipt(
            flash.PLATFORMS["badge-trio-xiao-s3"],
            self.VERSION,
            scanner_data,
            3,
            generation=7,
        )
        scanner_maintenance = copy.deepcopy(maintenance)
        scanner_maintenance["update_campaign"] = {
            "generation": 7,
            "target_slot_mask": 3,
            "pending_mask": 0,
            "worker_running": False,
            "readiness_probes": [1, 1],
            "scanners": [
                {"slot": 0, "state": "converged", "attempts": 1},
                {"slot": 1, "state": "converged", "attempts": 1},
            ],
        }
        descriptor = _usb_record("/dev/cu.initial", self.MAC)
        binding = flash.TrustedUplinkBinding(
            serial_number=self.MAC,
            location=descriptor.location,
            source="operator-selection",
        )
        events: list[tuple[str, object]] = []
        sessions: list[str] = []

        class FakeBadge:
            def __init__(
                inner_self,
                opened_descriptor,
                dry_run,
                expected_hardware_id=None,
            ):
                self.assertIs(opened_descriptor, descriptor)
                self.assertIs(dry_run, False)
                self.assertEqual(expected_hardware_id, self.MAC)
                inner_self.expected_hardware_id = expected_hardware_id
                inner_self.session = None
                inner_self.normal = False

            def __enter__(inner_self):
                return inner_self

            def __exit__(inner_self, *_args):
                return None

            def prepare_update_maintenance(
                inner_self,
                session: str,
                *,
                deadline: float,
                source_supports_update_maintenance: bool,
            ) -> dict:
                self.assertGreater(deadline, 0)
                self.assertIs(source_supports_update_maintenance, True)
                inner_self.session = session
                sessions.append(session)
                events.append(("prepare", session))
                return {
                    "ok": True,
                    "phase": "rebooting",
                    "session": session,
                    "retryable": True,
                    "reboot_required": True,
                }

            def reconnect_same_uplink(
                inner_self, *, deadline: float
            ) -> dict:
                self.assertGreater(deadline, 0)
                sessions.append(inner_self.session)
                events.append(("reconnect-maintenance", inner_self.session))
                return copy.deepcopy(maintenance)

            def upload_uplink_firmware(
                inner_self,
                _platform,
                frozen,
                version,
                recovery_rewrite_same_version,
                *,
                maintenance_status_validator,
            ) -> dict:
                self.assertIs(frozen, artifacts.uplink)
                self.assertEqual(version, self.VERSION)
                self.assertIs(recovery_rewrite_same_version, False)
                self.assertIs(
                    maintenance_status_validator,
                    live_maintenance_validator,
                )
                sessions.append(inner_self.session)
                events.append(("upload-uplink", inner_self.session))
                return {
                    "ok": True,
                    "skipped": True,
                    "phase": "current",
                    "hardware_id": self.MAC,
                    "version": self.VERSION,
                    "partition": "ota_0",
                }

            def status(inner_self, *, timeout_s: float = 5) -> dict:
                self.assertGreater(timeout_s, 0)
                sessions.append(inner_self.session)
                if inner_self.normal:
                    events.append(("normal-status", inner_self.session))
                    result = copy.deepcopy(final)
                    result["usb_health"]["responses_completed"] += 1
                    return result
                events.append(("maintenance-status", inner_self.session))
                return copy.deepcopy(maintenance)

            def finish_update_maintenance(
                inner_self, *, deadline: float
            ) -> dict:
                self.assertGreater(deadline, 0)
                sessions.append(inner_self.session)
                events.append(("finish", inner_self.session))
                return {
                    "ok": True,
                    "phase": "finishing",
                    "session": inner_self.session,
                    "retryable": False,
                    "reboot_required": True,
                }

            def reconnect_same_uplink_normal(
                inner_self, *, deadline: float
            ) -> dict:
                self.assertGreater(deadline, 0)
                sessions.append(inner_self.session)
                events.append(("reconnect-normal", inner_self.session))
                inner_self.normal = True
                return copy.deepcopy(final)

        scanner_result = (
            copy.deepcopy(preflight),
            scanner_maintenance,
            stage_receipt,
            frozenset({"ble", "wifi"}),
            frozenset(),
            {
                "ble": "e0:72:a1:f9:48:58",
                "wifi": "e0:72:a1:f9:48:59",
            },
        )

        def run_scanners(_badge, **kwargs):
            session = kwargs["session"]
            sessions.append(session)
            events.append(("scanner-lanes", session))
            self.assertEqual(kwargs["slots"], ["ble", "wifi"])
            return scanner_result

        def live_maintenance_validator(status, session):
            self.assertEqual(status["recovery_mode"], "update_maintenance")
            self.assertEqual(session, self.SESSION)
            events.append(("live-maintenance", session))

        def theme_control(
            badge,
            *,
            initial_status,
            expectation,
            initial_evidence,
            restored_status_validator=None,
        ):
            del badge, expectation
            events.append(("theme-proof", initial_evidence.version))
            restored = copy.deepcopy(initial_status)
            restored["usb_health"]["responses_completed"] += 1
            if restored_status_validator is not None:
                restored_status_validator(restored)
            return flash.verify_post_uplink_application(
                restored,
                expected_hardware_id=self.MAC,
                expected_version=self.VERSION,
                expected_partition="ota_0",
            )

        def issue_result(**kwargs):
            events.append(("issue-result", kwargs["stage_receipt"]))
            return "issued"

        with mock.patch.object(
            flash, "_new_update_session", return_value=self.SESSION
        ), mock.patch.object(
            flash, "BadgeSerial", FakeBadge
        ), mock.patch.object(
            flash, "wait_for_scanner_status_usb", return_value=preflight
        ), mock.patch.object(
            flash,
            "_run_scanner_update_in_maintenance",
            side_effect=run_scanners,
        ), mock.patch.object(
            flash, "coordinator_newer_skipped_slots", return_value=set()
        ), mock.patch.object(
            flash, "verify_scanners"
        ), mock.patch.object(
            flash, "verify_auto_update_convergence"
        ), mock.patch.object(
            flash, "wait_for_scanners_usb"
        ), mock.patch.object(
            flash,
            "_prove_reversible_usb_theme_control",
            side_effect=theme_control,
        ):
            result = flash._usb_update_maintenance_flow(
                self._args(),
                flash.PLATFORMS["badge-trio-xiao-s3"],
                True,
                ["ble", "wifi"],
                self.VERSION,
                initial_descriptor=descriptor,
                trusted_uplink_binding=binding,
                application_status=initial,
                frozen_artifacts=artifacts,
                scanner_image_size=len(
                    flash._frozen_firmware_bytes(
                        artifacts.scanner, role="scanner"
                    )
                ),
                legacy_bootstrap=False,
                _issue_scanner_flow_result=issue_result,
                maintenance_status_validator=live_maintenance_validator,
            )

        self.assertEqual(result, "issued")
        self.assertEqual(set(sessions), {self.SESSION})
        names = [name for name, _value in events]
        live_indexes = [
            index for index, name in enumerate(names)
            if name == "live-maintenance"
        ]
        self.assertEqual(len(live_indexes), 2)
        self.assertLess(live_indexes[0], names.index("upload-uplink"))
        self.assertLess(live_indexes[1], names.index("scanner-lanes"))
        for earlier, later in (
            ("prepare", "reconnect-maintenance"),
            ("reconnect-maintenance", "upload-uplink"),
            ("upload-uplink", "maintenance-status"),
            ("maintenance-status", "scanner-lanes"),
            ("scanner-lanes", "finish"),
            ("finish", "reconnect-normal"),
            ("reconnect-normal", "theme-proof"),
            ("theme-proof", "issue-result"),
        ):
            self.assertLess(names.index(earlier), names.index(later))

    def test_full_flow_retries_only_failed_lane_and_proves_all_scanners(
        self,
    ) -> None:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        artifacts = _test_frozen_usb_artifacts(self.VERSION)
        descriptor = _usb_record("/dev/cu.initial", self.MAC)
        binding = flash.TrustedUplinkBinding(
            serial_number=self.MAC,
            location=descriptor.location,
            source="operator-selection",
        )
        scanner_ids = {
            "ble": "e0:72:a1:f9:48:58",
            "wifi": "e0:72:a1:f9:48:59",
        }
        initial = self._normal_status(responses=20)
        preflight = copy.deepcopy(initial)
        preflight.update({
            "safe_mode": False,
            "usb_control_alive": True,
            "scanner_uart_alive": True,
            "scanners": [
                _scanner_status(
                    platform,
                    self.VERSION,
                    hardware_id=scanner_ids["ble"],
                    slot="ble",
                )["scanners"][0],
                _scanner_status(
                    platform,
                    "0.64.78-badge-defcon34",
                    hardware_id=scanner_ids["wifi"],
                    slot="wifi",
                )["scanners"][0],
            ],
        })
        final = copy.deepcopy(preflight)
        final["usb_health"]["responses_completed"] = 80
        final["scanners"][1]["ver"] = self.VERSION
        final["scanners"][1]["version"] = self.VERSION
        first_stage, first_maintenance = self._campaign_failure_fixture(
            generation=61,
            ble_state="converged",
            ble_attempts=1,
            wifi_state="failed",
            wifi_attempts=3,
            readiness_probes=[1, 1],
        )
        first_failure = flash.ScannerCampaignFailure(
            session=self.SESSION,
            requested_slots=["ble", "wifi"],
            stage_receipt=first_stage,
            campaign=first_maintenance["update_campaign"],
        )
        retry_session = "ABCDEF0123456789"
        retry_normal, _ = self._scanner_retry_normal_status(
            failed_slot="wifi",
            last_relay_error="ota_ack_timeout",
        )
        retry_result = self._single_lane_retry_success(
            session=retry_session,
            slot="wifi",
            generation=62,
            preflight=retry_normal,
            identities=scanner_ids,
        )
        first_attempt = flash._scanner_attempt_snapshot(
            ordinal=1,
            session=self.SESSION,
            requested_slots=["ble", "wifi"],
            pre_stage_status=preflight,
            stage_receipt=first_stage,
            campaign=first_failure.campaign,
            outcome="failed",
            classification="ota_ack_timeout",
            recovery_action="session_abort",
            platform=platform,
            artifacts=artifacts.scanner,
            version=self.VERSION,
        )
        second_attempt = flash._scanner_attempt_snapshot(
            ordinal=2,
            session=retry_session,
            requested_slots=["wifi"],
            pre_stage_status=retry_result[0],
            stage_receipt=retry_result[2],
            campaign=retry_result[1]["update_campaign"],
            outcome="converged",
            classification="ota_ack_timeout",
            recovery_action="session_abort",
            platform=platform,
            artifacts=artifacts.scanner,
            version=self.VERSION,
        )
        retry_sequence = flash._issue_scanner_retry_sequence(
            session=retry_session,
            latest_slots=["wifi"],
            scanner_result=retry_result,
            original_hardware_ids=scanner_ids,
            attempt_history=[first_attempt, second_attempt],
        )
        maintenance = self._maintenance_status(responses=30)
        finalized: list[str] = []

        class FakeBadge:
            def __init__(
                inner_self,
                opened_descriptor,
                dry_run,
                expected_hardware_id=None,
            ):
                self.assertIs(opened_descriptor, descriptor)
                self.assertIs(dry_run, False)
                inner_self.expected_hardware_id = expected_hardware_id
                inner_self._update_session = self.SESSION
                inner_self._descriptor = opened_descriptor

            def __enter__(inner_self):
                return inner_self

            def __exit__(inner_self, *_args):
                return None

            def prepare_update_maintenance(
                inner_self, session: str, **_kwargs
            ) -> dict:
                inner_self._update_session = session
                return {
                    "ok": True,
                    "phase": "rebooting",
                    "session": session,
                    "retryable": True,
                    "reboot_required": True,
                }

            def reconnect_same_uplink(
                inner_self, *, deadline: float
            ) -> dict:
                return copy.deepcopy(maintenance)

            def status(
                inner_self, *, timeout_s: float = 5
            ) -> dict:
                self.assertGreater(timeout_s, 0)
                status = copy.deepcopy(final)
                status["usb_health"]["responses_completed"] += 1
                return status

        final_evidence = flash.verify_post_uplink_application(
            final,
            expected_hardware_id=self.MAC,
            expected_version=self.VERSION,
            expected_partition="ota_0",
        )

        def finalize(_badge, **kwargs):
            finalized.append(kwargs["session"])
            return copy.deepcopy(final), final_evidence

        def theme_control(
            _badge,
            *,
            initial_status,
            restored_status_validator=None,
            **_kwargs,
        ):
            restored = copy.deepcopy(initial_status)
            restored["usb_health"]["responses_completed"] += 1
            if restored_status_validator is not None:
                restored_status_validator(restored)
            return flash.verify_post_uplink_application(
                restored,
                expected_hardware_id=self.MAC,
                expected_version=self.VERSION,
                expected_partition="ota_0",
            )

        issue = mock.Mock(return_value="issued")
        with mock.patch.object(
            flash, "_new_update_session", return_value=self.SESSION
        ), mock.patch.object(
            flash, "BadgeSerial", FakeBadge
        ), mock.patch.object(
            flash, "wait_for_scanner_status_usb", return_value=preflight
        ), mock.patch.object(
            flash, "_wait_for_maintenance_uplink_target"
        ), mock.patch.object(
            flash,
            "_run_scanner_update_in_maintenance",
            side_effect=first_failure,
        ), mock.patch.object(
            flash,
            "_retry_failed_scanner_campaigns",
            return_value=retry_sequence,
        ) as retry, mock.patch.object(
            flash,
            "_finalize_update_maintenance",
            side_effect=finalize,
        ), mock.patch.object(
            flash, "wait_for_scanners_usb"
        ) as final_wait, mock.patch.object(
            flash, "coordinator_newer_skipped_slots", return_value=set()
        ), mock.patch.object(
            flash, "verify_scanners"
        ) as verify_all, mock.patch.object(
            flash, "verify_auto_update_convergence"
        ) as verify_latest, mock.patch.object(
            flash,
            "_prove_reversible_usb_theme_control",
            side_effect=theme_control,
        ):
            result = flash._usb_update_maintenance_flow(
                self._args(),
                platform,
                False,
                ["ble", "wifi"],
                self.VERSION,
                initial_descriptor=descriptor,
                trusted_uplink_binding=binding,
                application_status=initial,
                frozen_artifacts=artifacts,
                scanner_image_size=len(
                    flash._frozen_firmware_bytes(
                        artifacts.scanner,
                        role="scanner",
                    )
                ),
                legacy_bootstrap=False,
                _issue_scanner_flow_result=issue,
            )

        self.assertEqual(result, "issued")
        retry.assert_called_once()
        self.assertEqual(
            retry.call_args.kwargs["original_hardware_ids"],
            scanner_ids,
        )
        self.assertEqual(finalized, [retry_session])
        self.assertEqual(final_wait.call_args.args[2], ["wifi"])
        self.assertTrue(any(
            call.args[2] == ["ble", "wifi"]
            for call in verify_all.call_args_list
        ))
        self.assertTrue(all(
            call.args[1] == ["wifi"]
            for call in verify_latest.call_args_list
        ))
        issued = issue.call_args.kwargs
        self.assertEqual(
            issued["stage_receipt"], retry_result[2]
        )
        self.assertEqual(
            issued["stage_receipts"],
            (first_stage, retry_result[2]),
        )
        self.assertEqual(
            issued["attempt_history"],
            retry_sequence.attempt_history,
        )

    def test_legacy_dot_78_bootstrap_enters_new_session_before_scanners(
        self,
    ) -> None:
        source_version = (
            flash.UPDATE_MAINTENANCE_BOOTSTRAP_SOURCE_VERSION
        )
        target_version = (
            flash.UPDATE_MAINTENANCE_BOOTSTRAP_TARGET_VERSION
        )
        post_bootstrap_session = "FEDCBA9876543210"
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        scanner_ids = {
            "ble": "e0:72:a1:f9:48:58",
            "wifi": "e0:72:a1:f9:48:59",
        }

        def scanner_status(
            uplink_version: str,
            *,
            scanner_version: str,
            partition: str,
            responses: int,
        ) -> dict:
            status = _uplink_status(
                uplink_version,
                hardware_id=self.MAC,
                partition=partition,
                responses=responses,
            )
            status.update({
                "safe_mode": False,
                "usb_control_alive": True,
                "scanner_uart_alive": True,
                "scanners": [
                    _scanner_status(
                        platform,
                        scanner_version,
                        hardware_id=scanner_ids[slot],
                        slot=slot,
                    )["scanners"][0]
                    for slot in ("ble", "wifi")
                ],
            })
            return status

        initial = _uplink_status(
            source_version,
            hardware_id=self.MAC,
            responses=20,
        )
        fresh_source = _uplink_status(
            source_version,
            hardware_id=self.MAC,
            responses=23,
        )
        initial_preflight = scanner_status(
            source_version,
            scanner_version=source_version,
            partition="ota_0",
            responses=21,
        )
        post_target = _uplink_status(
            target_version,
            hardware_id=self.MAC,
            partition="ota_1",
            responses=40,
        )
        target_fresh = copy.deepcopy(post_target)
        target_fresh["usb_health"]["responses_completed"] = 43
        target_preflight = scanner_status(
            target_version,
            scanner_version=source_version,
            partition="ota_1",
            responses=44,
        )
        maintenance = _update_maintenance_status(
            target_version,
            session=post_bootstrap_session,
            hardware_id=self.MAC,
            partition="ota_1",
            responses=50,
        )
        final_after_finish = scanner_status(
            target_version,
            scanner_version=target_version,
            partition="ota_1",
            responses=60,
        )
        post_scanner = copy.deepcopy(final_after_finish)
        post_scanner["usb_health"]["responses_completed"] = 63
        artifacts = _test_frozen_usb_artifacts(target_version)
        uplink_data = flash._frozen_firmware_bytes(
            artifacts.uplink, role="uplink"
        )
        scanner_data = flash._frozen_firmware_bytes(
            artifacts.scanner, role="scanner"
        )
        stage_receipt = _stage_receipt(
            platform,
            target_version,
            scanner_data,
            3,
            generation=9,
        )
        scanner_maintenance = copy.deepcopy(maintenance)
        scanner_maintenance["update_campaign"] = {
            "generation": 9,
            "target_slot_mask": 3,
            "pending_mask": 0,
            "worker_running": False,
            "readiness_probes": [1, 1],
            "scanners": [
                {"slot": 0, "state": "converged", "attempts": 1},
                {"slot": 1, "state": "converged", "attempts": 1},
            ],
        }
        descriptor = _usb_record("/dev/cu.initial", self.MAC)
        rebound = _usb_record(
            "/dev/cu.rebound",
            self.MAC,
            location=descriptor.location,
        )
        binding = flash.TrustedUplinkBinding(
            serial_number=self.MAC,
            location=descriptor.location,
            source="operator-selection",
        )
        events: list[str] = []

        class FakeBadge:
            def __init__(
                inner_self,
                opened_descriptor,
                dry_run,
                expected_hardware_id=None,
            ):
                self.assertIs(opened_descriptor, descriptor)
                self.assertIs(dry_run, False)
                self.assertEqual(expected_hardware_id, self.MAC)
                inner_self.expected_hardware_id = expected_hardware_id
                inner_self._descriptor = opened_descriptor
                inner_self.proofs = 0
                inner_self.prepare_calls = 0

            def __enter__(inner_self):
                return inner_self

            def __exit__(inner_self, *_args):
                return None

            def prepare_update_maintenance(
                inner_self,
                session: str,
                *,
                deadline: float,
                source_supports_update_maintenance: bool,
            ) -> dict:
                self.assertGreater(deadline, flash.time.monotonic())
                inner_self.prepare_calls += 1
                if inner_self.prepare_calls == 1:
                    self.assertIs(
                        source_supports_update_maintenance, False
                    )
                    self.assertEqual(session, self.SESSION)
                    events.append("prepare-legacy-rejected")
                    raise flash.UpdateMaintenanceUnsupportedError(
                        "production .78 rejected prepare_update"
                    )
                self.assertEqual(session, post_bootstrap_session)
                self.assertIs(source_supports_update_maintenance, True)
                events.append("prepare-dot79")
                return {
                    "ok": True,
                    "phase": "rebooting",
                    "session": session,
                    "retryable": True,
                    "reboot_required": True,
                }

            def _prove_open_application(
                inner_self, timeout_s: float
            ) -> dict:
                self.assertGreater(timeout_s, 0)
                inner_self.proofs += 1
                if inner_self.proofs == 1:
                    events.append("fresh-source-proof")
                    return copy.deepcopy(fresh_source)
                events.append("fresh-target-proof")
                return copy.deepcopy(target_fresh)

            def upload_uplink_firmware(
                inner_self,
                _platform,
                frozen,
                version,
                recovery_rewrite_same_version,
                *,
                expected_pre_status=None,
            ) -> dict:
                self.assertIs(frozen, artifacts.uplink)
                self.assertEqual(version, target_version)
                self.assertIs(recovery_rewrite_same_version, False)
                self.assertEqual(expected_pre_status, fresh_source)
                events.append("upload")
                return _uplink_receipt(
                    "committed",
                    partition="ota_1",
                    received=len(uplink_data),
                    total=len(uplink_data),
                    reboot_required=True,
                )

            def _close_serial(inner_self) -> None:
                events.append("close")

            def _open_serial(inner_self) -> None:
                events.append("open")

            def reconnect_same_uplink(
                inner_self, *, deadline: float
            ) -> dict:
                self.assertGreater(deadline, flash.time.monotonic())
                events.append("maintenance-proof")
                return copy.deepcopy(maintenance)

            def status(
                inner_self, *, timeout_s: float = 5
            ) -> dict:
                self.assertGreater(timeout_s, 0)
                events.append("post-scanner-status")
                return copy.deepcopy(post_scanner)

        def wait_post(expectation, *, timeout_s):
            self.assertGreater(timeout_s, 0)
            self.assertEqual(expectation.pre_version, source_version)
            self.assertEqual(expectation.pre_partition, "ota_0")
            self.assertEqual(expectation.expected_version, target_version)
            self.assertEqual(expectation.expected_partition, "ota_1")
            self.assertEqual(
                expectation.update_session, post_bootstrap_session
            )
            events.append("post-target-proof")
            return (
                rebound,
                flash.verify_post_uplink_application(
                    post_target,
                    expected_hardware_id=self.MAC,
                    expected_version=target_version,
                    expected_partition="ota_1",
                ),
            )

        preflights = iter((initial_preflight, target_preflight))

        def scanner_preflight(_badge, slots):
            self.assertEqual(slots, ["ble", "wifi"])
            status = next(preflights)
            events.append(
                "preflight-dot78"
                if status["version"] == source_version
                else "preflight-dot79"
            )
            return copy.deepcopy(status)

        def run_scanner_campaign(_badge, **kwargs):
            self.assertEqual(kwargs["session"], post_bootstrap_session)
            self.assertEqual(kwargs["slots"], ["ble", "wifi"])
            self.assertEqual(kwargs["preflight_status"], target_preflight)
            events.append("scanner-campaign")
            return (
                copy.deepcopy(target_preflight),
                copy.deepcopy(scanner_maintenance),
                copy.deepcopy(stage_receipt),
                frozenset({"ble", "wifi"}),
                frozenset(),
                dict(scanner_ids),
            )

        final_evidence = flash.verify_post_uplink_application(
            final_after_finish,
            expected_hardware_id=self.MAC,
            expected_version=target_version,
            expected_partition="ota_1",
        )

        def finalize(_badge, **kwargs):
            self.assertEqual(kwargs["session"], post_bootstrap_session)
            events.append("finish-maintenance")
            return copy.deepcopy(final_after_finish), final_evidence

        def theme_control(
            _badge,
            *,
            initial_status,
            expectation,
            initial_evidence,
            restored_status_validator=None,
        ):
            self.assertEqual(initial_status, post_scanner)
            self.assertEqual(
                expectation.update_session, post_bootstrap_session
            )
            self.assertEqual(
                initial_status["usb_health"]["responses_completed"],
                initial_evidence.responses_completed,
            )
            restored = copy.deepcopy(initial_status)
            restored["usb_health"]["responses_completed"] += 1
            if restored_status_validator is not None:
                restored_status_validator(restored)
            events.append("theme-proof")
            return flash.verify_post_uplink_application(
                restored,
                expected_hardware_id=self.MAC,
                expected_version=target_version,
                expected_partition="ota_1",
            )

        def issue_result(**kwargs):
            self.assertEqual(kwargs["stage_receipt"], stage_receipt)
            events.append("issue-result")
            return "issued"

        def post_direct_bootstrap_validator(status):
            self.assertEqual(status, target_fresh)
            events.append("post-bootstrap-live-validator")

        with mock.patch.object(
            flash,
            "_new_update_session",
            side_effect=(self.SESSION, post_bootstrap_session),
        ), mock.patch.object(
            flash, "BadgeSerial", FakeBadge
        ), mock.patch.object(
            flash,
            "wait_for_post_uplink_application",
            side_effect=wait_post,
        ), mock.patch.object(
            flash,
            "wait_for_scanner_status_usb",
            side_effect=scanner_preflight,
        ), mock.patch.object(
            flash, "_wait_for_maintenance_uplink_target"
        ), mock.patch.object(
            flash,
            "_run_scanner_update_in_maintenance",
            side_effect=run_scanner_campaign,
        ), mock.patch.object(
            flash,
            "_finalize_update_maintenance",
            side_effect=finalize,
        ), mock.patch.object(
            flash,
            "wait_for_scanners_usb",
            side_effect=lambda *_args, **_kwargs: events.append(
                "scanner-post-proof"
            ),
        ), mock.patch.object(
            flash, "coordinator_newer_skipped_slots", return_value=set()
        ), mock.patch.object(
            flash, "verify_scanners"
        ), mock.patch.object(
            flash, "verify_auto_update_convergence"
        ), mock.patch.object(
            flash,
            "_prove_reversible_usb_theme_control",
            side_effect=theme_control,
        ):
            result = flash._usb_update_maintenance_flow(
                self._args(),
                platform,
                True,
                ["ble", "wifi"],
                target_version,
                initial_descriptor=descriptor,
                trusted_uplink_binding=binding,
                application_status=initial,
                frozen_artifacts=artifacts,
                scanner_image_size=len(scanner_data),
                legacy_bootstrap=False,
                _issue_scanner_flow_result=issue_result,
                post_direct_bootstrap_status_validator=(
                    post_direct_bootstrap_validator
                ),
            )

        self.assertEqual(result, "issued")
        for earlier, later in (
            ("preflight-dot78", "prepare-legacy-rejected"),
            ("prepare-legacy-rejected", "fresh-source-proof"),
            ("fresh-source-proof", "upload"),
            ("upload", "post-target-proof"),
            ("post-target-proof", "fresh-target-proof"),
            ("fresh-target-proof", "post-bootstrap-live-validator"),
            ("post-bootstrap-live-validator", "preflight-dot79"),
            ("fresh-target-proof", "preflight-dot79"),
            ("preflight-dot79", "prepare-dot79"),
            ("prepare-dot79", "maintenance-proof"),
            ("maintenance-proof", "scanner-campaign"),
            ("scanner-campaign", "finish-maintenance"),
            ("finish-maintenance", "scanner-post-proof"),
            ("scanner-post-proof", "theme-proof"),
            ("theme-proof", "issue-result"),
        ):
            self.assertLess(events.index(earlier), events.index(later))

    def test_post_direct_prepare_uncertainty_recovers_new_session(
        self,
    ) -> None:
        source_version = (
            flash.UPDATE_MAINTENANCE_BOOTSTRAP_SOURCE_VERSION
        )
        target_version = (
            flash.UPDATE_MAINTENANCE_BOOTSTRAP_TARGET_VERSION
        )
        post_session = "FEDCBA9876543210"
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        scanner_ids = {
            "ble": "e0:72:a1:f9:48:58",
            "wifi": "e0:72:a1:f9:48:59",
        }

        def with_game(status: dict) -> dict:
            status.update({
                "game_seed": "immune",
                "game_state": "human",
                "game_active": False,
                "game_shield": 0,
            })
            return status

        def scanner_status(
            uplink_version: str,
            *,
            scanner_version: str,
            partition: str,
            responses: int,
        ) -> dict:
            status = with_game(_uplink_status(
                uplink_version,
                hardware_id=self.MAC,
                partition=partition,
                responses=responses,
            ))
            status.update({
                "safe_mode": False,
                "usb_control_alive": True,
                "scanner_uart_alive": True,
                "scanners": [
                    _scanner_status(
                        platform,
                        scanner_version,
                        hardware_id=scanner_ids[slot],
                        slot=slot,
                    )["scanners"][0]
                    for slot in ("ble", "wifi")
                ],
            })
            return status

        initial = with_game(_uplink_status(
            source_version,
            hardware_id=self.MAC,
            responses=20,
        ))
        fresh_source = with_game(_uplink_status(
            source_version,
            hardware_id=self.MAC,
            responses=23,
        ))
        source_preflight = scanner_status(
            source_version,
            scanner_version=source_version,
            partition="ota_0",
            responses=21,
        )
        post_target = with_game(_uplink_status(
            target_version,
            hardware_id=self.MAC,
            partition="ota_1",
            responses=40,
        ))
        target_fresh = with_game(copy.deepcopy(post_target))
        target_fresh["usb_health"]["responses_completed"] = 43
        target_preflight = scanner_status(
            target_version,
            scanner_version=source_version,
            partition="ota_1",
            responses=44,
        )
        maintenance = _update_maintenance_status(
            target_version,
            session=post_session,
            hardware_id=self.MAC,
            partition="ota_1",
            responses=50,
        )
        recovered = with_game(_uplink_status(
            target_version,
            hardware_id=self.MAC,
            partition="ota_1",
            responses=60,
        ))
        artifacts = _test_frozen_usb_artifacts(target_version)
        uplink_data = flash._frozen_firmware_bytes(
            artifacts.uplink, role="uplink"
        )
        scanner_data = flash._frozen_firmware_bytes(
            artifacts.scanner, role="scanner"
        )
        descriptor = _usb_record("/dev/cu.initial", self.MAC)
        rebound = _usb_record(
            "/dev/cu.rebound",
            self.MAC,
            location=descriptor.location,
        )
        binding = flash.TrustedUplinkBinding(
            serial_number=self.MAC,
            location=descriptor.location,
            source="operator-selection",
        )
        transition_error = flash.SerialTransportError(
            "post-direct prepare receipt lost",
            terminal_unavailable=True,
        )
        events: list[str] = []
        normal_reconnects = 0

        class FakeBadge:
            def __init__(
                inner_self,
                opened_descriptor,
                dry_run,
                expected_hardware_id=None,
            ):
                self.assertIs(opened_descriptor, descriptor)
                self.assertIs(dry_run, False)
                self.assertEqual(expected_hardware_id, self.MAC)
                inner_self.expected_hardware_id = expected_hardware_id
                inner_self._descriptor = opened_descriptor
                inner_self._update_session = None
                inner_self.prepare_calls = 0

            def __enter__(inner_self):
                return inner_self

            def __exit__(inner_self, *_args):
                events.append("context-close")
                return None

            def prepare_update_maintenance(
                inner_self,
                session: str,
                *,
                deadline: float,
                source_supports_update_maintenance: bool,
            ) -> dict:
                self.assertGreater(deadline, flash.time.monotonic())
                inner_self.prepare_calls += 1
                if inner_self.prepare_calls == 1:
                    self.assertEqual(session, self.SESSION)
                    self.assertIs(
                        source_supports_update_maintenance, False
                    )
                    events.append("prepare-dot78-unsupported")
                    raise flash.UpdateMaintenanceUnsupportedError(
                        "production source is legacy"
                    )
                self.assertEqual(session, post_session)
                self.assertIs(source_supports_update_maintenance, True)
                inner_self._update_session = session
                events.append("prepare-dot79-uncertain")
                raise transition_error

            def upload_uplink_firmware(
                inner_self,
                _platform,
                frozen,
                version,
                recovery_rewrite_same_version,
                *,
                expected_pre_status=None,
            ) -> dict:
                self.assertIs(frozen, artifacts.uplink)
                self.assertEqual(version, target_version)
                self.assertIs(recovery_rewrite_same_version, False)
                self.assertEqual(expected_pre_status, fresh_source)
                events.append("direct-uplink-upload")
                return _uplink_receipt(
                    "committed",
                    partition="ota_1",
                    received=len(uplink_data),
                    total=len(uplink_data),
                    reboot_required=True,
                )

            def _close_serial(inner_self) -> None:
                events.append("serial-close")

            def _open_serial(inner_self) -> None:
                events.append("serial-open")

            def _prove_open_application(
                inner_self, timeout_s: float
            ) -> dict:
                self.assertGreater(timeout_s, 0)
                events.append("target-application-proof")
                return copy.deepcopy(target_fresh)

            def reconnect_same_uplink_normal(
                inner_self, *, deadline: float
            ) -> dict:
                nonlocal normal_reconnects
                self.assertGreater(deadline, flash.time.monotonic())
                self.assertEqual(inner_self._update_session, post_session)
                normal_reconnects += 1
                events.append("normal-reconnect")
                if normal_reconnects == 1:
                    raise flash.SerialTransportError(
                        "post-direct badge remains in maintenance"
                    )
                return copy.deepcopy(recovered)

            def reconnect_same_uplink_recoverable_update(
                inner_self, *, deadline: float
            ) -> dict:
                self.assertGreater(deadline, flash.time.monotonic())
                self.assertEqual(inner_self._update_session, post_session)
                events.append("maintenance-reconnect")
                return copy.deepcopy(maintenance)

            def abort_update_maintenance(
                inner_self, *, deadline: float
            ) -> dict:
                self.assertGreater(deadline, flash.time.monotonic())
                self.assertEqual(inner_self._update_session, post_session)
                events.append("abort")
                return {
                    "ok": True,
                    "phase": "aborting",
                    "session": post_session,
                    "retryable": False,
                    "reboot_required": True,
                }

        def direct_reproof(*_args, **_kwargs) -> dict:
            events.append("direct-source-reproof")
            return copy.deepcopy(fresh_source)

        def wait_post(*_args, **_kwargs):
            events.append("post-uplink-proof")
            return (
                rebound,
                flash.verify_post_uplink_application(
                    post_target,
                    expected_hardware_id=self.MAC,
                    expected_version=target_version,
                    expected_partition="ota_1",
                ),
            )

        preflights = iter((source_preflight, target_preflight))

        def scanner_preflight(_badge, slots):
            self.assertEqual(slots, ["ble", "wifi"])
            status = next(preflights)
            events.append(
                "preflight-dot78"
                if status["version"] == source_version
                else "preflight-dot79"
            )
            return copy.deepcopy(status)

        scanner_campaign = mock.Mock(
            side_effect=AssertionError(
                "uncertain post-direct prepare reached scanner staging"
            )
        )
        finalize = mock.Mock(
            side_effect=AssertionError(
                "uncertain post-direct prepare finished update mode"
            )
        )

        with mock.patch.object(
            flash,
            "_new_update_session",
            side_effect=(self.SESSION, post_session),
        ), mock.patch.object(
            flash, "BadgeSerial", FakeBadge
        ), mock.patch.object(
            flash,
            "_prove_direct_bootstrap_source_after_rejection",
            side_effect=direct_reproof,
        ), mock.patch.object(
            flash,
            "wait_for_post_uplink_application",
            side_effect=wait_post,
        ), mock.patch.object(
            flash,
            "wait_for_scanner_status_usb",
            side_effect=scanner_preflight,
        ), mock.patch.object(
            flash,
            "_run_scanner_update_in_maintenance",
            scanner_campaign,
        ), mock.patch.object(
            flash,
            "_finalize_update_maintenance",
            finalize,
        ), self.assertRaises(flash.SerialTransportError) as caught:
            flash._usb_update_maintenance_flow(
                self._args(),
                platform,
                True,
                ["ble", "wifi"],
                target_version,
                initial_descriptor=descriptor,
                trusted_uplink_binding=binding,
                application_status=initial,
                frozen_artifacts=artifacts,
                scanner_image_size=len(scanner_data),
                legacy_bootstrap=False,
                _issue_scanner_flow_result=mock.Mock(
                    side_effect=AssertionError(
                        "uncertain post-direct prepare issued scanner proof"
                    )
                ),
            )

        self.assertIs(caught.exception, transition_error)
        scanner_campaign.assert_not_called()
        finalize.assert_not_called()
        self.assertLess(
            events.index("prepare-dot78-unsupported"),
            events.index("direct-uplink-upload"),
        )
        self.assertLess(
            events.index("preflight-dot79"),
            events.index("prepare-dot79-uncertain"),
        )
        self.assertEqual(events[events.index(
            "prepare-dot79-uncertain"
        ):], [
            "prepare-dot79-uncertain",
            "normal-reconnect",
            "maintenance-reconnect",
            "abort",
            "normal-reconnect",
            "context-close",
        ])

    def test_direct_bootstrap_rejects_all_nonlegacy_prepare_failures(
        self,
    ) -> None:
        source_version = (
            flash.UPDATE_MAINTENANCE_BOOTSTRAP_SOURCE_VERSION
        )
        target_version = (
            flash.UPDATE_MAINTENANCE_BOOTSTRAP_TARGET_VERSION
        )
        initial = _uplink_status(
            source_version,
            hardware_id=self.MAC,
            responses=20,
        )
        descriptor = _usb_record("/dev/cu.initial", self.MAC)
        binding = flash.TrustedUplinkBinding(
            serial_number=self.MAC,
            location=descriptor.location,
            source="operator-selection",
        )
        artifacts = _test_frozen_usb_artifacts(target_version)
        failures = (
            flash.SerialReadTimeout(
                "diagnostic activity",
                saw_activity=True,
                partial_frame=False,
            ),
            flash.SerialReadTimeout(
                "partial response",
                saw_activity=True,
                partial_frame=True,
            ),
            flash.SerialReadTimeout(
                "true silence",
                saw_activity=False,
                partial_frame=False,
            ),
            flash.SerialTransportError("malformed response"),
            flash.FlashError("explicit rejection"),
        )
        for failure in failures:
            class FakeBadge:
                def __init__(
                    inner_self,
                    opened_descriptor,
                    dry_run,
                    expected_hardware_id=None,
                ):
                    self.assertIs(opened_descriptor, descriptor)
                    self.assertIs(dry_run, False)
                    self.assertEqual(expected_hardware_id, self.MAC)

                def __enter__(inner_self):
                    return inner_self

                def __exit__(inner_self, *_args):
                    return None

                def prepare_update_maintenance(
                    inner_self,
                    _session: str,
                    *,
                    deadline: float,
                    source_supports_update_maintenance: bool,
                ) -> dict:
                    self.assertGreater(deadline, flash.time.monotonic())
                    self.assertIs(
                        source_supports_update_maintenance, False
                    )
                    raise failure

                def _prove_open_application(
                    inner_self, _timeout_s: float
                ) -> dict:
                    raise AssertionError(
                        "nonlegacy failure reached direct reproof"
                    )

                def upload_uplink_firmware(
                    inner_self, *_args, **_kwargs
                ) -> dict:
                    raise AssertionError(
                        "nonlegacy failure reached uplink mutation"
                    )

            with self.subTest(failure=failure), mock.patch.object(
                flash, "BadgeSerial", FakeBadge
            ), self.assertRaises(type(failure)) as caught:
                flash._usb_update_maintenance_flow(
                    self._args(),
                    flash.PLATFORMS["badge-trio-xiao-s3"],
                    True,
                    [],
                    target_version,
                    initial_descriptor=descriptor,
                    trusted_uplink_binding=binding,
                    application_status=initial,
                    frozen_artifacts=artifacts,
                    scanner_image_size=0,
                    legacy_bootstrap=False,
                    _issue_scanner_flow_result=mock.Mock(
                        side_effect=AssertionError(
                            "nonlegacy failure issued scanner result"
                        )
                    ),
                )
            self.assertIs(caught.exception, failure)

    def test_supported_prepare_rejection_never_enters_direct_bootstrap(
        self,
    ) -> None:
        initial = self._normal_status(responses=20)
        recovered = self._normal_status(responses=30)
        descriptor = _usb_record("/dev/cu.initial", self.MAC)
        binding = flash.TrustedUplinkBinding(
            serial_number=self.MAC,
            location=descriptor.location,
            source="operator-selection",
        )
        artifacts = _test_frozen_usb_artifacts(self.VERSION)
        primary = flash.UpdateMaintenanceUnsupportedError(
            "supported prepare response became ambiguous"
        )

        class FakeBadge:
            def __init__(
                inner_self,
                opened_descriptor,
                dry_run,
                expected_hardware_id=None,
            ):
                self.assertIs(opened_descriptor, descriptor)
                self.assertIs(dry_run, False)
                self.assertEqual(expected_hardware_id, self.MAC)
                inner_self._update_session = None

            def __enter__(inner_self):
                return inner_self

            def __exit__(inner_self, *_args):
                return None

            def prepare_update_maintenance(
                inner_self,
                session: str,
                *,
                deadline: float,
                source_supports_update_maintenance: bool,
            ) -> dict:
                self.assertGreater(deadline, flash.time.monotonic())
                self.assertIs(source_supports_update_maintenance, True)
                inner_self._update_session = session
                raise primary

            def reconnect_same_uplink_normal(
                inner_self, *, deadline: float
            ) -> dict:
                self.assertGreater(deadline, flash.time.monotonic())
                return copy.deepcopy(recovered)

        direct_bootstrap = mock.Mock(
            side_effect=AssertionError(
                "supported prepare rejection reached direct bootstrap"
            )
        )
        issue_result = mock.Mock(
            side_effect=AssertionError(
                "supported prepare rejection issued scanner result"
            )
        )
        with mock.patch.object(
            flash, "_new_update_session", return_value=self.SESSION
        ), mock.patch.object(
            flash, "BadgeSerial", FakeBadge
        ), mock.patch.object(
            flash,
            "_prove_direct_bootstrap_source_after_rejection",
            direct_bootstrap,
        ), self.assertRaises(
            flash.UpdateMaintenanceUnsupportedError
        ) as caught:
            flash._usb_update_maintenance_flow(
                self._args(),
                flash.PLATFORMS["badge-trio-xiao-s3"],
                True,
                [],
                self.VERSION,
                initial_descriptor=descriptor,
                trusted_uplink_binding=binding,
                application_status=initial,
                frozen_artifacts=artifacts,
                scanner_image_size=0,
                legacy_bootstrap=False,
                _issue_scanner_flow_result=issue_result,
            )

        self.assertIs(caught.exception, primary)
        direct_bootstrap.assert_not_called()
        issue_result.assert_not_called()

    def _exercise_maintenance_entry_failure(
        self,
        *,
        abort_error: BaseException | None,
        prepare_error: BaseException | None = None,
    ) -> tuple[BaseException, BaseException, list[str]]:
        initial = self._normal_status(responses=20)
        maintenance = self._maintenance_status(responses=30)
        recovered = self._normal_status(responses=40)
        preflight = copy.deepcopy(initial)
        preflight.update({
            "safe_mode": False,
            "usb_control_alive": True,
            "scanner_uart_alive": True,
            "scanners": [
                _scanner_status(
                    flash.PLATFORMS["badge-trio-xiao-s3"],
                    self.VERSION,
                    hardware_id="e0:72:a1:f9:48:58",
                    slot="ble",
                )["scanners"][0],
                _scanner_status(
                    flash.PLATFORMS["badge-trio-xiao-s3"],
                    self.VERSION,
                    hardware_id="e0:72:a1:f9:48:59",
                    slot="wifi",
                )["scanners"][0],
            ],
        })
        artifacts = _test_frozen_usb_artifacts(self.VERSION)
        descriptor = _usb_record("/dev/cu.initial", self.MAC)
        binding = flash.TrustedUplinkBinding(
            serial_number=self.MAC,
            location=descriptor.location,
            source="operator-selection",
        )
        primary = (
            prepare_error
            if prepare_error is not None
            else RuntimeError("first maintenance live gate failed")
        )
        events: list[str] = []
        normal_reconnects = 0

        class FakeBadge:
            def __init__(
                inner_self,
                opened_descriptor,
                dry_run,
                expected_hardware_id=None,
            ):
                self.assertIs(opened_descriptor, descriptor)
                self.assertIs(dry_run, False)
                self.assertEqual(expected_hardware_id, self.MAC)
                inner_self.expected_hardware_id = expected_hardware_id
                inner_self._update_session = None

            def __enter__(inner_self):
                return inner_self

            def __exit__(inner_self, *_args):
                events.append("close")
                return None

            def prepare_update_maintenance(
                inner_self,
                session: str,
                *,
                deadline: float,
                source_supports_update_maintenance: bool,
            ) -> dict:
                self.assertGreater(deadline, flash.time.monotonic())
                self.assertEqual(session, self.SESSION)
                self.assertIs(source_supports_update_maintenance, True)
                inner_self._update_session = session
                events.append("prepare")
                if prepare_error is not None:
                    raise prepare_error
                return {
                    "ok": True,
                    "phase": "rebooting",
                    "session": session,
                    "retryable": True,
                    "reboot_required": True,
                }

            def reconnect_same_uplink_recoverable_update(
                inner_self, *, deadline: float
            ) -> dict:
                self.assertGreater(deadline, flash.time.monotonic())
                events.append("maintenance-reconnect")
                return copy.deepcopy(maintenance)

            def reconnect_same_uplink(
                inner_self, *, deadline: float
            ) -> dict:
                return inner_self.reconnect_same_uplink_recoverable_update(
                    deadline=deadline
                )

            def reconnect_same_uplink_normal(
                inner_self, *, deadline: float
            ) -> dict:
                nonlocal normal_reconnects
                self.assertGreater(deadline, flash.time.monotonic())
                normal_reconnects += 1
                events.append("normal-reconnect")
                if normal_reconnects == 1:
                    raise flash.SerialTransportError(
                        "badge still in maintenance"
                    )
                return copy.deepcopy(recovered)

            def abort_update_maintenance(
                inner_self, *, deadline: float
            ) -> dict:
                self.assertGreater(deadline, flash.time.monotonic())
                events.append("abort")
                if abort_error is not None:
                    raise abort_error
                return {
                    "ok": True,
                    "phase": "aborting",
                    "session": self.SESSION,
                    "retryable": False,
                    "reboot_required": True,
                }

        def reject_first_maintenance(
            status: dict, session: str,
        ) -> None:
            self.assertEqual(status, maintenance)
            self.assertIsNot(status, maintenance)
            self.assertEqual(session, self.SESSION)
            events.append("validator")
            if prepare_error is not None:
                raise AssertionError(
                    "prepare uncertainty reached maintenance validator"
                )
            raise primary

        scanner_campaign = mock.Mock(
            side_effect=AssertionError(
                "first maintenance rejection reached scanner staging"
            )
        )
        finalize = mock.Mock(
            side_effect=AssertionError(
                "first maintenance rejection finished update mode"
            )
        )
        issue_result = mock.Mock(
            side_effect=AssertionError(
                "first maintenance rejection issued scanner proof"
            )
        )

        with mock.patch.object(
            flash, "_new_update_session", return_value=self.SESSION
        ), mock.patch.object(
            flash, "BadgeSerial", FakeBadge
        ), mock.patch.object(
            flash, "wait_for_scanner_status_usb", return_value=preflight
        ), mock.patch.object(
            flash,
            "_run_scanner_update_in_maintenance",
            scanner_campaign,
        ), mock.patch.object(
            flash,
            "_finalize_update_maintenance",
            finalize,
        ), self.assertRaises(RuntimeError) as caught:
            flash._usb_update_maintenance_flow(
                self._args(),
                flash.PLATFORMS["badge-trio-xiao-s3"],
                False,
                ["ble", "wifi"],
                self.VERSION,
                initial_descriptor=descriptor,
                trusted_uplink_binding=binding,
                application_status=initial,
                frozen_artifacts=artifacts,
                scanner_image_size=len(
                    flash._frozen_firmware_bytes(
                        artifacts.scanner, role="scanner"
                    )
                ),
                legacy_bootstrap=False,
                _issue_scanner_flow_result=issue_result,
                maintenance_status_validator=reject_first_maintenance,
            )

        scanner_campaign.assert_not_called()
        finalize.assert_not_called()
        issue_result.assert_not_called()
        return primary, caught.exception, events

    def test_first_maintenance_validator_failure_aborts_to_normal(
        self,
    ) -> None:
        primary, caught, events = \
            self._exercise_maintenance_entry_failure(
                abort_error=None,
            )

        self.assertIs(caught, primary)
        self.assertEqual(events, [
            "prepare",
            "maintenance-reconnect",
            "validator",
            "normal-reconnect",
            "maintenance-reconnect",
            "abort",
            "normal-reconnect",
            "close",
        ])

    def test_first_maintenance_validator_preserves_abort_uncertainty(
        self,
    ) -> None:
        abort_error = flash.FlashError("abort completion is uncertain")
        primary, caught, events = \
            self._exercise_maintenance_entry_failure(
                abort_error=abort_error,
            )

        self.assertIs(caught, primary)
        notes = getattr(caught, "__notes__", [])
        self.assertTrue(any(
            "abort receipt reconciliation" in note
            and "abort completion is uncertain" in note
            for note in notes
        ))
        self.assertEqual(events, [
            "prepare",
            "maintenance-reconnect",
            "validator",
            "normal-reconnect",
            "maintenance-reconnect",
            "abort",
            "normal-reconnect",
            "close",
        ])

    def test_supported_prepare_transition_uncertainty_aborts_to_normal(
        self,
    ) -> None:
        prepare_error = flash.SerialTransportError(
            "prepare transition receipt lost",
            terminal_unavailable=True,
        )
        primary, caught, events = \
            self._exercise_maintenance_entry_failure(
                abort_error=None,
                prepare_error=prepare_error,
            )

        self.assertIs(primary, prepare_error)
        self.assertIs(caught, prepare_error)
        self.assertEqual(events, [
            "prepare",
            "normal-reconnect",
            "maintenance-reconnect",
            "abort",
            "normal-reconnect",
            "close",
        ])

    def test_retry_recovery_returns_proven_normal_snapshot(
        self,
    ) -> None:
        descriptor = _usb_record("/dev/cu.recovered", self.MAC)
        recovered = self._normal_status(responses=40)
        primary = flash.ScannerCampaignFailure(
            session=self.SESSION,
            requested_slots=["ble"],
            stage_receipt={
                "target": "scanner-s3-combo-fof_badge",
                "sha256": "a" * 64,
                "size": 123,
                "slot_mask": 1,
                "generation": 7,
            },
            campaign={
                "generation": 7,
                "target_slot_mask": 1,
                "pending_mask": 0,
                "worker_running": False,
                "readiness_probes": [1, 0],
                "scanners": [
                    {"slot": 0, "state": "failed", "attempts": 1},
                    {"slot": 1, "state": "excluded", "attempts": 0},
                ],
            },
        )

        class FakeBadge:
            _descriptor = descriptor

            def reconnect_same_uplink_normal(
                inner_self, *, deadline: float
            ) -> dict:
                self.assertGreater(deadline, flash.time.monotonic())
                return copy.deepcopy(recovered)

        result = flash._restore_failed_update_maintenance(
            FakeBadge(),
            session=self.SESSION,
            persisted_game_state=("immune", "human", False, 0),
            primary=primary,
            reset_budget=flash._UpdateRetryResetBudget(),
        )

        self.assertEqual(result.action, "already_normal")
        self.assertIs(result.descriptor, descriptor)
        self.assertIs(result.usb_reset_used, False)
        self.assertEqual(result.status, recovered)
        self.assertIsNot(result.status, recovered)
        exposed = result.status
        exposed["game_seed"] = "infected"
        self.assertEqual(result.status["game_seed"], "immune")

    def test_retry_recovery_aborts_owned_session_and_returns_normal(
        self,
    ) -> None:
        descriptor = _usb_record("/dev/cu.recovered", self.MAC)
        maintenance = self._maintenance_status(responses=31)
        recovered = self._normal_status(responses=40)
        events: list[str] = []
        normal_calls = 0

        class FakeBadge:
            _descriptor = descriptor

            def reconnect_same_uplink_normal(
                inner_self, *, deadline: float
            ) -> dict:
                nonlocal normal_calls
                normal_calls += 1
                events.append("normal")
                if normal_calls == 1:
                    raise flash.FlashError("still in maintenance")
                return copy.deepcopy(recovered)

            def reconnect_same_uplink_recoverable_update(
                inner_self, *, deadline: float
            ) -> dict:
                events.append("owned")
                return copy.deepcopy(maintenance)

            def abort_update_maintenance(
                inner_self, *, deadline: float
            ) -> dict:
                events.append("abort")
                return {
                    "ok": True,
                    "phase": "aborting",
                    "session": self.SESSION,
                    "retryable": False,
                    "reboot_required": True,
                }

        result = flash._restore_failed_update_maintenance(
            FakeBadge(),
            session=self.SESSION,
            persisted_game_state=("immune", "human", False, 0),
            primary=flash.FlashError("retryable scanner campaign"),
            reset_budget=flash._UpdateRetryResetBudget(),
        )

        self.assertEqual(events, ["normal", "owned", "abort", "normal"])
        self.assertEqual(result.action, "session_abort")
        self.assertEqual(result.status, recovered)
        self.assertIs(result.usb_reset_used, False)

    def test_retry_recovery_uses_one_bound_nonwriting_reset(
        self,
    ) -> None:
        descriptor = _usb_record("/dev/cu.recovered", self.MAC)
        maintenance = self._maintenance_status(responses=31)
        recovered = self._normal_status(responses=40)
        reset_budget = flash._UpdateRetryResetBudget()
        normal_calls = 0

        class FakeBadge:
            _descriptor = descriptor

            def reconnect_same_uplink_normal(
                inner_self, *, deadline: float
            ) -> dict:
                nonlocal normal_calls
                normal_calls += 1
                raise flash.FlashError("maintenance marker still active")

            def reconnect_same_uplink_recoverable_update(
                inner_self, *, deadline: float
            ) -> dict:
                return copy.deepcopy(maintenance)

            def abort_update_maintenance(
                inner_self, *, deadline: float
            ) -> dict:
                raise flash.FlashError(
                    "update lifecycle session conflict"
                )

        reset = mock.Mock(return_value=(
            descriptor,
            copy.deepcopy(recovered),
        ))
        with mock.patch.object(
            flash,
            "_reset_bound_uplink_without_write",
            reset,
        ):
            result = flash._restore_failed_update_maintenance(
                FakeBadge(),
                session=self.SESSION,
                persisted_game_state=("immune", "human", False, 0),
                primary=flash.FlashError("retryable scanner campaign"),
                reset_budget=reset_budget,
            )

        reset.assert_called_once()
        self.assertEqual(normal_calls, 2)
        self.assertEqual(result.action, "usb_reset")
        self.assertIs(result.usb_reset_used, True)
        self.assertIs(reset_budget.used, True)
        self.assertEqual(result.status, recovered)

        with self.assertRaisesRegex(
            flash.FlashError, "already consumed"
        ):
            flash._restore_failed_update_maintenance(
                FakeBadge(),
                session=self.SESSION,
                persisted_game_state=("immune", "human", False, 0),
                primary=flash.FlashError("second retryable campaign"),
                reset_budget=reset_budget,
            )
        reset.assert_called_once()

    def test_malformed_recovered_normal_status_preserves_primary(
        self,
    ) -> None:
        primary = RuntimeError("scanner campaign primary")
        events: list[str] = []

        class FakeBadge:
            def reconnect_same_uplink_normal(
                inner_self, *, deadline: float
            ) -> dict:
                self.assertGreater(deadline, flash.time.monotonic())
                events.append("normal-reconnect")
                return {"malformed": "normal status"}

            def reconnect_same_uplink(
                inner_self, *, deadline: float
            ) -> dict:
                raise AssertionError(
                    "malformed recovered normal status attempted abort"
                )

            def abort_update_maintenance(
                inner_self, *, deadline: float
            ) -> dict:
                raise AssertionError(
                    "malformed recovered normal status issued abort"
                )

        with self.assertRaises(RuntimeError) as caught:
            flash._recover_failed_update_maintenance(
                FakeBadge(),
                session=self.SESSION,
                persisted_game_state=(
                    "immune", "human", False, 0,
                ),
                primary=primary,
            )

        self.assertIs(caught.exception, primary)
        self.assertEqual(events, ["normal-reconnect"])
        self.assertTrue(any(
            "normal-mode failure proof" in note
            for note in getattr(caught.exception, "__notes__", [])
        ))

    def test_preparing_status_is_never_normal_and_binds_exact_session(
        self,
    ) -> None:
        preparing = _update_preparing_status(
            self.VERSION,
            session=self.SESSION,
            hardware_id=self.MAC,
        )

        with self.assertRaises(flash.FlashError):
            flash.validate_uplink_application_status(preparing)
        actual = flash._validate_update_preparing_status(
            preparing,
            session=self.SESSION,
            expected_hardware_id=self.MAC,
        )
        self.assertEqual(actual, preparing)
        self.assertIsNot(actual, preparing)

        for mutation in (
            lambda value: value.update({
                "update_session": "FEDCBA9876543210",
            }),
            lambda value: value.update({
                "recovery_mode": "normal",
            }),
            lambda value: value.pop("update_session"),
        ):
            malformed = copy.deepcopy(preparing)
            mutation(malformed)
            with self.assertRaises(flash.FlashError):
                flash._validate_update_preparing_status(
                    malformed,
                    session=self.SESSION,
                    expected_hardware_id=self.MAC,
                )

    def test_failed_prepare_aborts_exact_preparing_marker_to_normal(
        self,
    ) -> None:
        primary = flash.SerialTransportError(
            "prepare receipt uncertain",
            terminal_unavailable=True,
        )
        preparing = _update_preparing_status(
            self.VERSION,
            session=self.SESSION,
            hardware_id=self.MAC,
            responses=31,
        )
        recovered = self._normal_status(responses=40)
        events: list[str] = []
        normal_attempts = 0

        class FakeBadge:
            def reconnect_same_uplink_normal(
                inner_self, *, deadline: float
            ) -> dict:
                nonlocal normal_attempts
                self.assertGreater(deadline, flash.time.monotonic())
                normal_attempts += 1
                events.append("normal")
                if normal_attempts == 1:
                    raise flash.FlashError(
                        "owned PREPARING marker is not normal"
                    )
                return copy.deepcopy(recovered)

            def reconnect_same_uplink_recoverable_update(
                inner_self, *, deadline: float
            ) -> dict:
                self.assertGreater(deadline, flash.time.monotonic())
                events.append("owned-update")
                return copy.deepcopy(preparing)

            def abort_update_maintenance(
                inner_self, *, deadline: float
            ) -> dict:
                self.assertGreater(deadline, flash.time.monotonic())
                events.append("abort")
                return {
                    "ok": True,
                    "phase": "aborting",
                    "session": self.SESSION,
                    "retryable": False,
                    "reboot_required": True,
                }

        with self.assertRaises(flash.SerialTransportError) as caught:
            flash._recover_failed_update_maintenance(
                FakeBadge(),
                session=self.SESSION,
                persisted_game_state=(
                    "immune", "human", False, 0,
                ),
                primary=primary,
            )

        self.assertIs(caught.exception, primary)
        self.assertEqual(events, [
            "normal", "owned-update", "abort", "normal",
        ])

    def test_failed_prepare_never_aborts_wrong_preparing_session(
        self,
    ) -> None:
        primary = flash.SerialTransportError(
            "prepare receipt uncertain",
            terminal_unavailable=True,
        )
        wrong = _update_preparing_status(
            self.VERSION,
            session="FEDCBA9876543210",
            hardware_id=self.MAC,
            responses=31,
        )
        abort = mock.Mock(
            side_effect=AssertionError(
                "wrong PREPARING session reached abort"
            )
        )

        class FakeBadge:
            def reconnect_same_uplink_normal(
                inner_self, *, deadline: float
            ) -> dict:
                raise flash.FlashError(
                    "owned PREPARING marker is not normal"
                )

            def reconnect_same_uplink_recoverable_update(
                inner_self, *, deadline: float
            ) -> dict:
                return copy.deepcopy(wrong)

            abort_update_maintenance = abort

        with self.assertRaises(flash.SerialTransportError) as caught:
            flash._recover_failed_update_maintenance(
                FakeBadge(),
                session=self.SESSION,
                persisted_game_state=None,
                primary=primary,
            )

        self.assertIs(caught.exception, primary)
        abort.assert_not_called()
        self.assertTrue(any(
            "bounded update-mode abort fallback" in note
            for note in getattr(primary, "__notes__", [])
        ))

    def test_lost_abort_receipt_reconciles_normal_before_failing(
        self,
    ) -> None:
        primary = RuntimeError("maintenance gate primary")
        abort_uncertainty = flash.SerialTransportError(
            "abort receipt lost",
            terminal_unavailable=True,
        )
        maintenance = self._maintenance_status(responses=31)
        recovered = self._normal_status(responses=40)
        events: list[str] = []
        normal_attempts = 0

        class FakeBadge:
            def reconnect_same_uplink_normal(
                inner_self, *, deadline: float
            ) -> dict:
                nonlocal normal_attempts
                normal_attempts += 1
                events.append("normal")
                if normal_attempts == 1:
                    raise flash.FlashError("still in update mode")
                return copy.deepcopy(recovered)

            def reconnect_same_uplink_recoverable_update(
                inner_self, *, deadline: float
            ) -> dict:
                events.append("owned-update")
                return copy.deepcopy(maintenance)

            def abort_update_maintenance(
                inner_self, *, deadline: float
            ) -> dict:
                events.append("abort")
                raise abort_uncertainty

        with self.assertRaises(RuntimeError) as caught:
            flash._recover_failed_update_maintenance(
                FakeBadge(),
                session=self.SESSION,
                persisted_game_state=(
                    "immune", "human", False, 0,
                ),
                primary=primary,
            )

        self.assertIs(caught.exception, primary)
        self.assertEqual(events, [
            "normal", "owned-update", "abort", "normal",
        ])
        self.assertTrue(any(
            "abort receipt reconciliation" in note
            and "abort receipt lost" in note
            for note in getattr(primary, "__notes__", [])
        ))

    def test_lost_abort_receipt_and_normal_proof_keep_both_notes(
        self,
    ) -> None:
        primary = RuntimeError("maintenance gate primary")
        maintenance = self._maintenance_status(responses=31)
        abort_uncertainty = flash.SerialTransportError(
            "abort receipt lost",
            terminal_unavailable=True,
        )
        proof_uncertainty = flash.SerialTransportError(
            "normal proof lost",
            terminal_unavailable=True,
        )
        normal_attempts = 0

        class FakeBadge:
            def reconnect_same_uplink_normal(
                inner_self, *, deadline: float
            ) -> dict:
                nonlocal normal_attempts
                normal_attempts += 1
                if normal_attempts == 1:
                    raise flash.FlashError("still in update mode")
                raise proof_uncertainty

            def reconnect_same_uplink_recoverable_update(
                inner_self, *, deadline: float
            ) -> dict:
                return copy.deepcopy(maintenance)

            def abort_update_maintenance(
                inner_self, *, deadline: float
            ) -> dict:
                raise abort_uncertainty

        with self.assertRaises(RuntimeError) as caught:
            flash._recover_failed_update_maintenance(
                FakeBadge(),
                session=self.SESSION,
                persisted_game_state=None,
                primary=primary,
            )

        self.assertIs(caught.exception, primary)
        notes = getattr(primary, "__notes__", [])
        self.assertTrue(any(
            "abort receipt reconciliation" in note
            and "abort receipt lost" in note
            for note in notes
        ))
        self.assertTrue(any(
            "post-abort normal-mode proof" in note
            and "normal proof lost" in note
            for note in notes
        ))

    def test_failed_campaign_uses_bounded_abort_and_preserves_primary(
        self,
    ) -> None:
        primary = flash.FlashError("scanner campaign failed")
        recovered = self._normal_status(responses=40)
        events: list[tuple[str, float | str]] = []

        class FakeBadge:
            def reconnect_same_uplink_normal(
                inner_self, *, deadline: float
            ) -> dict:
                events.append(("normal", deadline))
                if len([event for event in events if event[0] == "normal"]) == 1:
                    raise flash.SerialTransportError("still rebooting")
                return copy.deepcopy(recovered)

            def reconnect_same_uplink_recoverable_update(
                inner_self, *, deadline: float
            ) -> dict:
                events.append(("maintenance", deadline))
                return self._maintenance_status(responses=31)

            def abort_update_maintenance(
                inner_self, *, deadline: float
            ) -> dict:
                events.append(("abort", deadline))
                return {
                    "ok": True,
                    "phase": "aborting",
                    "session": self.SESSION,
                    "retryable": False,
                    "reboot_required": True,
                }

        with mock.patch.object(
            flash.time, "monotonic", return_value=100.0
        ), self.assertRaises(flash.FlashError) as caught:
            flash._recover_failed_update_maintenance(
                FakeBadge(),
                session=self.SESSION,
                persisted_game_state=(
                    "immune", "human", False, 0,
                ),
                primary=primary,
            )

        self.assertIs(caught.exception, primary)
        self.assertEqual([name for name, _deadline in events], [
            "normal", "maintenance", "abort", "normal",
        ])
        self.assertTrue(all(
            100.0 < deadline <= 115.0
            for _name, deadline in events
        ))

    def test_scanner_campaign_uses_real_minimal_maintenance_status(
        self,
    ) -> None:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        old_version = "0.64.78-badge-defcon34"
        preflight = _uplink_status(
            self.VERSION,
            hardware_id=self.MAC,
            responses=20,
        )
        preflight.update({
            "safe_mode": False,
            "usb_control_alive": True,
            "scanner_uart_alive": True,
            "scanners": [
                _scanner_status(
                    platform,
                    old_version,
                    hardware_id="e0:72:a1:f9:48:58",
                    slot="ble",
                )["scanners"][0],
                _scanner_status(
                    platform,
                    old_version,
                    hardware_id="e0:72:a1:f9:48:59",
                    slot="wifi",
                )["scanners"][0],
            ],
        })
        maintenance = self._maintenance_status(responses=25)
        maintenance["update_campaign"] = {
            "generation": 9,
            "target_slot_mask": 3,
            "pending_mask": 0,
            "worker_running": False,
            "readiness_probes": [1, 1],
            "scanners": [
                {"slot": 0, "state": "converged", "attempts": 1},
                {"slot": 1, "state": "converged", "attempts": 1},
            ],
        }
        self.assertNotIn("scanners", {
            key: value for key, value in maintenance.items()
            if key != "update_campaign"
        })
        self.assertNotIn("firmware_store", maintenance)
        scanner_data = flash._frozen_firmware_bytes(
            _test_frozen_usb_artifacts(self.VERSION).scanner,
            role="scanner",
        )
        stage_receipt = _stage_receipt(
            platform,
            self.VERSION,
            scanner_data,
            3,
            generation=9,
        )
        maintenance["update_scanner"] = {
            "phase": "committed",
            "session": self.SESSION,
            "target": stage_receipt["target"],
            "sha256": stage_receipt["sha256"],
            "size": stage_receipt["size"],
            "slot_mask": stage_receipt["slot_mask"],
            "received": stage_receipt["size"],
            "generation": stage_receipt["generation"],
        }

        class FakeBadge:
            expected_hardware_id = self.MAC

            def stage_scanner_firmware(
                inner_self, *_args, **_kwargs
            ) -> dict:
                return copy.deepcopy(stage_receipt)

            def status(
                inner_self, *, timeout_s: float = 5
            ) -> dict:
                self.assertGreater(timeout_s, 0)
                return copy.deepcopy(maintenance)

        result = flash._run_scanner_update_in_maintenance(
            FakeBadge(),
            platform=platform,
            artifacts=_test_frozen_usb_artifacts(self.VERSION).scanner,
            version=self.VERSION,
            slots=["ble", "wifi"],
            scanner_image_size=len(scanner_data),
            session=self.SESSION,
            recovery_rewrite_same_version=False,
            skip_command_probe=False,
            preflight_status=preflight,
            deadline=10**12,
        )

        self.assertEqual(result[0], preflight)
        self.assertEqual(result[2], stage_receipt)
        self.assertEqual(result[3], frozenset({"ble", "wifi"}))
        self.assertEqual(result[5], {
            "ble": "e0:72:a1:f9:48:58",
            "wifi": "e0:72:a1:f9:48:59",
        })

    def test_compact_campaign_treats_newer_skip_as_terminal_failure(
        self,
    ) -> None:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        scanner_data = flash._frozen_firmware_bytes(
            _test_frozen_usb_artifacts(self.VERSION).scanner,
            role="scanner",
        )
        stage_receipt = _stage_receipt(
            platform,
            self.VERSION,
            scanner_data,
            3,
            generation=11,
        )
        maintenance = self._maintenance_status(responses=25)
        maintenance["update_scanner"] = {
            "phase": "committed",
            "session": self.SESSION,
            "target": stage_receipt["target"],
            "sha256": stage_receipt["sha256"],
            "size": stage_receipt["size"],
            "slot_mask": stage_receipt["slot_mask"],
            "received": stage_receipt["size"],
            "generation": stage_receipt["generation"],
        }
        maintenance["update_campaign"] = {
            "generation": 11,
            "target_slot_mask": 3,
            "pending_mask": 0,
            "worker_running": False,
            "readiness_probes": [1, 1],
            "scanners": [
                {"slot": 0, "state": "newer_skipped", "attempts": 1},
                {"slot": 1, "state": "current", "attempts": 0},
            ],
        }

        class FakeBadge:
            expected_hardware_id = self.MAC

            def status(
                inner_self, *, timeout_s: float = 5
            ) -> dict:
                self.assertGreater(timeout_s, 0)
                return copy.deepcopy(maintenance)

        with self.assertRaisesRegex(
            flash.FlashError, "newer_skipped"
        ):
            flash._wait_for_maintenance_scanner_campaign(
                FakeBadge(),
                session=self.SESSION,
                stage_receipt=stage_receipt,
                slots=["ble", "wifi"],
                required_converged_slots=set(),
                deadline=10**12,
            )

    def _campaign_failure_fixture(
        self,
        *,
        generation: int,
        ble_state: str,
        ble_attempts: int,
        wifi_state: str,
        wifi_attempts: int,
        readiness_probes: list[int],
    ) -> tuple[dict, dict]:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        scanner_data = flash._frozen_firmware_bytes(
            _test_frozen_usb_artifacts(self.VERSION).scanner,
            role="scanner",
        )
        stage_receipt = _stage_receipt(
            platform,
            self.VERSION,
            scanner_data,
            3,
            generation=generation,
        )
        maintenance = self._maintenance_status(responses=25)
        maintenance["update_scanner"] = {
            "phase": "committed",
            "session": self.SESSION,
            "target": stage_receipt["target"],
            "sha256": stage_receipt["sha256"],
            "size": stage_receipt["size"],
            "slot_mask": stage_receipt["slot_mask"],
            "received": stage_receipt["size"],
            "generation": stage_receipt["generation"],
        }
        maintenance["update_campaign"] = {
            "generation": generation,
            "target_slot_mask": 3,
            "pending_mask": 0,
            "worker_running": False,
            "readiness_probes": list(readiness_probes),
            "scanners": [
                {
                    "slot": 0,
                    "state": ble_state,
                    "attempts": ble_attempts,
                },
                {
                    "slot": 1,
                    "state": wifi_state,
                    "attempts": wifi_attempts,
                },
            ],
        }
        return stage_receipt, maintenance

    def test_zero_attempt_campaign_exposes_typed_retry_evidence(
        self,
    ) -> None:
        stage_receipt, maintenance = self._campaign_failure_fixture(
            generation=13,
            ble_state="converged",
            ble_attempts=1,
            wifi_state="failed",
            wifi_attempts=0,
            readiness_probes=[1, flash.UPDATE_READINESS_MAX_PROBES],
        )
        controls: list[dict] = []

        class FakeBadge:
            expected_hardware_id = self.MAC

            def status(
                inner_self, *, timeout_s: float = 5
            ) -> dict:
                self.assertGreater(timeout_s, 0)
                return copy.deepcopy(maintenance)

            def ctl(
                inner_self,
                payload: dict,
                prefix: str = "FOF_CTL_OK:",
                timeout_s: float = 30,
            ) -> dict:
                controls.append(copy.deepcopy(payload))
                self.assertEqual(prefix, "FOF_CTL_OK:")
                self.assertGreater(timeout_s, 0)
                return {
                    "message": "firmware check requested",
                    "uart": "wifi",
                    "ble_sent": False,
                    "wifi_sent": True,
                    "deferred": False,
                    "error": "",
                }

        with mock.patch.object(
            flash,
            "UPDATE_ZERO_ATTEMPT_REPROMPT_GRACE_S",
            0.0,
        ), self.assertRaises(
            flash.ScannerCampaignFailure
        ) as caught:
            flash._wait_for_maintenance_scanner_campaign(
                FakeBadge(),
                session=self.SESSION,
                stage_receipt=stage_receipt,
                slots=["ble", "wifi"],
                required_converged_slots={"ble", "wifi"},
                deadline=10**12,
            )

        failure = caught.exception
        self.assertEqual(failure.session, self.SESSION)
        self.assertEqual(
            failure.requested_slots, frozenset({"ble", "wifi"})
        )
        self.assertEqual(failure.failed_slots, frozenset({"wifi"}))
        self.assertEqual(failure.successful_slots, frozenset({"ble"}))
        self.assertEqual(failure.stage_receipt, stage_receipt)
        self.assertIsNot(failure.stage_receipt, stage_receipt)
        self.assertEqual(
            failure.campaign, maintenance["update_campaign"]
        )
        self.assertIsNot(
            failure.campaign, maintenance["update_campaign"]
        )
        self.assertEqual(controls, [
            {"cmd": "fw_check_now", "uart": "wifi"},
        ])

        exposed = failure.campaign
        exposed["scanners"][1]["state"] = "converged"
        self.assertEqual(
            failure.campaign["scanners"][1]["state"], "failed"
        )

    def test_attempted_campaign_exposes_typed_retry_evidence(
        self,
    ) -> None:
        stage_receipt, maintenance = self._campaign_failure_fixture(
            generation=14,
            ble_state="failed",
            ble_attempts=3,
            wifi_state="converged",
            wifi_attempts=1,
            readiness_probes=[1, 1],
        )

        class FakeBadge:
            expected_hardware_id = self.MAC

            def status(
                inner_self, *, timeout_s: float = 5
            ) -> dict:
                self.assertGreater(timeout_s, 0)
                return copy.deepcopy(maintenance)

            def ctl(
                inner_self, *_args, **_kwargs
            ) -> dict:
                raise AssertionError(
                    "attempted relay failure must not use zero-attempt reprompt"
                )

        with self.assertRaises(
            flash.ScannerCampaignFailure
        ) as caught:
            flash._wait_for_maintenance_scanner_campaign(
                FakeBadge(),
                session=self.SESSION,
                stage_receipt=stage_receipt,
                slots=["ble", "wifi"],
                required_converged_slots={"ble", "wifi"},
                deadline=10**12,
            )

        failure = caught.exception
        self.assertEqual(failure.failed_slots, frozenset({"ble"}))
        self.assertEqual(failure.successful_slots, frozenset({"wifi"}))
        self.assertEqual(
            failure.campaign["scanners"],
            [
                {"slot": 0, "state": "failed", "attempts": 3},
                {"slot": 1, "state": "converged", "attempts": 1},
            ],
        )

    def _scanner_retry_normal_status(
        self,
        *,
        failed_slot: str,
        last_fw_error: str = "",
        last_relay_error: str = "",
        fw_state: str = "",
        fw_backoff_s: int = 0,
    ) -> tuple[dict, dict[str, str]]:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        identities = {
            "ble": "e0:72:a1:f9:48:58",
            "wifi": "e0:72:a1:f9:48:59",
        }
        scanner_entries = []
        for slot in ("ble", "wifi"):
            version = (
                "0.64.78-badge-defcon34"
                if slot == failed_slot
                else self.VERSION
            )
            entry = _scanner_status(
                platform,
                version,
                hardware_id=identities[slot],
                slot=slot,
            )["scanners"][0]
            if slot == failed_slot:
                entry.update({
                    "last_fw_error": last_fw_error,
                    "last_relay_error": last_relay_error,
                    "fw_state": fw_state,
                    "fw_backoff_s": fw_backoff_s,
                })
            scanner_entries.append(entry)
        status = self._normal_status(responses=40)
        status.update({
            "safe_mode": False,
            "usb_control_alive": True,
            "scanner_uart_alive": True,
            "scanners": scanner_entries,
        })
        return status, identities

    @staticmethod
    def _scanner_identity_placeholder(status: dict) -> dict:
        placeholder = copy.deepcopy(status)
        for entry in placeholder["scanners"]:
            entry.update({
                "firmware_name": "",
                "app_project": "",
                "hardware_type": "",
                "hardware_id": "",
                "ver": "",
                "version": "",
            })
        return placeholder

    def test_retry_classifier_accepts_only_known_transients(
        self,
    ) -> None:
        cases = (
            (
                "readiness_exhausted",
                "wifi",
                0,
                [1, flash.UPDATE_READINESS_MAX_PROBES],
                {},
            ),
            (
                "ota_ack_timeout",
                "ble",
                3,
                [1, 1],
                {"last_relay_error": "ota_ack_timeout"},
            ),
            (
                "offer_manifest_mismatch",
                "wifi",
                3,
                [1, 1],
                {
                    "last_fw_error": "offer_manifest_mismatch",
                    "fw_state": "deferred",
                    "fw_backoff_s": 120,
                },
            ),
            (
                "deferred_backoff",
                "ble",
                2,
                [1, 1],
                {
                    "fw_state": "deferred",
                    "fw_backoff_s": 90,
                },
            ),
        )
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        for reason, failed_slot, attempts, probes, fields in cases:
            with self.subTest(reason=reason):
                stage_receipt, maintenance = \
                    self._campaign_failure_fixture(
                        generation=20 + attempts,
                        ble_state=(
                            "failed" if failed_slot == "ble"
                            else "converged"
                        ),
                        ble_attempts=(
                            attempts if failed_slot == "ble" else 1
                        ),
                        wifi_state=(
                            "failed" if failed_slot == "wifi"
                            else "converged"
                        ),
                        wifi_attempts=(
                            attempts if failed_slot == "wifi" else 1
                        ),
                        readiness_probes=probes,
                    )
                failure = flash.ScannerCampaignFailure(
                    session=self.SESSION,
                    requested_slots=["ble", "wifi"],
                    stage_receipt=stage_receipt,
                    campaign=maintenance["update_campaign"],
                )
                normal, identities = self._scanner_retry_normal_status(
                    failed_slot=failed_slot,
                    **fields,
                )
                decision = flash._classify_scanner_campaign_retry(
                    failure,
                    status=normal,
                    platform=platform,
                    target_version=self.VERSION,
                    expected_hardware_ids=identities,
                )
                self.assertEqual(decision.slot, failed_slot)
                self.assertEqual(decision.reason, reason)
                self.assertEqual(
                    decision.scanner_hardware_id,
                    identities[failed_slot],
                )
                self.assertEqual(
                    decision.successful_slots,
                    frozenset(
                        {"ble", "wifi"} - {failed_slot}
                    ),
                )

    def test_retry_classifier_rejects_unknown_or_changed_identity(
        self,
    ) -> None:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        stage_receipt, maintenance = self._campaign_failure_fixture(
            generation=30,
            ble_state="failed",
            ble_attempts=3,
            wifi_state="converged",
            wifi_attempts=1,
            readiness_probes=[1, 1],
        )
        failure = flash.ScannerCampaignFailure(
            session=self.SESSION,
            requested_slots=["ble", "wifi"],
            stage_receipt=stage_receipt,
            campaign=maintenance["update_campaign"],
        )
        normal, identities = self._scanner_retry_normal_status(
            failed_slot="ble",
            last_fw_error="unknown_radio_fault",
        )
        with self.assertRaisesRegex(
            flash.FlashError, "not a recognized transient"
        ):
            flash._classify_scanner_campaign_retry(
                failure,
                status=normal,
                platform=platform,
                target_version=self.VERSION,
                expected_hardware_ids=identities,
            )

        changed = copy.deepcopy(normal)
        changed["scanners"][0]["hardware_id"] = \
            "e0:72:a1:f9:48:99"
        changed["scanners"][0]["last_fw_error"] = \
            "offer_manifest_mismatch"
        with self.assertRaisesRegex(
            flash.FlashError, "hardware id mismatch"
        ):
            flash._classify_scanner_campaign_retry(
                failure,
                status=changed,
                platform=platform,
                target_version=self.VERSION,
                expected_hardware_ids=identities,
            )

        incomplete_campaign = copy.deepcopy(
            maintenance["update_campaign"]
        )
        incomplete_campaign["scanners"][1]["state"] = "awaiting_check"
        incomplete_campaign["pending_mask"] = 2
        incomplete_failure = flash.ScannerCampaignFailure(
            session=self.SESSION,
            requested_slots=["ble", "wifi"],
            stage_receipt=stage_receipt,
            campaign=incomplete_campaign,
        )
        normal["scanners"][0]["last_fw_error"] = \
            "offer_manifest_mismatch"
        with self.assertRaisesRegex(
            flash.FlashError, "peer lanes did not converge"
        ):
            flash._classify_scanner_campaign_retry(
                incomplete_failure,
                status=normal,
                platform=platform,
                target_version=self.VERSION,
                expected_hardware_ids=identities,
            )

    def test_exact_lane_scanner_recovery_reboots_and_reproves_identity(
        self,
    ) -> None:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        status, identities = self._scanner_retry_normal_status(
            failed_slot="wifi",
            last_fw_error="offer_manifest_mismatch",
            fw_state="deferred",
            fw_backoff_s=120,
        )
        old_version = status["scanners"][1]["ver"]
        badge = flash.BadgeSerial(
            _usb_record("/dev/cu.recovery", self.MAC),
            False,
            expected_hardware_id=self.MAC,
        )
        badge.ctl = mock.Mock(return_value={  # type: ignore[method-assign]
            "message": "scanner safe mode command sent",
            "ble_sent": False,
            "wifi_sent": True,
            "enabled": False,
            "reboot_required": True,
        })
        badge.status = mock.Mock(  # type: ignore[method-assign]
            return_value=copy.deepcopy(status)
        )
        waited = mock.Mock()

        with mock.patch.object(
            flash,
            "wait_for_scanners_usb",
            waited,
        ):
            recovered = badge.recover_scanner_lane(
                "wifi",
                platform=platform,
                expected_hardware_id=identities["wifi"],
                expected_version=old_version,
                deadline=flash.time.monotonic() + 60,
            )

        badge.ctl.assert_called_once_with(
            {
                "cmd": "scanner_recovery",
                "uart": "wifi",
                "enabled": False,
            },
            timeout_s=5.0,
        )
        waited.assert_called_once()
        wait_args = waited.call_args
        self.assertIs(wait_args.args[0], badge)
        self.assertEqual(wait_args.args[2], ["wifi"])
        self.assertEqual(wait_args.args[3], old_version)
        self.assertEqual(
            wait_args.kwargs["expected_hardware_ids"],
            {"wifi": identities["wifi"]},
        )
        self.assertIs(wait_args.kwargs["require_auto_update"], False)
        self.assertEqual(recovered, status)
        self.assertIsNot(recovered, status)

    def test_exact_lane_scanner_recovery_rejects_ambiguous_receipt(
        self,
    ) -> None:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        base = {
            "message": "scanner safe mode command sent",
            "ble_sent": True,
            "wifi_sent": False,
            "enabled": False,
            "reboot_required": True,
        }
        cases = {
            "peer also sent": {"wifi_sent": True},
            "selected not sent": {"ble_sent": False},
            "enabled": {"enabled": True},
            "no reboot": {"reboot_required": False},
            "wrong message": {"message": "ok"},
            "extra field": {"unexpected": True},
        }
        for name, mutation in cases.items():
            receipt = dict(base)
            receipt.update(mutation)
            badge = flash.BadgeSerial(
                _usb_record("/dev/cu.recovery", self.MAC),
                False,
                expected_hardware_id=self.MAC,
            )
            badge.ctl = mock.Mock(  # type: ignore[method-assign]
                return_value=receipt
            )
            waited = mock.Mock()
            with self.subTest(name=name), mock.patch.object(
                flash,
                "wait_for_scanners_usb",
                waited,
            ), self.assertRaisesRegex(
                flash.FlashError, "recovery receipt"
            ):
                badge.recover_scanner_lane(
                    "ble",
                    platform=platform,
                    expected_hardware_id="e0:72:a1:f9:48:58",
                    expected_version="0.64.78-badge-defcon34",
                    deadline=flash.time.monotonic() + 60,
                )
            waited.assert_not_called()

    def _single_lane_retry_failure(
        self,
        *,
        session: str,
        slot: str,
        generation: int,
    ) -> flash.ScannerCampaignFailure:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        scanner_data = flash._frozen_firmware_bytes(
            _test_frozen_usb_artifacts(self.VERSION).scanner,
            role="scanner",
        )
        mask = 1 if slot == "ble" else 2
        stage_receipt = _stage_receipt(
            platform,
            self.VERSION,
            scanner_data,
            mask,
            generation=generation,
        )
        selected_id = 0 if slot == "ble" else 1
        campaign = {
            "generation": generation,
            "target_slot_mask": mask,
            "pending_mask": 0,
            "worker_running": False,
            "readiness_probes": [1, 1],
            "scanners": [
                {
                    "slot": scanner_id,
                    "state": (
                        "failed"
                        if scanner_id == selected_id
                        else "excluded"
                    ),
                    "attempts": 3 if scanner_id == selected_id else 0,
                }
                for scanner_id in (0, 1)
            ],
        }
        return flash.ScannerCampaignFailure(
            session=session,
            requested_slots=[slot],
            stage_receipt=stage_receipt,
            campaign=campaign,
        )

    def _single_lane_retry_success(
        self,
        *,
        session: str,
        slot: str,
        generation: int,
        preflight: dict,
        identities: dict[str, str],
    ) -> tuple:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        scanner_data = flash._frozen_firmware_bytes(
            _test_frozen_usb_artifacts(self.VERSION).scanner,
            role="scanner",
        )
        mask = 1 if slot == "ble" else 2
        stage_receipt = _stage_receipt(
            platform,
            self.VERSION,
            scanner_data,
            mask,
            generation=generation,
        )
        maintenance = _update_maintenance_status(
            self.VERSION,
            session=session,
            hardware_id=self.MAC,
            responses=70 + generation,
        )
        maintenance.update({
            "game_seed": "immune",
            "game_state": "human",
            "game_active": False,
            "game_shield": 0,
        })
        maintenance["update_scanner"] = {
            "phase": "committed",
            "session": session,
            "target": stage_receipt["target"],
            "sha256": stage_receipt["sha256"],
            "size": stage_receipt["size"],
            "slot_mask": mask,
            "received": stage_receipt["size"],
            "generation": generation,
        }
        selected_id = 0 if slot == "ble" else 1
        maintenance["update_campaign"] = {
            "generation": generation,
            "target_slot_mask": mask,
            "pending_mask": 0,
            "worker_running": False,
            "readiness_probes": [1, 1],
            "scanners": [
                {
                    "slot": scanner_id,
                    "state": (
                        "converged"
                        if scanner_id == selected_id
                        else "excluded"
                    ),
                    "attempts": 1 if scanner_id == selected_id else 0,
                }
                for scanner_id in (0, 1)
            ],
        }
        return (
            copy.deepcopy(preflight),
            maintenance,
            stage_receipt,
            frozenset({slot}),
            frozenset(),
            {slot: identities[slot]},
        )

    def test_retry_coordinator_preserves_peer_and_retries_failed_lane(
        self,
    ) -> None:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        artifacts = _test_frozen_usb_artifacts(self.VERSION)
        stage_receipt, maintenance = self._campaign_failure_fixture(
            generation=31,
            ble_state="converged",
            ble_attempts=1,
            wifi_state="failed",
            wifi_attempts=3,
            readiness_probes=[1, 1],
        )
        first_failure = flash.ScannerCampaignFailure(
            session=self.SESSION,
            requested_slots=["ble", "wifi"],
            stage_receipt=stage_receipt,
            campaign=maintenance["update_campaign"],
        )
        normal, identities = self._scanner_retry_normal_status(
            failed_slot="wifi",
            last_fw_error="offer_manifest_mismatch",
            fw_state="deferred",
            fw_backoff_s=120,
        )
        retry_session = "FEDCBA9876543210"
        retry_result = self._single_lane_retry_success(
            session=retry_session,
            slot="wifi",
            generation=32,
            preflight=normal,
            identities=identities,
        )
        descriptor = _usb_record("/dev/cu.retry", self.MAC)

        class FakeBadge:
            expected_hardware_id = self.MAC
            _descriptor = descriptor
            _update_session = self.SESSION

            def __init__(inner_self) -> None:
                inner_self.recovered: list[str] = []
                inner_self.prepared: list[str] = []

            def status(inner_self) -> dict:
                return copy.deepcopy(normal)

            def recover_scanner_lane(
                inner_self, slot: str, **kwargs
            ) -> dict:
                self.assertEqual(slot, "wifi")
                self.assertEqual(
                    kwargs["expected_hardware_id"],
                    identities["wifi"],
                )
                inner_self.recovered.append(slot)
                return copy.deepcopy(normal)

            def prepare_update_maintenance(
                inner_self,
                session: str,
                *,
                deadline: float,
                source_supports_update_maintenance: bool,
            ) -> dict:
                self.assertGreater(deadline, flash.time.monotonic())
                self.assertIs(source_supports_update_maintenance, True)
                inner_self._update_session = session
                inner_self.prepared.append(session)
                return {
                    "ok": True,
                    "phase": "rebooting",
                    "session": session,
                    "retryable": True,
                    "reboot_required": True,
                }

            def reconnect_same_uplink(
                inner_self, *, deadline: float
            ) -> dict:
                self.assertGreater(deadline, flash.time.monotonic())
                result = _update_maintenance_status(
                    self.VERSION,
                    session=inner_self._update_session,
                    hardware_id=self.MAC,
                    responses=60,
                )
                result.update({
                    "game_seed": "immune",
                    "game_state": "human",
                    "game_active": False,
                    "game_shield": 0,
                })
                return result

        badge = FakeBadge()
        recovery = flash._issue_update_maintenance_recovery_result(
            badge=badge,
            status=normal,
            action="session_abort",
            usb_reset_used=False,
            require_descriptor=True,
        )
        run_campaign = mock.Mock(return_value=retry_result)
        with mock.patch.object(
            flash,
            "_restore_failed_update_maintenance",
            return_value=recovery,
        ) as restore, mock.patch.object(
            flash,
            "_new_update_session",
            return_value=retry_session,
        ), mock.patch.object(
            flash,
            "_wait_for_maintenance_uplink_target",
        ), mock.patch.object(
            flash,
            "_run_scanner_update_in_maintenance",
            run_campaign,
        ):
            sequence = flash._retry_failed_scanner_campaigns(
                badge,
                failure=first_failure,
                platform=platform,
                artifacts=artifacts.scanner,
                version=self.VERSION,
                original_slots=["ble", "wifi"],
                original_hardware_ids=identities,
                first_preflight_status=normal,
                scanner_image_size=len(
                    flash._frozen_firmware_bytes(
                        artifacts.scanner,
                        role="scanner",
                    )
                ),
                persisted_game_state=flash._capture_persisted_game_state(
                    normal
                ),
                expected_uplink_partition="ota_0",
                deadline=flash.time.monotonic() + 300,
                recovery_rewrite_same_version=False,
                skip_command_probe=False,
            )

        self.assertEqual(sequence.session, retry_session)
        self.assertEqual(sequence.latest_slots, ("wifi",))
        self.assertEqual(badge.recovered, ["wifi"])
        self.assertEqual(badge.prepared, [retry_session])
        restore.assert_called_once()
        self.assertEqual(
            run_campaign.call_args.kwargs["slots"], ["wifi"]
        )
        self.assertEqual(
            run_campaign.call_args.kwargs["preflight_status"], normal
        )
        self.assertEqual(
            [entry["outcome"] for entry in sequence.attempt_history],
            ["failed", "converged"],
        )
        self.assertEqual(
            sequence.attempt_history[0]["recovery_action"],
            "session_abort",
        )
        self.assertEqual(
            sequence.attempt_history[1]["requested_slots"], ["wifi"]
        )

    def test_retry_coordinator_waits_out_recovery_identity_placeholders(
        self,
    ) -> None:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        artifacts = _test_frozen_usb_artifacts(self.VERSION)
        stage_receipt, maintenance = self._campaign_failure_fixture(
            generation=33,
            ble_state="converged",
            ble_attempts=1,
            wifi_state="failed",
            wifi_attempts=3,
            readiness_probes=[1, 1],
        )
        first_failure = flash.ScannerCampaignFailure(
            session=self.SESSION,
            requested_slots=["ble", "wifi"],
            stage_receipt=stage_receipt,
            campaign=maintenance["update_campaign"],
        )
        original_campaign = first_failure.campaign
        normal, identities = self._scanner_retry_normal_status(
            failed_slot="wifi",
            last_fw_error="offer_manifest_mismatch",
            fw_state="deferred",
            fw_backoff_s=120,
        )
        placeholder = self._scanner_identity_placeholder(normal)
        partially_ready = copy.deepcopy(placeholder)
        partially_ready["scanners"][1] = copy.deepcopy(
            normal["scanners"][1]
        )
        retry_session = "FEDCBA9876543210"
        retry_result = self._single_lane_retry_success(
            session=retry_session,
            slot="wifi",
            generation=34,
            preflight=normal,
            identities=identities,
        )
        badge = mock.Mock()
        badge.expected_hardware_id = self.MAC
        badge._descriptor = _usb_record("/dev/cu.retry-wait", self.MAC)
        badge._update_session = self.SESSION
        badge.status.return_value = copy.deepcopy(normal)
        badge.recover_scanner_lane.return_value = copy.deepcopy(normal)
        badge.prepare_update_maintenance.return_value = {
            "ok": True,
            "phase": "rebooting",
            "session": retry_session,
            "retryable": True,
            "reboot_required": True,
        }
        retry_maintenance = _update_maintenance_status(
            self.VERSION,
            session=retry_session,
            hardware_id=self.MAC,
            responses=60,
        )
        retry_maintenance.update({
            "game_seed": "immune",
            "game_state": "human",
            "game_active": False,
            "game_shield": 0,
        })
        badge.reconnect_same_uplink.return_value = retry_maintenance
        recovery = flash._issue_update_maintenance_recovery_result(
            badge=badge,
            status=partially_ready,
            action="session_abort",
            usb_reset_used=False,
            require_descriptor=True,
        )
        run_campaign = mock.Mock(return_value=retry_result)

        with mock.patch.object(
            flash,
            "_restore_failed_update_maintenance",
            return_value=recovery,
        ), mock.patch.object(
            flash,
            "_new_update_session",
            return_value=retry_session,
        ), mock.patch.object(
            flash,
            "_wait_for_maintenance_uplink_target",
        ), mock.patch.object(
            flash,
            "_run_scanner_update_in_maintenance",
            run_campaign,
        ), mock.patch.object(
            flash.time,
            "sleep",
        ):
            sequence = flash._retry_failed_scanner_campaigns(
                badge,
                failure=first_failure,
                platform=platform,
                artifacts=artifacts.scanner,
                version=self.VERSION,
                original_slots=["ble", "wifi"],
                original_hardware_ids=identities,
                first_preflight_status=normal,
                scanner_image_size=len(
                    flash._frozen_firmware_bytes(
                        artifacts.scanner,
                        role="scanner",
                    )
                ),
                persisted_game_state=flash._capture_persisted_game_state(
                    normal
                ),
                expected_uplink_partition="ota_0",
                deadline=flash.time.monotonic() + 300,
                recovery_rewrite_same_version=False,
                skip_command_probe=False,
            )

        self.assertEqual(badge.status.call_count, 1)
        self.assertEqual(sequence.latest_slots, ("wifi",))
        self.assertEqual(
            run_campaign.call_args.kwargs["preflight_status"], normal
        )
        self.assertEqual(
            sequence.attempt_history[0]["campaign"], original_campaign
        )
        self.assertEqual(first_failure.campaign, original_campaign)

    def test_retry_coordinator_rejects_hard_recovery_evidence_before_poll(
        self,
    ) -> None:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        artifacts = _test_frozen_usb_artifacts(self.VERSION)
        stage_receipt, maintenance = self._campaign_failure_fixture(
            generation=35,
            ble_state="converged",
            ble_attempts=1,
            wifi_state="failed",
            wifi_attempts=3,
            readiness_probes=[1, 1],
        )
        first_failure = flash.ScannerCampaignFailure(
            session=self.SESSION,
            requested_slots=["ble", "wifi"],
            stage_receipt=stage_receipt,
            campaign=maintenance["update_campaign"],
        )
        normal, identities = self._scanner_retry_normal_status(
            failed_slot="wifi",
            last_fw_error="offer_manifest_mismatch",
            fw_state="deferred",
            fw_backoff_s=120,
        )
        cases = {
            "disconnected lane": lambda status: status["scanners"][0].update({
                "connected": False,
            }),
            "complete wrong identity": (
                lambda status: status["scanners"][0].update({
                    "hardware_id": "e0:72:a1:f9:48:99",
                })
            ),
            "partial identity": lambda status: status["scanners"][0].update({
                "hardware_id": "",
            }),
            "unsafe scanner state": (
                lambda status: status["scanners"][0].update({
                    "rollback_pending": True,
                })
            ),
            "uplink version drift": lambda status: status.update({
                "version": "0.64.80-badge-defcon34",
            }),
        }

        for name, mutate in cases.items():
            with self.subTest(name=name):
                invalid = copy.deepcopy(normal)
                mutate(invalid)
                badge = mock.Mock()
                badge.expected_hardware_id = self.MAC
                badge._descriptor = _usb_record(
                    f"/dev/cu.retry-hard-{len(name)}", self.MAC
                )
                badge.status.return_value = copy.deepcopy(normal)
                badge.recover_scanner_lane.side_effect = flash.FlashError(
                    "retry mutation reached after hard recovery evidence"
                )
                recovery = flash._issue_update_maintenance_recovery_result(
                    badge=badge,
                    status=invalid,
                    action="session_abort",
                    usb_reset_used=False,
                    require_descriptor=True,
                )

                with mock.patch.object(
                    flash,
                    "_restore_failed_update_maintenance",
                    return_value=recovery,
                ), self.assertRaises(flash.FlashError):
                    flash._retry_failed_scanner_campaigns(
                        badge,
                        failure=first_failure,
                        platform=platform,
                        artifacts=artifacts.scanner,
                        version=self.VERSION,
                        original_slots=["ble", "wifi"],
                        original_hardware_ids=identities,
                        first_preflight_status=normal,
                        scanner_image_size=len(
                            flash._frozen_firmware_bytes(
                                artifacts.scanner,
                                role="scanner",
                            )
                        ),
                        persisted_game_state=(
                            flash._capture_persisted_game_state(normal)
                        ),
                        expected_uplink_partition="ota_0",
                        deadline=flash.time.monotonic() + 300,
                        recovery_rewrite_same_version=False,
                        skip_command_probe=False,
                    )

                badge.status.assert_not_called()
                badge.recover_scanner_lane.assert_not_called()
                badge.prepare_update_maintenance.assert_not_called()
                badge.stage_scanner_firmware.assert_not_called()
                badge.relay_scanner.assert_not_called()

    def test_retry_coordinator_placeholder_timeout_fails_before_mutation(
        self,
    ) -> None:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        artifacts = _test_frozen_usb_artifacts(self.VERSION)
        stage_receipt, maintenance = self._campaign_failure_fixture(
            generation=35,
            ble_state="converged",
            ble_attempts=1,
            wifi_state="failed",
            wifi_attempts=3,
            readiness_probes=[1, 1],
        )
        first_failure = flash.ScannerCampaignFailure(
            session=self.SESSION,
            requested_slots=["ble", "wifi"],
            stage_receipt=stage_receipt,
            campaign=maintenance["update_campaign"],
        )
        normal, identities = self._scanner_retry_normal_status(
            failed_slot="wifi",
            last_fw_error="offer_manifest_mismatch",
            fw_state="deferred",
            fw_backoff_s=120,
        )
        placeholder = self._scanner_identity_placeholder(normal)
        badge = mock.Mock()
        badge.expected_hardware_id = self.MAC
        badge._descriptor = _usb_record("/dev/cu.retry-timeout", self.MAC)
        badge.status.side_effect = lambda: copy.deepcopy(placeholder)
        recovery = flash._issue_update_maintenance_recovery_result(
            badge=badge,
            status=placeholder,
            action="session_abort",
            usb_reset_used=False,
            require_descriptor=True,
        )
        original_campaign = first_failure.campaign
        original_stage = first_failure.stage_receipt
        clock = SimpleNamespace(monotonic=100.0, wall=1000.0)
        status_budgets: list[float] = []
        sleep_budgets: list[float] = []
        run_campaign = mock.Mock(
            side_effect=AssertionError("staging reached after timeout")
        )
        new_session = mock.Mock(
            side_effect=AssertionError("new maintenance session created")
        )

        def blocking_status(timeout_s: float = 5) -> dict:
            status_budgets.append(timeout_s)
            clock.monotonic += timeout_s
            clock.wall -= 0.25
            return copy.deepcopy(placeholder)

        badge.status.side_effect = blocking_status

        def advance(seconds: float) -> None:
            sleep_budgets.append(seconds)
            clock.monotonic += seconds
            clock.wall += seconds

        with self.assertRaisesRegex(
            flash.FlashError,
            "scanner identities were not ready before retry deadline",
        ) as caught:
            with mock.patch.object(
                flash,
                "_restore_failed_update_maintenance",
                return_value=recovery,
            ), mock.patch.object(
                flash,
                "_new_update_session",
                new_session,
            ), mock.patch.object(
                flash,
                "_run_scanner_update_in_maintenance",
                run_campaign,
            ), mock.patch.object(
                flash.time,
                "monotonic",
                side_effect=lambda: clock.monotonic,
            ), mock.patch.object(
                flash.time,
                "time",
                side_effect=lambda: clock.wall,
            ), mock.patch.object(
                flash.time,
                "sleep",
                side_effect=advance,
            ):
                flash._retry_failed_scanner_campaigns(
                    badge,
                    failure=first_failure,
                    platform=platform,
                    artifacts=artifacts.scanner,
                    version=self.VERSION,
                    original_slots=["ble", "wifi"],
                    original_hardware_ids=identities,
                    first_preflight_status=normal,
                    scanner_image_size=len(
                        flash._frozen_firmware_bytes(
                            artifacts.scanner,
                            role="scanner",
                        )
                    ),
                    persisted_game_state=flash._capture_persisted_game_state(
                        normal
                    ),
                    expected_uplink_partition="ota_0",
                    deadline=103.0,
                    recovery_rewrite_same_version=False,
                    skip_command_probe=False,
                )

        self.assertEqual(len(status_budgets), 1)
        self.assertLessEqual(status_budgets[0], 3.0)
        self.assertLessEqual(clock.monotonic, 103.0)
        self.assertEqual(sleep_budgets, [])
        self.assertIs(caught.exception.__cause__, first_failure)
        self.assertEqual(first_failure.campaign, original_campaign)
        self.assertEqual(first_failure.stage_receipt, original_stage)
        badge.recover_scanner_lane.assert_not_called()
        badge.prepare_update_maintenance.assert_not_called()
        badge.stage_scanner_firmware.assert_not_called()
        badge.relay_scanner.assert_not_called()
        new_session.assert_not_called()
        run_campaign.assert_not_called()

    def test_retry_coordinator_stops_after_three_campaigns_per_lane(
        self,
    ) -> None:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        artifacts = _test_frozen_usb_artifacts(self.VERSION)
        stage_receipt, maintenance = self._campaign_failure_fixture(
            generation=41,
            ble_state="converged",
            ble_attempts=1,
            wifi_state="failed",
            wifi_attempts=3,
            readiness_probes=[1, 1],
        )
        failures = [
            flash.ScannerCampaignFailure(
                session=self.SESSION,
                requested_slots=["ble", "wifi"],
                stage_receipt=stage_receipt,
                campaign=maintenance["update_campaign"],
            ),
            self._single_lane_retry_failure(
                session="1111111111111111",
                slot="wifi",
                generation=42,
            ),
            self._single_lane_retry_failure(
                session="2222222222222222",
                slot="wifi",
                generation=43,
            ),
        ]
        normal, identities = self._scanner_retry_normal_status(
            failed_slot="wifi",
            last_relay_error="ota_ack_timeout",
        )
        descriptor = _usb_record("/dev/cu.retry", self.MAC)

        class FakeBadge:
            expected_hardware_id = self.MAC
            _descriptor = descriptor
            _update_session = self.SESSION

            def __init__(inner_self) -> None:
                inner_self.recovered: list[str] = []
                inner_self.prepared: list[str] = []

            def status(inner_self) -> dict:
                return copy.deepcopy(normal)

            def recover_scanner_lane(
                inner_self, slot: str, **_kwargs
            ) -> dict:
                inner_self.recovered.append(slot)
                return copy.deepcopy(normal)

            def prepare_update_maintenance(
                inner_self, session: str, **_kwargs
            ) -> dict:
                inner_self._update_session = session
                inner_self.prepared.append(session)
                return {
                    "ok": True,
                    "phase": "rebooting",
                    "session": session,
                    "retryable": True,
                    "reboot_required": True,
                }

            def reconnect_same_uplink(
                inner_self, *, deadline: float
            ) -> dict:
                result = _update_maintenance_status(
                    self.VERSION,
                    session=inner_self._update_session,
                    hardware_id=self.MAC,
                    responses=60,
                )
                result.update({
                    "game_seed": "immune",
                    "game_state": "human",
                    "game_active": False,
                    "game_shield": 0,
                })
                return result

        badge = FakeBadge()

        def recovered_result(*_args, **_kwargs):
            return flash._issue_update_maintenance_recovery_result(
                badge=badge,
                status=normal,
                action="already_normal",
                usb_reset_used=False,
                require_descriptor=True,
            )

        with mock.patch.object(
            flash,
            "_restore_failed_update_maintenance",
            side_effect=recovered_result,
        ) as restore, mock.patch.object(
            flash,
            "_new_update_session",
            side_effect=("1111111111111111", "2222222222222222"),
        ), mock.patch.object(
            flash,
            "_wait_for_maintenance_uplink_target",
        ), mock.patch.object(
            flash,
            "_run_scanner_update_in_maintenance",
            side_effect=failures[1:],
        ) as run_campaign, self.assertRaisesRegex(
            flash.FlashError, "three host campaigns"
        ) as caught:
            flash._retry_failed_scanner_campaigns(
                badge,
                failure=failures[0],
                platform=platform,
                artifacts=artifacts.scanner,
                version=self.VERSION,
                original_slots=["ble", "wifi"],
                original_hardware_ids=identities,
                first_preflight_status=normal,
                scanner_image_size=len(
                    flash._frozen_firmware_bytes(
                        artifacts.scanner,
                        role="scanner",
                    )
                ),
                persisted_game_state=flash._capture_persisted_game_state(
                    normal
                ),
                expected_uplink_partition="ota_0",
                deadline=flash.time.monotonic() + 300,
                recovery_rewrite_same_version=False,
                skip_command_probe=False,
            )

        self.assertEqual(run_campaign.call_count, 2)
        self.assertEqual(restore.call_count, 3)
        self.assertEqual(badge.recovered, ["wifi", "wifi"])
        self.assertEqual(
            badge.prepared,
            ["1111111111111111", "2222222222222222"],
        )
        self.assertEqual(
            len(caught.exception.attempt_history), 3
        )

    def test_retry_coordinator_stops_before_mutation_on_peer_regression(
        self,
    ) -> None:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        artifacts = _test_frozen_usb_artifacts(self.VERSION)
        stage_receipt, maintenance = self._campaign_failure_fixture(
            generation=51,
            ble_state="converged",
            ble_attempts=1,
            wifi_state="failed",
            wifi_attempts=3,
            readiness_probes=[1, 1],
        )
        failure = flash.ScannerCampaignFailure(
            session=self.SESSION,
            requested_slots=["ble", "wifi"],
            stage_receipt=stage_receipt,
            campaign=maintenance["update_campaign"],
        )
        normal, identities = self._scanner_retry_normal_status(
            failed_slot="wifi",
            last_fw_error="offer_manifest_mismatch",
        )
        normal["scanners"][0]["hardware_id"] = \
            "e0:72:a1:f9:48:99"
        badge = mock.Mock()
        badge.expected_hardware_id = self.MAC
        badge._descriptor = _usb_record("/dev/cu.retry", self.MAC)
        badge.status.return_value = copy.deepcopy(normal)
        recovery = flash._issue_update_maintenance_recovery_result(
            badge=badge,
            status=normal,
            action="already_normal",
            usb_reset_used=False,
            require_descriptor=True,
        )

        with mock.patch.object(
            flash,
            "_restore_failed_update_maintenance",
            return_value=recovery,
        ), self.assertRaisesRegex(
            flash.FlashError, "hardware id mismatch"
        ):
            flash._retry_failed_scanner_campaigns(
                badge,
                failure=failure,
                platform=platform,
                artifacts=artifacts.scanner,
                version=self.VERSION,
                original_slots=["ble", "wifi"],
                original_hardware_ids=identities,
                first_preflight_status=normal,
                scanner_image_size=1,
                persisted_game_state=flash._capture_persisted_game_state(
                    normal
                ),
                expected_uplink_partition="ota_0",
                deadline=flash.time.monotonic() + 300,
                recovery_rewrite_same_version=False,
                skip_command_probe=False,
            )

        badge.recover_scanner_lane.assert_not_called()
        badge.prepare_update_maintenance.assert_not_called()

    def test_busy_peer_defers_one_targeted_reprompt_and_stale_snapshot(
        self,
    ) -> None:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        scanner_data = flash._frozen_firmware_bytes(
            _test_frozen_usb_artifacts(self.VERSION).scanner,
            role="scanner",
        )
        stage_receipt = _stage_receipt(
            platform,
            self.VERSION,
            scanner_data,
            3,
            generation=12,
        )
        failed = self._maintenance_status(responses=25)
        failed["update_scanner"] = {
            "phase": "committed",
            "session": self.SESSION,
            "target": stage_receipt["target"],
            "sha256": stage_receipt["sha256"],
            "size": stage_receipt["size"],
            "slot_mask": stage_receipt["slot_mask"],
            "received": stage_receipt["size"],
            "generation": stage_receipt["generation"],
        }
        failed["update_campaign"] = {
            "generation": 12,
            "target_slot_mask": 3,
            "pending_mask": 0,
            "worker_running": False,
            "readiness_probes": [1, 3],
            "scanners": [
                {"slot": 0, "state": "converged", "attempts": 1},
                {"slot": 1, "state": "failed", "attempts": 0},
            ],
        }
        busy = copy.deepcopy(failed)
        busy["update_campaign"] = {
            "generation": 12,
            "target_slot_mask": 3,
            "pending_mask": 0,
            "worker_running": True,
            "readiness_probes": [3, 2],
            "scanners": [
                {"slot": 0, "state": "failed", "attempts": 0},
                {"slot": 1, "state": "relaying", "attempts": 1},
            ],
        }
        terminal = copy.deepcopy(failed)
        terminal["update_campaign"] = {
            "generation": 12,
            "target_slot_mask": 3,
            "pending_mask": 0,
            "worker_running": False,
            "readiness_probes": [3, 2],
            "scanners": [
                {"slot": 0, "state": "failed", "attempts": 0},
                {"slot": 1, "state": "converged", "attempts": 1},
            ],
        }
        recovered = copy.deepcopy(terminal)
        recovered["update_campaign"] = {
            "generation": 12,
            "target_slot_mask": 3,
            "pending_mask": 0,
            "worker_running": False,
            "readiness_probes": [1, 0],
            "scanners": [
                {"slot": 0, "state": "converged", "attempts": 1},
                {"slot": 1, "state": "converged", "attempts": 1},
            ],
        }
        controls: list[dict] = []
        status_calls = 0

        class FakeBadge:
            expected_hardware_id = self.MAC

            def __init__(inner_self) -> None:
                # The coordinator update is asynchronous.  One status poll
                # can still expose the durable pre-reprompt snapshot.
                inner_self.statuses = [
                    busy, terminal, terminal, recovered,
                ]

            def status(
                inner_self, *, timeout_s: float = 5
            ) -> dict:
                nonlocal status_calls
                self.assertGreater(timeout_s, 0)
                status_calls += 1
                return copy.deepcopy(inner_self.statuses.pop(0))

            def ctl(
                inner_self, payload: dict,
                prefix: str = "FOF_CTL_OK:",
                timeout_s: float = 30,
            ) -> dict:
                controls.append(copy.deepcopy(payload))
                self.assertEqual(status_calls, 2)
                self.assertEqual(prefix, "FOF_CTL_OK:")
                self.assertGreater(timeout_s, 0)
                self.assertLessEqual(timeout_s, 5)
                return {
                    "message": "firmware check requested",
                    "uart": "ble",
                    "ble_sent": True,
                    "wifi_sent": False,
                    "deferred": False,
                    "error": "",
                }

        result = flash._wait_for_maintenance_scanner_campaign(
            FakeBadge(),
            session=self.SESSION,
            stage_receipt=stage_receipt,
            slots=["ble", "wifi"],
            required_converged_slots={"ble", "wifi"},
            deadline=10**12,
        )

        self.assertEqual(result, recovered)
        self.assertEqual(controls, [
            {"cmd": "fw_check_now", "uart": "ble"},
        ])


class ReversibleUsbThemeControlTests(unittest.TestCase):
    VERSION = "0.64.76-badge-defcon34"
    OLD_VERSION = "0.64.75-badge-defcon34"
    MAC = "e0:72:a1:f9:47:fc"
    PORT = "/dev/cu.theme"

    def setUp(self) -> None:
        self.frozen_usb_artifacts = _test_frozen_usb_artifacts(self.VERSION)
        artifact_patcher = mock.patch.object(
            flash,
            "_prepare_frozen_usb_firmware_artifacts",
            return_value=self.frozen_usb_artifacts,
        )
        artifact_patcher.start()
        self.addCleanup(artifact_patcher.stop)
        attestation_patcher = mock.patch.object(
            flash,
            "_attest_frozen_uplink_flash_authority",
        )
        attestation_patcher.start()
        self.addCleanup(attestation_patcher.stop)

        def selected_descriptor(
            *,
            selected_port: str | None,
            operator_acknowledged: bool,
            trusted_binding=None,
        ):
            del operator_acknowledged, trusted_binding
            record = _usb_record(selected_port or self.PORT, self.MAC)
            return record, flash.TrustedUplinkBinding(
                serial_number=record.serial_number,
                location=None,
                source="operator-selection",
            )

        patcher = mock.patch.object(
            flash,
            "select_trusted_uplink_descriptor",
            side_effect=selected_descriptor,
        )
        patcher.start()
        self.addCleanup(patcher.stop)

    @classmethod
    def _theme(cls, *, brightness: int = 100,
               palette: str = "field",
               background: str = "dark",
               accents: dict | None = None) -> dict:
        return _test_badge_theme(
            brightness=brightness,
            palette=palette,
            background=background,
            accents=accents,
        )

    @classmethod
    def _status(cls, *, theme: dict | None = None,
                responses: int = 20,
                version: str | None = None,
                partition: str = "ota_0",
                hardware_id: str | None = None) -> dict:
        status = _uplink_status(
            version or cls.VERSION,
            hardware_id=hardware_id or cls.MAC,
            partition=partition,
            responses=responses,
        )
        chosen = json.loads(json.dumps(theme or cls._theme()))
        status["theme"] = chosen
        status["theme_hash"] = _test_badge_theme_hash(chosen)
        return status

    @classmethod
    def _expectation(cls) -> flash._PostUplinkExpectation:
        return flash._PostUplinkExpectation(
            expected_hardware_id=cls.MAC,
            expected_version=cls.VERSION,
            expected_partition="ota_0",
            expected_sha256="",
            expected_size=0,
            pre_version=cls.VERSION,
            pre_partition="ota_0",
            mutation_expected=False,
            source="current",
            update_session="0123456789ABCDEF",
        )

    @classmethod
    def _evidence(cls, status: dict) -> flash.PostUplinkApplicationEvidence:
        return flash.verify_post_uplink_application(
            status,
            expected_hardware_id=cls.MAC,
            expected_version=cls.VERSION,
            expected_partition="ota_0",
        )

    @staticmethod
    def _ack(theme: dict) -> dict:
        return {
            "message": "badge theme updated",
            "theme_hash": _test_badge_theme_hash(theme),
            "persisted": False,
            "reboot_required": False,
        }

    class ScriptedBadge:
        def __init__(self, ctl_results: list[object], statuses: list[object]):
            self.ctl_results = list(ctl_results)
            self.statuses = list(statuses)
            self.commands: list[dict] = []
            self.events: list[str] = []

        def ctl(self, payload: dict, *args, **kwargs) -> dict:
            self.events.append("ctl")
            self.commands.append(json.loads(json.dumps(payload)))
            result = self.ctl_results.pop(0)
            if isinstance(result, BaseException):
                raise result
            return result

        def status(self, *args, **kwargs) -> dict:
            self.events.append("status")
            result = self.statuses.pop(0)
            if isinstance(result, BaseException):
                raise result
            return result

        def upload_uplink_firmware(self, *args, **kwargs):
            raise AssertionError("theme proof attempted firmware upload")

        def stage_scanner_firmware(self, *args, **kwargs):
            raise AssertionError("theme proof attempted scanner stage")

        def relay_scanner(self, *args, **kwargs):
            raise AssertionError("theme proof attempted scanner relay")

    def test_firmware_theme_hash_vectors_and_mixed_case(self) -> None:
        self.assertEqual(
            flash._badge_theme_hash(self._theme(brightness=100)),
            3282709133,
        )
        self.assertEqual(
            flash._badge_theme_hash(self._theme(brightness=99)),
            1134970802,
        )
        mixed = self._theme(
            brightness=77,
            palette="NeOn",
            background="ScanLine",
            accents=dict(zip(
                _THEME_ACCENT_ORDER,
                (0, 1, 0x1234, 0xFFFF, 0xABCD, 42),
            )),
        )
        self.assertEqual(flash._badge_theme_hash(mixed), 3406521279)

    def test_theme_snapshot_is_frozen_exact_and_case_preserving(self) -> None:
        theme = self._theme(
            brightness=77, palette="NeOn", background="ScanLine"
        )
        snapshot = flash._snapshot_badge_theme(self._status(theme=theme))
        self.assertTrue(
            flash._BadgeThemeSnapshot.__dataclass_params__.frozen
        )
        self.assertEqual(flash._badge_theme_payload(snapshot), theme)
        with self.assertRaises(dataclasses.FrozenInstanceError):
            snapshot.brightness = 50
        payload = flash._badge_theme_payload(snapshot)
        payload["accents"]["drone"] = 0
        self.assertEqual(flash._badge_theme_payload(snapshot), theme)

    def test_snapshot_rejects_overlong_accents_and_contract_sets_are_immutable(
        self,
    ) -> None:
        snapshot = flash._snapshot_badge_theme(self._status())
        forged = dataclasses.replace(
            snapshot, accents=snapshot.accents + (0x1234,)
        )
        with self.assertRaises(flash.FlashError):
            flash._badge_theme_payload(forged)
        for contract in (
            flash.BADGE_THEME_KEYS,
            flash.BADGE_THEME_PALETTES,
            flash.BADGE_THEME_BACKGROUNDS,
            flash.BADGE_THEME_ACK_KEYS,
        ):
            self.assertIsInstance(contract, frozenset)

    def test_malformed_theme_status_causes_zero_control(self) -> None:
        class DictSubclass(dict):
            pass

        def with_theme(change) -> dict:
            status = self._status()
            change(status)
            return status

        malformed = [
            with_theme(lambda s: s.pop("theme")),
            with_theme(lambda s: s["theme"].update({"extra": 1})),
            with_theme(lambda s: s.__setitem__("theme", [])),
            with_theme(lambda s: s.__setitem__("theme", DictSubclass(s["theme"]))),
            with_theme(lambda s: s["theme"].pop("version")),
            with_theme(lambda s: s["theme"].__setitem__("version", True)),
            with_theme(lambda s: s["theme"].__setitem__("version", 2)),
            with_theme(lambda s: s["theme"].__setitem__("brightness", True)),
            with_theme(lambda s: s["theme"].__setitem__("brightness", 24)),
            with_theme(lambda s: s["theme"].__setitem__("brightness", 101)),
            with_theme(lambda s: s["theme"].__setitem__("palette", "other")),
            with_theme(lambda s: s["theme"].__setitem__("palette", 1)),
            with_theme(lambda s: s["theme"].__setitem__("background", "other")),
            with_theme(lambda s: s["theme"].__setitem__("background", 1)),
            with_theme(lambda s: s["theme"].__setitem__("accents", [])),
            with_theme(lambda s: s["theme"].__setitem__(
                "accents", DictSubclass(s["theme"]["accents"])
            )),
            with_theme(lambda s: s["theme"]["accents"].pop("clear")),
            with_theme(lambda s: s["theme"]["accents"].update({"extra": 1})),
            with_theme(lambda s: s["theme"]["accents"].__setitem__(
                "DRONE", s["theme"]["accents"].pop("drone")
            )),
            with_theme(lambda s: s["theme"]["accents"].__setitem__("drone", True)),
            with_theme(lambda s: s["theme"]["accents"].__setitem__("drone", -1)),
            with_theme(lambda s: s["theme"]["accents"].__setitem__("drone", 65536)),
            with_theme(lambda s: s.pop("theme_hash")),
            with_theme(lambda s: s.__setitem__("theme_hash", True)),
            with_theme(lambda s: s.__setitem__("theme_hash", -1)),
            with_theme(lambda s: s.__setitem__("theme_hash", 0x100000000)),
            with_theme(lambda s: s.__setitem__("theme_hash", 1)),
        ]
        expectation = self._expectation()
        for index, status in enumerate(malformed):
            badge = self.ScriptedBadge([], [])
            with self.subTest(index=index), self.assertRaises(flash.FlashError):
                flash._prove_reversible_usb_theme_control(
                    badge,
                    initial_status=status,
                    expectation=expectation,
                    initial_evidence=self._evidence(status),
                )
            self.assertEqual(badge.commands, [])

    def test_temporary_theme_exact_ack_and_exact_restoration(self) -> None:
        original_theme = self._theme(brightness=100)
        temporary_theme = self._theme(brightness=99)
        initial = self._status(theme=original_theme, responses=20)
        temporary = self._status(theme=temporary_theme, responses=21)
        restored = self._status(theme=original_theme, responses=22)
        badge = self.ScriptedBadge(
            [self._ack(temporary_theme), self._ack(original_theme)],
            [temporary, restored],
        )

        evidence = flash._prove_reversible_usb_theme_control(
            badge,
            initial_status=initial,
            expectation=self._expectation(),
            initial_evidence=self._evidence(initial),
        )

        self.assertEqual(evidence.responses_completed, 22)
        self.assertEqual(badge.events, ["ctl", "status", "ctl", "status"])
        self.assertEqual(len(badge.commands), 2)
        for command in badge.commands:
            self.assertEqual(set(command), {"cmd", "theme", "persist"})
            self.assertEqual(command["cmd"], "badge_theme")
            self.assertIs(command["persist"], False)
            self.assertNotEqual(command["cmd"], "badge_theme_reset")
        self.assertEqual(badge.commands[0]["theme"], temporary_theme)
        self.assertEqual(badge.commands[1]["theme"], original_theme)
        for key in set(original_theme) - {"brightness"}:
            self.assertEqual(
                badge.commands[0]["theme"][key], original_theme[key]
            )

    def test_theme_ack_requires_exact_schema_and_values(self) -> None:
        class DictSubclass(dict):
            pass

        expected_hash = 1134970802
        valid = {
            "message": "badge theme updated",
            "theme_hash": expected_hash,
            "persisted": False,
            "reboot_required": False,
        }
        flash._validate_badge_theme_ack(valid, expected_hash=expected_hash)
        malformed = []
        for key in valid:
            item = dict(valid)
            item.pop(key)
            malformed.append(item)
        malformed.append(DictSubclass(valid))
        item = dict(valid); item["extra"] = 1; malformed.append(item)
        for key, value in (
            ("message", "badge theme reset"),
            ("theme_hash", expected_hash + 1),
            ("theme_hash", True),
            ("persisted", 0),
            ("persisted", True),
            ("reboot_required", 0),
            ("reboot_required", True),
        ):
            item = dict(valid); item[key] = value; malformed.append(item)
        for ack in malformed:
            with self.subTest(ack=ack), self.assertRaises(flash.FlashError):
                flash._validate_badge_theme_ack(
                    ack, expected_hash=expected_hash
                )

    def test_temporary_proof_failures_always_restore(self) -> None:
        original_theme = self._theme(brightness=60)
        temporary_theme = self._theme(brightness=61)
        initial = self._status(theme=original_theme, responses=20)
        failures = (
            self._status(theme=self._theme(brightness=62), responses=21),
            self._status(
                theme=temporary_theme, responses=21,
                version=self.OLD_VERSION,
            ),
            self._status(theme=temporary_theme, responses=20),
        )
        for failed_status in failures:
            badge = self.ScriptedBadge(
                [self._ack(temporary_theme), self._ack(original_theme)],
                [failed_status, self._status(
                    theme=original_theme, responses=22
                )],
            )
            with self.subTest(status=failed_status), self.assertRaises(
                flash.FlashError
            ):
                flash._prove_reversible_usb_theme_control(
                    badge,
                    initial_status=initial,
                    expectation=self._expectation(),
                    initial_evidence=self._evidence(initial),
                )
            self.assertEqual(len(badge.commands), 2)
            self.assertEqual(badge.commands[-1]["theme"], original_theme)

    def test_first_control_exception_still_restores_and_preserves_primary(self) -> None:
        original_theme = self._theme()
        temporary_theme = self._theme(brightness=99)
        initial = self._status(theme=original_theme, responses=20)
        primary = flash.SerialTransportError("temporary write failed")
        badge = self.ScriptedBadge(
            [primary, self._ack(original_theme)],
            [self._status(theme=original_theme, responses=21)],
        )
        with self.assertRaises(flash.SerialTransportError) as caught:
            flash._prove_reversible_usb_theme_control(
                badge,
                initial_status=initial,
                expectation=self._expectation(),
                initial_evidence=self._evidence(initial),
            )
        self.assertIs(caught.exception, primary)
        self.assertEqual(len(badge.commands), 2)
        self.assertEqual(badge.commands[0]["theme"], temporary_theme)
        self.assertEqual(badge.commands[1]["theme"], original_theme)

    def test_restore_ack_failure_still_reads_status_and_fails(self) -> None:
        original_theme = self._theme()
        temporary_theme = self._theme(brightness=99)
        initial = self._status(theme=original_theme, responses=20)
        restore_error = flash.SerialTransportError("restore ack missing")
        badge = self.ScriptedBadge(
            [self._ack(temporary_theme), restore_error],
            [
                self._status(theme=temporary_theme, responses=21),
                self._status(theme=original_theme, responses=22),
            ],
        )
        with self.assertRaisesRegex(flash.FlashError, "restor"):
            flash._prove_reversible_usb_theme_control(
                badge,
                initial_status=initial,
                expectation=self._expectation(),
                initial_evidence=self._evidence(initial),
            )
        self.assertEqual(badge.events, ["ctl", "status", "ctl", "status"])

    def test_primary_and_restore_failures_are_both_surfaced(self) -> None:
        original_theme = self._theme()
        temporary_theme = self._theme(brightness=99)
        initial = self._status(theme=original_theme, responses=20)
        primary = flash.FlashError("temporary proof mismatch")
        badge = self.ScriptedBadge(
            [self._ack(temporary_theme), {"bad": True}],
            [
                primary,
                self._status(theme=temporary_theme, responses=21),
            ],
        )
        with self.assertRaises(flash.FlashError) as caught:
            flash._prove_reversible_usb_theme_control(
                badge,
                initial_status=initial,
                expectation=self._expectation(),
                initial_evidence=self._evidence(initial),
            )
        self.assertIs(caught.exception, primary)
        self.assertTrue(any(
            "restor" in note.lower()
            for note in getattr(caught.exception, "__notes__", [])
        ))
        self.assertEqual(badge.events, ["ctl", "status", "ctl", "status"])

    def test_restore_counter_must_be_strictly_newer(self) -> None:
        original_theme = self._theme()
        temporary_theme = self._theme(brightness=99)
        initial = self._status(theme=original_theme, responses=20)
        badge = self.ScriptedBadge(
            [self._ack(temporary_theme), self._ack(original_theme)],
            [
                self._status(theme=temporary_theme, responses=21),
                self._status(theme=original_theme, responses=21),
            ],
        )
        with self.assertRaisesRegex(flash.FlashError, "fresh|newer|counter"):
            flash._prove_reversible_usb_theme_control(
                badge,
                initial_status=initial,
                expectation=self._expectation(),
                initial_evidence=self._evidence(initial),
            )

    def test_dry_run_never_starts_theme_control(self) -> None:
        args = SimpleNamespace(
            port=None,
            platform="badge-trio-xiao-s3",
            dry_run=True,
            skip_command_probe=False,
            recovery_rewrite_same_version=False,
        )
        with mock.patch.object(
            flash, "_prove_reversible_usb_theme_control", create=True,
            side_effect=AssertionError("dry-run started theme control"),
        ) as control, mock.patch.object(
            flash, "BadgeSerial",
            side_effect=AssertionError("dry-run opened serial"),
        ):
            flash.usb_flow(
                args, flash.PLATFORMS["badge-trio-xiao-s3"],
                True, ["ble", "wifi"], self.VERSION,
            )
        control.assert_not_called()

    def test_uplink_only_flow_runs_roundtrip_after_fresh_post_proof(self) -> None:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        initial = self._status(responses=10)
        post = self._evidence(self._status(responses=20))
        fresh = self._status(responses=21)
        temporary_theme = self._theme(brightness=99)
        temporary = self._status(theme=temporary_theme, responses=22)
        restored = self._status(responses=23)
        events: list[str] = []
        commands: list[dict] = []
        instances = 0

        class FakeBadge:
            def __init__(
                self, descriptor, dry_run, expected_hardware_id=None
            ):
                nonlocal instances
                self.index = instances
                instances += 1
                events.append(
                    f"open:{descriptor.device}:{expected_hardware_id}"
                )
                self.statuses = [fresh, temporary, restored] if self.index else []
            def __enter__(self): return self
            def __exit__(self, *_args): return None
            def upload_uplink_firmware(self, *_args):
                events.append("upload")
                return {
                    "ok": True, "skipped": True, "phase": "current",
                    "hardware_id": self_test.MAC,
                    "version": self_test.VERSION,
                    "partition": "ota_0",
                }
            def status(self):
                events.append("status")
                return self.statuses.pop(0)
            def ctl(self, payload, *args, **kwargs):
                events.append("ctl")
                commands.append(json.loads(json.dumps(payload)))
                return ReversibleUsbThemeControlTests._ack(payload["theme"])

        self_test = self
        args = SimpleNamespace(
            port=self.PORT, platform="badge-trio-xiao-s3", dry_run=False,
            skip_command_probe=False, recovery_rewrite_same_version=False,
        )
        with mock.patch.object(flash, "probe_rom_device", return_value=None), \
             mock.patch.object(flash, "probe_application", return_value=initial), \
             mock.patch.object(
                 flash, "wait_for_post_uplink_application",
                 return_value=(_usb_record("/dev/cu.rebound-theme"), post),
             ), mock.patch.object(flash, "BadgeSerial", FakeBadge):
            flash.usb_flow(args, platform, True, [], self.VERSION)

        self.assertEqual(len(commands), 2)
        self.assertEqual(events[-5:], ["status", "ctl", "status", "ctl", "status"])
        self.assertIn(f"open:/dev/cu.rebound-theme:{self.MAC}", events)

    def test_scanner_gates_precede_theme_and_gate_failure_blocks_control(self) -> None:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        initial = self._status(responses=10)
        post = self._evidence(self._status(responses=20))
        preflight = _badge_status_with_scanners(
            self.VERSION,
            _scanner_status(platform, self.OLD_VERSION, slot="ble"),
            responses=21,
        )
        final = _badge_status_with_scanners(
            self.VERSION,
            _scanner_status(platform, self.VERSION, slot="ble"),
            responses=22,
        )
        final["theme"] = self._theme()
        final["theme_hash"] = _test_badge_theme_hash(final["theme"])

        for fail_gate in (False, True):
            events: list[str] = []
            stage_receipt = {"generation": 1, "slot_mask": 1}

            class FakeBadge:
                def __init__(self, *_args, **_kwargs): pass
                def __enter__(self): return self
                def __exit__(self, *_args): return None
                def stage_scanner_firmware(self, *_args):
                    events.append("stage")
                    return stage_receipt
                def relay_scanner(self, *_args, **_kwargs):
                    events.append("relay")
                def status(self):
                    events.append("final_status")
                    return final

            def verify_scanners(*args, **kwargs):
                events.append("verify_scanners")
                if fail_gate:
                    raise flash.FlashError("final scanner gate failed")

            def verify_auto(*args, **kwargs):
                events.append("verify_auto")
                self.assertIs(
                    kwargs.get("expected_stage_receipt"), stage_receipt
                )

            def prove_theme(*_args, **kwargs):
                events.append("theme")
                return _complete_mocked_theme_control(*_args, **kwargs)

            flow_result = None
            with self.subTest(fail_gate=fail_gate), mock.patch.object(
                flash, "probe_rom_device", return_value=None
            ), mock.patch.object(
                flash, "probe_application", return_value=initial
            ), mock.patch.object(
                flash, "wait_for_post_uplink_application",
                return_value=(_usb_record("/dev/cu.rebound"), post),
            ), mock.patch.object(
                flash, "BadgeSerial", FakeBadge
            ), mock.patch.object(
                flash, "wait_for_scanner_status_usb",
                side_effect=lambda *_args: events.append("preflight") or preflight,
            ), mock.patch.object(
                flash, "wait_for_scanners_usb",
                side_effect=lambda *_args, **_kwargs: events.append("final_wait"),
            ), mock.patch.object(
                flash, "coordinator_newer_skipped_slots", return_value=set()
            ), mock.patch.object(
                flash, "verify_scanners", side_effect=verify_scanners
            ), mock.patch.object(
                flash, "verify_auto_update_convergence", side_effect=verify_auto
            ), mock.patch.object(
                flash, "_prove_reversible_usb_theme_control", create=True,
                side_effect=prove_theme,
            ) as theme_control:
                args = SimpleNamespace(
                    port=self.PORT, platform="badge-trio-xiao-s3",
                    dry_run=False, skip_command_probe=False,
                    recovery_rewrite_same_version=False,
                )
                if fail_gate:
                    with self.assertRaisesRegex(flash.FlashError, "gate"):
                        flow_result = flash.usb_flow(
                            args, platform, False, ["ble"], self.VERSION
                        )
                else:
                    flow_result = flash.usb_flow(
                        args, platform, False, ["ble"], self.VERSION
                    )
            if fail_gate:
                self.assertIsNone(flow_result)
                theme_control.assert_not_called()
                self.assertNotIn("theme", events)
            else:
                self.assertIs(type(flow_result), flash.UsbScannerFlowResult)
                self.assertLess(events.index("final_wait"), events.index("final_status"))
                self.assertLess(events.index("final_status"), events.index("verify_scanners"))
                self.assertLess(events.index("verify_scanners"), events.index("verify_auto"))
                self.assertLess(events.index("verify_auto"), events.index("theme"))

    def test_scanner_flow_reproves_restored_frame_before_success(self) -> None:
        platform = flash.PLATFORMS["badge-trio-xiao-s3"]
        scanner_image = b"scanner-image-for-restored-frame-proof"
        stage_receipt = _stage_receipt(
            platform, self.VERSION, scanner_image, 1, generation=8
        )
        initial = self._status(responses=10)
        post = self._evidence(self._status(responses=20))
        preflight = _badge_status_with_scanners(
            self.VERSION,
            _scanner_status(platform, self.OLD_VERSION, slot="ble"),
            responses=21,
        )

        def converged_status(*, responses: int, brightness: int = 100) -> dict:
            status = _badge_status_with_scanners(
                self.VERSION,
                _scanner_status(platform, self.VERSION, slot="ble"),
                responses=responses,
            )
            theme = self._theme(brightness=brightness)
            status["theme"] = theme
            status["theme_hash"] = _test_badge_theme_hash(theme)
            status["firmware_store"] = {
                "stored": True,
                "target": stage_receipt["target"],
                "app_project": stage_receipt["app_project"],
                "hardware_type": stage_receipt["hardware_type"],
                "version": stage_receipt["version"],
                "size": stage_receipt["size"],
                "crc32": stage_receipt["crc32"],
                "sha256": stage_receipt["sha256"],
                "generation": stage_receipt["generation"],
                "auto_update": {
                    "generation": stage_receipt["generation"],
                    "target_slot_mask": 1,
                    "pending_mask": 0,
                    "worker_running": False,
                    "readiness_probes": [1, 0],
                    "scanners": [
                        {"slot": 0, "attempts": 1, "state": "converged"},
                        {"slot": 1, "attempts": 0, "state": "excluded"},
                    ],
                },
            }
            return status

        restored_cases = {
            "healthy": None,
            "disconnected": lambda status: status["scanners"][0].update(
                {"connected": False}
            ),
            "scanner_mac_swap": lambda status: status["scanners"][0].update(
                {"hardware_id": "14:C1:9F:52:CA:B0"}
            ),
            "late_identity_mac_swap": lambda status: status[
                "scanners"
            ][0].update({"hardware_id": "14:C1:9F:52:CA:B0"}),
            "role_unhealthy": lambda status: status["scanners"][0].update(
                {"role_acked": False}
            ),
            "radio_unhealthy": lambda status: status["scanners"][0].update(
                {"ble_scanning": False}
            ),
            "rollback_unhealthy": lambda status: status["scanners"][0].update(
                {"rollback_pending": True}
            ),
            "coordinator_regression": lambda status: status[
                "firmware_store"
            ]["auto_update"].update({"pending_mask": 1}),
            "required_slot_regression": lambda status: status[
                "firmware_store"
            ]["auto_update"]["scanners"][0].update({
                "state": "current",
                "attempts": 0,
            }),
            "stage_receipt_regression": lambda status: status[
                "firmware_store"
            ].update({"sha256": "b" * 64}),
        }

        for name, corrupt in restored_cases.items():
            case_preflight = json.loads(json.dumps(preflight))
            if name == "late_identity_mac_swap":
                case_preflight["scanners"][0]["connected"] = False
            healthy_final = converged_status(responses=22)
            temporary = converged_status(responses=23, brightness=99)
            restored = converged_status(responses=24)
            if corrupt is not None:
                corrupt(restored)
            status_calls = 0
            commands: list[dict] = []

            class FakeBadge:
                def __init__(self, *_args, **_kwargs):
                    self.statuses = [healthy_final, temporary, restored]
                def __enter__(self): return self
                def __exit__(self, *_args): return None
                def status(self):
                    nonlocal status_calls
                    status_calls += 1
                    return self.statuses.pop(0)
                def stage_scanner_firmware(self, *_args):
                    return stage_receipt
                def relay_scanner(self, *_args, **_kwargs):
                    raise AssertionError("unexpected recovery relay")
                def ctl(self, payload, *_args, **_kwargs):
                    commands.append(json.loads(json.dumps(payload)))
                    return ReversibleUsbThemeControlTests._ack(payload["theme"])

            args = SimpleNamespace(
                port=self.PORT,
                platform="badge-trio-xiao-s3",
                dry_run=False,
                skip_command_probe=False,
                recovery_rewrite_same_version=False,
            )
            with self.subTest(name=name), mock.patch.object(
                flash, "probe_rom_device", return_value=None
            ), mock.patch.object(
                flash, "probe_application", return_value=initial
            ), mock.patch.object(
                flash, "wait_for_post_uplink_application",
                return_value=(_usb_record("/dev/cu.restored-proof"), post),
            ), mock.patch.object(
                flash, "BadgeSerial", FakeBadge
            ), mock.patch.object(
                flash, "wait_for_scanner_status_usb",
                return_value=case_preflight,
            ), mock.patch.object(
                flash, "wait_for_scanners_usb"
            ), mock.patch.object(
                flash, "scanner_firmware_size", return_value=len(scanner_image)
            ):
                flow_result = None
                if corrupt is None:
                    flow_result = flash.usb_flow(
                        args, platform, False, ["ble"], self.VERSION
                    )
                else:
                    with self.assertRaises(flash.FlashError):
                        flow_result = flash.usb_flow(
                            args, platform, False, ["ble"], self.VERSION
                        )
            self.assertEqual(status_calls, 3)
            self.assertEqual(len(commands), 2)
            self.assertIs(commands[0]["persist"], False)
            self.assertIs(commands[1]["persist"], False)
            if corrupt is None:
                self.assertIs(type(flow_result), flash.UsbScannerFlowResult)
                self.assertEqual(
                    flow_result.final_restored_status, restored
                )
            else:
                self.assertIsNone(flow_result)



class BoundProductionRomFlashTests(unittest.TestCase):
    VERSION = "0.64.76-badge-defcon34"
    MAC = "e0:72:a1:f9:47:fc"

    def setUp(self) -> None:
        self.descriptor = _usb_record(
            "/dev/cu.bound", self.MAC, location="3-1"
        )
        self.artifacts = _frozen_uplink_set(self.VERSION)
        self.stage = _bound_rom_stage(
            self.descriptor, self.artifacts, self.VERSION
        )

    def _session(self, events: list[object]) -> object:
        stage = self.stage

        class Session:
            transcript = (stage.write, stage.verify, stage.run)

            def __enter__(self):
                events.append(("enter", self))
                return self

            def __exit__(self, *_args):
                events.append(("exit", self))

            def probe(self):
                events.append(("probe", self))
                return stage.probe

            def write_layout(self, artifacts):
                events.append(("write", self, artifacts))
                return stage.write

            def verify_layout(self, artifacts):
                events.append(("verify", self, artifacts))
                return stage.verify

            def run_application(self):
                events.append(("run", self))
                return stage.run

        return Session()

    def test_exact_descriptor_frozen_set_and_one_session_order(self) -> None:
        events: list[object] = []
        session = self._session(events)
        with mock.patch.object(
            flash.BoundRomSession,
            "open",
            side_effect=lambda descriptor, base_mac: events.append(
                ("open", descriptor, base_mac, session)
            ) or session,
        ) as opened:
            evidence = flash.flash_complete_uplink_layout(
                self.descriptor,
                self.artifacts,
                self.VERSION,
            )
        self.assertEqual(evidence, self.stage)
        opened.assert_called_once_with(self.descriptor, self.MAC)
        self.assertEqual(
            [event[0] for event in events],
            ["open", "enter", "probe", "write", "verify", "run", "exit"],
        )
        self.assertTrue(all(
            event[1] is session
            for event in events
            if event[0] in ("enter", "probe", "write", "verify", "run", "exit")
        ))
        self.assertIs(events[3][2], self.artifacts)
        self.assertIs(events[4][2], self.artifacts)
        self.assertFalse(evidence.application_health_verified)
        self.assertFalse(evidence.rollback_cleared)

    def test_production_rom_flash_never_reads_a_path_after_freeze(self) -> None:
        session = self._session([])
        with mock.patch.object(
            flash.BoundRomSession, "open", return_value=session
        ), mock.patch.object(
            Path, "read_bytes", side_effect=AssertionError("path read")
        ), mock.patch(
            "builtins.open", side_effect=AssertionError("path open")
        ), mock.patch.object(
            os, "open", side_effect=AssertionError("os path open")
        ):
            flash.flash_complete_uplink_layout(
                self.descriptor,
                self.artifacts,
                self.VERSION,
            )

    def test_runner_or_layout_injection_is_not_in_the_interface(self) -> None:
        parameters = inspect.signature(
            flash.flash_complete_uplink_layout
        ).parameters
        self.assertEqual(
            tuple(parameters),
            ("descriptor", "artifacts", "version"),
        )
        self.assertNotIn("esptool_runner", parameters)
        with self.assertRaises(TypeError):
            flash.flash_complete_uplink_layout(
                self.descriptor,
                self.artifacts,
                self.VERSION,
                esptool_runner=object(),
            )

    def test_post_mutation_uncertainty_stops_without_run(self) -> None:
        events: list[str] = []
        stage = self.stage

        class Session:
            transcript = ()

            def __enter__(self): return self
            def __exit__(self, *_args): return None
            def probe(self):
                events.append("probe")
                return stage.probe
            def write_layout(self, artifacts):
                self.assert_artifacts = artifacts
                events.append("write")
                return stage.write
            def verify_layout(self, artifacts):
                self.assert_artifacts = artifacts
                events.append("verify")
                raise flash.BoundRomMutationUncertainError("lost")
            def run_application(self):
                events.append("run")
                raise AssertionError("run after uncertainty")

        with mock.patch.object(
            flash.BoundRomSession, "open", return_value=Session()
        ), self.assertRaises(flash.RomFlashUncertainError):
            flash.flash_complete_uplink_layout(
                self.descriptor,
                self.artifacts,
                self.VERSION,
            )
        self.assertEqual(events, ["probe", "write", "verify"])

    def test_any_baseexception_from_write_call_is_conservatively_uncertain(
        self,
    ) -> None:
        stage = self.stage
        for failure in (
            RuntimeError("write evidence failed"),
            KeyboardInterrupt("write interrupted"),
            SystemExit("write exited"),
            MemoryError("write allocation failed"),
        ):
            events: list[str] = []

            class Session:
                transcript = ()

                def __enter__(inner_self):
                    return inner_self

                def __exit__(inner_self, *_args):
                    return None

                def probe(inner_self):
                    events.append("probe")
                    return stage.probe

                def write_layout(inner_self, _artifacts):
                    events.append("write")
                    raise failure

                def verify_layout(inner_self, _artifacts):
                    events.append("verify")
                    raise AssertionError("verify after uncertain write")

                def run_application(inner_self):
                    events.append("run")
                    raise AssertionError("run after uncertain write")

            with self.subTest(failure=type(failure).__name__), \
                    mock.patch.object(
                        flash.BoundRomSession,
                        "open",
                        return_value=Session(),
                    ), self.assertRaises(flash.RomFlashUncertainError):
                flash.flash_complete_uplink_layout(
                    self.descriptor,
                    self.artifacts,
                    self.VERSION,
                )
            self.assertEqual(events, ["probe", "write"])

    def test_post_mutation_close_failure_is_uncertain_not_retryable(
        self,
    ) -> None:
        session = self._session([])

        def failed_close(*_args):
            raise flash.BoundRomUnavailableError("late detach")

        session.__class__.__exit__ = failed_close
        with mock.patch.object(
            flash.BoundRomSession, "open", return_value=session
        ), self.assertRaises(flash.RomFlashUncertainError):
            flash.flash_complete_uplink_layout(
                self.descriptor,
                self.artifacts,
                self.VERSION,
            )

    def test_stage_attestation_binds_every_operation_to_frozen_aggregate(
        self,
    ) -> None:
        flash._expectation_from_rom_flash(
            self.stage,
            layout_version=self.VERSION,
            artifacts=self.artifacts,
            update_session="0123456789ABCDEF",
        )
        for field_name, replacement in (
            ("aggregate_sha256", "0" * 64),
            ("base_mac", "00:11:22:33:44:55"),
            ("write", self.stage.verify),
            (
                "run",
                dataclasses.replace(
                    self.stage.run,
                    member_sha256=self.stage.write.member_sha256,
                ),
            ),
        ):
            candidate = dataclasses.replace(
                self.stage, **{field_name: replacement}
            )
            with self.subTest(field=field_name), self.assertRaises(
                flash.FlashError
            ):
                flash._expectation_from_rom_flash(
                    candidate,
                    layout_version=self.VERSION,
                    artifacts=self.artifacts,
                    update_session="0123456789ABCDEF",
                )

    def test_already_rom_and_chord_reenumeration_use_bound_descriptors(
        self,
    ) -> None:
        binding = flash.TrustedUplinkBinding(
            serial_number=self.MAC,
            location="3-1",
            source="retained-session",
        )
        with mock.patch.object(
            flash,
            "_take_badge_usb_descriptor_census",
            return_value=(self.descriptor,),
        ), mock.patch.object(
            flash,
            "flash_complete_uplink_layout",
            return_value=self.stage,
        ) as mutate:
            got = flash._wait_for_bound_rom_flash(
                binding,
                self.artifacts,
                self.VERSION,
                timeout_s=5,
            )
        self.assertIs(got, self.stage)
        mutate.assert_called_once_with(
            self.descriptor, self.artifacts, self.VERSION
        )

        rebound = dataclasses.replace(
            self.descriptor,
            device="/dev/cu.rebound",
            stat_inode=self.descriptor.stat_inode + 1,
        )
        events: list[object] = []
        with mock.patch.object(
            flash,
            "flash_complete_uplink_layout",
            side_effect=[
                flash.BoundRomUnavailableError("not ROM"),
                self.stage,
            ],
        ) as mutate, mock.patch.object(
            flash,
            "_take_badge_usb_descriptor_census",
            return_value=(rebound,),
        ), mock.patch.object(
            flash, "log", side_effect=events.append
        ):
            got = flash._flash_silent_uplink_with_chord_fallback(
                self.descriptor,
                binding,
                self.artifacts,
                self.VERSION,
            )
        self.assertIs(got, self.stage)
        self.assertEqual(events, [flash.ROM_ENTRY_PROMPT])
        self.assertEqual(
            mutate.call_args_list,
            [
                mock.call(
                    self.descriptor, self.artifacts, self.VERSION
                ),
                mock.call(rebound, self.artifacts, self.VERSION),
            ],
        )

    def test_initial_descriptor_immediately_strengthens_location_binding(
        self,
    ) -> None:
        serial_only = flash.TrustedUplinkBinding(
            serial_number=self.MAC,
            location=None,
            source="retained-session",
        )
        strengthened = flash._strengthen_trusted_uplink_binding(
            self.descriptor,
            serial_only,
        )
        self.assertEqual(strengthened.serial_number, self.MAC)
        self.assertEqual(strengthened.location, "3-1")

        no_location = dataclasses.replace(
            self.descriptor,
            location=None,
        )
        self.assertIsNone(
            flash._strengthen_trusted_uplink_binding(
                no_location,
                serial_only,
            ).location
        )
        with self.assertRaisesRegex(flash.FlashError, "location"):
            flash._strengthen_trusted_uplink_binding(
                no_location,
                dataclasses.replace(serial_only, location="3-1"),
            )

    def test_every_reenumeration_requires_exact_initial_location(self) -> None:
        binding = flash.TrustedUplinkBinding(
            serial_number=self.MAC,
            location="3-1",
            source="retained-session",
        )
        rebound = dataclasses.replace(
            self.descriptor,
            device="/dev/cu.rebound",
            stat_inode=self.descriptor.stat_inode + 1,
        )
        with mock.patch.object(
            flash,
            "_take_badge_usb_descriptor_census",
            return_value=(rebound,),
        ):
            self.assertEqual(
                flash._fresh_descriptor_for_trusted_uplink(binding),
                rebound,
            )

        dropped = dataclasses.replace(rebound, location=None)
        with mock.patch.object(
            flash,
            "_take_badge_usb_descriptor_census",
            return_value=(dropped,),
        ), self.assertRaisesRegex(flash.FlashError, "location"):
            flash._fresh_descriptor_for_trusted_uplink(binding)

        none_binding = dataclasses.replace(binding, location=None)
        with mock.patch.object(
            flash,
            "_take_badge_usb_descriptor_census",
            return_value=(dropped,),
        ):
            self.assertEqual(
                flash._fresh_descriptor_for_trusted_uplink(none_binding),
                dropped,
            )
        with mock.patch.object(
            flash,
            "_take_badge_usb_descriptor_census",
            return_value=(rebound,),
        ), self.assertRaisesRegex(flash.FlashError, "location"):
            flash._fresh_descriptor_for_trusted_uplink(none_binding)

    def test_reenumeration_location_drift_and_security_failures_are_hard(
        self,
    ) -> None:
        binding = flash.TrustedUplinkBinding(
            serial_number=self.MAC,
            location="3-1",
            source="retained-session",
        )
        moved = dataclasses.replace(self.descriptor, location="9-9")
        with mock.patch.object(
            flash,
            "_take_badge_usb_descriptor_census",
            return_value=(moved,),
        ), self.assertRaisesRegex(flash.FlashError, "location"):
            flash._wait_for_bound_rom_flash(
                binding,
                self.artifacts,
                self.VERSION,
                timeout_s=5,
            )

        provenance = flash.FlashError("provenance failed")
        with mock.patch.object(
            flash,
            "_take_badge_usb_descriptor_census",
            return_value=(self.descriptor,),
        ), mock.patch.object(
            flash,
            "flash_complete_uplink_layout",
            side_effect=provenance,
        ) as mutate, self.assertRaisesRegex(
            flash.FlashError, "provenance"
        ):
            flash._wait_for_bound_rom_flash(
                binding,
                self.artifacts,
                self.VERSION,
                timeout_s=5,
            )
        mutate.assert_called_once()

    def test_package_import_loads_bound_rom_dependencies(self) -> None:
        completed = subprocess.run(
            [
                sys.executable,
                "-c",
                (
                    "import scripts.bound_rom as bound\n"
                    "import scripts.esptool_provenance as provenance\n"
                    "import scripts.fof_badge_flash as flash\n"
                    "assert flash.UsbDescriptorRecord is "
                    "bound.UsbDescriptorRecord\n"
                    "assert flash.UsbDescriptorBindingError is "
                    "bound.UsbDescriptorBindingError\n"
                    "assert bound.VerifiedEsptoolRuntime is "
                    "provenance.VerifiedEsptoolRuntime\n"
                    "assert bound.EsptoolProvenanceError is "
                    "provenance.EsptoolProvenanceError\n"
                    "record=flash.UsbDescriptorRecord("
                    "device='/dev/cu.identity',vid=0x303a,pid=0x1001,"
                    "serial_number='e0:72:a1:f9:47:fc',location='3-1',"
                    "stat_device=1,stat_inode=2,stat_rdev=3)\n"
                    "assert type(record) is bound.UsbDescriptorRecord\n"
                    "class StopBeforeHardware(BaseException):\n"
                    "    pass\n"
                    "def stop():\n"
                    "    raise StopBeforeHardware()\n"
                    "bound.load_verified_platformio_esptool=stop\n"
                    "passed=False\n"
                    "\ntry: bound.BoundRomSession.open("
                    "record,record.serial_number)\n"
                    "except StopBeforeHardware: passed=True\n"
                    "assert passed"
                ),
            ],
            cwd=Path(__file__).resolve().parent.parent,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)


if __name__ == "__main__":
    unittest.main()
