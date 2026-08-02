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
from tests.firmware_images import esp32s3_app_image


PRODUCTION_VERSION = "0.64.68-live-follow"
BADGE_VERSION = "0.67.2-badge-defcon34"
RELEASE_TAG = "v0.64.68-live-follow"

NAMED_IDENTITIES = {
    "uplink-s3": ("fof_uplink", "esp32-s3-devkitc-1"),
    "uplink-s3-fof_badge": ("fof_badge_uplink", "seeed_xiao_esp32s3"),
    "scanner-s3-combo": ("fof_scanner", "esp32-s3-devkitc-1"),
    "scanner-s3-combo-seed": ("fof_scanner_seed", "esp32-s3-devkitc-1"),
    "scanner-s3-combo-fof_badge": (
        "fof_badge_scanner",
        "seeed_xiao_esp32s3",
    ),
}


def _esp_firmware_image(
    version: str,
    *,
    project: str = "friendorfoe",
    identity_records: tuple[bytes, ...] = (),
    payload_fill: bytes = b"A",
    payload_size: int = 64 * 1024,
) -> bytes:
    placements = tuple(
        (0x120 + index * len(record), record)
        for index, record in enumerate(identity_records)
    )
    return esp32s3_app_image(
        version,
        project=project,
        placements=placements,
        payload_size=payload_size,
        payload_fill=payload_fill,
    )


def _named_firmware_image(name: str, version: str) -> bytes:
    project, hardware = NAMED_IDENTITIES[name]
    return esp32s3_app_image(
        version,
        project=project,
        placements=((0x200, f"{name}\0{hardware}\0".encode("ascii")),),
        payload_size=64 * 1024,
        payload_fill=b"A",
    )


def _badge_firmware_image(name: str, version: str = BADGE_VERSION) -> bytes:
    return _named_firmware_image(name, version)


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
    exact_size: int | None = None,
) -> bytes:
    identities = {
        "uplink-s3-backend": (
            "fof_backend_uplink", "seeed_xiao_esp32s3", 0,
        ),
        "scanner-s3-combo-backend": (
            "fof_backend_scanner", "seeed_xiao_esp32s3", 1,
        ),
        "uplink-s3-fullsize-backend": (
            "fof_backend_uplink_fullsize", "esp32s3_n16r8_fullsize", 0,
        ),
        "scanner-s3-combo-fullsize-backend": (
            "fof_backend_scanner_fullsize", "esp32s3_n16r8_fullsize", 1,
        ),
    }
    project, hardware, image_kind = identities[target]
    identity = _backend_identity_record(
        target=target,
        project=project,
        hardware=hardware,
        version=version,
        image_kind=image_kind,
    )
    return _esp_firmware_image(
        version,
        project=project,
        identity_records=(identity,),
        payload_fill=payload_fill,
        payload_size=(exact_size - 0x150) if exact_size is not None else 64 * 1024,
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
    payload = _named_firmware_image("uplink-s3", PRODUCTION_VERSION)
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
    payload = _named_firmware_image("scanner-s3-combo-seed", PRODUCTION_VERSION)
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
    firmware_name = "scanner-s3-combo-fof_badge"
    payload = _badge_firmware_image(firmware_name)
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
    image = (
        _badge_firmware_image(name, embedded_version)
        if name.endswith("-fof_badge")
        else _named_firmware_image(name, embedded_version)
    )
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
async def test_malformed_github_fallback_endpoint_is_rejected(
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

    assert latest.status_code == 404
    assert download.status_code == 404
@pytest.mark.asyncio
async def test_download_returns_304_on_matching_if_none_match():
    payload = _named_firmware_image("scanner-s3-combo", PRODUCTION_VERSION)
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
@pytest.mark.parametrize(
    ("name", "family", "component"),
    [
        ("uplink-s3-backend", "badge_lite", "uplink"),
        ("uplink-s3-fullsize-backend", "s3_fullsize", "uplink"),
        ("scanner-s3-combo-fullsize-backend", "s3_fullsize", "scanner"),
    ],
)
async def test_latest_body_stays_exactly_11_keys_and_management_is_download_headers(
    monkeypatch,
    tmp_path,
    name: str,
    family: str,
    component: str,
):
    image = _backend_image(name)
    manager = _cached_github_manager(monkeypatch, tmp_path, name, image)
    monkeypatch.setattr(nodes, "_firmware_mgr", manager)

    async with AsyncClient(transport=ASGITransport(app=app), base_url="http://test") as client:
        latest = await client.get(f"/nodes/firmware/latest/{name}")
        download = await client.get(f"/nodes/firmware/download/{name}")

    assert latest.status_code == 200, latest.text
    assert set(latest.json()) == {
        "name", "target", "description", "board", "project", "hardware",
        "version", "size", "sha256", "crc32", "download_url",
    }
    assert not ({"product_family", "firmware_line", "component"} & set(latest.json()))
    assert download.headers["x-fof-product-family"] == family
    assert download.headers["x-fof-firmware-line"] == "backend"
    assert download.headers["x-fof-component"] == component


@pytest.mark.asyncio
@pytest.mark.parametrize(
    ("name", "size", "expected_status"),
    [
        ("scanner-s3-combo-backend", 0x200001, 400),
        ("scanner-s3-combo-fullsize-backend", 0x300000, 200),
        ("scanner-s3-combo-fullsize-backend", 0x300001, 400),
    ],
)
async def test_custom_scanner_upload_uses_exact_target_partition_capacity(
    client,
    name: str,
    size: int,
    expected_status: int,
):
    image = (
        _backend_image(name, exact_size=size)
        if expected_status == 200
        else b"not-a-valid-image".ljust(size, b"X")
    )
    assert len(image) == size
    try:
        response = await client.post(
            f"/nodes/firmware/upload/{name}",
            files={"firmware": ("scanner.bin", image, "application/octet-stream")},
        )
        assert response.status_code == expected_status, response.text
    finally:
        nodes._firmware_mgr.clear_custom_firmware(name)


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
    payload = _named_firmware_image(name, PRODUCTION_VERSION)
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
