import asyncio
import copy
import subprocess
import time

import pytest
from httpx import ASGITransport, AsyncClient

from app.main import app
from app.routers import detections, nodes


def test_release_catalog_versions_match_live_follow():
    production_version = "0.64.68-live-follow"
    badge_version = "0.67.2-badge-defcon34"

    assert app.version == production_version
    assert detections._EXPECTED_BACKEND_VERSION == production_version
    assert detections._EXPECTED_FIRMWARE_VERSION == production_version
    assert detections._EXPECTED_BADGE_FIRMWARE_VERSION == badge_version
    assert detections._firmware_version_state(
        badge_version, "uplink-s3-fof_badge",
    ) == "current"
    assert detections._firmware_version_state(
        badge_version, "scanner-s3-combo-fof_badge",
    ) == "current"
    assert detections._firmware_version_state(
        "0.64.76-badge-defcon34", "uplink-s3-fof_badge",
    ) == "drift"


def test_backend_versions_are_target_aware_without_repurposing_api_version():
    assert detections._EXPECTED_BACKEND_VERSION == app.version
    for target in (
        "uplink-s3-backend",
        "scanner-s3-combo-backend",
        "uplink-s3-fullsize-backend",
        "scanner-s3-combo-fullsize-backend",
    ):
        assert detections._firmware_version_state("0.2.0-backend", target) == "current"
        assert detections._firmware_version_state("0.1.0-backend", target) == "drift"
    assert detections._firmware_version_state(
        detections._EXPECTED_FIRMWARE_VERSION, "uplink-s3",
    ) == "current"


def _completed(cmd, stdout: bytes):
    return subprocess.CompletedProcess(cmd, 0, stdout=stdout, stderr=b"")


def _fullsize_fleet_heartbeat() -> dict:
    return {
        "device_id": "uplink_FULL",
        "ip": "192.168.1.80",
        "last_seen": time.time(),
        "product_family": "s3_fullsize",
        "firmware_line": "backend",
        "component": "uplink",
        "hardware_mac": "AA:BB:CC:DD:EE:80",
        "boot_id": 80,
        "firmware_target": "uplink-s3-fullsize-backend",
        "app_project": "fof_backend_uplink_fullsize",
        "hardware_type": "esp32s3_n16r8_fullsize",
        "scanners": [
            {
                "uart": "ble", "slot": 0, "mac": "AA:BB:CC:DD:EE:81",
                "boot_id": 81, "product_family": "s3_fullsize",
                "firmware_line": "backend", "component": "scanner",
                "firmware_target": "scanner-s3-combo-fullsize-backend",
                "app_project": "fof_backend_scanner_fullsize",
                "hardware_type": "esp32s3_n16r8_fullsize",
                "firmware_version": "0.2.0-backend", "cmd_rx": 1,
            },
            {
                "uart": "wifi", "slot": 1, "mac": "AA:BB:CC:DD:EE:82",
                "boot_id": 82, "product_family": "s3_fullsize",
                "firmware_line": "backend", "component": "scanner",
                "firmware_target": "scanner-s3-combo-fullsize-backend",
                "app_project": "fof_backend_scanner_fullsize",
                "hardware_type": "esp32s3_n16r8_fullsize",
                "firmware_version": "0.2.0-backend", "cmd_rx": 1,
            },
        ],
    }


@pytest.fixture(autouse=True)
def scanner_fleet_state(monkeypatch: pytest.MonkeyPatch):
    now = time.time()
    monkeypatch.setattr(
        detections,
        "_node_heartbeats",
        {
            "uplink_A": {
                "device_id": "uplink_A",
                "ip": "192.168.1.10",
                "last_seen": now,
                "firmware_target": "uplink-s3-backend",
                "app_project": "fof_backend_uplink",
                "hardware_type": "seeed_xiao_esp32s3",
                "hardware_mac": "AA:BB:CC:DD:EE:A0",
                "scanners": [
                    {
                        "uart": "ble",
                        "slot": 0,
                        "mac": "AA:BB:CC:DD:EE:01",
                        "boot_id": 1,
                        "firmware_target": "scanner-s3-combo-backend",
                        "app_project": "fof_backend_scanner",
                        "hardware_type": "seeed_xiao_esp32s3",
                        "board": "scanner-s3-combo-backend",
                        "ver": "0.63.0-svc140",
                        "cmd_rx": 3,
                        "fw_check_count": 1,
                        "fw_state": "idle",
                    },
                    {
                        "uart": "wifi",
                        "slot": 1,
                        "mac": "AA:BB:CC:DD:EE:02",
                        "boot_id": 2,
                        "firmware_target": "scanner-s3-combo-backend",
                        "app_project": "fof_backend_scanner",
                        "hardware_type": "seeed_xiao_esp32s3",
                        "board": "scanner-s3-combo-backend",
                        "ver": "0.63.0-svc140",
                        "cmd_rx": 1,
                        "fw_check_count": 1,
                        "fw_state": "idle",
                    },
                ],
            },
            "uplink_B": {
                "device_id": "uplink_B",
                "ip": "192.168.1.11",
                "last_seen": now,
                "firmware_target": "uplink-s3-backend",
                "app_project": "fof_backend_uplink",
                "hardware_type": "seeed_xiao_esp32s3",
                "hardware_mac": "AA:BB:CC:DD:EE:B0",
                "scanners": [
                    {
                        "uart": "ble",
                        "slot": 0,
                        "mac": "AA:BB:CC:DD:EE:B1",
                        "boot_id": 11,
                        "firmware_target": "scanner-s3-combo-backend",
                        "app_project": "fof_backend_scanner",
                        "hardware_type": "seeed_xiao_esp32s3",
                        "board": "scanner-s3-combo-backend",
                        "ver": "0.63.0-svc139",
                        "cmd_rx": 0,
                        "fw_check_count": 0,
                        "fw_state": "idle",
                    }
                ],
            },
        },
    )

    async def fake_binary(name: str) -> bytes:
        assert name == "scanner-s3-combo-backend"
        return b"fake fleet firmware"

    async def fake_version(name: str) -> str:
        assert name == "scanner-s3-combo-backend"
        return "0.1.1-backend"

    def fake_version_for_bytes(name: str, image: bytes) -> str:
        assert name == "scanner-s3-combo-backend"
        assert image == b"fake fleet firmware"
        return "0.1.1-backend"

    def accept_fleet_fixture_image(device_id, firmware_name, image, uart=None, **kwargs):
        assert firmware_name == "scanner-s3-combo-backend"
        return kwargs.get("snapshot")

    monkeypatch.setattr(nodes._firmware_mgr, "get_firmware_binary", fake_binary)
    monkeypatch.setattr(nodes._firmware_mgr, "get_firmware_version", fake_version)
    monkeypatch.setattr(
        nodes._firmware_mgr,
        "get_firmware_version_for_bytes",
        fake_version_for_bytes,
    )
    monkeypatch.setattr(nodes, "_require_ota_compatibility", accept_fleet_fixture_image)


