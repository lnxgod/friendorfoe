#!/usr/bin/env python3
"""Materialize and strictly verify badge-scanner flash manifests."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

from secure_artifact_tree import VerifiedBadgeArtifactSnapshot
from verified_badge_artifacts import (
    SCANNER_ROLE,
    materialize_role_aliases,
    prepare_verified_role_snapshot,
)
from verify_badge_uplink_build import (
    _DuplicateJsonKeyError,
    _csv_ota0_offset,
    _decode_ota0,
    _json_object_without_duplicates,
    _parse_text_manifest,
    _safe_referenced_file,
    _verify_canary_artifact_isolation,
    _verify_canary_final_elf,
    _verify_canary_linker_map,
    _verify_expected_sdkconfig,
    _verify_full_partition_table,
)


APP_OFFSET = 0x20000
APP_FILENAME = "fof_badge_scanner.bin"
EXPECTED_FULL = {
    0x00000: "bootloader/bootloader.bin",
    0x08000: "partition_table/partition-table.bin",
    0x0F000: "ota_data_initial.bin",
    APP_OFFSET: APP_FILENAME,
}
EXPECTED_APP = {APP_OFFSET: APP_FILENAME}
TEXT_MANIFESTS = {
    "flash_args": EXPECTED_FULL,
    "app-flash_args": EXPECTED_APP,
    "flash_app_args": EXPECTED_APP,
    "flash_project_args": EXPECTED_FULL,
}
ALIASES = {
    "bootloader/bootloader.bin": "bootloader.bin",
    "partition_table/partition-table.bin": "partitions.bin",
    APP_FILENAME: "firmware.bin",
}
SCANNER_PRODUCTION_ENV = "scanner-s3-combo-fof_badge"
SCANNER_CANARY_ENV = "scanner-s3-combo-fof_badge-con-crud-canary"
SCANNER_CANARY_MAP_FILENAME = "fof_badge_scanner.map"
SCANNER_CANARY_MAX_INTERNAL_RAM_BYTES = 180_224
SCANNER_CANARY_MAX_APP_BYTES = 1_363_148
SCANNER_CANARY_SDKCONFIG = {
    "CONFIG_BT_ENABLED": "y",
    "CONFIG_BT_NIMBLE_ENABLED": "y",
    "CONFIG_BT_CONTROLLER_ONLY": "n",
    "CONFIG_BT_CTRL_BLE_SCAN": "y",
    "CONFIG_ESP_WIFI_ENABLED": "y",
    "CONFIG_SPIRAM": "y",
    "CONFIG_SPIRAM_USE_CAPS_ALLOC": "y",
    "CONFIG_SPIRAM_USE_MALLOC": "n",
}
SCANNER_CANARY_REQUIRED_SOURCE_FILES = (
    "badge_con_observer.c",
    "ble_remote_id.c",
    "esp_nimble_hci.c",
)
SCANNER_CANARY_REQUIRED_SYMBOLS = (
    "badge_con_observer_init",
    "ble_remote_id_init",
    "esp_wifi_set_promiscuous",
    "esp_nimble_hci_init",
    "nimble_port_init",
    "ble_gap_ext_disc",
)
SCANNER_CANARY_FORBIDDEN_SOURCE_FILES = (
    "badge_con_vhci.c",
)
SCANNER_CANARY_FORBIDDEN_SYMBOLS = (
    "badge_con_radio_runtime_poll",
    "badge_con_vhci_init",
)


def verify_badge_scanner_sdkconfig(sdkconfig: Path) -> list[str]:
    """Require the generated config that controls bootloader/layout safety."""
    try:
        lines = set(Path(sdkconfig).read_text().splitlines())
    except OSError as exc:
        return [f"sdkconfig: cannot read: {exc}"]
    errors: list[str] = []
    if "CONFIG_PARTITION_TABLE_CUSTOM=y" not in lines:
        errors.append(
            "sdkconfig: CONFIG_PARTITION_TABLE_CUSTOM must be enabled"
        )
    if (
        'CONFIG_PARTITION_TABLE_CUSTOM_FILENAME='
        '"partitions_s3_scanner_8mb.csv"' not in lines
    ):
        errors.append(
            "sdkconfig: custom partition filename must be "
            "partitions_s3_scanner_8mb.csv"
        )
    if "CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y" not in lines:
        errors.append(
            "sdkconfig: CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE must be "
            "enabled"
        )
    return errors


def verify_badge_scanner_canary_sdkconfig(
    sdkconfig: Path,
) -> list[str]:
    errors = verify_badge_scanner_sdkconfig(sdkconfig)
    errors.extend(_verify_expected_sdkconfig(
        sdkconfig, SCANNER_CANARY_SDKCONFIG
    ))
    return errors


def verify_badge_scanner_canary_acceptance(
    canary_build_dir: Path,
    sdkconfig: Path,
    production_build_dir: Path,
) -> list[str]:
    """Fail closed unless scanner parity, memory, and isolation evidence passes."""
    canary_build_dir = Path(canary_build_dir)
    errors = verify_badge_scanner_canary_sdkconfig(sdkconfig)
    errors.extend(_verify_canary_linker_map(
        canary_build_dir / SCANNER_CANARY_MAP_FILENAME,
        max_internal_ram_bytes=SCANNER_CANARY_MAX_INTERNAL_RAM_BYTES,
    ))
    errors.extend(_verify_canary_final_elf(
        canary_build_dir / "firmware.elf",
        required_symbols=SCANNER_CANARY_REQUIRED_SYMBOLS,
        required_source_files=SCANNER_CANARY_REQUIRED_SOURCE_FILES,
        forbidden_symbols=SCANNER_CANARY_FORBIDDEN_SYMBOLS,
        forbidden_source_files=SCANNER_CANARY_FORBIDDEN_SOURCE_FILES,
    ))
    errors.extend(_verify_canary_artifact_isolation(
        canary_build_dir,
        production_build_dir,
        canary_env=SCANNER_CANARY_ENV,
        production_env=SCANNER_PRODUCTION_ENV,
        max_app_bytes=SCANNER_CANARY_MAX_APP_BYTES,
    ))
    return errors


def materialize_badge_scanner_aliases(build_dir: Path) -> None:
    """Create regular-file aliases referenced by ESP-IDF manifests."""
    materialize_role_aliases(build_dir, ALIASES)


def prepare_verified_badge_scanner_snapshot(
    build_dir: Path,
    partition_source: Path,
    sdkconfig: Path,
    *,
    private_parent: Path,
    materialize_missing_aliases: bool,
) -> VerifiedBadgeArtifactSnapshot:
    """Return the only live capability for verified scanner artifact bytes."""
    return prepare_verified_role_snapshot(
        SCANNER_ROLE,
        build_dir,
        partition_source,
        sdkconfig,
        private_parent=private_parent,
        materialize_missing_aliases=materialize_missing_aliases,
    )


def _parse_json_manifest(path: Path) -> tuple[dict[int, str], list[str]]:
    errors: list[str] = []
    try:
        payload = json.loads(
            path.read_text(),
            object_pairs_hook=_json_object_without_duplicates,
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
            errors.append(
                f"{path.name}: invalid flash_files offset {raw_offset!r}"
            )
            continue
        if not isinstance(raw_path, str):
            errors.append(
                f"{path.name}: path at {raw_offset!r} must be a string"
            )
            continue
        if offset in entries:
            errors.append(
                f"{path.name}: duplicate decoded offset {offset:#x}"
            )
            continue
        entries[offset] = raw_path
    app = payload.get("app") if isinstance(payload, dict) else None
    if (
        not isinstance(app, dict) or
        app.get("offset") != "0x20000" or
        app.get("file") != APP_FILENAME
    ):
        errors.append(
            f"{path.name}: app must be 0x20000 {APP_FILENAME}"
        )
    return entries, errors


def verify_badge_scanner_build(
    build_dir: Path,
    partition_source: Path | None = None,
) -> list[str]:
    """Return every exact-layout violation; an empty list is the only pass."""
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
                "flasher_args.json: mappings differ from required "
                f"{EXPECTED_FULL}"
            )

    for manifest, entries in parsed.items():
        if 0x10000 in entries:
            errors.append(
                f"{manifest}: forbidden 0x10000 application entry"
            )
        if entries.get(APP_OFFSET) != APP_FILENAME:
            errors.append(
                f"{manifest}: application must be exactly 0x20000 "
                f"{APP_FILENAME}"
            )
        for relative in entries.values():
            path_error = _safe_referenced_file(
                build_dir, manifest, relative
            )
            if path_error:
                errors.append(path_error)

    for alias, canonical in ALIASES.items():
        canonical_path = build_dir / canonical
        alias_path = build_dir / alias
        if not canonical_path.is_file():
            errors.append(f"canonical artifact missing: {canonical}")
        elif (
            alias_path.is_file() and
            alias_path.read_bytes() != canonical_path.read_bytes()
        ):
            errors.append(
                f"alias differs from canonical artifact: {alias}"
            )

    ota0_offsets, partition_errors = _decode_ota0(
        build_dir / "partitions.bin"
    )
    errors.extend(partition_errors)
    if ota0_offsets != [APP_OFFSET]:
        rendered = (
            ", ".join(hex(offset) for offset in ota0_offsets) or "none"
        )
        errors.append(
            "partitions.bin: decoded ota_0 must be exactly 0x20000; "
            f"got {rendered}"
        )
    if partition_source is not None:
        partition_source = Path(partition_source)
        source_offset, source_error = _csv_ota0_offset(partition_source)
        if source_error:
            errors.append(source_error)
        elif source_offset != APP_OFFSET or source_offset not in ota0_offsets:
            errors.append(
                "partition source and decoded partitions.bin ota_0 must "
                "match 0x20000"
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
        materialize_badge_scanner_aliases(args.build_dir)
    errors = verify_badge_scanner_build(
        args.build_dir, args.partition_source
    )
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
            errors.extend(verify_badge_scanner_canary_acceptance(
                args.build_dir,
                args.sdkconfig,
                args.canary_production_build_dir,
            ))
    elif args.canary_production_build_dir is not None:
        errors.append(
            "--canary-production-build-dir is only valid for a "
            "CON CRUD canary build"
        )
    elif args.sdkconfig is not None:
        errors.extend(verify_badge_scanner_sdkconfig(args.sdkconfig))
    if errors:
        for error in errors:
            print(f"ERROR: {error}")
        return 1
    print("badge scanner manifests: strict verification passed")
    for name in ("firmware.bin", "partitions.bin", *ALIASES):
        path = args.build_dir / name
        print(f"sha256 {name} {_sha256(path)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
