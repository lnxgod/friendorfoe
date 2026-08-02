import asyncio
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
    assert detections._firmware_version_state(
        "0.1.0-backend", "uplink-s3-backend",
    ) == "current"
    assert detections._firmware_version_state(
        "0.1.1-backend", "uplink-s3-backend",
    ) == "drift"
    assert detections._firmware_version_state(
        detections._EXPECTED_FIRMWARE_VERSION, "uplink-s3",
    ) == "current"


def _completed(cmd, stdout: bytes):
    return subprocess.CompletedProcess(cmd, 0, stdout=stdout, stderr=b"")


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
                "scanners": [
                    {
                        "uart": "ble",
                        "mac": "AA:BB:CC:DD:EE:01",
                        "boot_id": 1,
                        "board": "scanner-s3-combo",
                        "ver": "0.63.0-svc140",
                        "cmd_rx": 3,
                        "fw_check_count": 1,
                        "fw_state": "idle",
                    },
                    {
                        "uart": "wifi",
                        "mac": "AA:BB:CC:DD:EE:02",
                        "boot_id": 2,
                        "board": "scanner-s3-combo",
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
                "scanners": [
                    {
                        "uart": "ble",
                        "board": "scanner-s3-combo",
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
        assert name == "scanner-s3-combo"
        return b"fake fleet firmware"

    async def fake_version(name: str) -> str:
        assert name == "scanner-s3-combo"
        return "0.63.0-svc153"

    monkeypatch.setattr(nodes._firmware_mgr, "get_firmware_binary", fake_binary)
    monkeypatch.setattr(nodes._firmware_mgr, "get_firmware_version", fake_version)


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


@pytest.mark.asyncio
async def test_scanner_readiness_flags_missing_target_version(
    monkeypatch: pytest.MonkeyPatch,
):
    async def missing_version(name: str) -> None:
        assert name == "scanner-s3-combo"
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
                "scanners": [{
                    "firmware_target": "scanner-s3-combo-backend",
                    "app_project": "fof_backend_scanner",
                    "hardware_type": "seeed_xiao_esp32s3",
                    "firmware_version": "0.1.0-backend",
                    "mac": "AA:BB:CC:DD:EE:70",
                    "boot_id": 70,
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

    async def capture_upload(cmd, **kwargs):
        uploads.append(next(part for part in cmd if isinstance(part, str) and part.startswith("http://")))
        return _completed(cmd, b'{"ok":true,"stored":true}')

    def accept_test_image(device_id, firmware_name, image, uart=None, **kwargs):
        assert firmware_name == "scanner-s3-combo-backend"
        return kwargs.get("snapshot")

    monkeypatch.setattr(nodes._firmware_mgr, "get_firmware_binary", backend_binary)
    monkeypatch.setattr(nodes._firmware_mgr, "get_firmware_version", backend_version)
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
    assert rollout.status_code == 200, rollout.text
    assert [row["uart"] for row in rollout.json()["targets"]] == ["ble"]


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
    assert all(row["target_version"] == "0.63.0-svc153" for row in payload["results"])
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
        assert name == "scanner-s3-combo"
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
        "firmware": "scanner-s3-combo",
        "state": "blocked",
        "error": "stale_heartbeat",
    }]
    assert calls == []


@pytest.mark.asyncio
async def test_stage_fleet_uses_scanner_board_for_mixed_variants(
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
    assert payload["ok"] is True
    assert {row["firmware"] for row in payload["results"]} == {
        "scanner-s3-combo-seed",
        "scanner-s3-combo",
    }
    assert any("name=scanner-s3-combo-seed" in url for url in uploads)
    assert any("name=scanner-s3-combo&" in url for url in uploads)


@pytest.mark.asyncio
async def test_trigger_check_calls_uplink_trigger_endpoint(monkeypatch: pytest.MonkeyPatch):
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
    assert payload["ok"] is True
    assert payload["count"] == 2
    assert len(calls) == 2


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
        return True, "0.63.0-svc153", {
            "uart": "ble",
            "ver": "0.63.0-svc153",
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
async def test_gate_canary_rollout_both_targets_ble_then_wifi(
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
        assert start.status_code == 200, start.text
        assert start.json()["target_count"] == 2
        rollout_id = start.json()["rollout_id"]
        for _ in range(20):
            await asyncio.sleep(0)
            status = await client.get(f"/nodes/firmware/rollouts/{rollout_id}")
            payload = status.json()
            if payload["task_done"]:
                break

    assert payload["status"] == "done"
    assert list(payload["targets"]) == ["uplink_D0A148/ble", "uplink_D0A148/wifi"]
    assert waited == ["uplink_D0A148/ble", "uplink_D0A148/wifi"]
    trigger_urls = [url for url in calls if "/api/fw/trigger" in url]
    assert trigger_urls == [
        "http://192.168.1.202/api/fw/trigger?uart=ble",
        "http://192.168.1.202/api/fw/trigger?uart=wifi",
    ]


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
        return True, "0.63.0-svc153", {
            "uart": "ble",
            "ver": "0.63.0-svc153",
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
