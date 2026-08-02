"""Tests for /nodes/firmware/latest/{name} and /firmware/download/{name}.

These are the endpoints uplinks poll for self-update + scanner-cache refresh.
"""

import hashlib
import struct
import time
import zlib

import pytest
from httpx import ASGITransport, AsyncClient

from app.main import app
from app.routers import nodes
from app.services import firmware_manager
from app.services.firmware_manager import FIRMWARE_TYPES, FirmwareAsset, FirmwareManager


PRODUCTION_VERSION = "0.64.68-live-follow"
BADGE_VERSION = "0.67.2-badge-defcon34"
RELEASE_TAG = "v0.64.68-live-follow"


def _esp_firmware_image(
    version: str,
    *,
    project: str = "friendorfoe",
    identity_records: tuple[bytes, ...] = (),
    payload_fill: bytes = b"A",
) -> bytes:
    encoded_version = version.encode("ascii")
    image = bytearray(0x20 + 256)
    image[0] = 0xE9
    struct.pack_into("<I", image, 0x20, 0xABCD5432)
    image[0x30:0x50] = encoded_version.ljust(32, b"\x00")
    image[0x50:0x70] = project.encode("ascii").ljust(32, b"\x00")
    image[0x70:0x80] = b"12:34:56".ljust(16, b"\x00")
    image[0x80:0x90] = b"Jul 16 2026".ljust(16, b"\x00")
    for record in identity_records:
        image.extend(record)
    image.extend(payload_fill * 4096)
    return bytes(image)


def _fixed_identity_string(value: str, width: int) -> bytes:
    encoded = value.encode("ascii")
    assert 0 < len(encoded) < width
    return encoded + bytes(width - len(encoded))


def _backend_identity_record(
    *, target: str, project: str, hardware: str, version: str, image_kind: int,
) -> bytes:
    prefix = struct.pack("<IHH", 0x42464F46, 1, image_kind) + b"".join((
        _fixed_identity_string(target, 40),
        _fixed_identity_string(project, 40),
        _fixed_identity_string(hardware, 40),
        _fixed_identity_string(version, 32),
    ))
    return prefix + struct.pack("<I", zlib.crc32(prefix) & 0xFFFFFFFF)


def _backend_image(
    target: str,
    *,
    version: str = "0.1.0-backend",
    payload_fill: bytes = b"A",
) -> bytes:
    project = "fof_backend_uplink" if target == "uplink-s3-backend" else "fof_backend_scanner"
    identity = _backend_identity_record(
        target=target,
        project=project,
        hardware="seeed_xiao_esp32s3",
        version=version,
        image_kind=0 if target == "uplink-s3-backend" else 1,
    )
    return _esp_firmware_image(
        version, project=project, identity_records=(identity,), payload_fill=payload_fill,
    )


def _cached_github_manager(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path,
    name: str,
    image: bytes,
) -> FirmwareManager:
    cache_dir = tmp_path / "firmware-cache"
    monkeypatch.setattr(firmware_manager, "CACHE_DIR", cache_dir)
    for target, info in FIRMWARE_TYPES.items():
        monkeypatch.setitem(info, "local_bin", tmp_path / "missing" / target / "firmware.bin")

    manager = FirmwareManager()
    manager.release_tag = RELEASE_TAG
    manager.last_check = time.time()
    cache_path = cache_dir / f"{RELEASE_TAG}_{name}.bin"
    cache_path.write_bytes(image)
    info = FIRMWARE_TYPES[name]
    manager.assets[name] = FirmwareAsset(
        name,
        info["description"],
        RELEASE_TAG,
        len(image),
        f"https://example.test/{name}.bin",
        str(cache_path),
        time.time(),
    )
    return manager


@pytest.mark.asyncio
async def test_latest_returns_404_for_unknown_name():
    async with AsyncClient(transport=ASGITransport(app=app), base_url="http://test") as c:
        r = await c.get("/nodes/firmware/latest/totally-fake-board")
    assert r.status_code == 404