@pytest.mark.asyncio
async def test_scanner_readiness_flags_usb_recovery_blocker():
    transport = ASGITransport(app=app)
    async with AsyncClient(transport=transport, base_url="http://test") as client:
        resp = await client.get("/nodes/firmware/scanner/readiness")

    assert resp.status_code == 200, resp.text
    payload = resp.json()
    blocked = [s for s in payload["scanners"] if s["device_id"] == "uplink_B"]
    assert payload["needs_usb_recovery_count"] == 1
    assert blocked[0]["needs_usb_recovery"] is True
    assert "scanner_command_ingress_unreachable" in blocked[0]["blockers"]


@pytest.mark.asyncio
async def test_scanner_readiness_filters_gate_canary_device_and_uart(
    monkeypatch: pytest.MonkeyPatch,
):
    now = time.time()
    monkeypatch.setattr(
        detections,
        "_node_heartbeats",
        {
            "uplink_D0A148": {
                "device_id": "uplink_D0A148",
                "ip": "192.168.1.202",
                "last_seen": now,
                "scanners": [
                    {
                        "uart": "ble",
                        "board": "scanner-s3-combo-seed",
                        "ver": "0.63.0-svc148",
                        "cmd_rx": 0,
                        "fw_check_count": 0,
                        "fw_state": "idle",
                    },
                    {
                        "uart": "wifi",
                        "board": "scanner-s3-combo-seed",
                        "ver": "0.63.0-svc148",
                        "cmd_rx": 0,
                        "fw_check_count": 0,
                        "fw_state": "idle",
                    },
                ],
            },
            "uplink_OTHER": {
                "device_id": "uplink_OTHER",
                "ip": "192.168.1.99",
                "last_seen": now,
                "scanners": [
                    {
                        "uart": "ble",
                        "board": "scanner-s3-combo",
                        "ver": "0.63.0-svc140",
                        "cmd_rx": 1,
                        "fw_check_count": 1,
                    }
                ],
            },
        },
    )

    async def fake_version(name: str) -> str:
        return "0.63.0-svc153"

    monkeypatch.setattr(nodes._firmware_mgr, "get_firmware_version", fake_version)

    transport = ASGITransport(app=app)
    async with AsyncClient(transport=transport, base_url="http://test") as client:
        resp = await client.get(
            "/nodes/firmware/scanner/readiness"
            "?device_id=uplink_D0A148&uart=wifi&firmware_name=scanner-s3-combo-seed"
        )

    assert resp.status_code == 200, resp.text
    payload = resp.json()
    assert payload["count"] == 1
    assert payload["scanners"][0]["device_id"] == "uplink_D0A148"
    assert payload["scanners"][0]["uart"] == "wifi"
    assert payload["scanners"][0]["target_firmware"] == "scanner-s3-combo-seed"
    assert payload["scanners"][0]["product_family"] is None
    assert payload["scanners"][0]["remote_update_eligible"] is False
    assert "unsupported_target" in payload["scanners"][0]["blockers"]


@pytest.mark.asyncio
async def test_fullsize_scanner_readiness_groups_exact_family_and_complete_trio(
    monkeypatch: pytest.MonkeyPatch,
):
    now = time.time()
    monkeypatch.setattr(detections, "_node_heartbeats", {
        "uplink_FULL": {
            "device_id": "uplink_FULL", "ip": "192.168.1.80", "last_seen": now,
            "product_family": "s3_fullsize", "firmware_line": "backend",
            "component": "uplink", "hardware_mac": "AA:BB:CC:DD:EE:80",
            "boot_id": 80,
            "firmware_target": "uplink-s3-fullsize-backend",
            "app_project": "fof_backend_uplink_fullsize",
            "hardware_type": "esp32s3_n16r8_fullsize",
            "scanners": [
                {
                    "uart": "ble", "slot": 0, "mac": "AA:BB:CC:DD:EE:81",
                    "boot_id": 81, "product_family": "s3_fullsize",
                    "firmware_line": "backend", "component": "scanner",
                    "firmware_target": "scanner-s3-combo-fullsize-backend",
                    "app_project": "fof_backend_scanner_fullsize",
                    "hardware_type": "esp32s3_n16r8_fullsize",
                    "firmware_version": "0.1.0-backend", "cmd_rx": 1,
                },
                {
                    "uart": "wifi", "slot": 1, "mac": "AA:BB:CC:DD:EE:82",
                    "boot_id": 82, "product_family": "s3_fullsize",
                    "firmware_line": "backend", "component": "scanner",
                    "firmware_target": "scanner-s3-combo-fullsize-backend",
                    "app_project": "fof_backend_scanner_fullsize",
                    "hardware_type": "esp32s3_n16r8_fullsize",
                    "firmware_version": "0.1.0-backend", "cmd_rx": 1,
                },
            ],
        },
    })

    async def fullsize_version(name: str) -> str:
        assert name == "scanner-s3-combo-fullsize-backend"
        return "0.1.0-backend"

    monkeypatch.setattr(nodes._firmware_mgr, "get_firmware_version", fullsize_version)

    async with AsyncClient(transport=ASGITransport(app=app), base_url="http://test") as client:
        response = await client.get(
            "/nodes/firmware/scanner/readiness"
            "?firmware_name=scanner-s3-combo-fullsize-backend"
        )

    assert response.status_code == 200, response.text
    payload = response.json()
    assert payload["families"] == {
        "Badge": 0, "Badge Lite": 0, "S3 Fullsize": 2, "Legacy / Unsupported": 0,
    }
    assert payload["ready_count"] == 2
    assert {row["product_family"] for row in payload["scanners"]} == {"s3_fullsize"}
    assert all(row["remote_update_eligible"] for row in payload["scanners"])
    assert all(row["blockers"] == [] for row in payload["scanners"])


