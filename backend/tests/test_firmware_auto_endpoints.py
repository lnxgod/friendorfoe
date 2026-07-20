"""Tests for /nodes/firmware/latest/{name} and /firmware/download/{name}.

These are the endpoints uplinks poll for self-update + scanner-cache refresh.
"""

import hashlib
import struct
import time

import pytest
from httpx import ASGITransport, AsyncClient

from app.main import app
from app.routers import nodes
from app.services import firmware_manager
from app.services.firmware_manager import FIRMWARE_TYPES, FirmwareAsset, FirmwareManager


PRODUCTION_VERSION = "0.64.68-live-follow"
BADGE_VERSION = "0.64.76-badge-defcon34"
RELEASE_TAG = "v0.64.68-live-follow"


def _esp_firmware_image(version: str) -> bytes:
    encoded_version = version.encode("ascii")
    image = bytearray(0x20 + 256)
    image[0] = 0xE9
    struct.pack_into("<I", image, 0x20, 0xABCD5432)
    image[0x30:0x50] = encoded_version.ljust(32, b"\x00")
    return bytes(image)


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
    nodes._FW_HASH_CACHE.pop("uplink-s3", None)
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
        nodes._FW_HASH_CACHE.pop("uplink-s3", None)


@pytest.mark.asyncio
async def test_download_returns_bytes_with_sha256_etag():
    payload = b"\xE9SCANNER-PAYLOAD" + b"\x00" * 4080
    nodes._firmware_mgr.set_custom_firmware("scanner-s3-combo-seed", payload)
    nodes._FW_HASH_CACHE.pop("scanner-s3-combo-seed", None)
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
        nodes._FW_HASH_CACHE.pop("scanner-s3-combo-seed", None)


@pytest.mark.asyncio
async def test_badge_scanner_firmware_name_is_served_for_auto_refresh():
    payload = b"\xE9BADGE-SCANNER-PAYLOAD" + b"\x00" * 4074
    firmware_name = "scanner-s3-combo-fof_badge"
    nodes._firmware_mgr.set_custom_firmware(firmware_name, payload)
    nodes._FW_HASH_CACHE.pop(firmware_name, None)
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
        nodes._FW_HASH_CACHE.pop(firmware_name, None)


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
    nodes._FW_HASH_CACHE.pop(name, None)

    try:
        async with AsyncClient(transport=ASGITransport(app=app), base_url="http://test") as c:
            latest = await c.get(f"/nodes/firmware/latest/{name}")
            download = await c.get(f"/nodes/firmware/download/{name}")

        assert latest.status_code == 200, latest.text
        assert latest.json()["version"] == embedded_version
        assert latest.json()["sha256"] == hashlib.sha256(image).hexdigest()
        assert download.status_code == 200
        assert download.content == image
        assert download.headers["x-fof-firmware-version"] == embedded_version
    finally:
        nodes._FW_HASH_CACHE.pop(name, None)


@pytest.mark.asyncio
async def test_malformed_github_fallback_endpoint_version_is_unknown(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path,
):
    name = "uplink-s3"
    image = b"not-an-esp-image"
    manager = _cached_github_manager(monkeypatch, tmp_path, name, image)
    monkeypatch.setattr(nodes, "_firmware_mgr", manager)
    nodes._FW_HASH_CACHE.pop(name, None)

    try:
        async with AsyncClient(transport=ASGITransport(app=app), base_url="http://test") as c:
            latest = await c.get(f"/nodes/firmware/latest/{name}")
            download = await c.get(f"/nodes/firmware/download/{name}")

        assert latest.status_code == 200, latest.text
        assert latest.json()["version"] == "unknown"
        assert download.status_code == 200
        assert download.headers["x-fof-firmware-version"] == "unknown"
    finally:
        nodes._FW_HASH_CACHE.pop(name, None)


@pytest.mark.asyncio
async def test_download_returns_304_on_matching_if_none_match():
    payload = b"\xE9NOT-MODIFIED" + b"\x00" * 4083
    nodes._firmware_mgr.set_custom_firmware("scanner-s3-combo", payload)
    nodes._FW_HASH_CACHE.pop("scanner-s3-combo", None)
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
        nodes._FW_HASH_CACHE.pop("scanner-s3-combo", None)


@pytest.mark.asyncio
async def test_download_returns_404_for_unknown_name():
    async with AsyncClient(transport=ASGITransport(app=app), base_url="http://test") as c:
        r = await c.get("/nodes/firmware/download/no-such-fw")
    assert r.status_code == 404