@pytest.mark.asyncio
async def test_latest_returns_metadata_for_uploaded_custom_firmware():
    payload = b"\xE9" + b"FW" + b"\x00" * 4093  # 4 KB blob; large enough to pass upload size check
    nodes._firmware_mgr.set_custom_firmware("uplink-s3", payload)
    try:
        async with AsyncClient(transport=ASGITransport(app=app), base_url="http://test") as c:
            r = await c.get("/nodes/firmware/latest/uplink-s3")
        assert r.status_code == 200, r.text
        body = r.json()
        assert body["name"] == "uplink-s3"
        assert body["size"] == len(payload)
        assert body["sha256"] == hashlib.sha256(payload).hexdigest()
        assert body["download_url"] == "/nodes/firmware/download/uplink-s3"
        assert body["board"] == "esp32s3"
        # version comes from _custom_firmware path → "custom" sentinel
        assert body["version"] == "custom"
    finally:
        nodes._firmware_mgr.clear_custom_firmware("uplink-s3")


@pytest.mark.asyncio
async def test_download_returns_bytes_with_sha256_etag():
    payload = b"\xE9SCANNER-PAYLOAD" + b"\x00" * 4080
    nodes._firmware_mgr.set_custom_firmware("scanner-s3-combo-seed", payload)
    expected_sha = hashlib.sha256(payload).hexdigest()
    try:
        async with AsyncClient(transport=ASGITransport(app=app), base_url="http://test") as c:
            r = await c.get("/nodes/firmware/download/scanner-s3-combo-seed")
        assert r.status_code == 200
        assert r.content == payload
        assert r.headers["content-length"] == str(len(payload))
        assert r.headers["etag"].strip('"') == expected_sha
        assert r.headers["x-fof-firmware-sha256"] == expected_sha
    finally:
        nodes._firmware_mgr.clear_custom_firmware("scanner-s3-combo-seed")


@pytest.mark.asyncio
async def test_badge_scanner_firmware_name_is_served_for_auto_refresh():
    payload = b"\xE9BADGE-SCANNER-PAYLOAD" + b"\x00" * 4074
    firmware_name = "scanner-s3-combo-fof_badge"
    nodes._firmware_mgr.set_custom_firmware(firmware_name, payload)
    try:
        async with AsyncClient(transport=ASGITransport(app=app), base_url="http://test") as c:
            latest = await c.get(f"/nodes/firmware/latest/{firmware_name}")
            download = await c.get(f"/nodes/firmware/download/{firmware_name}")
        assert latest.status_code == 200, latest.text
        body = latest.json()
        assert body["name"] == firmware_name
        assert body["download_url"] == f"/nodes/firmware/download/{firmware_name}"
        assert body["sha256"] == hashlib.sha256(payload).hexdigest()
        assert download.status_code == 200
        assert download.content == payload
    finally:
        nodes._firmware_mgr.clear_custom_firmware(firmware_name)


@pytest.mark.asyncio
@pytest.mark.parametrize(
    ("name", "embedded_version"),
    [
        ("uplink-s3", PRODUCTION_VERSION),
        ("uplink-s3-fof_badge", BADGE_VERSION),
    ],
)
async def test_github_fallback_endpoints_report_embedded_image_version(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path,
    name: str,
    embedded_version: str,
):
    image = _esp_firmware_image(embedded_version)
    manager = _cached_github_manager(monkeypatch, tmp_path, name, image)
    monkeypatch.setattr(nodes, "_firmware_mgr", manager)

    async with AsyncClient(transport=ASGITransport(app=app), base_url="http://test") as c:
        latest = await c.get(f"/nodes/firmware/latest/{name}")
        download = await c.get(f"/nodes/firmware/download/{name}")

    assert latest.status_code == 200, latest.text
    assert latest.json()["version"] == embedded_version
    assert latest.json()["sha256"] == hashlib.sha256(image).hexdigest()
    assert download.status_code == 200
    assert download.content == image
    assert download.headers["x-fof-firmware-version"] == embedded_version
