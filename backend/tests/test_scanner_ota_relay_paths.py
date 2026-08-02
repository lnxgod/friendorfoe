import subprocess
import struct
import time
import zlib

import pytest
from httpx import ASGITransport, AsyncClient

from app.main import app
from app.routers import detections, nodes
from app.services import firmware_manager


def _completed(cmd, stdout: bytes):
    return subprocess.CompletedProcess(cmd, 0, stdout=stdout, stderr=b"")


def _backend_scanner_image() -> bytes:
    info = firmware_manager.FIRMWARE_TYPES["scanner-s3-combo-backend"]
    app_desc = bytearray(112)
    struct.pack_into("<I", app_desc, 0, 0xABCD5432)
    app_desc[16:48] = b"0.1.0-backend".ljust(32, b"\0")
    app_desc[48:80] = info["project"].encode().ljust(32, b"\0")
    app_desc[80:96] = b"12:00:00".ljust(16, b"\0")
    app_desc[96:112] = b"2026-08-01".ljust(16, b"\0")
    record = bytearray(firmware_manager._BACKEND_IDENTITY_STRUCT.size)
    firmware_manager._BACKEND_IDENTITY_STRUCT.pack_into(
        record, 0, 0x42464F46, 1, info["image_kind"],
        b"scanner-s3-combo-backend".ljust(40, b"\0"),
        info["project"].encode().ljust(40, b"\0"),
        info["hardware"].encode().ljust(40, b"\0"),
        b"0.1.0-backend".ljust(32, b"\0"), 0,
    )
    struct.pack_into("<I", record, 160, zlib.crc32(record[:160]) & 0xFFFFFFFF)
    image = bytearray(1200)
    image[0] = 0xE9
    image[0x20:0x90] = app_desc
    image[256:256 + len(record)] = record
    return bytes(image)


def _production_scanner_image() -> bytes:
    app_desc = bytearray(112)
    struct.pack_into("<I", app_desc, 0, 0xABCD5432)
    app_desc[16:48] = b"0.63.0-svc148".ljust(32, b"\0")
    app_desc[48:80] = b"fof_scanner".ljust(32, b"\0")
    app_desc[80:96] = b"12:00:00".ljust(16, b"\0")
    app_desc[96:112] = b"2026-08-01".ljust(16, b"\0")
    image = bytearray(1200)
    image[0] = 0xE9
    image[0x20:0x90] = app_desc
    markers = b"scanner-s3-combo\0esp32-s3-devkitc-1\0"
    image[256:256 + len(markers)] = markers
    return bytes(image)


PRODUCTION_SCANNER_IMAGE = _production_scanner_image()


@pytest.fixture(autouse=True)
def scanner_ota_state(monkeypatch: pytest.MonkeyPatch):
    monkeypatch.setattr(
        detections,
        "_node_heartbeats",
        {
            "uplink_TEST": {
                "ip": "192.168.1.10",
                "last_seen": time.time(),
                "scanners": [{
                    "uart": "ble",
                    "ver": "0.63.0-svc140",
                    "firmware_target": "scanner-s3-combo",
                    "app_project": "fof_scanner",
                    "hardware_type": "esp32-s3-devkitc-1",
                }],
            }
        },
    )

    async def fake_binary(name: str) -> bytes:
        assert name == "scanner-s3-combo"
        return PRODUCTION_SCANNER_IMAGE

    async def fake_version(name: str) -> str:
        assert name == "scanner-s3-combo"
        return "0.63.0-svc148"

    monkeypatch.setattr(nodes._firmware_mgr, "get_firmware_binary", fake_binary)
    monkeypatch.setattr(nodes._firmware_mgr, "get_firmware_version", fake_version)


