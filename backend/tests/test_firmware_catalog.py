import struct
import time

import pytest

from app.services import firmware_manager
from app.services.firmware_manager import FIRMWARE_TYPES, FirmwareAsset, FirmwareManager


PRODUCTION_VERSION = "0.64.68-live-follow"
BADGE_VERSION = "0.64.68-badge-live-follow"
RELEASE_TAG = "v0.64.68-live-follow"


def _esp_firmware_image(version: str) -> bytes:
    encoded_version = version.encode("ascii")
    assert len(encoded_version) < 32

    image = bytearray(0x20 + 256)
    image[0] = 0xE9
    struct.pack_into("<I", image, 0x20, 0xABCD5432)
    image[0x30:0x50] = encoded_version.ljust(32, b"\x00")
    image[0x50:0x70] = b"friendorfoe".ljust(32, b"\x00")
    image[0x70:0x80] = b"12:34:56".ljust(16, b"\x00")
    image[0x80:0x90] = b"Jul 16 2026".ljust(16, b"\x00")
    return bytes(image)


def _github_manager(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path,
    images: dict[str, bytes],
    *,
    cached: bool = True,
) -> FirmwareManager:
    cache_dir = tmp_path / "firmware-cache"
    monkeypatch.setattr(firmware_manager, "CACHE_DIR", cache_dir)
    for name, info in FIRMWARE_TYPES.items():
        monkeypatch.setitem(info, "local_bin", tmp_path / "missing" / name / "firmware.bin")

    manager = FirmwareManager()
    manager.release_tag = RELEASE_TAG
    manager.last_check = time.time()
    for name, image in images.items():
        cache_path = cache_dir / f"{RELEASE_TAG}_{name}.bin"
        if cached:
            cache_path.write_bytes(image)
        info = FIRMWARE_TYPES[name]
        manager.assets[name] = FirmwareAsset(
            name,
            info["description"],
            RELEASE_TAG,
            len(image),
            f"https://example.test/{name}.bin",
            str(cache_path) if cached else None,
            time.time() if cached else 0,
        )
    return manager


def test_live_fleet_firmware_targets_are_present():
    assert set(FIRMWARE_TYPES) == {
        "scanner-s3-combo",
        "scanner-s3-combo-fof_badge",
        "scanner-s3-combo-seed",
        "uplink-s3",
        "uplink-s3-fof_badge",
    }


def test_live_fleet_targets_point_at_expected_local_builds():
    assert str(FIRMWARE_TYPES["scanner-s3-combo"]["local_bin"]).endswith(
        "/esp32/scanner/.pio/build/scanner-s3-combo/firmware.bin"
    )
    assert str(FIRMWARE_TYPES["scanner-s3-combo-seed"]["local_bin"]).endswith(
        "/esp32/scanner/.pio/build/scanner-s3-combo-seed/firmware.bin"
    )
    assert str(FIRMWARE_TYPES["uplink-s3"]["local_bin"]).endswith(
        "/esp32/uplink/.pio/build/uplink-s3/firmware.bin"
    )
    assert str(FIRMWARE_TYPES["scanner-s3-combo-fof_badge"]["local_bin"]).endswith(
        "/esp32/scanner/.pio/build/scanner-s3-combo-fof_badge/firmware.bin"
    )
    assert str(FIRMWARE_TYPES["uplink-s3-fof_badge"]["local_bin"]).endswith(
        "/esp32/uplink/.pio/build/uplink-s3-fof_badge/firmware.bin"
    )


def test_release_asset_patterns_match_current_bin_asset_names():
    for target, info in FIRMWARE_TYPES.items():
        assert info["asset_pattern"] == target


@pytest.mark.parametrize(
    ("name", "embedded_version"),
    [
        ("scanner-s3-combo", PRODUCTION_VERSION),
        ("scanner-s3-combo-fof_badge", BADGE_VERSION),
    ],
)
def test_bytes_parser_reads_embedded_app_version(name: str, embedded_version: str):
    parser = getattr(firmware_manager, "_parse_app_desc_bytes", None)

    assert parser is not None, f"bytes parser missing for {name}"
    assert parser(_esp_firmware_image(embedded_version))["version"] == embedded_version


def test_bytes_parser_rejects_malformed_images():
    parser = getattr(firmware_manager, "_parse_app_desc_bytes", None)
    bad_magic = bytearray(_esp_firmware_image(PRODUCTION_VERSION))
    struct.pack_into("<I", bad_magic, 0x20, 0)
    bad_version = bytearray(_esp_firmware_image(PRODUCTION_VERSION))
    bad_version[0x30:0x50] = b"\xff".ljust(32, b"\x00")

    assert parser is not None
    assert parser(b"") is None
    assert parser(bytes(bad_magic)) is None
    assert parser(bytes(bad_version)) is None
    assert parser(_esp_firmware_image("")) is None


@pytest.mark.asyncio
@pytest.mark.parametrize(
    ("name", "embedded_version"),
    [
        ("scanner-s3-combo", PRODUCTION_VERSION),
        ("scanner-s3-combo-fof_badge", BADGE_VERSION),
    ],
)
async def test_cached_github_firmware_version_comes_from_image(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path,
    name: str,
    embedded_version: str,
):
    manager = _github_manager(
        monkeypatch,
        tmp_path,
        {name: _esp_firmware_image(embedded_version)},
    )

    assert await manager.get_firmware_version(name) == embedded_version