@pytest.mark.asyncio
async def test_malformed_github_fallback_endpoint_version_is_unknown(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path,
):
    name = "uplink-s3"
    image = b"not-an-esp-image"
    manager = _cached_github_manager(monkeypatch, tmp_path, name, image)
    monkeypatch.setattr(nodes, "_firmware_mgr", manager)

    async with AsyncClient(transport=ASGITransport(app=app), base_url="http://test") as c:
        latest = await c.get(f"/nodes/firmware/latest/{name}")
        download = await c.get(f"/nodes/firmware/download/{name}")

    assert latest.status_code == 200, latest.text
    assert latest.json()["version"] == "unknown"
    assert download.status_code == 200
    assert download.headers["x-fof-firmware-version"] == "unknown"
@pytest.mark.asyncio
async def test_download_returns_304_on_matching_if_none_match():
    payload = b"\xE9NOT-MODIFIED" + b"\x00" * 4083
    nodes._firmware_mgr.set_custom_firmware("scanner-s3-combo", payload)
    sha = hashlib.sha256(payload).hexdigest()
    try:
        async with AsyncClient(transport=ASGITransport(app=app), base_url="http://test") as c:
            r = await c.get(
                "/nodes/firmware/download/scanner-s3-combo",
                headers={"If-None-Match": f'"{sha}"'},
            )
        assert r.status_code == 304
        # Body must be empty on 304; ETag must still echo
        assert r.headers["etag"].strip('"') == sha
    finally:
        nodes._firmware_mgr.clear_custom_firmware("scanner-s3-combo")


@pytest.mark.asyncio
async def test_download_returns_404_for_unknown_name():
    async with AsyncClient(transport=ASGITransport(app=app), base_url="http://test") as c:
        r = await c.get("/nodes/firmware/download/no-such-fw")
    assert r.status_code == 404


@pytest.mark.asyncio
async def test_backend_latest_metadata_has_exact_identity(monkeypatch, tmp_path):
    name = "uplink-s3-backend"
    image = _backend_image(name)
    manager = _cached_github_manager(monkeypatch, tmp_path, name, image)
    monkeypatch.setattr(nodes, "_firmware_mgr", manager)
    async with AsyncClient(transport=ASGITransport(app=app), base_url="http://test") as client:
        response = await client.get(f"/nodes/firmware/latest/{name}")
        download = await client.get(f"/nodes/firmware/download/{name}")
    assert response.status_code == 200, response.text
    meta = response.json()
    assert meta["name"] == name
    assert meta["target"] == name
    assert meta["project"] == "fof_backend_uplink"
    assert meta["hardware"] == "seeed_xiao_esp32s3"
    assert meta["version"] == "0.1.0-backend"
    assert meta["size"] == len(image)
    assert meta["sha256"] == hashlib.sha256(image).hexdigest()
    assert meta["crc32"] == (zlib.crc32(image) & 0xFFFFFFFF)
    assert meta["download_url"] == f"/nodes/firmware/download/{name}"
    assert download.headers["x-fof-firmware-target"] == name
    assert download.headers["x-fof-app-project"] == "fof_backend_uplink"
    assert download.headers["x-fof-hardware-type"] == "seeed_xiao_esp32s3"


