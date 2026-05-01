"""Tests for /nodes/firmware/latest/{name} and /firmware/download/{name}.

These are the endpoints uplinks poll for self-update + scanner-cache refresh.
"""

import hashlib

import pytest
from httpx import ASGITransport, AsyncClient

from app.main import app
from app.routers import nodes


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