@pytest.mark.asyncio
async def test_catalog_uses_production_and_badge_versions_from_github_images(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path,
):
    manager = _github_manager(
        monkeypatch,
        tmp_path,
        {
            "scanner-s3-combo": _esp_firmware_image(PRODUCTION_VERSION),
            "scanner-s3-combo-fof_badge": _esp_firmware_image(BADGE_VERSION),
        },
    )

    catalog = {row["name"]: row for row in await manager.get_catalog()}

    assert catalog["scanner-s3-combo"]["version"] == PRODUCTION_VERSION
    assert catalog["scanner-s3-combo-fof_badge"]["version"] == BADGE_VERSION
    assert catalog["scanner-s3-combo"]["source"] == "github"
    assert catalog["scanner-s3-combo-fof_badge"]["source"] == "github"


@pytest.mark.asyncio
async def test_malformed_github_image_version_fails_closed(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path,
):
    name = "scanner-s3-combo"
    manager = _github_manager(monkeypatch, tmp_path, {name: b"not-an-esp-image"})

    assert await manager.get_firmware_version(name) is None
    catalog = {row["name"]: row for row in await manager.get_catalog()}
    assert catalog[name]["version"] is None
    assert catalog[name]["available"] is True


@pytest.mark.asyncio
async def test_local_binary_version_wins_over_repo_source_header(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path,
):
    cache_dir = tmp_path / "firmware-cache"
    monkeypatch.setattr(firmware_manager, "CACHE_DIR", cache_dir)
    for name, info in FIRMWARE_TYPES.items():
        monkeypatch.setitem(info, "local_bin", tmp_path / "missing" / name / "firmware.bin")

    name = "scanner-s3-combo"
    embedded_version = "0.64.60-stale-local"
    local_bin = tmp_path / "local" / name / "firmware.bin"
    local_bin.parent.mkdir(parents=True)
    local_bin.write_bytes(_esp_firmware_image(embedded_version))
    monkeypatch.setitem(FIRMWARE_TYPES[name], "local_bin", local_bin)

    manager = FirmwareManager()

    async def no_refresh(force: bool = False):
        return None

    monkeypatch.setattr(manager, "refresh_from_github", no_refresh)
    manager.assets[name] = FirmwareAsset(
        name,
        FIRMWARE_TYPES[name]["description"],
        RELEASE_TAG,
        len(_esp_firmware_image(PRODUCTION_VERSION)),
        f"https://example.test/{name}.bin",
    )

    assert await manager.get_firmware_version(name) == embedded_version
    catalog = {row["name"]: row for row in await manager.get_catalog()}
    assert catalog[name]["version"] == embedded_version
    assert catalog[name]["source"] == "local"
    assert catalog[name]["cached"] is True


@pytest.mark.asyncio
@pytest.mark.parametrize("cache_state", ["missing", "unreadable"])
async def test_unusable_github_cache_clears_stale_cached_path(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path,
    cache_state: str,
):
    name = "uplink-s3"
    image = _esp_firmware_image(PRODUCTION_VERSION)
    manager = _github_manager(monkeypatch, tmp_path, {name: image}, cached=False)
    asset = manager.assets[name]
    cache_path = firmware_manager.CACHE_DIR / f"{RELEASE_TAG}_{name}.bin"
    if cache_state == "unreadable":
        cache_path.mkdir()
    asset.cached_path = str(cache_path)
    asset.cached_at = time.time()

    class FailingAsyncClient:
        def __init__(self, **kwargs):
            pass

        async def __aenter__(self):
            return self

        async def __aexit__(self, exc_type, exc, tb):
            return False

        async def get(self, url: str):
            class Response:
                status_code = 503

            return Response()

    monkeypatch.setattr(firmware_manager.httpx, "AsyncClient", FailingAsyncClient)

    catalog = {row["name"]: row for row in await manager.get_catalog()}

    assert asset.cached_path is None
    assert asset.cached_at == 0
    assert catalog[name]["cached"] is False


@pytest.mark.asyncio
async def test_downloaded_github_image_version_is_parsed_and_download_reused(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path,
):
    name = "uplink-s3"
    image = _esp_firmware_image(PRODUCTION_VERSION)
    manager = _github_manager(monkeypatch, tmp_path, {name: image}, cached=False)
    download_url = manager.assets[name].download_url
    download_calls: list[str] = []

    class FakeAsyncClient:
        def __init__(self, **kwargs):
            pass

        async def __aenter__(self):
            return self

        async def __aexit__(self, exc_type, exc, tb):
            return False

        async def get(self, url: str):
            download_calls.append(url)

            class Response:
                status_code = 200
                content = image

            return Response()

    monkeypatch.setattr(firmware_manager.httpx, "AsyncClient", FakeAsyncClient)

    assert await manager.get_firmware_version(name) == PRODUCTION_VERSION
    assert await manager.get_firmware_version(name) == PRODUCTION_VERSION
    catalog = {row["name"]: row for row in await manager.get_catalog()}
    assert catalog[name]["version"] == PRODUCTION_VERSION
    assert download_calls == [download_url]