@pytest.mark.asyncio
async def test_backend_scanner_ota_requires_exact_running_family(monkeypatch: pytest.MonkeyPatch):
    now = time.time()
    detections._node_heartbeats["uplink_TEST"] = {
        "device_id": "uplink_TEST", "ip": "192.168.1.10", "last_seen": now,
        "firmware_target": "uplink-s3-backend",
        "app_project": "fof_backend_uplink",
        "hardware_type": "seeed_xiao_esp32s3",
        "scanners": [
            {"uart": "ble", "mac": "AA:BB:CC:DD:EE:01", "boot_id": 1,
             "firmware_target": "scanner-s3-combo-backend",
             "app_project": "fof_backend_scanner", "hardware_type": "seeed_xiao_esp32s3",
             "ver": "0.1.0-backend", "cmd_rx": 1},
            {"uart": "wifi", "mac": "AA:BB:CC:DD:EE:02", "boot_id": 2,
             "firmware_target": "scanner-s3-combo-backend",
             "app_project": "fof_backend_scanner", "hardware_type": "seeed_xiao_esp32s3",
             "ver": "0.1.0-backend", "cmd_rx": 1},
        ],
    }

    async def fake_binary(name: str) -> bytes:
        return _backend_scanner_image() if name.endswith("-backend") else b"P" * 1200

    async def fake_version(name: str) -> str:
        return "0.1.0-backend" if name.endswith("-backend") else "0.64.68-live-follow"

    async def fake_run_subprocess(cmd, **kwargs):
        url = next((part for part in cmd if isinstance(part, str) and part.startswith("http://")), "")
        if "/api/fw/upload" in url:
            return _completed(cmd, b'{"ok":true,"stored":true}')
        if "/api/ota/relay" in url:
            return _completed(cmd, b'{"ok":true,"scanner_response":"sent"}')
        raise AssertionError(f"unexpected upload route {url}")

    async def exact_version(*args, **kwargs):
        return True, "0.1.0-backend", detections._node_heartbeats["uplink_TEST"]["scanners"][0]

    monkeypatch.setattr(nodes._firmware_mgr, "get_firmware_binary", fake_binary)
    monkeypatch.setattr(nodes._firmware_mgr, "get_firmware_version", fake_version)
    monkeypatch.setattr(nodes, "_run_subprocess", fake_run_subprocess)
    monkeypatch.setattr(nodes, "_wait_for_scanner_version", exact_version)

    transport = ASGITransport(app=app)
    async with AsyncClient(transport=transport, base_url="http://test") as client:
        pushed = await client.post(
            "/nodes/uplink_TEST/ota/scanner/scanner-s3-combo-backend?uart=ble&relay_mode=direct_legacy"
        )
        staged = await client.post(
            "/nodes/uplink_TEST/ota/scanner/scanner-s3-combo-backend/stage"
        )
        detections._node_heartbeats["uplink_TEST"]["scanners"][1]["firmware_target"] = "scanner-s3-combo-fof_badge"
        detections._node_heartbeats["uplink_TEST"]["scanners"][1]["app_project"] = "fof_badge_scanner"
        badge = await client.post(
            "/nodes/uplink_TEST/ota/scanner/scanner-s3-combo-backend?uart=wifi&relay_mode=direct_legacy"
        )
        production = await client.post(
            "/nodes/uplink_TEST/ota/scanner/scanner-s3-combo?uart=ble&relay_mode=direct_legacy"
        )
        both = await client.post(
            "/nodes/uplink_TEST/ota/scanner/scanner-s3-combo-backend?uart=both&relay_mode=direct_legacy"
        )
        blocked_stage = await client.post(
            "/nodes/uplink_TEST/ota/scanner/scanner-s3-combo-backend/stage"
        )

    assert pushed.status_code == 200, pushed.text
    assert staged.status_code == 200, staged.text
    assert badge.status_code == 409
    assert production.status_code == 409
    assert both.status_code == 409
    assert blocked_stage.status_code == 409


