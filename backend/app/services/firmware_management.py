"""Authoritative firmware-family resolution and node readiness gates."""

from __future__ import annotations

import re
import time
from typing import Any

from app.services.firmware_manager import FIRMWARE_TYPES


HEARTBEAT_STALE_S = 120.0

_MAC_RE = re.compile(r"^[0-9A-F]{2}(?::[0-9A-F]{2}){5}$")

_LEGACY_UPLINK_PARTITIONS = (
    ("nvs", "data", "nvs", 0x9000, 0x6000),
    ("otadata", "data", "ota", 0xF000, 0x2000),
    ("phy_init", "data", "phy", 0x11000, 0x1000),
    ("ota_0", "app", "ota_0", 0x20000, 0x200000),
    ("ota_1", "app", "ota_1", 0x220000, 0x200000),
    ("fw_scanner_s3", "data", "0x40", 0x420000, 0x200000),
    ("fw_scanner_esp32", "data", "0x40", 0x620000, 0x100000),
    ("fw_scanner_c5", "data", "0x40", 0x720000, 0x100000),
    ("fw_self", "data", "0x40", 0x820000, 0x200000),
    ("storage", "data", "spiffs", 0xA20000, 0x100000),
    ("reserved", "data", "fat", 0xB20000, 0x4E0000),
)

_LEGACY_SCANNER_PARTITIONS = (
    ("nvs", "data", "nvs", 0x9000, 0x6000),
    ("otadata", "data", "ota", 0xF000, 0x2000),
    ("phy_init", "data", "phy", 0x11000, 0x1000),
    ("ota_0", "app", "ota_0", 0x20000, 0x300000),
    ("ota_1", "app", "ota_1", 0x320000, 0x300000),
    ("storage", "data", "spiffs", 0x620000, 0x100000),
    ("reserved", "data", "fat", 0x720000, 0x8E0000),
)


def _deduplicated(values: list[str]) -> list[str]:
    return list(dict.fromkeys(values))


def _reported(report: dict, name: str) -> Any:
    diagnostic_name = f"reported_{name}"
    value = (
        report.get(diagnostic_name)
        if diagnostic_name in report
        else report.get(name)
    )
    return value if value is not None else None


def _catalog_match(report: dict) -> tuple[str | None, dict | None, str | None]:
    target = str(
        report.get("firmware_target")
        or report.get("firmware_name")
        or ""
    ).strip()
    if not target:
        return None, None, "unknown_target"
    info = FIRMWARE_TYPES.get(target)
    if info is None:
        return target, None, "unknown_target"
    project = str(report.get("app_project") or "").strip()
    hardware = str(report.get("hardware_type") or "").strip()
    if project != info["project"] or hardware != info["hardware"]:
        return target, None, "identity_mismatch"
    return target, info, None


def resolve_component_management_identity(
    report: dict,
    component_hint: str | None = None,
) -> dict:
    """Resolve management fields from an exact server catalog triple.

    Client family, flash-size, and GPIO claims are retained only as reported
    diagnostics. They never select a catalog family.
    """
    report = report if isinstance(report, dict) else {}
    target, info, resolution_error = _catalog_match(report)
    blockers: list[str] = []
    if resolution_error:
        blockers.append(resolution_error)

    product_family = info.get("product_family") if info else None
    firmware_line = info.get("firmware_line") if info else None
    component = info.get("component") if info else component_hint
    reported_family = _reported(report, "product_family")
    reported_line = _reported(report, "firmware_line")
    reported_component = _reported(report, "component")

    for reported_value, resolved_value, blocker in (
        (reported_family, product_family, "reported_product_family_conflict"),
        (reported_line, firmware_line, "reported_firmware_line_conflict"),
        (reported_component, component, "reported_component_conflict"),
    ):
        if reported_value is not None and reported_value != resolved_value:
            blockers.append(blocker)

    if component_hint and component != component_hint:
        blockers.append("component_mismatch")
    if reported_component is not None and component_hint and reported_component != component_hint:
        blockers.append("component_mismatch")

    migration_required = bool(info and info.get("migration_required"))
    supported = bool(info and info.get("supported"))
    catalog_remote_eligible = bool(info and info.get("remote_update_eligible"))
    if migration_required:
        blockers.extend(("legacy_firmware", "migration_required"))
    elif info is not None and not supported:
        blockers.append("unsupported_target")

    blockers = _deduplicated(blockers)
    remote_update_eligible = bool(catalog_remote_eligible and not blockers)
    return {
        "catalog_name": target if info is not None else None,
        "product_family": product_family,
        "firmware_line": firmware_line,
        "component": component,
        "desired_firmware_line": (
            info.get("desired_firmware_line") if info else "backend"
        ),
        "capabilities": list(info.get("capabilities") or []) if info else [],
        "partition_capacity": info.get("partition_capacity") if info else None,
        "scanner_cache_capacity": info.get("scanner_cache_capacity") if info else None,
        "companion_target": info.get("companion_target") if info else None,
        "supported": supported,
        "migration_required": migration_required,
        "remote_update_eligible": remote_update_eligible,
        "management_blockers": blockers,
        "reported_product_family": reported_family,
        "reported_firmware_line": reported_line,
        "reported_component": reported_component,
    }