@pytest.mark.asyncio
@pytest.mark.parametrize(
    "path",
    [
        "/nodes/firmware/scanner/stage-fleet"
        "?firmware_name=scanner-s3-combo-fullsize-backend",
        "/nodes/firmware/scanner/stage-fleet",
        "/nodes/firmware/scanner/rollout"
        "?firmware_name=scanner-s3-combo-fullsize-backend&mode=canary",
        "/nodes/firmware/scanner/rollout?mode=canary",
    ],
)
async def test_fullsize_fleet_and_rollout_reject_before_catalog_fetch_or_mutation(
    monkeypatch: pytest.MonkeyPatch,
    path: str,
):
    monkeypatch.setattr(
        detections,
        "_node_heartbeats",
        {"uplink_FULL": _fullsize_fleet_heartbeat()},
    )
    fetched: list[str] = []
    transfers: list[str] = []

    async def must_not_fetch(name: str):
        fetched.append(name)
        raise AssertionError("Fullsize old fleet path must reject before fetch")

    async def must_not_transfer(cmd, **kwargs):
        transfers.append(str(cmd))
        raise AssertionError("Fullsize old fleet path must reject before mutation")

    monkeypatch.setattr(nodes._firmware_mgr, "get_firmware_binary", must_not_fetch)
    monkeypatch.setattr(nodes._firmware_mgr, "get_firmware_version", must_not_fetch)
    monkeypatch.setattr(nodes, "_run_subprocess", must_not_transfer)

    async with AsyncClient(transport=ASGITransport(app=app), base_url="http://test") as client:
        response = await client.post(path)

    assert response.status_code == 409, response.text
    assert "backend-ota" in response.json()["detail"]
    assert fetched == []
    assert transfers == []


@pytest.mark.asyncio
async def test_fullsize_fleet_guard_fails_closed_on_canonical_scanner_target(
    monkeypatch: pytest.MonkeyPatch,
):
    heartbeat = copy.deepcopy(detections._node_heartbeats["uplink_A"])
    heartbeat["scanners"] = [{
        "uart": "ble",
        "firmware_target": "scanner-s3-combo-fullsize-backend",
        "app_project": "missing-authoritative-project",
    }]
    monkeypatch.setattr(detections, "_node_heartbeats", {"uplink_A": heartbeat})
    fetches: list[str] = []

    async def must_not_fetch(name: str):
        fetches.append(name)
        raise AssertionError("canonical Fullsize target must reject before fetch")

    monkeypatch.setattr(nodes._firmware_mgr, "get_firmware_binary", must_not_fetch)
    monkeypatch.setattr(nodes._firmware_mgr, "get_firmware_version", must_not_fetch)

    async with AsyncClient(transport=ASGITransport(app=app), base_url="http://test") as client:
        response = await client.post("/nodes/firmware/scanner/stage-fleet")

    assert response.status_code == 409, response.text
    assert "backend-ota" in response.json()["detail"]
    assert fetches == []


@pytest.mark.asyncio
async def test_scanner_readiness_flags_missing_target_version(
    monkeypatch: pytest.MonkeyPatch,
):
    async def missing_version(name: str) -> None:
        assert name == "scanner-s3-combo-backend"
        return None

    monkeypatch.setattr(nodes._firmware_mgr, "get_firmware_version", missing_version)

    transport = ASGITransport(app=app)
    async with AsyncClient(transport=transport, base_url="http://test") as client:
        resp = await client.get("/nodes/firmware/scanner/readiness")

    assert resp.status_code == 200, resp.text
    payload = resp.json()
    assert payload["scanners"][0]["target_version"] == ""
    assert "target_version_unknown" in payload["scanners"][0]["blockers"]


@pytest.mark.asyncio
async def test_default_readiness_and_stage_use_canonical_backend_scanner_identity(
    monkeypatch: pytest.MonkeyPatch,
):
    monkeypatch.setattr(
        detections,
        "_node_heartbeats",
        {
            "uplink_CANON": {
                "device_id": "uplink_CANON",
                "ip": "192.168.1.70",
                "last_seen": time.time(),
                "firmware_target": "uplink-s3-backend",
                "app_project": "fof_backend_uplink",
                "hardware_type": "seeed_xiao_esp32s3",
                "hardware_mac": "AA:BB:CC:DD:EE:6F",
                "scanners": [{
                    "firmware_target": "scanner-s3-combo-backend",
                    "app_project": "fof_backend_scanner",
                    "hardware_type": "seeed_xiao_esp32s3",
                    "firmware_version": "0.1.0-backend",
                    "mac": "AA:BB:CC:DD:EE:70",
                    "boot_id": 70,
                    "slot": 0,
                    "uart": "ble",
                }],
            },
        },
    )
    uploads: list[str] = []

    async def backend_binary(name: str) -> bytes:
        assert name == "scanner-s3-combo-backend"
        return b"canonical backend scanner image"

    async def backend_version(name: str) -> str:
        assert name == "scanner-s3-combo-backend"
        return "0.1.0-backend"

    def backend_version_for_bytes(name: str, image: bytes) -> str:
        assert name == "scanner-s3-combo-backend"
        assert image == b"canonical backend scanner image"
        return "0.1.0-backend"

    async def capture_upload(cmd, **kwargs):
        uploads.append(next(part for part in cmd if isinstance(part, str) and part.startswith("http://")))
        return _completed(cmd, b'{"ok":true,"stored":true}')

    def accept_test_image(device_id, firmware_name, image, uart=None, **kwargs):
        assert firmware_name == "scanner-s3-combo-backend"
        return kwargs.get("snapshot")

    monkeypatch.setattr(nodes._firmware_mgr, "get_firmware_binary", backend_binary)
    monkeypatch.setattr(nodes._firmware_mgr, "get_firmware_version", backend_version)
    monkeypatch.setattr(
        nodes._firmware_mgr,
        "get_firmware_version_for_bytes",
        backend_version_for_bytes,
    )
    monkeypatch.setattr(nodes, "_require_ota_compatibility", accept_test_image)
    monkeypatch.setattr(nodes, "_run_subprocess", capture_upload)

    transport = ASGITransport(app=app)
    async with AsyncClient(transport=transport, base_url="http://test") as client:
        readiness = await client.get("/nodes/firmware/scanner/readiness")
        staged = await client.post("/nodes/firmware/scanner/stage-fleet")

    assert readiness.status_code == 200, readiness.text
    scanner = readiness.json()["scanners"][0]
    assert scanner["target_firmware"] == "scanner-s3-combo-backend"
    assert scanner["current_version"] == "0.1.0-backend"
    assert scanner["already_current"] is True
    assert staged.status_code == 200, staged.text
    assert staged.json()["results"][0]["firmware"] == "scanner-s3-combo-backend"
    assert uploads == [
        "http://192.168.1.70/api/fw/upload?name=scanner-s3-combo-backend&version=0.1.0-backend"
    ]


