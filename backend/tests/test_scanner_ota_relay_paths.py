import subprocess
import struct
import time
import zlib

import pytest
from fastapi import HTTPException
from httpx import ASGITransport, AsyncClient

from app.main import app
from app.routers import detections, nodes
from app.services import firmware_manager
from tests.firmware_images import esp32s3_app_image


def _completed(cmd, stdout: bytes):
    return subprocess.CompletedProcess(cmd, 0, stdout=stdout, stderr=b"")


def _backend_scanner_image(
    target: str = "scanner-s3-combo-backend",
) -> bytes:
    info = firmware_manager.FIRMWARE_TYPES[target]
    record = bytearray(firmware_manager._BACKEND_IDENTITY_STRUCT.size)
    firmware_manager._BACKEND_IDENTITY_STRUCT.pack_into(
        record, 0, 0x42464F46, 1, info["image_kind"],
        target.encode().ljust(40, b"\0"),
        info["project"].encode().ljust(40, b"\0"),
        info["hardware"].encode().ljust(40, b"\0"),
        b"0.1.0-backend".ljust(32, b"\0"), 0,
    )
    struct.pack_into("<I", record, 160, zlib.crc32(record[:160]) & 0xFFFFFFFF)
    return esp32s3_app_image(
        "0.1.0-backend",
        project=info["project"],
        placements=((0x120, bytes(record)),),
    )


def _fullsize_heartbeat() -> dict:
    return {
        "device_id": "uplink_TEST",
        "ip": "192.168.1.10",
        "last_seen": time.time(),
        "product_family": "s3_fullsize",
        "firmware_line": "backend",
        "component": "uplink",
        "firmware_target": "uplink-s3-fullsize-backend",
        "app_project": "fof_backend_uplink_fullsize",
        "hardware_type": "esp32s3_n16r8_fullsize",
        "hardware_mac": "AA:BB:CC:DD:EE:10",
        "boot_id": 10,
        "scanners": [
            {
                "uart": "ble", "slot": 0,
                "product_family": "s3_fullsize",
                "firmware_line": "backend", "component": "scanner",
                "firmware_target": "scanner-s3-combo-fullsize-backend",
                "app_project": "fof_backend_scanner_fullsize",
                "hardware_type": "esp32s3_n16r8_fullsize",
                "mac": "AA:BB:CC:DD:EE:11", "boot_id": 11,
            },
            {
                "uart": "wifi", "slot": 1,
                "product_family": "s3_fullsize",
                "firmware_line": "backend", "component": "scanner",
                "firmware_target": "scanner-s3-combo-fullsize-backend",
                "app_project": "fof_backend_scanner_fullsize",
                "hardware_type": "esp32s3_n16r8_fullsize",
                "mac": "AA:BB:CC:DD:EE:12", "boot_id": 12,
            },
        ],
    }


