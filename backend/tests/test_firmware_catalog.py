import hashlib
import struct
import time
import zipfile
from pathlib import Path
import zlib

import pytest

from app.services import firmware_manager
from app.services.firmware_manager import FIRMWARE_TYPES, FirmwareAsset, FirmwareManager
from tests.firmware_images import esp32s3_app_image, resign_esp_image


PRODUCTION_VERSION = "0.64.68-live-follow"
BADGE_VERSION = "0.67.2-badge-defcon34"
RELEASE_TAG = "v0.64.68-live-follow"

BADGE_IDENTITIES = {
    "uplink-s3-fof_badge": ("fof_badge_uplink", "seeed_xiao_esp32s3"),
    "scanner-s3-combo-fof_badge": (
        "fof_badge_scanner",
        "seeed_xiao_esp32s3",
    ),
}
NONBACKEND_IDENTITIES = {
    "uplink-s3": ("fof_uplink", "esp32-s3-devkitc-1"),
    "uplink-s3-fof_badge": BADGE_IDENTITIES["uplink-s3-fof_badge"],
    "scanner-s3-combo": ("fof_scanner", "esp32-s3-devkitc-1"),
    "scanner-s3-combo-seed": ("fof_scanner_seed", "esp32-s3-devkitc-1"),
    "scanner-s3-combo-fof_badge": BADGE_IDENTITIES[
        "scanner-s3-combo-fof_badge"
    ],
}


def _esp_firmware_image(
    version: str,
    *,
    project: str = "friendorfoe",
    identity_records: tuple[bytes, ...] = (),
    trailer: bytes = b"",
    payload_fill: bytes = b"A",
) -> bytes:
    placements: list[tuple[int, bytes]] = []
    cursor = 0x120
    for record in identity_records:
        placements.append((cursor, record))
        cursor += len(record)
    if trailer:
        placements.append((cursor, trailer))
    return esp32s3_app_image(
        version,
        project=project,
        placements=tuple(placements),
        payload_fill=payload_fill,
    )


BACKEND_IDENTITY_STRUCT = struct.Struct("<IHH40s40s40s32sI")
RAW_INVALID_MAGIC = struct.pack("<I", 0x42464F46) + bytes(160)


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
    assert len(prefix) == 160
    return prefix + struct.pack("<I", zlib.crc32(prefix) & 0xFFFFFFFF)


def _backend_image(
    target: str,
    *,
    identity_records: tuple[bytes, ...] | None = None,
    descriptor_project: str | None = None,
    descriptor_version: str = "0.1.0-backend",
    trailer: bytes = b"",
    payload_fill: bytes = b"A",
) -> bytes:
    project = "fof_backend_uplink" if target == "uplink-s3-backend" else "fof_backend_scanner"
    image_kind = 0 if target == "uplink-s3-backend" else 1
    identity = _backend_identity_record(
        target=target,
        project=project,
        hardware="seeed_xiao_esp32s3",
        version="0.1.0-backend",
        image_kind=image_kind,
    )
    return _esp_firmware_image(
        descriptor_version,
        project=descriptor_project or project,
        identity_records=identity_records or (identity,),
        trailer=trailer,
        payload_fill=payload_fill,
    )


def _badge_image(
    target: str,
    *,
    version: str = BADGE_VERSION,
    project: str | None = None,
    target_marker: str | None = None,
    hardware_marker: str | None = None,
) -> bytes:
    expected_project, expected_hardware = BADGE_IDENTITIES[target]
    trailer = b"\0".join((
        (target_marker or target).encode("ascii"),
        (hardware_marker or expected_hardware).encode("ascii"),
    )) + b"\0"
    return _esp_firmware_image(
        version,
        project=project or expected_project,
        trailer=trailer,
    )


def _named_nonbackend_image(
    target: str,
    *,
    version: str | None = None,
) -> bytes:
    project, hardware = NONBACKEND_IDENTITIES[target]
    embedded_version = version or (
        BADGE_VERSION if target.endswith("-fof_badge") else PRODUCTION_VERSION
    )
    return _esp_firmware_image(
        embedded_version,
        project=project,
        trailer=f"{target}\0{hardware}\0".encode("ascii"),
    )


def release(tag: str, asset_name: str) -> dict:
    return {
        "tag_name": tag,
        "draft": False,
        "assets": [{
            "name": asset_name,
            "size": 1024,
            "browser_download_url": f"https://example.test/{asset_name}",
        }],
    }


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


