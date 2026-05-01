import subprocess

import pytest
from httpx import ASGITransport, AsyncClient

from app.main import app
from app.routers import detections, nodes


def _completed(cmd, stdout: bytes):
    return subprocess.CompletedProcess(cmd, 0, stdout=stdout, stderr=b"")


@pytest.fixture(autouse=True)
def scanner_ota_state(monkeypatch: pytest.MonkeyPatch):
    monkeypatch.setattr(
        detections,
        "_node_heartbeats",
        {
            "uplink_TEST": {
                "ip": "192.168.1.10",
                "scanners": [{"uart": "ble", "ver": "0.63.0-svc140"}],
            }
        },
    )

    async def fake_binary(name: str) -> bytes:
        assert name == "scanner-s3-combo"
        return b"fake firmware"

    async def fake_version(name: str) -> str:
        assert name == "scanner-s3-combo"
        return "0.63.0-svc148"

    monkeypatch.setattr(nodes._firmware_mgr, "get_firmware_binary", fake_binary)
    monkeypatch.setattr(nodes._firmware_mgr, "get_firmware_version", fake_version)


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
            assert kwargs.get("input") == b"fake firmware"
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
    assert any("/api/ota/relay" in url and body == b"fake firmware" for url, body in calls)


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
        assert kwargs.get("input") == b"fake firmware"
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
