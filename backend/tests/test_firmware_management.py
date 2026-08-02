import copy
import importlib

import pytest


def _management():
    return importlib.import_module("app.services.firmware_management")


def resolve_component_management_identity(*args, **kwargs):
    return _management().resolve_component_management_identity(*args, **kwargs)


def remote_update_blockers(*args, **kwargs):
    return _management().remote_update_blockers(*args, **kwargs)


def enrich_node_management(*args, **kwargs):
    return _management().enrich_node_management(*args, **kwargs)


def resolve_attended_migration_candidate(*args, **kwargs):
    return _management().resolve_attended_migration_candidate(*args, **kwargs)


NOW = 1_785_600_000.0

FULLSIZE_UPLINK = {
    "device_id": "uplink_FULL01",
    "ip": "192.0.2.10",
    "last_seen": NOW - 5,
    "product_family": "s3_fullsize",
    "firmware_line": "backend",
    "component": "uplink",
    "firmware_target": "uplink-s3-fullsize-backend",
    "app_project": "fof_backend_uplink_fullsize",
    "hardware_type": "esp32s3_n16r8_fullsize",
    "hardware_mac": "AA:BB:CC:DD:EE:10",
    "boot_id": 100,
}

FULLSIZE_SCANNERS = [
    {
        "uart": "ble",
        "slot": 0,
        "product_family": "s3_fullsize",
        "firmware_line": "backend",
        "component": "scanner",
        "firmware_target": "scanner-s3-combo-fullsize-backend",
        "app_project": "fof_backend_scanner_fullsize",
        "hardware_type": "esp32s3_n16r8_fullsize",
        "mac": "AA:BB:CC:DD:EE:11",
        "boot_id": 101,
    },
    {
        "uart": "wifi",
        "slot": 1,
        "product_family": "s3_fullsize",
        "firmware_line": "backend",
        "component": "scanner",
        "firmware_target": "scanner-s3-combo-fullsize-backend",
        "app_project": "fof_backend_scanner_fullsize",
        "hardware_type": "esp32s3_n16r8_fullsize",
        "mac": "AA:BB:CC:DD:EE:12",
        "boot_id": 102,
    },
]


def healthy_fullsize_heartbeat() -> dict:
    return {**copy.deepcopy(FULLSIZE_UPLINK), "scanners": copy.deepcopy(FULLSIZE_SCANNERS)}


@pytest.mark.parametrize(
    ("report", "component_hint", "expected"),
    [
        (
            {
                "firmware_target": "uplink-s3-fof_badge",
                "app_project": "fof_badge_uplink",
                "hardware_type": "seeed_xiao_esp32s3",
            },
            "uplink",
            ("badge", "native_badge", "uplink", False, True),
        ),
        (
            {
                "firmware_target": "scanner-s3-combo-fof_badge",
                "app_project": "fof_badge_scanner",
                "hardware_type": "seeed_xiao_esp32s3",
            },
            "scanner",
            ("badge", "native_badge", "scanner", False, True),
        ),
        (
            {
                "firmware_target": "uplink-s3-backend",
                "app_project": "fof_backend_uplink",
                "hardware_type": "seeed_xiao_esp32s3",
            },
            "uplink",
            ("badge_lite", "backend", "uplink", False, True),
        ),
        (
            {
                "firmware_target": "scanner-s3-combo-backend",
                "app_project": "fof_backend_scanner",
                "hardware_type": "seeed_xiao_esp32s3",
            },
            "scanner",
            ("badge_lite", "backend", "scanner", False, True),
        ),
        (
            {
                "firmware_target": "uplink-s3-fullsize-backend",
                "app_project": "fof_backend_uplink_fullsize",
                "hardware_type": "esp32s3_n16r8_fullsize",
            },
            "uplink",
            ("s3_fullsize", "backend", "uplink", False, True),
        ),
        (
            {
                "firmware_target": "scanner-s3-combo-fullsize-backend",
                "app_project": "fof_backend_scanner_fullsize",
                "hardware_type": "esp32s3_n16r8_fullsize",
            },
            "scanner",
            ("s3_fullsize", "backend", "scanner", False, True),
        ),
        (
            {
                "firmware_target": "uplink-s3",
                "app_project": "fof_uplink",
                "hardware_type": "esp32-s3-devkitc-1",
            },
            "uplink",
            (None, "legacy", "uplink", True, False),
        ),
        (
            {
                "firmware_target": "scanner-s3-combo",
                "app_project": "fof_scanner",
                "hardware_type": "esp32-s3-devkitc-1",
            },
            "scanner",
            (None, "legacy", "scanner", True, False),
        ),
        (
            {
                "firmware_target": "scanner-s3-combo-seed",
                "app_project": "fof_scanner_seed",
                "hardware_type": "esp32-s3-devkitc-1",
            },
            "scanner",
            (None, "legacy", "scanner", False, False),
        ),
    ],
)
def test_exact_server_catalog_identity_controls_management(
    report: dict,
    component_hint: str,
    expected: tuple,
):
    resolved = resolve_component_management_identity(report, component_hint)

    assert (
        resolved["product_family"],
        resolved["firmware_line"],
        resolved["component"],
        resolved["migration_required"],
        resolved["remote_update_eligible"],
    ) == expected
    assert resolved["reported_product_family"] is None
    assert resolved["reported_firmware_line"] is None
    assert resolved["reported_component"] is None
    if resolved["firmware_line"] == "legacy":
        assert resolved["desired_firmware_line"] == "backend"