@pytest.mark.asyncio
async def test_custom_backend_image_uses_embedded_version_and_content_sha(client):
    name = "uplink-s3-backend"
    first = _backend_image(name, payload_fill=b"A")
    second = _backend_image(name, payload_fill=b"B")
    assert len(first) == len(second) and first != second
    try:
        uploaded = await client.post(
            f"/nodes/firmware/upload/{name}",
            files={"firmware": ("backend.bin", first, "application/octet-stream")},
        )
        assert uploaded.status_code == 200
        first_meta = (await client.get(f"/nodes/firmware/latest/{name}")).json()
        assert first_meta["version"] == "0.1.0-backend"
        assert first_meta["sha256"] == hashlib.sha256(first).hexdigest()

        replaced = await client.post(
            f"/nodes/firmware/upload/{name}",
            files={"firmware": ("backend.bin", second, "application/octet-stream")},
        )
        assert replaced.status_code == 200
        second_meta = (await client.get(f"/nodes/firmware/latest/{name}")).json()
        assert second_meta["version"] == "0.1.0-backend"
        assert second_meta["sha256"] == hashlib.sha256(second).hexdigest()
        assert second_meta["sha256"] != first_meta["sha256"]
        download = await client.get(f"/nodes/firmware/download/{name}")
        assert download.headers["etag"] == f'"{second_meta["sha256"]}"'
    finally:
        nodes._firmware_mgr.clear_custom_firmware(name)


@pytest.mark.asyncio
async def test_custom_backend_upload_rejects_mismatched_identity(client):
    name = "uplink-s3-backend"
    wrong = _backend_image("scanner-s3-combo-backend")
    response = await client.post(
        f"/nodes/firmware/upload/{name}",
        files={"firmware": ("wrong.bin", wrong, "application/octet-stream")},
    )
    assert response.status_code == 400
    assert name not in nodes._firmware_mgr._custom_firmware


@pytest.mark.asyncio
async def test_custom_legacy_upload_still_reports_custom_version(client):
    name = "uplink-s3"
    payload = b"\xE9legacy" + bytes(4096)
    try:
        nodes._firmware_mgr.set_custom_firmware(name, payload)
        async with AsyncClient(transport=ASGITransport(app=app), base_url="http://test") as client:
            response = await client.get(f"/nodes/firmware/latest/{name}")
        assert response.status_code == 200
        assert response.json()["version"] == "custom"
    finally:
        nodes._firmware_mgr.clear_custom_firmware(name)


@pytest.mark.asyncio
async def test_latest_metadata_binds_version_to_its_selected_backend_bytes(monkeypatch):
    name = "uplink-s3-backend"
    first = _backend_image(name, version="0.1.0-backend", payload_fill=b"A")
    second = _backend_image(name, version="0.1.1-backend", payload_fill=b"B")
    manager = FirmwareManager()
    calls: list[str] = []

    async def select_next(requested_name: str) -> bytes:
        assert requested_name == name
        calls.append(requested_name)
        return (first, second)[len(calls) - 1]

    monkeypatch.setattr(manager, "get_firmware_binary", select_next)
    monkeypatch.setattr(nodes, "_firmware_mgr", manager)

    meta = await nodes._firmware_metadata(name)

    assert calls == [name]
    assert meta["version"] == "0.1.0-backend"
    assert meta["size"] == len(first)
    assert meta["sha256"] == hashlib.sha256(first).hexdigest()
    assert meta["crc32"] == (zlib.crc32(first) & 0xFFFFFFFF)


@pytest.mark.asyncio
async def test_download_binds_headers_to_its_selected_backend_bytes(monkeypatch):
    name = "uplink-s3-backend"
    first = _backend_image(name, version="0.1.0-backend", payload_fill=b"A")
    second = _backend_image(name, version="0.1.1-backend", payload_fill=b"B")
    manager = FirmwareManager()
    calls: list[str] = []

    async def select_next(requested_name: str) -> bytes:
        assert requested_name == name
        calls.append(requested_name)
        return (first, second)[len(calls) - 1]

    monkeypatch.setattr(manager, "get_firmware_binary", select_next)
    monkeypatch.setattr(nodes, "_firmware_mgr", manager)

    async with AsyncClient(transport=ASGITransport(app=app), base_url="http://test") as client:
        response = await client.get(f"/nodes/firmware/download/{name}")

    assert calls == [name]
    assert response.content == first
    assert response.headers["x-fof-firmware-version"] == "0.1.0-backend"
    assert response.headers["etag"] == f'"{hashlib.sha256(first).hexdigest()}"'