def _mock_github_releases(
    monkeypatch: pytest.MonkeyPatch,
    releases: list[dict],
) -> None:
    class FakeAsyncClient:
        def __init__(self, **kwargs):
            pass

        async def __aenter__(self):
            return self

        async def __aexit__(self, exc_type, exc, tb):
            return False

        async def get(self, url: str):
            class Response:
                status_code = 200

                def json(self):
                    return releases

            return Response()

    monkeypatch.setattr(firmware_manager.httpx, "AsyncClient", FakeAsyncClient)


def test_live_fleet_firmware_targets_are_present():
    assert set(FIRMWARE_TYPES) == {
        "scanner-s3-combo",
        "scanner-s3-combo-fof_badge",
        "scanner-s3-combo-seed",
        "scanner-s3-combo-backend",
        "uplink-s3",
        "uplink-s3-fof_badge",
        "uplink-s3-backend",
    }


def test_backend_targets_have_exact_identity_and_isolated_paths():
    assert FIRMWARE_TYPES["uplink-s3-backend"] == {
        "description": "Backend sensor uplink (Seeed XIAO ESP32-S3)",
        "asset_pattern": "uplink-s3-backend",
        "board": "esp32s3",
        "project": "fof_backend_uplink",
        "hardware": "seeed_xiao_esp32s3",
        "image_kind": 0,
        "partition_capacity": 0x200000,
        "local_bin": firmware_manager._REPO_ROOT / "backend-firmware/uplink/.pio/build/uplink-s3-backend/firmware.bin",
    }
    assert FIRMWARE_TYPES["scanner-s3-combo-backend"]["project"] == "fof_backend_scanner"
    assert FIRMWARE_TYPES["scanner-s3-combo-backend"]["image_kind"] == 1
    assert FIRMWARE_TYPES["scanner-s3-combo-backend"]["partition_capacity"] == 0x200000
    assert str(FIRMWARE_TYPES["scanner-s3-combo-backend"]["local_bin"]).endswith(
        "/backend-firmware/scanner/.pio/build/scanner-s3-combo-backend/firmware.bin"
    )


def test_every_catalog_target_declares_its_exact_runtime_family_identity():
    expected = {
        "uplink-s3": ("fof_uplink", "esp32-s3-devkitc-1"),
        "uplink-s3-fof_badge": ("fof_badge_uplink", "seeed_xiao_esp32s3"),
        "uplink-s3-backend": ("fof_backend_uplink", "seeed_xiao_esp32s3"),
        "scanner-s3-combo": ("fof_scanner", "esp32-s3-devkitc-1"),
        "scanner-s3-combo-seed": ("fof_scanner_seed", "esp32-s3-devkitc-1"),
        "scanner-s3-combo-fof_badge": (
            "fof_badge_scanner",
            "seeed_xiao_esp32s3",
        ),
        "scanner-s3-combo-backend": (
            "fof_backend_scanner",
            "seeed_xiao_esp32s3",
        ),
    }

    actual = {
        name: (info.get("project"), info.get("hardware"))
        for name, info in FIRMWARE_TYPES.items()
    }

    assert actual == expected


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


@pytest.mark.asyncio
async def test_refresh_skips_newer_apk_only_release_for_firmware_release(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path,
):
    _mock_github_releases(
        monkeypatch,
        [
            {
                "tag_name": "v0.64.77-android",
                "draft": False,
                "assets": [
                    {
                        "name": "FriendOrFoe-v0.64.77.apk",
                        "size": 1,
                        "browser_download_url": "https://example.test/app.apk",
                    }
                ],
            },
            {
                "tag_name": "v0.64.76-firmware",
                "draft": False,
                "assets": [
                    {
                        "name": "scanner-s3-combo.bin",
                        "size": 2,
                        "browser_download_url": "https://example.test/scanner.bin",
                    }
                ],
            },
        ],
    )
    monkeypatch.setattr(firmware_manager, "CACHE_DIR", tmp_path / "firmware-cache")
    manager = FirmwareManager()

    await manager.refresh_from_github(force=True)

    assert manager.release_tag == "v0.64.76-firmware"
    assert manager.assets["scanner-s3-combo"].download_url == "https://example.test/scanner.bin"