@pytest.mark.asyncio
async def test_backend_scanner_identity_mismatch_is_blocked_from_fleet_execution(
    monkeypatch: pytest.MonkeyPatch,
):
    now = time.time()
    monkeypatch.setattr(
        detections,
        "_node_heartbeats",
        {
            "uplink_BACK": {
                "device_id": "uplink_BACK", "ip": "192.168.1.10", "last_seen": now,
                "firmware_target": "uplink-s3-backend",
                "app_project": "fof_backend_uplink", "hardware_type": "seeed_xiao_esp32s3",
                "scanners": [
                    {"uart": "ble", "mac": "AA:BB:CC:DD:EE:21", "boot_id": 21,
                     "firmware_target": "scanner-s3-combo-backend",
                     "app_project": "fof_backend_scanner", "hardware_type": "seeed_xiao_esp32s3",
                     "ver": "0.1.0-backend", "cmd_rx": 1, "fw_check_count": 1},
                    {"uart": "wifi", "firmware_target": "scanner-s3-combo-fof_badge",
                     "app_project": "fof_badge_scanner", "hardware_type": "seeed_xiao_esp32s3",
                     "ver": "0.67.2-badge-defcon34", "cmd_rx": 1, "fw_check_count": 1},
                ],
            },
        },
    )

    async def fake_version(name: str) -> str:
        assert name == "scanner-s3-combo-backend"
        return "0.1.0-backend"

    async def no_rollout(*args, **kwargs):
        return None

    monkeypatch.setattr(nodes._firmware_mgr, "get_firmware_version", fake_version)
    monkeypatch.setattr(nodes, "_run_scanner_rollout", no_rollout)

    transport = ASGITransport(app=app)
    async with AsyncClient(transport=transport, base_url="http://test") as client:
        readiness = await client.get(
            "/nodes/firmware/scanner/readiness?firmware_name=scanner-s3-combo-backend"
        )
        rollout = await client.post(
            "/nodes/firmware/scanner/rollout?firmware_name=scanner-s3-combo-backend"
        )

    assert readiness.status_code == 200, readiness.text
    mismatched = next(row for row in readiness.json()["scanners"] if row["uart"] == "wifi")
    assert "identity_mismatch" in mismatched["blockers"]
    assert rollout.status_code == 404, rollout.text
    assert rollout.json()["detail"] == "No online scanner targets found"


@pytest.mark.asyncio
async def test_stage_fleet_records_version_size_and_crc(monkeypatch: pytest.MonkeyPatch):
    calls: list[str] = []

    async def fake_run_subprocess(cmd, **kwargs):
        url = next((part for part in cmd if isinstance(part, str) and part.startswith("http://")), "")
        calls.append(url)
        assert "/api/fw/upload" in url
        assert kwargs.get("input") == b"fake fleet firmware"
        return _completed(cmd, b'{"ok":true,"stored":true,"size":19,"checksum":1234}')

    monkeypatch.setattr(nodes, "_run_subprocess", fake_run_subprocess)

    transport = ASGITransport(app=app)
    async with AsyncClient(transport=transport, base_url="http://test") as client:
        resp = await client.post("/nodes/firmware/scanner/stage-fleet")

    assert resp.status_code == 200, resp.text
    payload = resp.json()
    assert payload["ok"] is True
    assert payload["count"] == 2
    assert all(row["target_version"] == "0.1.1-backend" for row in payload["results"])
    assert all(row["size"] == 19 for row in payload["results"])
    assert all(row["crc32"] for row in payload["results"])
    assert len(calls) == 2


@pytest.mark.asyncio
async def test_stage_fleet_preflights_every_target_before_any_upload(
    monkeypatch: pytest.MonkeyPatch,
):
    now = time.time()
    detections._node_heartbeats["uplink_A"].update({
        "firmware_target": "uplink-s3-backend",
        "app_project": "fof_backend_uplink",
        "hardware_type": "seeed_xiao_esp32s3",
        "scanners": [
            {"uart": "ble", "mac": "AA:BB:CC:DD:EE:10", "boot_id": 10,
             "firmware_target": "scanner-s3-combo-backend",
             "app_project": "fof_backend_scanner", "hardware_type": "seeed_xiao_esp32s3"},
            {"uart": "wifi", "mac": "AA:BB:CC:DD:EE:11", "boot_id": 11,
             "firmware_target": "scanner-s3-combo-fof_badge",
             "app_project": "fof_badge_scanner", "hardware_type": "seeed_xiao_esp32s3"},
        ],
    })
    detections._node_heartbeats["uplink_B"]["last_seen"] = now - 120
    calls: list[str] = []

    async def backend_version(name: str) -> str:
        assert name == "scanner-s3-combo-backend"
        return "0.1.0-backend"

    async def must_not_transfer(cmd, **kwargs):
        calls.append(next(part for part in cmd if isinstance(part, str) and part.startswith("http://")))
        raise AssertionError("fleet must finish preflight before any upload")

    monkeypatch.setattr(nodes._firmware_mgr, "get_firmware_version", backend_version)
    monkeypatch.setattr(nodes, "_run_subprocess", must_not_transfer)

    transport = ASGITransport(app=app)
    async with AsyncClient(transport=transport, base_url="http://test") as client:
        readiness = await client.get(
            "/nodes/firmware/scanner/readiness"
            "?firmware_name=scanner-s3-combo-backend&include_offline=true"
        )
        response = await client.post(
            "/nodes/firmware/scanner/stage-fleet"
            "?firmware_name=scanner-s3-combo-backend&include_offline=true"
        )

    stale = next(row for row in readiness.json()["scanners"] if row["device_id"] == "uplink_B")
    assert "stale_heartbeat" in stale["blockers"]
    assert "identity_mismatch" not in stale["blockers"]
    assert response.status_code == 200, response.text
    assert response.json()["ok"] is False
    assert {row["error"] for row in response.json()["results"]} == {
        "identity_mismatch", "stale_heartbeat",
    }
    assert calls == []