def _scanner_mac(scanner: dict) -> str:
    return str(scanner.get("mac") or scanner.get("hardware_mac") or "").strip().upper()


def _has_duplicate_nonempty(values: list[str]) -> bool:
    filtered = [value for value in values if value]
    return len(filtered) != len(set(filtered))


def _target_blockers(identity: dict) -> list[str]:
    return list(identity.get("management_blockers") or [])


def remote_update_blockers(
    heartbeat: dict,
    requested_target: str | None = None,
    now: float | None = None,
) -> list[str]:
    """Return stable reason codes that make remote mutation ineligible."""
    now = time.time() if now is None else now
    if not isinstance(heartbeat, dict):
        return ["missing_uplink"]

    blockers: list[str] = []
    try:
        if now - float(heartbeat["last_seen"]) >= HEARTBEAT_STALE_S:
            blockers.append("stale_heartbeat")
    except (KeyError, TypeError, ValueError):
        blockers.append("stale_heartbeat")

    uplink = resolve_component_management_identity(heartbeat, "uplink")
    if not heartbeat.get("firmware_target") and not heartbeat.get("firmware_name"):
        blockers.append("missing_uplink")
    blockers.extend(_target_blockers(uplink))

    requested = FIRMWARE_TYPES.get(requested_target) if requested_target else None
    if requested_target and requested is None:
        blockers.append("unknown_target")
    elif requested is not None:
        if requested.get("migration_required"):
            blockers.append("migration_required")
        elif not requested.get("supported"):
            blockers.append("unsupported_target")
        if not requested.get("remote_update_eligible"):
            blockers.append("remote_update_ineligible")

    scanners = [
        dict(item) for item in heartbeat.get("scanners") or []
        if isinstance(item, dict)
    ]
    requires_trio = bool(
        uplink.get("product_family") == "s3_fullsize"
        or (requested and requested.get("product_family") == "s3_fullsize")
    )
    if requires_trio:
        if len(scanners) < 2:
            blockers.append("missing_scanner")
        elif len(scanners) > 2:
            blockers.append("unexpected_scanner")

    scanner_identities = [
        resolve_component_management_identity(scanner, "scanner")
        for scanner in scanners
    ]
    for identity in scanner_identities:
        blockers.extend(_target_blockers(identity))

    if _has_duplicate_nonempty([_scanner_mac(scanner) for scanner in scanners]):
        blockers.append("duplicate_scanner_mac")
    if _has_duplicate_nonempty([
        str(scanner.get("boot_id")) if scanner.get("boot_id") is not None else ""
        for scanner in scanners
    ]):
        blockers.append("duplicate_scanner_boot_id")

    resolved_families = [
        uplink.get("product_family"),
        *(identity.get("product_family") for identity in scanner_identities),
    ]
    known_families = {family for family in resolved_families if family is not None}
    if len(known_families) > 1:
        blockers.append("mixed_family")
    if known_families and any(family is None for family in resolved_families):
        blockers.append("mixed_family")

    if requested is not None:
        requested_family = requested.get("product_family")
        if requested.get("component") == "uplink":
            if uplink.get("catalog_name") != requested_target:
                blockers.append("target_mismatch")
        elif requested.get("component") == "scanner":
            if not scanners:
                blockers.append("missing_scanner")
            if any(
                identity.get("catalog_name") != requested_target
                for identity in scanner_identities
            ):
                blockers.append("target_mismatch")
        if requested_family is not None and any(
            family != requested_family for family in resolved_families
            if family is not None
        ):
            blockers.append("mixed_family")

    return _deduplicated(blockers)


