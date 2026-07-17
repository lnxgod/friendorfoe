import importlib.util
import json
import struct
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
MODULE_PATH = REPO_ROOT / "esp32" / "scripts" / "firmware_version.py"


def _load_module():
    spec = importlib.util.spec_from_file_location("fof_firmware_version", MODULE_PATH)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _esp_image(version: str) -> bytes:
    image = bytearray(0x20 + 112)
    image[0] = 0xE9
    struct.pack_into("<I", image, 0x20, 0xABCD5432)
    image[0x30:0x50] = version.encode("ascii").ljust(32, b"\x00")
    return bytes(image)


def test_shared_header_selects_production_and_badge_tracks():
    module = _load_module()
    header = REPO_ROOT / "esp32" / "shared" / "version.h"

    assert module.expected_version_for_env(header, "scanner-s3-combo") == (
        "0.64.68-live-follow"
    )
    assert module.expected_version_for_env(header, "uplink-s3-fof_badge") == (
        "0.64.68-badge-live-follow"
    )


def test_stale_generated_project_version_invalidates_only_cmake_cache(tmp_path):
    module = _load_module()
    build_dir = tmp_path / "build"
    build_dir.mkdir()
    cache = build_dir / "CMakeCache.txt"
    cache.write_text("generated cache")
    sentinel = build_dir / "firmware.bin"
    sentinel.write_bytes(b"keep")
    (build_dir / "project_description.json").write_text(
        json.dumps({"project_version": "0.64.67-old"})
    )

    changed = module.invalidate_stale_cmake_cache(
        build_dir,
        "0.64.68-live-follow",
    )

    assert changed is True
    assert not cache.exists()
    assert sentinel.read_bytes() == b"keep"


def test_current_generated_project_version_keeps_cmake_cache(tmp_path):
    module = _load_module()
    build_dir = tmp_path / "build"
    build_dir.mkdir()
    cache = build_dir / "CMakeCache.txt"
    cache.write_text("generated cache")
    (build_dir / "project_description.json").write_text(
        json.dumps({"project_version": "0.64.68-badge-live-follow"})
    )

    changed = module.invalidate_stale_cmake_cache(
        build_dir,
        "0.64.68-badge-live-follow",
    )

    assert changed is False
    assert cache.exists()


def test_non_object_project_metadata_invalidates_cmake_cache(tmp_path):
    module = _load_module()

    for payload in (None, []):
        build_dir = tmp_path / str(payload)
        build_dir.mkdir()
        cache = build_dir / "CMakeCache.txt"
        cache.write_text("generated cache")
        (build_dir / "project_description.json").write_text(json.dumps(payload))

        changed = module.invalidate_stale_cmake_cache(
            build_dir,
            "0.64.68-live-follow",
        )

        assert changed is True
        assert not cache.exists()


def test_embedded_app_descriptor_version_is_parsed_and_mismatch_is_reported(tmp_path):
    module = _load_module()
    production = tmp_path / "scanner.bin"
    badge = tmp_path / "badge.bin"
    production.write_bytes(_esp_image("0.64.68-live-follow"))
    badge.write_bytes(_esp_image("0.64.67-badge-old"))
    header = REPO_ROOT / "esp32" / "shared" / "version.h"

    assert module.parse_app_desc_version(production.read_bytes()) == (
        "0.64.68-live-follow"
    )
    errors = module.verify_firmware_images(
        header,
        {
            "scanner-s3-combo": production,
            "scanner-s3-combo-fof_badge": badge,
        },
    )

    assert len(errors) == 1
    assert "scanner-s3-combo-fof_badge" in errors[0]
    assert "0.64.67-badge-old" in errors[0]