@pytest.mark.asyncio
async def test_backend_scanner_ota_rejects_same_family_scanner_replacement_during_fetch(
    monkeypatch: pytest.MonkeyPatch,
):
    now = time.time()
    detections._node_heartbeats["uplink_TEST"] = {
        "device_id": "uplink_TEST", "ip": "192.168.1.10", "last_seen": now,
        "scanners": [{
            "uart": "ble", "mac": "AA:BB:CC:DD:EE:01", "boot_id": 1,
            "firmware_target": "scanner-s3-combo-backend",
            "app_project": "fof_backend_scanner", "hardware_type": "seeed_xiao_esp32s3",
            "ver": "0.1.0-backend", "cmd_rx": 1,
        }],
    }
    transfers: list[str] = []

    async def replace_scanner(name: str) -> bytes:
        assert name == "scanner-s3-combo-backend"
        detections._node_heartbeats["uplink_TEST"]["scanners"][0]["boot_id"] = 2
        return _backend_scanner_image()

    async def backend_version(name: str) -> str:
        assert name == "scanner-s3-combo-backend"
        return "0.1.0-backend"

    async def must_not_transfer(cmd, **kwargs):
        transfers.append(next(part for part in cmd if isinstance(part, str) and part.startswith("http://")))
        raise AssertionError("scanner replacement must stop OTA before transfer")

    monkeypatch.setattr(nodes._firmware_mgr, "get_firmware_binary", replace_scanner)
    monkeypatch.setattr(nodes._firmware_mgr, "get_firmware_version", backend_version)
    monkeypatch.setattr(nodes, "_run_subprocess", must_not_transfer)

    transport = ASGITransport(app=app)
    async with AsyncClient(transport=transport, base_url="http://test") as client:
        response = await client.post(
            "/nodes/uplink_TEST/ota/scanner/scanner-s3-combo-backend"
            "?uart=ble&relay_mode=direct_legacy"
        )

    assert response.status_code == 409
    assert "identity changed" in response.json()["detail"]
    assert transfers == []


@pytest.mark.asyncio
async def test_scanner_ota_both_rejects_duplicate_ble_identity(monkeypatch: pytest.MonkeyPatch):
    now = time.time()
    detections._node_heartbeats["uplink_TEST"] = {
        "device_id": "uplink_TEST", "ip": "192.168.1.10", "last_seen": now,
        "scanners": [
            {"uart": "ble", "ver": "0.63.0-svc140"},
            {"uart": "ble", "ver": "0.63.0-svc140"},
        ],
    }
    # The endpoint must fail in preflight, without loading or transferring bytes.
    async def must_not_load(name: str) -> bytes:
        raise AssertionError("must reject ambiguous uart=both before firmware load")

    monkeypatch.setattr(nodes._firmware_mgr, "get_firmware_binary", must_not_load)

    transport = ASGITransport(app=app)
    async with AsyncClient(transport=transport, base_url="http://test") as client:
        duplicate_uart = await client.post(
            "/nodes/uplink_TEST/ota/scanner/scanner-s3-combo?uart=both&relay_mode=direct_legacy"
        )
        detections._node_heartbeats["uplink_TEST"]["scanners"] = [
            {"uart": "ble", "mac": "AA:BB:CC:DD:EE:FF", "boot_id": 9},
            {"uart": "wifi", "mac": "AA:BB:CC:DD:EE:FF", "boot_id": 9},
        ]
        duplicate_identity = await client.post(
            "/nodes/uplink_TEST/ota/scanner/scanner-s3-combo?uart=both&relay_mode=direct_legacy"
        )

    assert duplicate_uart.status_code == 409
    assert duplicate_identity.status_code == 409
    assert "ambiguous" in duplicate_uart.json()["detail"]
    assert "ambiguous" in duplicate_identity.json()["detail"]