@pytest.mark.asyncio
async def test_stage_fleet_final_revalidation_blocks_later_target_changed_during_prefetch(
    monkeypatch: pytest.MonkeyPatch,
):
    calls: list[str] = []

    async def mutate_later_target(name: str) -> bytes:
        assert name == "scanner-s3-combo-backend"
        detections._node_heartbeats["uplink_B"]["last_seen"] = time.time() - 120
        return b"fake fleet firmware"

    async def must_not_transfer(cmd, **kwargs):
        calls.append(next(part for part in cmd if isinstance(part, str) and part.startswith("http://")))
        raise AssertionError("fleet must revalidate every target before the first transfer")

    monkeypatch.setattr(nodes._firmware_mgr, "get_firmware_binary", mutate_later_target)
    monkeypatch.setattr(nodes, "_run_subprocess", must_not_transfer)

    transport = ASGITransport(app=app)
    async with AsyncClient(transport=transport, base_url="http://test") as client:
        response = await client.post("/nodes/firmware/scanner/stage-fleet")

    assert response.status_code == 200, response.text
    assert response.json()["ok"] is False
    assert response.json()["results"] == [{
        "ok": False,
        "device_id": "uplink_B",
        "firmware": "scanner-s3-combo-backend",
        "state": "blocked",
        "error": "stale_heartbeat",
    }]
    assert calls == []


@pytest.mark.asyncio
async def test_stage_fleet_revalidates_after_each_unique_binary_fetch_a_b_a(
    monkeypatch: pytest.MonkeyPatch,
):
    lite = copy.deepcopy(detections._node_heartbeats["uplink_A"])
    lite["scanners"] = [lite["scanners"][0]]
    badge = {
        "device_id": "uplink_BADGE",
        "ip": "192.168.1.20",
        "last_seen": time.time(),
        "firmware_target": "uplink-s3-fof_badge",
        "app_project": "fof_badge_uplink",
        "hardware_type": "seeed_xiao_esp32s3",
        "hardware_mac": "AA:BB:CC:DD:EE:20",
        "scanners": [{
            "uart": "ble", "slot": 0, "mac": "AA:BB:CC:DD:EE:21", "boot_id": 21,
            "firmware_target": "scanner-s3-combo-fof_badge",
            "app_project": "fof_badge_scanner",
            "hardware_type": "seeed_xiao_esp32s3",
            "board": "scanner-s3-combo-fof_badge", "cmd_rx": 1,
        }],
    }
    monkeypatch.setattr(
        detections,
        "_node_heartbeats",
        {"uplink_A": lite, "uplink_BADGE": badge},
    )
    binary_fetches: list[str] = []
    transfers: list[str] = []

    async def version(name: str) -> str:
        return "0.2.0-backend" if name.endswith("-backend") else "0.67.2-badge-defcon34"

    async def mutate_then_restore(name: str) -> bytes:
        binary_fetches.append(name)
        if len(binary_fetches) == 1:
            lite["scanners"][0]["mac"] = "AA:BB:CC:DD:EE:99"
        else:
            lite["scanners"][0]["mac"] = "AA:BB:CC:DD:EE:01"
        return f"valid fixture for {name}".encode()

    async def must_not_transfer(cmd, **kwargs):
        transfers.append(str(cmd))
        raise AssertionError("fleet A -> B -> A change must reject before upload")

    monkeypatch.setattr(nodes._firmware_mgr, "get_firmware_version", version)
    monkeypatch.setattr(nodes._firmware_mgr, "get_firmware_binary", mutate_then_restore)
    monkeypatch.setattr(
        nodes,
        "_require_ota_compatibility",
        lambda *args, **kwargs: kwargs.get("snapshot"),
    )
    monkeypatch.setattr(nodes, "_run_subprocess", must_not_transfer)

    async with AsyncClient(transport=ASGITransport(app=app), base_url="http://test") as client:
        response = await client.post("/nodes/firmware/scanner/stage-fleet")

    assert response.status_code == 200, response.text
    assert response.json()["ok"] is False
    assert binary_fetches == ["scanner-s3-combo-backend"]
    assert transfers == []


@pytest.mark.asyncio
async def test_stage_fleet_rejects_legacy_and_seed_variants_before_upload(
    monkeypatch: pytest.MonkeyPatch,
):
    now = time.time()
    monkeypatch.setattr(
        detections,
        "_node_heartbeats",
        {
            "uplink_MIXED": {
                "device_id": "uplink_MIXED",
                "ip": "192.168.1.50",
                "last_seen": now,
                "scanners": [
                    {
                        "uart": "ble",
                        "board": "scanner-s3-combo-seed",
                        "ver": "0.64.42-node-redeploy",
                        "cmd_rx": 1,
                        "fw_check_count": 1,
                    },
                    {
                        "uart": "wifi",
                        "board": "scanner-s3-combo",
                        "ver": "0.64.42-node-redeploy",
                        "cmd_rx": 1,
                        "fw_check_count": 1,
                    },
                ],
            },
        },
    )

    async def fake_binary(name: str) -> bytes:
        assert name in {"scanner-s3-combo-seed", "scanner-s3-combo"}
        return f"{name} firmware".encode()

    async def fake_version(name: str) -> str:
        assert name in {"scanner-s3-combo-seed", "scanner-s3-combo"}
        return "0.64.68-live-follow"

    uploads: list[str] = []

    async def fake_run_subprocess(cmd, **kwargs):
        url = next((part for part in cmd if isinstance(part, str) and part.startswith("http://")), "")
        uploads.append(url)
        return _completed(cmd, b'{"ok":true,"stored":true,"size":19,"checksum":1234}')

    monkeypatch.setattr(nodes._firmware_mgr, "get_firmware_binary", fake_binary)
    monkeypatch.setattr(nodes._firmware_mgr, "get_firmware_version", fake_version)
    monkeypatch.setattr(nodes, "_run_subprocess", fake_run_subprocess)

    transport = ASGITransport(app=app)
    async with AsyncClient(transport=transport, base_url="http://test") as client:
        resp = await client.post("/nodes/firmware/scanner/stage-fleet")

    assert resp.status_code == 200, resp.text
    payload = resp.json()
    assert payload["ok"] is False
    assert {row["error"] for row in payload["results"]} == {"identity_mismatch"}
    assert uploads == []