@pytest.mark.asyncio
async def test_refresh_preserves_existing_catalog_when_no_firmware_release_exists(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path,
):
    _mock_github_releases(
        monkeypatch,
        [
            {
                "tag_name": "v0.64.77-android",
                "draft": False,
                "assets": [
                    {
                        "name": "FriendOrFoe-v0.64.77.apk",
                        "size": 1,
                        "browser_download_url": "https://example.test/app.apk",
                    }
                ],
            }
        ],
    )
    manager = _github_manager(
        monkeypatch,
        tmp_path,
        {"scanner-s3-combo": _esp_firmware_image(PRODUCTION_VERSION)},
    )
    existing_assets = manager.assets
    existing_tag = manager.release_tag
    manager.last_check = 0

    await manager.refresh_from_github(force=True)

    assert manager.release_tag == existing_tag
    assert manager.assets is existing_assets
    assert set(manager.assets) == {"scanner-s3-combo"}
    assert manager.last_check > 0


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


def _descriptor_only_image(size: int, *, project: str, version: str) -> bytearray:
    image = bytearray(size)
    image[0] = 0xE9
    struct.pack_into("<I", image, 0x20, 0xABCD5432)
    image[0x30:0x50] = version.encode("ascii").ljust(32, b"\0")
    image[0x50:0x70] = project.encode("ascii").ljust(32, b"\0")
    image[0x70:0x80] = b"12:34:56".ljust(16, b"\0")
    image[0x80:0x90] = b"Aug 02 2026".ljust(16, b"\0")
    return image


def test_catalog_rejects_descriptor_only_named_badge_bytes():
    image = _descriptor_only_image(
        183,
        project="fof_badge_uplink",
        version=BADGE_VERSION,
    )
    image[144:183] = b"uplink-s3-fof_badge\0seeed_xiao_esp32s3\0"

    assert len(image) == 183
    assert not FirmwareManager().validate_firmware_image(
        "uplink-s3-fof_badge",
        bytes(image),
    )


def test_catalog_rejects_descriptor_and_record_only_backend_bytes():
    image = _descriptor_only_image(
        308,
        project="fof_backend_uplink",
        version="0.1.0-backend",
    )
    image[144:308] = _backend_identity_record(
        target="uplink-s3-backend",
        project="fof_backend_uplink",
        hardware="seeed_xiao_esp32s3",
        version="0.1.0-backend",
        image_kind=0,
    )

    assert len(image) == 308
    assert not FirmwareManager().validate_firmware_image(
        "uplink-s3-backend",
        bytes(image),
    )


@pytest.mark.parametrize(
    "mutation",
    [
        "entry_address",
        "load_address",
        "load_range",
        "chip_id",
        "zero_segments",
        "too_many_segments",
        "missing_hash_flag",
        "padding",
        "checksum",
        "digest",
        "truncated",
        "trailing_data",
    ],
)
def test_catalog_rejects_invalid_esp32s3_image_layout(mutation: str):
    image = bytearray(_badge_image("uplink-s3-fof_badge"))
    if mutation == "entry_address":
        struct.pack_into("<I", image, 4, 0)
        image = bytearray(resign_esp_image(bytes(image)))
    elif mutation == "load_address":
        struct.pack_into("<I", image, 24, 0)
        image = bytearray(resign_esp_image(bytes(image)))
    elif mutation == "load_range":
        struct.pack_into("<I", image, 24, 0x3DFFF000)
        image = bytearray(resign_esp_image(bytes(image)))
    elif mutation == "chip_id":
        struct.pack_into("<H", image, 12, 5)
        image = bytearray(resign_esp_image(bytes(image)))
    elif mutation == "zero_segments":
        image[1] = 0
        image = bytearray(resign_esp_image(bytes(image)))
    elif mutation == "too_many_segments":
        image[1] = 17
        image = bytearray(resign_esp_image(bytes(image)))
    elif mutation == "missing_hash_flag":
        image[23] = 0
        image = bytearray(resign_esp_image(bytes(image)))
    elif mutation == "padding":
        image[-34] ^= 1
        image = bytearray(resign_esp_image(bytes(image)))
    elif mutation == "checksum":
        image[-33] ^= 1
        image = bytearray(resign_esp_image(bytes(image)))
    elif mutation == "digest":
        image[-1] ^= 1
    elif mutation == "truncated":
        image = image[:-1]
    elif mutation == "trailing_data":
        image.extend(b"\0")

    assert not FirmwareManager().validate_firmware_image(
        "uplink-s3-fof_badge",
        bytes(image),
    )