def enrich_node_management(heartbeat: dict, now: float | None = None) -> dict:
    """Return a copy with reported diagnostics and authoritative decisions."""
    enriched = dict(heartbeat)
    uplink = resolve_component_management_identity(heartbeat, "uplink")
    enriched.update(uplink)

    scanners = []
    for scanner in heartbeat.get("scanners") or []:
        if not isinstance(scanner, dict):
            scanners.append(scanner)
            continue
        scanner_entry = dict(scanner)
        scanner_entry.update(resolve_component_management_identity(scanner, "scanner"))
        scanners.append(scanner_entry)
    enriched["scanners"] = scanners

    blockers = remote_update_blockers(heartbeat, now=now)
    enriched["management_blockers"] = blockers
    enriched["migration_required"] = bool(
        uplink.get("migration_required")
        or any(
            isinstance(scanner, dict) and scanner.get("migration_required")
            for scanner in scanners
        )
    )
    enriched["remote_update_eligible"] = bool(
        uplink.get("remote_update_eligible") and not blockers
    )
    return enriched


def _normalized_partitions(value: Any) -> tuple[tuple[Any, ...], ...] | None:
    if not isinstance(value, list):
        return None
    rows: list[tuple[Any, ...]] = []
    for row in value:
        if not isinstance(row, (list, tuple)) or len(row) != 5:
            return None
        rows.append(tuple(row))
    return tuple(rows)


def resolve_attended_migration_candidate(receipt: dict) -> dict | None:
    """Validate one trusted local inventory receipt for attended USB only.

    This function does not accept a runtime report and never assigns an
    installed product family. Its caller owns receipt provenance.
    """
    if not isinstance(receipt, dict) or set(receipt) != {
        "schema", "authority", "rom_chip_model", "flash_size", "partitions",
        "board_attestation", "led_gpio", "mac", "physical_role",
        "firmware_target", "app_project", "hardware_type", "secure_boot",
        "flash_encryption",
    }:
        return None
    if receipt["schema"] != 1 or receipt["authority"] not in {
        "server_inventory", "local_operator",
    }:
        return None
    if receipt["rom_chip_model"] != "ESP32-S3" or receipt["flash_size"] != 0x1000000:
        return None
    if receipt["board_attestation"] != "production_gpio48" or receipt["led_gpio"] != 48:
        return None
    if receipt["secure_boot"] is not False or receipt["flash_encryption"] is not False:
        return None
    mac = str(receipt["mac"]).upper()
    if _MAC_RE.fullmatch(mac) is None:
        return None

    target = receipt["firmware_target"]
    if target == "uplink-s3":
        expected_role = {"uplink"}
        expected_project = "fof_uplink"
        expected_partitions = _LEGACY_UPLINK_PARTITIONS
    elif target == "scanner-s3-combo":
        expected_role = {"scanner0", "scanner1"}
        expected_project = "fof_scanner"
        expected_partitions = _LEGACY_SCANNER_PARTITIONS
    else:
        return None
    if receipt["physical_role"] not in expected_role:
        return None
    if receipt["app_project"] != expected_project:
        return None
    if receipt["hardware_type"] != "esp32-s3-devkitc-1":
        return None
    if _normalized_partitions(receipt["partitions"]) != expected_partitions:
        return None

    return {
        "candidate_label": "s3_fullsize_migration_candidate",
        "authorized_action": "attended_usb_migration",
        "runtime_product_family": None,
        "mac": mac,
        "physical_role": receipt["physical_role"],
    }