@pytest.mark.asyncio
async def test_trigger_check_mutates_only_complete_exact_node(monkeypatch: pytest.MonkeyPatch):
    calls: list[str] = []

    async def fake_run_subprocess(cmd, **kwargs):
        url = next((part for part in cmd if isinstance(part, str) and part.startswith("http://")), "")
        calls.append(url)
        assert "/api/fw/trigger?uart=both" in url
        return _completed(cmd, b'{"ok":true,"uart":"both","ble_sent":true,"wifi_sent":true}')

    monkeypatch.setattr(nodes, "_run_subprocess", fake_run_subprocess)

    transport = ASGITransport(app=app)
    async with AsyncClient(transport=transport, base_url="http://test") as client:
        resp = await client.post("/nodes/firmware/scanner/trigger-check")

    assert resp.status_code == 200, resp.text
    payload = resp.json()
    assert payload["ok"] is False
    assert payload["count"] == 2
    assert len(calls) == 1
    assert next(row for row in payload["results"] if row["device_id"] == "uplink_B")[
        "state"
    ] == "blocked"


@pytest.mark.asyncio
@pytest.mark.parametrize(
    "mutation",
    ["legacy", "seed", "generic", "mixed", "stale", "incomplete", "fullsize"],
)
async def test_trigger_check_rejects_non_authoritative_nodes_without_mutation(
    monkeypatch: pytest.MonkeyPatch,
    mutation: str,
):
    if mutation == "fullsize":
        heartbeat = _fullsize_fleet_heartbeat()
    else:
        heartbeat = copy.deepcopy(detections._node_heartbeats["uplink_A"])
        heartbeat["device_id"] = "uplink_BLOCKED"
        heartbeat["ip"] = "192.168.1.90"
        if mutation == "legacy":
            heartbeat.update({
                "firmware_target": "uplink-s3",
                "app_project": "fof_uplink",
                "hardware_type": "esp32-s3-devkitc-1",
            })
            heartbeat["scanners"][0].update({
                "firmware_target": "scanner-s3-combo",
                "app_project": "fof_scanner",
                "hardware_type": "esp32-s3-devkitc-1",
            })
        elif mutation == "seed":
            heartbeat["scanners"][0].update({
                "firmware_target": "scanner-s3-combo-seed",
                "app_project": "fof_scanner_seed",
                "hardware_type": "esp32-s3-devkitc-1",
            })
        elif mutation == "generic":
            for field in ("firmware_target", "app_project", "hardware_type"):
                heartbeat.pop(field, None)
                heartbeat["scanners"][0].pop(field, None)
        elif mutation == "mixed":
            heartbeat["scanners"][1].update({
                "firmware_target": "scanner-s3-combo-fof_badge",
                "app_project": "fof_badge_scanner",
                "hardware_type": "seeed_xiao_esp32s3",
            })
        elif mutation == "stale":
            heartbeat["last_seen"] = time.time() - 121
        elif mutation == "incomplete":
            heartbeat["scanners"][0].pop("mac")
            heartbeat["scanners"][0].pop("boot_id")
    monkeypatch.setattr(
        detections,
        "_node_heartbeats",
        {heartbeat["device_id"]: heartbeat},
    )
    calls: list[str] = []

    async def must_not_transfer(cmd, **kwargs):
        calls.append(str(cmd))
        raise AssertionError("blocked trigger-check node must not mutate")

    monkeypatch.setattr(nodes, "_run_subprocess", must_not_transfer)

    async with AsyncClient(transport=ASGITransport(app=app), base_url="http://test") as client:
        response = await client.post(
            "/nodes/firmware/scanner/trigger-check",
            params={
                "device_id": heartbeat["device_id"],
                "uart": "both",
                "include_offline": "true",
            },
        )

    assert response.status_code == 200, response.text
    payload = response.json()
    assert payload["ok"] is False
    assert payload["count"] == 1
    assert payload["results"][0]["state"] == "blocked"
    assert calls == []


@pytest.mark.asyncio
async def test_trigger_helper_rejects_fullsize_before_preflight_or_subprocess(
    monkeypatch: pytest.MonkeyPatch,
):
    monkeypatch.setattr(
        detections,
        "_node_heartbeats",
        {"uplink_FULL": _fullsize_fleet_heartbeat()},
    )
    preflights: list[str] = []
    calls: list[str] = []

    real_preflight = nodes._require_named_family_preflight

    def track_preflight(device_id: str, firmware_name: str, uart: str):
        preflights.append(firmware_name)
        return real_preflight(device_id, firmware_name, uart)

    async def must_not_transfer(cmd, **kwargs):
        calls.append(str(cmd))
        raise AssertionError("direct trigger helper must not mutate Fullsize")

    monkeypatch.setattr(nodes, "_require_named_family_preflight", track_preflight)
    monkeypatch.setattr(nodes, "_run_subprocess", must_not_transfer)

    with pytest.raises(nodes.HTTPException) as error:
        await nodes._trigger_scanner_firmware_check(
            "uplink_FULL",
            "scanner-s3-combo-fullsize-backend",
            "ble",
        )

    assert error.value.status_code == 409
    assert "backend-ota" in str(error.value.detail)
    assert preflights == []
    assert calls == []


@pytest.mark.asyncio
@pytest.mark.parametrize("stage_first", [True, False], ids=["stage", "trigger"])
async def test_rollout_worker_revalidates_bound_identity_before_each_mutation(
    monkeypatch: pytest.MonkeyPatch,
    stage_first: bool,
):
    heartbeat = detections._node_heartbeats["uplink_A"]
    scanner = copy.deepcopy(heartbeat["scanners"][0])
    firmware = "scanner-s3-combo-backend"
    initial = nodes._require_named_family_preflight("uplink_A", firmware, "ble")
    target = {
        "device_id": "uplink_A",
        "ip": heartbeat["ip"],
        "uart": "ble",
        "scanner": scanner,
        "target_firmware": firmware,
        "target_version": "0.2.0-backend",
        "self_update_capable": True,
        "identity_snapshot": initial,
    }
    rollout_id = f"identity-change-{stage_first}"
    nodes._firmware_rollouts[rollout_id] = {
        "rollout_id": rollout_id,
        "status": "queued",
        "stage_results": [],
        "targets": {
            "uplink_A/ble": {
                "device_id": "uplink_A", "uart": "ble", "state": "pending",
            },
        },
    }
    heartbeat["scanners"][0]["mac"] = "AA:BB:CC:DD:EE:99"
    fetches: list[str] = []
    transfers: list[str] = []

    async def must_not_fetch(name: str):
        fetches.append(name)
        raise AssertionError("changed rollout identity must reject before fetch")

    async def must_not_transfer(cmd, **kwargs):
        transfers.append(str(cmd))
        raise AssertionError("changed rollout identity must reject before mutation")

    monkeypatch.setattr(nodes._firmware_mgr, "get_firmware_binary", must_not_fetch)
    monkeypatch.setattr(nodes, "_run_subprocess", must_not_transfer)

    await nodes._run_scanner_rollout(rollout_id, [target], stage_first=stage_first)

    item = nodes._firmware_rollouts[rollout_id]["targets"]["uplink_A/ble"]
    assert item["state"] == "blocked"
    assert fetches == []
    assert transfers == []