@pytest.mark.parametrize(
    "report",
    [
        {
            "firmware_target": "uplink-s3",
            "app_project": "fof_uplink",
            "hardware_type": "esp32-s3-devkitc-1",
            "product_family": "s3_fullsize",
            "flash_size": 0x1000000,
            "led_gpio": 48,
        },
        {
            "firmware_target": "uplink-s3",
            "app_project": "fof_uplink",
            "hardware_type": "esp32-s3-devkitc-1-v1.1-gpio38",
            "product_family": "s3_fullsize",
            "flash_size": 0x1000000,
            "led_gpio": 38,
        },
        {
            "firmware_target": "esp32-s3-devkitc-1",
            "app_project": "fof_uplink",
            "hardware_type": "esp32-s3-devkitc-1",
            "product_family": "s3_fullsize",
            "flash_size": 0x1000000,
            "led_gpio": 48,
        },
    ],
)
def test_client_flash_gpio_and_family_claims_cannot_promote_fullsize(report: dict):
    resolved = resolve_component_management_identity(report, "uplink")

    assert resolved["product_family"] is None
    assert resolved["remote_update_eligible"] is False
    assert "reported_product_family_conflict" in resolved["management_blockers"]


def test_exact_fullsize_trio_is_remote_update_ready():
    heartbeat = healthy_fullsize_heartbeat()

    assert remote_update_blockers(
        heartbeat,
        "scanner-s3-combo-fullsize-backend",
        now=NOW,
    ) == []

    enriched = enrich_node_management(heartbeat, now=NOW)
    assert enriched["product_family"] == "s3_fullsize"
    assert enriched["firmware_line"] == "backend"
    assert enriched["component"] == "uplink"
    assert enriched["remote_update_eligible"] is True
    assert enriched["management_blockers"] == []
    assert [scanner["component"] for scanner in enriched["scanners"]] == [
        "scanner",
        "scanner",
    ]


def test_reported_identity_conflict_survives_repeated_enrichment():
    heartbeat = healthy_fullsize_heartbeat()
    heartbeat["product_family"] = "badge_lite"

    first = enrich_node_management(heartbeat, now=NOW)
    second = enrich_node_management(first, now=NOW)

    assert second["product_family"] == "s3_fullsize"
    assert second["reported_product_family"] == "badge_lite"
    assert "reported_product_family_conflict" in second["management_blockers"]


