import asyncio
import subprocess
import time

import pytest
from httpx import ASGITransport, AsyncClient

from app.main import app
from app.routers import detections, nodes


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
                        "board": "scanner-s3-combo",
                        "ver": "0.63.0-svc140",
                        "cmd_rx": 3,
                        "fw_check_count": 1,
                        "fw_state": "idle",
                    },
                    {
                        "uart": "wifi",
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