@pytest.mark.asyncio
@pytest.mark.parametrize(
    "stage_error",
    [
        "firmware_not_available",
        "stage_timeout",
        "stage_curl:connection refused",
        "stage_rejected",
    ],
)
async def test_rollout_worker_never_triggers_after_returned_stage_failure(
    monkeypatch: pytest.MonkeyPatch,
    stage_error: str,
):
    heartbeat = detections._node_heartbeats["uplink_A"]
    firmware = "scanner-s3-combo-backend"
    initial = nodes._require_named_family_preflight("uplink_A", firmware, "ble")
    target = {
        "device_id": "uplink_A",
        "ip": heartbeat["ip"],
        "uart": "ble",
        "scanner": copy.deepcopy(heartbeat["scanners"][0]),
        "target_firmware": firmware,
        "target_version": "0.2.0-backend",
        "self_update_capable": True,
        "identity_snapshot": initial,
    }
    rollout_id = f"returned-stage-failure-{stage_error}"
    nodes._firmware_rollouts[rollout_id] = {
        "rollout_id": rollout_id,
        "status": "queued",
        "stage_results": [],
        "targets": {
            "uplink_A/ble": {
                "device_id": "uplink_A", "uart": "ble", "state": "pending",
            },
        },
    }
    trigger_calls: list[str] = []

    async def failed_stage(*args, **kwargs) -> dict:
        return {
            "ok": False,
            "device_id": "uplink_A",
            "firmware": firmware,
            "state": "failed",
            "error": stage_error,
        }

    async def must_not_trigger(device_id: str, *args, **kwargs):
        trigger_calls.append(device_id)
        raise AssertionError("a failed stage must block its later trigger")

    monkeypatch.setattr(nodes, "_stage_scanner_firmware_on_uplink", failed_stage)
    monkeypatch.setattr(nodes, "_trigger_scanner_firmware_check", must_not_trigger)

    await nodes._run_scanner_rollout(rollout_id, [target], stage_first=True)

    item = nodes._firmware_rollouts[rollout_id]["targets"]["uplink_A/ble"]
    assert item["state"] == "blocked"
    assert item["error"] == stage_error
    assert trigger_calls == []


@pytest.mark.asyncio
async def test_rollout_start_hands_worker_an_authoritative_identity_snapshot(
    monkeypatch: pytest.MonkeyPatch,
):
    captured: list[dict] = []

    async def capture_rollout(rollout_id: str, targets: list[dict], *, stage_first: bool):
        captured.extend(targets)

    monkeypatch.setattr(nodes, "_run_scanner_rollout", capture_rollout)

    async with AsyncClient(transport=ASGITransport(app=app), base_url="http://test") as client:
        response = await client.post(
            "/nodes/firmware/scanner/rollout",
            params={
                "mode": "canary",
                "firmware_name": "scanner-s3-combo-backend",
                "canary_device_id": "uplink_A",
                "canary_uart": "ble",
            },
        )
        await asyncio.sleep(0)

    assert response.status_code == 200, response.text
    assert len(captured) == 1
    assert captured[0]["identity_snapshot"]["device_id"] == "uplink_A"
    assert captured[0]["identity_snapshot"]["scanners"][0]["mac"] == (
        "AA:BB:CC:DD:EE:01"
    )


@pytest.mark.asyncio
async def test_explicit_lite_canary_ignores_unrelated_fullsize_inventory(
    monkeypatch: pytest.MonkeyPatch,
):
    detections._node_heartbeats["uplink_FULL"] = _fullsize_fleet_heartbeat()
    captured: list[dict] = []

    async def capture_rollout(rollout_id: str, targets: list[dict], *, stage_first: bool):
        captured.extend(targets)

    monkeypatch.setattr(nodes, "_run_scanner_rollout", capture_rollout)

    async with AsyncClient(transport=ASGITransport(app=app), base_url="http://test") as client:
        response = await client.post(
            "/nodes/firmware/scanner/rollout",
            params={
                "mode": "canary",
                "firmware_name": "scanner-s3-combo-backend",
                "canary_device_id": "uplink_A",
                "canary_uart": "ble",
            },
        )
        await asyncio.sleep(0)

    assert response.status_code == 200, response.text
    assert [(target["device_id"], target["uart"]) for target in captured] == [
        ("uplink_A", "ble"),
    ]


@pytest.mark.asyncio
async def test_explicit_fullsize_canary_remains_blocked_with_unrelated_lite_inventory(
    monkeypatch: pytest.MonkeyPatch,
):
    detections._node_heartbeats["uplink_FULL"] = _fullsize_fleet_heartbeat()
    calls: list[str] = []

    async def must_not_fetch(name: str):
        calls.append(name)
        raise AssertionError("selected Fullsize canary must reject before fetch")

    monkeypatch.setattr(nodes._firmware_mgr, "get_firmware_version", must_not_fetch)

    async with AsyncClient(transport=ASGITransport(app=app), base_url="http://test") as client:
        response = await client.post(
            "/nodes/firmware/scanner/rollout",
            params={
                "mode": "canary",
                "canary_device_id": "uplink_FULL",
                "canary_uart": "ble",
            },
        )

    assert response.status_code == 409, response.text
    assert "backend-ota" in response.json()["detail"]
    assert calls == []


@pytest.mark.asyncio
async def test_canary_rollout_stages_triggers_and_requires_heartbeat_proof(
    monkeypatch: pytest.MonkeyPatch,
):
    calls: list[str] = []

    async def fake_run_subprocess(cmd, **kwargs):
        url = next((part for part in cmd if isinstance(part, str) and part.startswith("http://")), "")
        calls.append(url)
        if "/api/fw/upload" in url:
            return _completed(cmd, b'{"ok":true,"stored":true,"size":19,"checksum":1234}')
        if "/api/fw/trigger" in url:
            return _completed(cmd, b'{"ok":true,"uart":"ble","ble_sent":true}')
        raise AssertionError(f"unexpected URL {url}")

    async def fake_wait_for_scanner_version(*args, **kwargs):
        return True, "0.1.1-backend", {
            "uart": "ble",
            "ver": "0.1.1-backend",
            "cmd_rx": 2,
            "fw_check_count": 1,
        }

    monkeypatch.setattr(nodes, "_run_subprocess", fake_run_subprocess)
    monkeypatch.setattr(nodes, "_wait_for_scanner_version", fake_wait_for_scanner_version)

    transport = ASGITransport(app=app)
    async with AsyncClient(transport=transport, base_url="http://test") as client:
        start = await client.post("/nodes/firmware/scanner/rollout?mode=canary")
        assert start.status_code == 200, start.text
        rollout_id = start.json()["rollout_id"]
        for _ in range(20):
            await asyncio.sleep(0)
            status = await client.get(f"/nodes/firmware/rollouts/{rollout_id}")
            payload = status.json()
            if payload["task_done"]:
                break

    assert payload["status"] == "done"
    assert list(payload["targets"].values())[0]["state"] == "verified"
    assert any("/api/fw/upload" in url for url in calls)
    assert any("/api/fw/trigger?uart=ble" in url for url in calls)