def _production_scanner_image() -> bytes:
    markers = b"scanner-s3-combo\0esp32-s3-devkitc-1\0"
    return esp32s3_app_image(
        "0.63.0-svc148",
        project="fof_scanner",
        placements=((0x200, markers),),
    )


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
                    "slot": 0,
                    "mac": "AA:BB:CC:DD:EE:01",
                    "boot_id": 1,
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
            {"uart": "ble", "slot": 0, "mac": "AA:BB:CC:DD:EE:01", "boot_id": 1,
             "firmware_target": "scanner-s3-combo-backend",
             "app_project": "fof_backend_scanner", "hardware_type": "seeed_xiao_esp32s3",
             "ver": "0.1.0-backend", "cmd_rx": 1},
            {"uart": "wifi", "slot": 1, "mac": "AA:BB:CC:DD:EE:02", "boot_id": 2,
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
            "uart": "ble", "slot": 0,
            "mac": "AA:BB:CC:DD:EE:01", "boot_id": 1,
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
@pytest.mark.parametrize("mutation", ["uplink_mac", "other_scanner_boot_id"])
async def test_fullsize_scanner_ota_binds_the_complete_trio_across_awaited_fetch(
    monkeypatch: pytest.MonkeyPatch,
    mutation: str,
):
    detections._node_heartbeats["uplink_TEST"] = _fullsize_heartbeat()
    loaded: list[str] = []
    transfers: list[str] = []

    async def mutate_during_fetch(name: str) -> bytes:
        loaded.append(name)
        heartbeat = detections._node_heartbeats["uplink_TEST"]
        if mutation == "uplink_mac":
            heartbeat["hardware_mac"] = "AA:BB:CC:DD:EE:99"
        else:
            heartbeat["scanners"][1]["boot_id"] = 99
        return _backend_scanner_image("scanner-s3-combo-fullsize-backend")

    async def version(name: str) -> str:
        return "0.1.0-backend"

    async def must_not_transfer(cmd, **kwargs):
        transfers.append(str(cmd))
        raise AssertionError("changed trio must reject before transfer")

    monkeypatch.setattr(nodes._firmware_mgr, "get_firmware_binary", mutate_during_fetch)
    monkeypatch.setattr(nodes._firmware_mgr, "get_firmware_version", version)
    monkeypatch.setattr(nodes, "_run_subprocess", must_not_transfer)

    async with AsyncClient(transport=ASGITransport(app=app), base_url="http://test") as client:
        response = await client.post(
            "/nodes/uplink_TEST/ota/scanner/"
            "scanner-s3-combo-fullsize-backend?uart=ble&relay_mode=staged"
        )

    assert response.status_code == 409, response.text
    assert "identity changed" in response.json()["detail"]
    assert loaded == ["scanner-s3-combo-fullsize-backend"]
    assert transfers == []


@pytest.mark.asyncio
@pytest.mark.parametrize(
    "query",
    [
        "uart=ble&relay_mode=direct_legacy",
        "uart=ble&relay_mode=staged_legacy",
        "uart=ble&relay_mode=staged&legacy=true",
    ],
)
async def test_fullsize_scanner_ota_rejects_legacy_paths_before_load_or_transfer(
    monkeypatch: pytest.MonkeyPatch,
    query: str,
):
    detections._node_heartbeats["uplink_TEST"] = _fullsize_heartbeat()

    async def must_not_load(name: str) -> bytes:
        raise AssertionError("legacy path must reject before firmware load")

    async def must_not_transfer(cmd, **kwargs):
        raise AssertionError("legacy path must reject before transfer")

    monkeypatch.setattr(nodes._firmware_mgr, "get_firmware_binary", must_not_load)
    monkeypatch.setattr(nodes, "_run_subprocess", must_not_transfer)

    async with AsyncClient(transport=ASGITransport(app=app), base_url="http://test") as client:
        response = await client.post(
            "/nodes/uplink_TEST/ota/scanner/"
            f"scanner-s3-combo-fullsize-backend?{query}"
        )

    assert response.status_code == 409, response.text
    assert response.json()["detail"] == "legacy relay paths are not eligible for S3 Fullsize"


def test_fullsize_scanner_image_must_fit_both_scanner_slot_and_uplink_cache(
    monkeypatch: pytest.MonkeyPatch,
):
    detections._node_heartbeats["uplink_TEST"] = _fullsize_heartbeat()
    target = "scanner-s3-combo-fullsize-backend"
    image = _backend_scanner_image(target)
    snapshot = nodes._require_named_family_preflight(
        "uplink_TEST", target, "ble",
    )
    monkeypatch.setitem(
        firmware_manager.FIRMWARE_TYPES["uplink-s3-fullsize-backend"],
        "scanner_cache_capacity",
        len(image) - 1,
    )

    with pytest.raises(HTTPException) as error:
        nodes._require_ota_compatibility(
            "uplink_TEST", target, image, "ble", snapshot=snapshot,
        )

    assert getattr(error.value, "status_code", None) == 400
    assert "cache capacity" in str(getattr(error.value, "detail", ""))


@pytest.mark.asyncio
async def test_scanner_ota_both_rejects_duplicate_ble_identity(monkeypatch: pytest.MonkeyPatch):
    detections._node_heartbeats["uplink_TEST"] = _fullsize_heartbeat()
    detections._node_heartbeats["uplink_TEST"]["scanners"][1]["uart"] = "ble"
    # The endpoint must fail in preflight, without loading or transferring bytes.
    async def must_not_load(name: str) -> bytes:
        raise AssertionError("must reject ambiguous uart=both before firmware load")

    monkeypatch.setattr(nodes._firmware_mgr, "get_firmware_binary", must_not_load)

    transport = ASGITransport(app=app)
    async with AsyncClient(transport=transport, base_url="http://test") as client:
        duplicate_uart = await client.post(
            "/nodes/uplink_TEST/ota/scanner/scanner-s3-combo-fullsize-backend"
            "?uart=both&relay_mode=staged"
        )
        detections._node_heartbeats["uplink_TEST"] = _fullsize_heartbeat()
        detections._node_heartbeats["uplink_TEST"]["scanners"][1]["mac"] = (
            detections._node_heartbeats["uplink_TEST"]["scanners"][0]["mac"]
        )
        detections._node_heartbeats["uplink_TEST"]["scanners"][1]["boot_id"] = 11
        duplicate_identity = await client.post(
            "/nodes/uplink_TEST/ota/scanner/scanner-s3-combo-fullsize-backend"
            "?uart=both&relay_mode=staged"
        )

    assert duplicate_uart.status_code == 409
    assert duplicate_identity.status_code == 409
    assert "ambiguous" in duplicate_uart.json()["detail"]
    assert "ambiguous" in duplicate_identity.json()["detail"]


@pytest.mark.asyncio
async def test_scanner_ota_auto_rejects_legacy_target_before_transfer(
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

    assert resp.status_code == 409, resp.text
    assert "not remote-update eligible" in resp.json()["detail"]
    assert calls == []


@pytest.mark.asyncio
async def test_scanner_ota_direct_legacy_rejects_legacy_target_before_transfer(
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

    assert resp.status_code == 409, resp.text
    assert "not remote-update eligible" in resp.json()["detail"]
    assert calls == []