def test_catalog_rejects_tiny_but_fully_checksummed_named_app():
    image = esp32s3_app_image(
        BADGE_VERSION,
        project="fof_badge_uplink",
        placements=((
            0x200,
            b"uplink-s3-fof_badge\0seeed_xiao_esp32s3\0",
        ),),
        payload_size=1200,
    )

    assert len(image) < 64 * 1024
    assert not FirmwareManager().validate_firmware_image(
        "uplink-s3-fof_badge",
        image,
    )


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
        {
            name: _named_nonbackend_image(
                name,
                version=embedded_version,
            )
        },
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
            "scanner-s3-combo": _named_nonbackend_image(
                "scanner-s3-combo",
            ),
            "scanner-s3-combo-fof_badge": _badge_image(
                "scanner-s3-combo-fof_badge",
            ),
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
    assert catalog[name]["available"] is False


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
    local_bin.write_bytes(_named_nonbackend_image(
        name,
        version=embedded_version,
    ))
    monkeypatch.setitem(FIRMWARE_TYPES[name], "local_bin", local_bin)

    manager = FirmwareManager()

    async def no_refresh(force: bool = False):
        return None

    monkeypatch.setattr(manager, "refresh_from_github", no_refresh)
    manager.assets[name] = FirmwareAsset(
        name,
        FIRMWARE_TYPES[name]["description"],
        RELEASE_TAG,
        len(_named_nonbackend_image(name)),
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
    image = _named_nonbackend_image(name)
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
    image = _named_nonbackend_image(name)
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


@pytest.mark.asyncio
async def test_backend_catalog_has_exact_identity(monkeypatch, tmp_path):
    name = "uplink-s3-backend"
    image = _backend_image(name)
    manager = _github_manager(monkeypatch, tmp_path, {name: image})
    catalog = {row["name"]: row for row in await manager.get_catalog()}
    assert catalog[name]["target"] == name
    assert catalog[name]["project"] == "fof_backend_uplink"
    assert catalog[name]["hardware"] == "seeed_xiao_esp32s3"
    assert catalog[name]["version"] == "0.1.0-backend"


@pytest.mark.asyncio
async def test_backend_catalog_rejects_badge_project_under_backend_target(monkeypatch, tmp_path):
    wrong = _esp_firmware_image(
        "0.1.0-backend",
        project="fof_badge_uplink",
        identity_records=(_backend_identity_record(
            target="uplink-s3-backend",
            project="fof_backend_uplink",
            hardware="seeed_xiao_esp32s3",
            version="0.1.0-backend",
            image_kind=0,
        ),),
    )
    manager = _github_manager(monkeypatch, tmp_path, {"uplink-s3-backend": wrong})
    assert await manager.get_firmware_binary("uplink-s3-backend") is None


@pytest.mark.parametrize("target", sorted(BADGE_IDENTITIES))
@pytest.mark.parametrize(
    "version",
    [BADGE_VERSION, "0.68.0-badge-defcon34"],
)
def test_badge_catalog_accepts_exact_identity_for_current_and_future_versions(
    target: str,
    version: str,
):
    manager = FirmwareManager()

    assert manager.validate_firmware_image(
        target,
        _badge_image(target, version=version),
    )


@pytest.mark.parametrize("target", sorted(BADGE_IDENTITIES))
@pytest.mark.parametrize(
    "mutation",
    ["descriptor", "version", "project", "target", "hardware"],
)
def test_badge_catalog_rejects_missing_or_cross_family_identity(
    target: str,
    mutation: str,
):
    kwargs: dict[str, str] = {}
    if mutation == "version":
        kwargs["version"] = "0.68.0-backend"
    elif mutation == "project":
        kwargs["project"] = "fof_backend_uplink"
    elif mutation == "target":
        kwargs["target_marker"] = "uplink-s3-backend"
    elif mutation == "hardware":
        kwargs["hardware_marker"] = "esp32-s3-devkitc-1"
    image = b"not-an-esp-image" if mutation == "descriptor" else _badge_image(target, **kwargs)

    assert not FirmwareManager().validate_firmware_image(target, image)


@pytest.mark.parametrize(
    ("role", "target"),
    [
        ("uplink", "uplink-s3-fof_badge"),
        ("scanner", "scanner-s3-combo-fof_badge"),
    ],
)
def test_badge_catalog_accepts_the_native_0672_usb_bundle(role: str, target: str):
    bundle = (
        Path(__file__).resolve().parents[2]
        / "tools/badge_flasher/resources/badge-factory-flasher-embedded.zip"
    )
    with zipfile.ZipFile(bundle) as archive:
        image = archive.read(f"{role}/firmware.bin")

    assert FirmwareManager().validate_firmware_image(target, image)


@pytest.mark.parametrize(
    ("requested_target", "image_target"),
    [
        ("uplink-s3", "uplink-s3-fof_badge"),
        ("scanner-s3-combo", "scanner-s3-combo-seed"),
        ("scanner-s3-combo-seed", "scanner-s3-combo-fof_badge"),
    ],
)
def test_nonbadge_catalog_rejects_another_named_family_image(
    requested_target: str,
    image_target: str,
):
    assert not FirmwareManager().validate_firmware_image(
        requested_target,
        _named_nonbackend_image(image_target),
    )


@pytest.mark.asyncio
async def test_each_firmware_target_selects_its_newest_matching_release(monkeypatch, tmp_path):
    _mock_github_releases(monkeypatch, [
        release("backend-fw-0.1.0-backend", "uplink-s3-backend.bin"),
        release("v0.67.2-badge-defcon34", "uplink-s3-fof_badge.bin"),
        release("v0.64.68-live-follow", "uplink-s3.bin"),
    ])
    monkeypatch.setattr(firmware_manager, "CACHE_DIR", tmp_path / "cache")
    manager = FirmwareManager()
    await manager.refresh_from_github(force=True)
    assert manager.assets["uplink-s3-backend"].release_tag == "backend-fw-0.1.0-backend"
    assert manager.assets["uplink-s3-fof_badge"].release_tag == "v0.67.2-badge-defcon34"
    assert manager.assets["uplink-s3"].release_tag == "v0.64.68-live-follow"


@pytest.mark.parametrize("target", ["uplink-s3-backend", "scanner-s3-combo-backend"])
def test_exactly_one_backend_identity_record_is_accepted(target):
    image = _backend_image(target)
    parsed = firmware_manager._parse_backend_identity(image)
    assert parsed is not None
    assert parsed["target"] == target
    assert firmware_manager._validated_backend_image_info(target, image) is not None


def test_one_valid_record_plus_raw_invalid_magic_is_accepted():
    target = "uplink-s3-backend"
    image = _backend_image(target, trailer=RAW_INVALID_MAGIC)
    assert firmware_manager._parse_backend_identity(image)["target"] == target
    assert firmware_manager._validated_backend_image_info(target, image) is not None


def test_two_valid_identity_records_are_rejected():
    valid = _backend_identity_record(target="uplink-s3-backend", project="fof_backend_uplink", hardware="seeed_xiao_esp32s3", version="0.1.0-backend", image_kind=0)
    image = _backend_image("uplink-s3-backend", identity_records=(valid, valid))
    assert firmware_manager._parse_backend_identity(image) is None
    assert firmware_manager._validated_backend_image_info("uplink-s3-backend", image) is None


def test_valid_canonical_record_plus_malformed_record_shaped_candidate_is_rejected():
    candidate = bytearray(_backend_identity_record(
        target="scanner-s3-combo-backend",
        project="fof_backend_scanner",
        hardware="seeed_xiao_esp32s3",
        version="0.1.0-backend",
        image_kind=1,
    ))
    candidate[-1] ^= 1
    image = _backend_image(
        "uplink-s3-backend",
        trailer=bytes(candidate),
    )

    assert firmware_manager._parse_backend_identity(image) is None
    assert firmware_manager._validated_backend_image_info(
        "uplink-s3-backend",
        image,
    ) is None


def test_raw_invalid_magic_without_a_valid_record_is_rejected():
    image = _backend_image("uplink-s3-backend", identity_records=(RAW_INVALID_MAGIC,))
    assert firmware_manager._parse_backend_identity(image) is None
    assert firmware_manager._validated_backend_image_info("uplink-s3-backend", image) is None


def _identity_recrc(prefix: bytes) -> bytes:
    assert len(prefix) == 160
    return prefix + struct.pack("<I", zlib.crc32(prefix) & 0xFFFFFFFF)


def _identity_replace(record: bytes, start: int, width: int, value: str) -> bytes:
    prefix = bytearray(record[:160])
    prefix[start:start + width] = _fixed_identity_string(value, width)
    return _identity_recrc(bytes(prefix))


@pytest.mark.parametrize("mutation", [
    "bad_crc", "schema", "image_kind", "target", "project", "hardware",
    "invalid_backend_version", "descriptor_project", "descriptor_version",
    "nonzero_after_nul", "oversized_partition",
])
def test_backend_identity_mutation_matrix_fails_closed(mutation):
    target = "uplink-s3-backend"
    project = "fof_backend_uplink"
    record = _backend_identity_record(target=target, project=project, hardware="seeed_xiao_esp32s3", version="0.1.0-backend", image_kind=0)
    descriptor_project = project
    descriptor_version = "0.1.0-backend"
    if mutation == "bad_crc":
        record = record[:-1] + bytes([record[-1] ^ 0x01])
    elif mutation == "schema":
        prefix = bytearray(record[:160]); struct.pack_into("<H", prefix, 4, 2); record = _identity_recrc(bytes(prefix))
    elif mutation == "image_kind":
        prefix = bytearray(record[:160]); struct.pack_into("<H", prefix, 6, 1); record = _identity_recrc(bytes(prefix))
    elif mutation == "target":
        record = _identity_replace(record, 8, 40, "scanner-s3-combo-backend")
    elif mutation == "project":
        record = _identity_replace(record, 48, 40, "fof_badge_uplink")
    elif mutation == "hardware":
        record = _identity_replace(record, 88, 40, "esp32s3_other")
    elif mutation == "invalid_backend_version":
        record = _identity_replace(record, 128, 32, "0.1.0-badge"); descriptor_version = "0.1.0-badge"
    elif mutation == "descriptor_project":
        descriptor_project = "fof_badge_uplink"
    elif mutation == "descriptor_version":
        descriptor_version = "0.1.1-backend"
    elif mutation == "nonzero_after_nul":
        prefix = bytearray(record[:160]); prefix[8 + len(target) + 1] = 0x41; record = _identity_recrc(bytes(prefix))
    image = _backend_image(target, identity_records=(record,), descriptor_project=descriptor_project, descriptor_version=descriptor_version)
    if mutation == "oversized_partition":
        image = image.ljust(FIRMWARE_TYPES[target]["partition_capacity"] + 1, b"\xA5")
    assert firmware_manager._validated_backend_image_info(target, image) is None


@pytest.mark.parametrize(("asset_name", "expected"), [
    ("scanner-s3-combo-backend.bin", "scanner-s3-combo-backend"),
    ("scanner-s3-combo-fof_badge-v0.1.0-mixed.bin", "scanner-s3-combo-fof_badge"),
    ("scanner-s3-combo-seed.bin", "scanner-s3-combo-seed"),
    ("scanner-s3-combo-v0.1.0-mixed.bin", "scanner-s3-combo"),
    ("uplink-s3-backend-v0.1.0-mixed.bin", "uplink-s3-backend"),
    ("uplink-s3-fof_badge.bin", "uplink-s3-fof_badge"),
    ("uplink-s3.bin", "uplink-s3"),
    ("scanner-s3-combo-backendish-v0.1.0-mixed.bin", None),
    ("scanner-s3-combo-backend-v0.1.0-mixed-extra.bin", None),
    ("scanner-s3-combo-backend-v0.1.0-mixed.bin.sig", None),
    ("prefix-uplink-s3-backend-v0.1.0-mixed.bin", None),
])
def test_asset_target_requires_longest_exact_filename(asset_name, expected):
    assert firmware_manager._asset_target(asset_name, "v0.1.0-mixed") == expected


@pytest.mark.asyncio
async def test_scanner_base_target_cannot_claim_backend_or_badge_asset(monkeypatch, tmp_path):
    tag = "v0.1.0-mixed"
    _mock_github_releases(monkeypatch, [{"tag_name": tag, "draft": False, "assets": [
        {"name": f"scanner-s3-combo-backend-{tag}.bin", "size": 1024, "browser_download_url": "https://example.test/backend.bin"},
        {"name": f"scanner-s3-combo-fof_badge-{tag}.bin", "size": 1024, "browser_download_url": "https://example.test/badge.bin"},
    ]}])
    monkeypatch.setattr(firmware_manager, "CACHE_DIR", tmp_path / "cache")
    manager = FirmwareManager()
    await manager.refresh_from_github(force=True)
    assert set(manager.assets) == {"scanner-s3-combo-backend", "scanner-s3-combo-fof_badge"}
    assert "scanner-s3-combo" not in manager.assets