@pytest.mark.asyncio
async def test_seed_canary_is_rejected_before_stage_or_trigger(
    monkeypatch: pytest.MonkeyPatch,
):
    now = time.time()
    monkeypatch.setattr(
        detections,
        "_node_heartbeats",
        {
            "uplink_D0A148": {
                "device_id": "uplink_D0A148",
                "ip": "192.168.1.202",
                "last_seen": now,
                "scanners": [
                    {
                        "uart": "ble",
                        "board": "scanner-s3-combo-seed",
                        "ver": "0.63.0-svc148",
                        "cmd_rx": 2,
                        "fw_check_count": 1,
                        "fw_state": "idle",
                    },
                    {
                        "uart": "wifi",
                        "board": "scanner-s3-combo-seed",
                        "ver": "0.63.0-svc148",
                        "cmd_rx": 2,
                        "fw_check_count": 1,
                        "fw_state": "idle",
                    },
                ],
            },
            "uplink_OTHER": {
                "device_id": "uplink_OTHER",
                "ip": "192.168.1.99",
                "last_seen": now,
                "scanners": [
                    {
                        "uart": "ble",
                        "board": "scanner-s3-combo",
                        "ver": "0.63.0-svc140",
                        "cmd_rx": 2,
                        "fw_check_count": 1,
                    }
                ],
            },
        },
    )

    calls: list[str] = []
    waited: list[str] = []

    async def fake_binary(name: str) -> bytes:
        assert name == "scanner-s3-combo-seed"
        return b"fake seed firmware"

    async def fake_version(name: str) -> str:
        assert name == "scanner-s3-combo-seed"
        return "0.63.0-svc153"

    async def fake_run_subprocess(cmd, **kwargs):
        url = next((part for part in cmd if isinstance(part, str) and part.startswith("http://")), "")
        calls.append(url)
        if "/api/fw/upload" in url:
            return _completed(cmd, b'{"ok":true,"stored":true,"size":18,"checksum":5678}')
        if "/api/fw/trigger" in url:
            return _completed(cmd, b'{"ok":true,"uart":"slot","ble_sent":true,"wifi_sent":true}')
        raise AssertionError(f"unexpected URL {url}")

    async def fake_wait_for_scanner_version(device_id, uart, *args, **kwargs):
        waited.append(f"{device_id}/{uart}")
        return True, "0.63.0-svc153", {
            "uart": uart,
            "ver": "0.63.0-svc153",
            "cmd_rx": 3,
            "fw_check_count": 2,
        }

    monkeypatch.setattr(nodes._firmware_mgr, "get_firmware_binary", fake_binary)
    monkeypatch.setattr(nodes._firmware_mgr, "get_firmware_version", fake_version)
    monkeypatch.setattr(nodes, "_run_subprocess", fake_run_subprocess)
    monkeypatch.setattr(nodes, "_wait_for_scanner_version", fake_wait_for_scanner_version)

    transport = ASGITransport(app=app)
    async with AsyncClient(transport=transport, base_url="http://test") as client:
        start = await client.post(
            "/nodes/firmware/scanner/rollout"
            "?mode=canary&canary_device_id=uplink_D0A148"
            "&canary_uart=both&firmware_name=scanner-s3-combo-seed"
        )
        assert start.status_code == 404, start.text

    assert calls == []
    assert waited == []


@pytest.mark.asyncio
async def test_canary_rollout_success_requires_update_telemetry(
    monkeypatch: pytest.MonkeyPatch,
):
    calls: list[str] = []

    async def fake_run_subprocess(cmd, **kwargs):
        url = next((part for part in cmd if isinstance(part, str) and part.startswith("http://")), "")
        calls.append(url)
        if "/api/fw/upload" in url:
            return _completed(cmd, b'{"ok":true,"stored":true,"size":19,"checksum":1234}')
        if "/api/fw/trigger" in url:
            return _completed(cmd, b'{"ok":true,"uart":"ble","ble_sent":true}')
        raise AssertionError(f"unexpected URL {url}")

    async def fake_wait_for_scanner_version(*args, **kwargs):
        return True, "0.1.1-backend", {
            "uart": "ble",
            "ver": "0.1.1-backend",
            "cmd_rx": 0,
            "fw_check_count": 0,
            "fw_state": "idle",
        }

    monkeypatch.setattr(nodes, "_run_subprocess", fake_run_subprocess)
    monkeypatch.setattr(nodes, "_wait_for_scanner_version", fake_wait_for_scanner_version)

    transport = ASGITransport(app=app)
    async with AsyncClient(transport=transport, base_url="http://test") as client:
        start = await client.post("/nodes/firmware/scanner/rollout?mode=canary")
        assert start.status_code == 200, start.text
        rollout_id = start.json()["rollout_id"]
        for _ in range(20):
            await asyncio.sleep(0)
            status = await client.get(f"/nodes/firmware/rollouts/{rollout_id}")
            payload = status.json()
            if payload["task_done"]:
                break

    target = list(payload["targets"].values())[0]
    assert payload["status"] == "failed"
    assert target["state"] == "failed"
    assert target["error"] == "scanner_update_telemetry_missing"
    assert any("/api/fw/trigger?uart=ble" in url for url in calls)


@pytest.mark.asyncio
async def test_fleet_rollout_rejects_until_canary_verified():
    transport = ASGITransport(app=app)
    async with AsyncClient(transport=transport, base_url="http://test") as client:
        resp = await client.post("/nodes/firmware/scanner/rollout?mode=fleet")

    assert resp.status_code == 409
    assert "canary" in resp.json()["detail"].lower()