@pytest.mark.asyncio
async def test_scanner_ota_auto_tries_direct_legacy_but_requires_version_proof(
    monkeypatch: pytest.MonkeyPatch,
):
    calls: list[tuple[str, bytes | None]] = []

    async def fake_run_subprocess(cmd, **kwargs):
        url = next((part for part in cmd if isinstance(part, str) and part.startswith("http://")), "")
        calls.append((url, kwargs.get("input")))
        if "/api/fw/upload" in url:
            return _completed(cmd, b'{"ok":true,"size":13}')
        if "/api/fw/relay" in url and "legacy=1" in url:
            return _completed(
                cmd,
                b'{"ok":false,"legacy":true,"stage":"end",'
                b'"error":"legacy_verify_timeout","chunks":2}',
            )
        if "/api/fw/relay" in url:
            return _completed(
                cmd,
                b'{"ok":false,"stage":"stop","error":"stop_ack_timeout"}',
            )
        if "/api/ota/relay" in url:
            assert kwargs.get("input") == PRODUCTION_SCANNER_IMAGE
            return _completed(
                cmd,
                b'{"ok":true,"mode":"streaming","legacy":true,"bytes":13,'
                b'"chunks":1,"scanner_response":"legacy_sent","scanner_error":""}',
            )
        raise AssertionError(f"unexpected curl URL {url}")

    async def fake_wait_for_scanner_version(*args, **kwargs):
        return False, "0.63.0-svc140", {"uart": "ble", "ver": "0.63.0-svc140"}

    monkeypatch.setattr(nodes, "_run_subprocess", fake_run_subprocess)
    monkeypatch.setattr(nodes, "_wait_for_scanner_version", fake_wait_for_scanner_version)

    transport = ASGITransport(app=app)
    async with AsyncClient(transport=transport, base_url="http://test") as client:
        resp = await client.post(
            "/nodes/uplink_TEST/ota/scanner/scanner-s3-combo?uart=ble&relay_mode=auto"
        )

    assert resp.status_code == 200, resp.text
    payload = resp.json()
    assert payload["ok"] is False
    assert payload["error"] == "scanner_version_verify_timeout"
    assert payload["verification"]["scanner_version"] == "0.63.0-svc140"
    assert [attempt["mode"] for attempt in payload["attempts"]] == [
        "staged",
        "staged_legacy",
        "direct_legacy",
    ]
    assert any(
        "/api/ota/relay" in url and body == PRODUCTION_SCANNER_IMAGE
        for url, body in calls
    )


@pytest.mark.asyncio
async def test_scanner_ota_direct_legacy_succeeds_only_after_heartbeat_version_match(
    monkeypatch: pytest.MonkeyPatch,
):
    calls: list[str] = []

    async def fake_run_subprocess(cmd, **kwargs):
        url = next((part for part in cmd if isinstance(part, str) and part.startswith("http://")), "")
        calls.append(url)
        assert "/api/fw/upload" not in url
        assert "/api/ota/relay" in url
        assert kwargs.get("input") == PRODUCTION_SCANNER_IMAGE
        return _completed(
            cmd,
            b'{"ok":true,"mode":"streaming","legacy":true,"bytes":13,'
            b'"chunks":1,"scanner_response":"legacy_sent","scanner_error":""}',
        )

    async def fake_wait_for_scanner_version(*args, **kwargs):
        return True, "0.63.0-svc148", {"uart": "ble", "ver": "0.63.0-svc148"}

    monkeypatch.setattr(nodes, "_run_subprocess", fake_run_subprocess)
    monkeypatch.setattr(nodes, "_wait_for_scanner_version", fake_wait_for_scanner_version)

    transport = ASGITransport(app=app)
    async with AsyncClient(transport=transport, base_url="http://test") as client:
        resp = await client.post(
            "/nodes/uplink_TEST/ota/scanner/scanner-s3-combo?uart=ble&relay_mode=direct_legacy"
        )

    assert resp.status_code == 200, resp.text
    payload = resp.json()
    assert payload["ok"] is True
    assert payload["verification"]["verified"] is True
    assert payload["relay_response"]["scanner_response"] == "legacy_sent"
    assert [attempt["mode"] for attempt in payload["attempts"]] == ["direct_legacy"]
    assert len(calls) == 1
