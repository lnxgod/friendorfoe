import hashlib
import struct
import time
import zlib

import pytest

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
    trailer: bytes = b"",
    payload_fill: bytes = b"A",
) -> bytes:
    encoded_version = version.encode("ascii")
    assert len(encoded_version) < 32

    image = bytearray(0x20 + 256)
    image[0] = 0xE9
    struct.pack_into("<I", image, 0x20, 0xABCD5432)
    image[0x30:0x50] = encoded_version.ljust(32, b"\x00")
    image[0x50:0x70] = project.encode("ascii").ljust(32, b"\x00")
    image[0x70:0x80] = b"12:34:56".ljust(16, b"\x00")
    image[0x80:0x90] = b"Jul 16 2026".ljust(16, b"\x00")
    for record in identity_records:
        image.extend(record)
    image.extend(trailer)
    image.extend(payload_fill * 8)
    return bytes(image)


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
