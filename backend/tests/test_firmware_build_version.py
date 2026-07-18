import importlib.util
import json
import struct
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
MODULE_PATH = REPO_ROOT / "esp32" / "scripts" / "firmware_version.py"

TARGET_IDENTITIES = {
    "scanner-s3-combo": (
        "fof_scanner",
        "esp32-s3-devkitc-1",
        "0.64.68-live-follow",
    ),
    "scanner-s3-combo-seed": (
        "fof_scanner_seed",
        "esp32-s3-devkitc-1",
        "0.64.68-live-follow",
    ),
    "scanner-s3-combo-fof_badge": (
        "fof_badge_scanner",
        "seeed_xiao_esp32s3",
        "0.64.68-badge-live-follow",
    ),
    "uplink-s3": (
        "fof_uplink",
        "esp32-s3-devkitc-1",
        "0.64.68-live-follow",
    ),
    "uplink-s3-fof_badge": (
        "fof_badge_uplink",
        "seeed_xiao_esp32s3",
        "0.64.68-badge-live-follow",
    ),
}


def _load_module():
    spec = importlib.util.spec_from_file_location("fof_firmware_version", MODULE_PATH)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _esp_image(project: str, version: str, *markers: str) -> bytes:
    image = bytearray(0x20 + 112)
    image[0] = 0xE9
    struct.pack_into("<I", image, 0x20, 0xABCD5432)
    image[0x30:0x50] = version.encode("ascii").ljust(32, b"\x00")
    image[0x50:0x70] = project.encode("ascii").ljust(32, b"\x00")
    for marker in markers:
        image.extend(b"\x00" + marker.encode("ascii") + b"\x00")
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


def test_badge_uplink_descriptor_has_badge_project_and_version():
    module = _load_module()

    info = module.parse_firmware_identity(
        _esp_image("fof_badge_uplink", "0.64.68-badge-live-follow")
    )

    assert info.project == "fof_badge_uplink"
    assert info.version == "0.64.68-badge-live-follow"


def test_all_five_targets_have_stable_project_hardware_and_version_identity():
    module = _load_module()
    header = REPO_ROOT / "esp32" / "shared" / "version.h"

    for target, (project, hardware, version) in TARGET_IDENTITIES.items():
        identity = module.expected_identity_for_env(header, target)
        assert identity.target == target
        assert identity.project == project
        assert identity.hardware == hardware
        assert identity.version == version


def test_cmake_descriptor_projects_and_c_identity_map_cannot_drift():
    header = (REPO_ROOT / "esp32" / "shared" / "version.h").read_text()
    scanner_cmake = (REPO_ROOT / "esp32" / "scanner" / "CMakeLists.txt").read_text()
    uplink_cmake = (REPO_ROOT / "esp32" / "uplink" / "CMakeLists.txt").read_text()

    for target, (project, hardware, _version) in TARGET_IDENTITIES.items():
        cmake = uplink_cmake if target.startswith("uplink") else scanner_cmake
        assert target in cmake
        assert project in cmake
        assert target in header
        assert project in header
        assert hardware in header