@pytest.mark.parametrize(
    ("mutation", "expected_blocker"),
    [
        ("stale", "stale_heartbeat"),
        ("missing_uplink", "missing_uplink"),
        ("missing_scanner", "missing_scanner"),
        ("duplicate_scanner_mac", "duplicate_scanner_mac"),
        ("duplicate_scanner_boot_id", "duplicate_scanner_boot_id"),
        ("mixed_family", "mixed_family"),
        ("contradictory_family", "reported_product_family_conflict"),
        ("wrong_component", "component_mismatch"),
        ("legacy_063", "migration_required"),
        ("unsupported_seed", "unsupported_target"),
        ("unknown_target", "unknown_target"),
    ],
)
def test_remote_readiness_fails_closed_for_invalid_topology(
    mutation: str,
    expected_blocker: str,
):
    heartbeat = healthy_fullsize_heartbeat()
    if mutation == "stale":
        heartbeat["last_seen"] = NOW - 121
    elif mutation == "missing_uplink":
        heartbeat.pop("firmware_target")
    elif mutation == "missing_scanner":
        heartbeat["scanners"].pop()
    elif mutation == "duplicate_scanner_mac":
        heartbeat["scanners"][1]["mac"] = heartbeat["scanners"][0]["mac"]
    elif mutation == "duplicate_scanner_boot_id":
        heartbeat["scanners"][1]["boot_id"] = heartbeat["scanners"][0]["boot_id"]
    elif mutation == "mixed_family":
        heartbeat["scanners"][1].update({
            "product_family": "badge_lite",
            "firmware_target": "scanner-s3-combo-backend",
            "app_project": "fof_backend_scanner",
            "hardware_type": "seeed_xiao_esp32s3",
        })
    elif mutation == "contradictory_family":
        heartbeat["scanners"][1]["product_family"] = "badge_lite"
    elif mutation == "wrong_component":
        heartbeat["scanners"][1]["component"] = "uplink"
    elif mutation == "legacy_063":
        heartbeat.update({
            "product_family": "s3_fullsize",
            "firmware_line": "legacy",
            "firmware_target": "uplink-s3",
            "app_project": "fof_uplink",
            "hardware_type": "esp32-s3-devkitc-1",
        })
    elif mutation == "unsupported_seed":
        heartbeat["scanners"][1].update({
            "product_family": "s3_fullsize",
            "firmware_line": "legacy",
            "firmware_target": "scanner-s3-combo-seed",
            "app_project": "fof_scanner_seed",
            "hardware_type": "esp32-s3-devkitc-1",
        })
    elif mutation == "unknown_target":
        heartbeat["scanners"][1]["firmware_target"] = "scanner-s3-unknown"

    blockers = remote_update_blockers(
        heartbeat,
        "scanner-s3-combo-fullsize-backend",
        now=NOW,
    )

    assert expected_blocker in blockers


LEGACY_UPLINK_PARTITIONS = [
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
]


def attended_uplink_receipt() -> dict:
    return {
        "schema": 1,
        "authority": "local_operator",
        "rom_chip_model": "ESP32-S3",
        "flash_size": 0x1000000,
        "partitions": [list(row) for row in LEGACY_UPLINK_PARTITIONS],
        "board_attestation": "production_gpio48",
        "led_gpio": 48,
        "mac": "AA:BB:CC:DD:EE:10",
        "physical_role": "uplink",
        "firmware_target": "uplink-s3",
        "app_project": "fof_uplink",
        "hardware_type": "esp32-s3-devkitc-1",
        "secure_boot": False,
        "flash_encryption": False,
    }


def test_attended_exact_inventory_receipt_only_labels_a_migration_candidate():
    candidate = resolve_attended_migration_candidate(attended_uplink_receipt())

    assert candidate == {
        "candidate_label": "s3_fullsize_migration_candidate",
        "authorized_action": "attended_usb_migration",
        "runtime_product_family": None,
        "mac": "AA:BB:CC:DD:EE:10",
        "physical_role": "uplink",
    }


@pytest.mark.parametrize(
    "mutation",
    ["8mb", "gpio38", "mixed_role", "altered_partition", "secure_boot", "encrypted"],
)
def test_attended_inventory_receipt_rejects_incomplete_or_unsafe_evidence(mutation: str):
    receipt = attended_uplink_receipt()
    if mutation == "8mb":
        receipt["flash_size"] = 0x800000
    elif mutation == "gpio38":
        receipt["led_gpio"] = 38
        receipt["board_attestation"] = "devkitc_v1_1_gpio38"
    elif mutation == "mixed_role":
        receipt["physical_role"] = "scanner0"
    elif mutation == "altered_partition":
        receipt["partitions"][4][4] = 0x300000
    elif mutation == "secure_boot":
        receipt["secure_boot"] = True
    elif mutation == "encrypted":
        receipt["flash_encryption"] = True

    assert resolve_attended_migration_candidate(receipt) is None