def test_compile_selected_identity_replaces_hard_coded_runtime_names():
    version_header = (REPO_ROOT / "esp32" / "shared" / "version.h").read_text()
    uplink_main = (REPO_ROOT / "esp32" / "uplink" / "main" / "main.c").read_text()
    scanner_main = (REPO_ROOT / "esp32" / "scanner" / "main" / "main.c").read_text()
    heartbeat = (
        REPO_ROOT / "esp32" / "uplink" / "main" / "comms" / "http_upload.c"
    ).read_text()
    serial_status = (
        REPO_ROOT / "esp32" / "uplink" / "main" / "core" / "serial_config.c"
    ).read_text()
    http_status = (
        REPO_ROOT / "esp32" / "uplink" / "main" / "network" / "http_status.c"
    ).read_text()
    auto_check = (
        REPO_ROOT / "esp32" / "uplink" / "main" / "network" / "fw_auto_check.c"
    ).read_text()

    for macro in ("FOF_FIRMWARE_TARGET", "FOF_APP_PROJECT", "FOF_HARDWARE_TYPE"):
        assert macro in version_header
    assert '#define FIRMWARE_NAME "uplink-s3"' not in uplink_main
    assert '#define FIRMWARE_NAME "scanner"' not in scanner_main
    assert "FOF_PRINT_IDENT(TAG, FOF_FIRMWARE_TARGET)" in uplink_main
    assert "FOF_PRINT_IDENT(TAG, FOF_FIRMWARE_TARGET)" in scanner_main
    assert '\\"board_type\\":\\"uplink-s3\\"' not in heartbeat
    for source in (heartbeat, serial_status, http_status):
        assert '"firmware_name"' in source
        assert "FOF_FIRMWARE_TARGET" in source
        assert '"hardware_type"' in source
        assert "FOF_HARDWARE_TYPE" in source
        assert '"app_project"' in source
        assert "FOF_APP_PROJECT" in source
    assert "fetch_metadata(backend_base, FOF_FIRMWARE_TARGET" in auto_check
    assert "download_to_partition(backend_base, FOF_FIRMWARE_TARGET" in auto_check


def test_scanner_info_carries_runtime_base_mac_through_uplink_status():
    scanner_tx = (
        REPO_ROOT / "esp32" / "scanner" / "main" / "comms" / "uart_tx.c"
    ).read_text()
    uplink_header = (
        REPO_ROOT / "esp32" / "uplink" / "main" / "comms" / "uart_rx.h"
    ).read_text()
    uplink_rx = (
        REPO_ROOT / "esp32" / "uplink" / "main" / "comms" / "uart_rx.c"
    ).read_text()
    serial_status = (
        REPO_ROOT / "esp32" / "uplink" / "main" / "core" / "serial_config.c"
    ).read_text()
    http_status = (
        REPO_ROOT / "esp32" / "uplink" / "main" / "network" / "http_status.c"
    ).read_text()

    assert "esp_efuse_mac_get_default" in scanner_tx
    for field in ("firmware_name", "app_project", "hardware_type", "hardware_id"):
        assert f'"{field}"' in scanner_tx
        assert field in uplink_header
        assert f'"{field}"' in uplink_rx
    assert "info->hardware_id" in serial_status
    assert "info->hardware_id" in http_status


def test_all_five_descriptor_identities_are_verified_and_mismatch_is_reported(tmp_path):
    module = _load_module()
    header = REPO_ROOT / "esp32" / "shared" / "version.h"
    images = {}
    for target, (project, hardware, version) in TARGET_IDENTITIES.items():
        image = tmp_path / f"{target}.bin"
        image.write_bytes(_esp_image(project, version, target, hardware))
        images[target] = image

    assert module.verify_firmware_images(header, images) == []

    badge = images["scanner-s3-combo-fof_badge"]
    badge.write_bytes(_esp_image(
        "fof_scanner",
        "0.64.68-badge-live-follow",
        "scanner-s3-combo-fof_badge",
        "seeed_xiao_esp32s3",
    ))
    errors = module.verify_firmware_images(header, images)

    assert len(errors) == 1
    assert "scanner-s3-combo-fof_badge" in errors[0]
    assert "fof_scanner" in errors[0]
    assert "fof_badge_scanner" in errors[0]


def test_verifier_rejects_missing_target_or_hardware_identity_markers(tmp_path):
    module = _load_module()
    header = REPO_ROOT / "esp32" / "shared" / "version.h"
    target = "uplink-s3-fof_badge"
    image = tmp_path / "uplink.bin"
    image.write_bytes(_esp_image(
        "fof_badge_uplink",
        "0.64.68-badge-live-follow",
        target,
    ))

    errors = module.verify_firmware_images(header, {target: image})

    assert len(errors) == 1
    assert "hardware" in errors[0]
    assert "seeed_xiao_esp32s3" in errors[0]


def test_version_only_parser_remains_backward_compatible():
    module = _load_module()
    image = _esp_image("fof_scanner", "0.64.68-live-follow")

    assert module.parse_app_desc_version(image) == (
        "0.64.68-live-follow"
    )
