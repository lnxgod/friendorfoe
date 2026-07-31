import importlib.util
import json
import struct
import sys
from pathlib import Path

import pytest


REPO_ROOT = Path(__file__).resolve().parents[2]
MODULE_PATH = REPO_ROOT / "esp32" / "scripts" / "firmware_version.py"
VERIFY_MODULE_PATH = (
    REPO_ROOT / "esp32" / "scripts" / "verify_firmware_versions.py"
)
BADGE_BUILD_VERIFY_MODULE_PATH = (
    REPO_ROOT / "esp32" / "scripts" / "verify_badge_uplink_build.py"
)
BADGE_SCANNER_BUILD_VERIFY_MODULE_PATH = (
    REPO_ROOT / "esp32" / "scripts" / "verify_badge_scanner_build.py"
)

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
        "0.64.78-badge-defcon34",
    ),
    "scanner-s3-combo-fof_badge-con-crud-canary": (
        "fof_badge_scanner",
        "seeed_xiao_esp32s3",
        "0.67.2-badge-defcon34",
    ),
    "uplink-s3": (
        "fof_uplink",
        "esp32-s3-devkitc-1",
        "0.64.68-live-follow",
    ),
    "uplink-s3-fof_badge": (
        "fof_badge_uplink",
        "seeed_xiao_esp32s3",
        "0.64.78-badge-defcon34",
    ),
    "uplink-s3-fof_badge-con-crud-canary": (
        "fof_badge_uplink",
        "seeed_xiao_esp32s3",
        "0.67.2-badge-defcon34",
    ),
}


def test_ubuntu_esp32_workflow_runs_only_portable_host_guards():
    workflow = (
        REPO_ROOT / ".github" / "workflows" / "esp32-web-flasher.yml"
    ).read_text()

    assert "runs-on: ubuntu-latest" in workflow
    assert "Test portable badge release guard" in workflow
    assert "test_fof_flash_release.py" in workflow
    for macos_only_suite in (
        "test_verify_badge_usb_hardening",
        "test_bound_rom",
        "test_esptool_provenance",
    ):
        assert macos_only_suite not in workflow


def _load_module():
    spec = importlib.util.spec_from_file_location("fof_firmware_version", MODULE_PATH)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _load_verify_module():
    scripts_dir = str(VERIFY_MODULE_PATH.parent)
    if scripts_dir not in sys.path:
        sys.path.insert(0, scripts_dir)
    spec = importlib.util.spec_from_file_location(
        "fof_verify_firmware_versions", VERIFY_MODULE_PATH
    )
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _load_badge_build_verify_module():
    scripts_dir = str(BADGE_BUILD_VERIFY_MODULE_PATH.parent)
    if scripts_dir not in sys.path:
        sys.path.insert(0, scripts_dir)
    spec = importlib.util.spec_from_file_location(
        "fof_verify_badge_uplink_build", BADGE_BUILD_VERIFY_MODULE_PATH
    )
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _load_badge_scanner_build_verify_module():
    scripts_dir = str(BADGE_SCANNER_BUILD_VERIFY_MODULE_PATH.parent)
    if scripts_dir not in sys.path:
        sys.path.insert(0, scripts_dir)
    spec = importlib.util.spec_from_file_location(
        "fof_verify_badge_scanner_build",
        BADGE_SCANNER_BUILD_VERIFY_MODULE_PATH,
    )
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _write_partition_table(
    path: Path,
    ota0_offset: int = 0x20000,
    ota1_offset: int | None = None,
) -> None:
    entries = [
        struct.pack(
            "<HBBII16sI",
            0x50AA,
            0,
            0x10,
            ota0_offset,
            0x200000,
            b"ota_0".ljust(16, b"\x00"),
            0,
        )
    ]
    if ota1_offset is not None:
        entries.append(struct.pack(
            "<HBBII16sI",
            0x50AA,
            0,
            0x11,
            ota1_offset,
            0x200000,
            b"ota_1".ljust(16, b"\x00"),
            0,
        ))
    path.write_bytes(b"".join(entries))


def _write_web_flasher_site(module, site_root: Path) -> None:
    for target, spec in module.WEB_FLASHER_TARGETS.items():
        project, hardware, version = TARGET_IDENTITIES[target]
        firmware_dir = site_root / spec["firmware_dir"]
        firmware_dir.mkdir(parents=True, exist_ok=True)
        (firmware_dir / "bootloader.bin").write_bytes(b"boot")
        (firmware_dir / "partition-table.bin").write_bytes(b"partition")
        (firmware_dir / "firmware.bin").write_bytes(
            _esp_image(project, version, target, hardware)
        )
        manifest = {
            "name": target,
            "version": version,
            "builds": [{
                "chipFamily": "ESP32-S3",
                "parts": [
                    {"path": f'{spec["firmware_dir"]}/bootloader.bin', "offset": 0},
                    {"path": f'{spec["firmware_dir"]}/partition-table.bin', "offset": 32768},
                    {"path": f'{spec["firmware_dir"]}/firmware.bin', "offset": 131072},
                ],
            }],
        }
        (site_root / spec["manifest"]).write_text(json.dumps(manifest))


def test_web_flasher_site_validator_requires_exact_five_target_release(tmp_path):
    module = _load_verify_module()
    header = REPO_ROOT / "esp32" / "shared" / "version.h"
    _write_web_flasher_site(module, tmp_path)

    assert module.verify_web_flasher_site(header, tmp_path) == []

    badge_manifest = tmp_path / "manifest-badge-scanner.json"
    payload = json.loads(badge_manifest.read_text())
    payload["version"] = "0.64.68-badge-live-follow"
    badge_manifest.write_text(json.dumps(payload))
    (tmp_path / "firmware/badge-uplink/firmware.bin").unlink()

    errors = module.verify_web_flasher_site(header, tmp_path)
    assert any("manifest version" in error for error in errors)
    assert any("cannot read" in error for error in errors)


def test_pages_deploy_fails_closed_on_build_or_artifact_validation_failure():
    workflow = (
        REPO_ROOT / ".github" / "workflows" / "esp32-web-flasher.yml"
    ).read_text()
    deploy = workflow[workflow.index("  deploy:") :]

    assert "needs.build.result == 'success'" in deploy
    assert "continue-on-error: true" not in deploy
    assert "verify_firmware_versions.py --repo-root . --site-root _site" in deploy


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
        "0.64.78-badge-defcon34"
    )
    assert module.expected_identity_for_env(
        header, "uplink-s3-fof_badge-con-crud-canary"
    ).version == "0.67.2-badge-defcon34"
    assert module.expected_identity_for_env(
        header, "scanner-s3-combo-fof_badge"
    ).version == "0.64.78-badge-defcon34"
    assert module.expected_identity_for_env(
        header, "scanner-s3-combo-fof_badge-con-crud-canary"
    ).version == "0.67.2-badge-defcon34"
    assert module.runtime_target_for_env(
        "uplink-s3-fof_badge-con-crud-canary"
    ) == "uplink-s3-fof_badge"
    assert module.runtime_target_for_env(
        "scanner-s3-combo-fof_badge-con-crud-canary"
    ) == "scanner-s3-combo-fof_badge"


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
        json.dumps({"project_version": "0.64.78-badge-defcon34"})
    )

    changed = module.invalidate_stale_cmake_cache(
        build_dir,
        "0.64.78-badge-defcon34",
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
        _esp_image("fof_badge_uplink", "0.64.76-badge-defcon34")
    )

    assert info.project == "fof_badge_uplink"
    assert info.version == "0.64.76-badge-defcon34"


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
        runtime_target = target.removesuffix("-con-crud-canary")
        assert runtime_target in header
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
        assert '"firmware_name"' in source or '\\"firmware_name\\"' in source
        assert "FOF_FIRMWARE_TARGET" in source
        assert '"hardware_type"' in source or '\\"hardware_type\\"' in source
        assert "FOF_HARDWARE_TYPE" in source
        assert '"app_project"' in source or '\\"app_project\\"' in source
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


def test_uplink_reserves_detection_queue_before_heap_heavy_boot_steps():
    source = (
        REPO_ROOT / "esp32" / "uplink" / "main" / "main.c"
    ).read_text()
    app_main = source[source.index("void app_main(void)") :]

    marker = "QueueHandle_t detection_queue = xQueueCreate("
    assert app_main.count(marker) == 1
    queue_create = app_main.index(marker)
    queue_statement = app_main[
        queue_create : app_main.index(";", queue_create)
    ]
    assert "CONFIG_DETECTION_QUEUE_SIZE" in queue_statement
    assert "sizeof(drone_detection_t)" in queue_statement

    for call in (
        "badge_runtime_init(s_ota_pending_verify);",
        "oled_init();",
        "badge_usb_transport_set_dispatch_ready()",
        "esp_event_loop_create_default()",
    ):
        assert queue_create < app_main.index(call), (
            f"detection queue must be created before {call}"
        )


def test_badge_android_control_is_usb_only_and_bluetooth_is_compiled_out():
    defaults = (
        REPO_ROOT / "esp32" / "uplink" / "sdkconfig.esp32s3-fof_badge.defaults"
    ).read_text()

    assert "CONFIG_BT_ENABLED=n" in defaults
    assert "CONFIG_BT_NIMBLE_ENABLED=y" not in defaults
    assert "CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_INTERNAL=y" not in defaults

    policy = (REPO_ROOT / "esp32" / "shared" / "psram_policy.md").read_text()
    assert "USB-only badge control" in policy
    assert "task stacks" in policy

    main = (REPO_ROOT / "esp32" / "uplink" / "main" / "main.c").read_text()
    serial = (
        REPO_ROOT / "esp32" / "uplink" / "main" / "core" / "serial_config.c"
    ).read_text()
    registry = (
        REPO_ROOT
        / "esp32"
        / "uplink"
        / "main"
        / "core"
        / "badge_usb_control_schema.c"
    ).read_text()
    assert "badge_ble_control_init();" not in main
    assert '"badge_theme", "badge_theme", THEME' in registry
    assert "BADGE_USB_CONTROL_HANDLER_THEME" in registry
    assert "BADGE_USB_CONTROL_HANDLER_THEME" in serial
    assert '"badge_display_policy"' in registry
    assert "BADGE_USB_CONTROL_HANDLER_DISPLAY_POLICY" in registry
    assert "BADGE_USB_CONTROL_HANDLER_DISPLAY_POLICY" in serial


def test_uplink_bluetooth_guard_allows_only_explicit_badge_game_canary():
    main = (
        REPO_ROOT / "esp32" / "uplink" / "main" / "main.c"
    ).read_text()
    guard = (
        "#if CONFIG_BT_ENABLED && "
        "!(defined(FOF_BADGE_VARIANT) && "
        "defined(FOF_DC34_GAME_CANARY))"
    )

    assert guard in main
    assert (
        '#error "uplink firmware must keep Bluetooth disabled; '
        'only the explicit controller-only badge game canary may enable it"'
        in main
    )


def test_uplink_checks_required_worker_creation_before_operational_banner():
    main = (
        REPO_ROOT / "esp32" / "uplink" / "main" / "main.c"
    ).read_text()
    uart_header = (
        REPO_ROOT / "esp32" / "uplink" / "main" / "comms" / "uart_rx.h"
    ).read_text()
    uart_source = (
        REPO_ROOT / "esp32" / "uplink" / "main" / "comms" / "uart_rx.c"
    ).read_text()
    upload_header = (
        REPO_ROOT / "esp32" / "uplink" / "main" / "comms" / "http_upload.h"
    ).read_text()
    upload_source = (
        REPO_ROOT / "esp32" / "uplink" / "main" / "comms" / "http_upload.c"
    ).read_text()
    transport_header = (
        REPO_ROOT / "esp32" / "uplink" / "main" / "core" / "badge_usb_transport.h"
    ).read_text()
    transport_source = (
        REPO_ROOT / "esp32" / "uplink" / "main" / "core" / "badge_usb_transport.c"
    ).read_text()

    assert "bool uart_rx_start(void);" in uart_header
    assert "bool uart_rx_start(void)" in uart_source
    assert "bool http_upload_start(void);" in upload_header
    assert "bool http_upload_start(void)" in upload_source
    assert "bool badge_usb_transport_start(uint32_t boot_window_ms);" in transport_header
    assert "bool badge_usb_transport_start(uint32_t boot_window_ms)" in transport_source
    assert "xTaskCreateStatic(" in main
    assert "if (!s_display_task_handle)" in main

    failure_gate = main.index("if (!required_tasks_started)")
    operational_banner = main.index(
        'ESP_LOGI(TAG, "All tasks started. Uplink is operational.")'
    )
    assert failure_gate < operational_banner


def test_badge_build_does_not_allocate_outbound_http_upload_worker():
    main = (
        REPO_ROOT / "esp32" / "uplink" / "main" / "main.c"
    ).read_text()

    assert (
        "#ifndef FOF_BADGE_VARIANT\n"
        "    http_upload_init(detection_queue);"
    ) in main

    badge_worker_start = main.index(
        "#ifdef FOF_BADGE_VARIANT\n"
        "    if (!badge_safe_usb) {\n"
        "        uart_startup_gate_result_t uart_startup"
    )
    badge_worker_else = main.index("#else", badge_worker_start)
    badge_branch = main[badge_worker_start:badge_worker_else]
    standard_branch = main[
        badge_worker_else:main.index("#endif", badge_worker_else)
    ]
    assert "http_upload_start()" not in badge_branch
    assert "if (!http_upload_start())" in standard_branch
    assert "USB control and UART firmware relay remain active" in main


def test_usb_status_frame_is_built_before_one_required_transport_transaction():
    serial = (
        REPO_ROOT / "esp32" / "uplink" / "main" / "core" / "serial_config.c"
    ).read_text()
    start = serial.index("static void send_badge_status_response(void)")
    end = serial.index("static void send_control_ok", start)
    status = serial[start:end]

    allocation = status.index("heap_caps_malloc(")
    first_byte = status.index('status_printf("FOF_STATUS:')
    emit = status.rindex("render_emit(")
    assert allocation < first_byte < emit
    assert "BADGE_USB_FRAME_REQUIRED" in status[emit:]
    assert "fflush(stdout)" not in status
    assert "flockfile(stdout)" not in status

    # Every known blocking or heap-locking snapshot happens before stdout is
    # held, preventing stdout -> subsystem-lock inversions with ESP_LOG tasks.
    for capture in (
        "oled_badge_get_display_state(&display_state)",
        "badge_ble_investigation_status_json(investigation_status",
        "badge_display_policy_runtime_json(policy_json",
        "badge_theme_runtime_json(theme_json",
        "serial_live_metrics_snapshot()",
        "uart_rx_get_scanner_uart_diag(0, &ble_uart_diag)",
    ):
        assert status.index(capture) < first_byte

    display_helper_start = serial.index(
        "static void print_badge_display_state_field(", 0, start
    )
    display_helper_end = serial.index(
        "static void print_badge_button_state_field(", display_helper_start
    )
    display_helper = serial[display_helper_start:display_helper_end]
    assert "oled_badge_get_display_state" not in display_helper


def test_all_five_descriptor_identities_are_verified_and_mismatch_is_reported(tmp_path):
    module = _load_module()
    header = REPO_ROOT / "esp32" / "shared" / "version.h"
    images = {}
    for target, (project, hardware, version) in TARGET_IDENTITIES.items():
        image = tmp_path / f"{target}.bin"
        runtime_target = target.removesuffix("-con-crud-canary")
        image.write_bytes(_esp_image(
            project,
            version,
            runtime_target,
            hardware,
        ))
        images[target] = image

    assert module.verify_firmware_images(header, images) == []

    badge = images["scanner-s3-combo-fof_badge"]
    badge.write_bytes(_esp_image(
        "fof_scanner",
        "0.64.78-badge-defcon34",
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
        "0.64.78-badge-defcon34",
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


def test_badge_uplink_upload_offset_matches_decoded_ota0_and_generated_args():
    ini = (REPO_ROOT / "esp32" / "uplink" / "platformio.ini").read_text()
    badge_env = ini[ini.index("[env:uplink-s3-fof_badge]") :]
    assert "board_upload.offset_address = 0x20000" in badge_env
    assert "board_build.partitions = partitions_s3_fof_badge_8mb.csv" in badge_env
    assert (
        'board_build.cmake_extra_args = '
        '-DSDKCONFIG_DEFAULTS="sdkconfig.esp32s3-fof_badge.defaults"'
    ) in badge_env

    sdkconfig_defaults = (
        REPO_ROOT / "esp32" / "uplink" /
        "sdkconfig.esp32s3-fof_badge.defaults"
    ).read_text()
    assert "CONFIG_PARTITION_TABLE_CUSTOM=y" in sdkconfig_defaults
    assert (
        'CONFIG_PARTITION_TABLE_CUSTOM_FILENAME='
        '"partitions_s3_fof_badge_8mb.csv"'
    ) in sdkconfig_defaults

    partitions = (
        REPO_ROOT / "esp32" / "uplink" / "partitions_s3_fof_badge_8mb.csv"
    ).read_text()
    ota0 = next(line for line in partitions.splitlines() if line.startswith("ota_0,"))
    assert int(ota0.split(",")[3].strip(), 0) == 0x20000

    build_dir = (
        REPO_ROOT / "esp32" / "uplink" / ".pio" / "build" /
        "uplink-s3-fof_badge"
    )
    generated_paths = {
        name: build_dir / name
        for name in (
            "flash_args", "flash_app_args", "flash_project_args",
            "flasher_args.json",
        )
    }
    if any(path.exists() for path in generated_paths.values()):
        assert all(path.exists() for path in generated_paths.values())
        for name in ("flash_args", "flash_app_args", "flash_project_args"):
            contents = generated_paths[name].read_text()
            assert "0x20000 fof_badge_uplink.bin" in contents, name
            assert "0x10000 fof_badge_uplink.bin" not in contents, name

        manifest = json.loads(generated_paths["flasher_args.json"].read_text())
        assert manifest["app"]["offset"] == "0x20000"
        assert "0x10000" not in manifest["flash_files"]


def _write_badge_uplink_build_inputs(
    build_dir: Path, ota1_offset: int | None = None
) -> None:
    build_dir.mkdir(parents=True, exist_ok=True)
    (build_dir / "bootloader.bin").write_bytes(b"bootloader")
    _write_partition_table(
        build_dir / "partitions.bin", ota1_offset=ota1_offset
    )
    (build_dir / "firmware.bin").write_bytes(b"application")
    (build_dir / "ota_data_initial.bin").write_bytes(b"otadata")
    common = "\n".join((
        "--flash_mode dio --flash_freq 80m --flash_size 8MB",
        "0x0 bootloader/bootloader.bin",
        "0x20000 fof_badge_uplink.bin",
        "0x8000 partition_table/partition-table.bin",
        "0xf000 ota_data_initial.bin",
    ))
    (build_dir / "flash_args").write_text(common)
    (build_dir / "flash_project_args").write_text(common)
    (build_dir / "flash_app_args").write_text(
        "--flash_mode dio\n0x20000 fof_badge_uplink.bin\n"
    )
    (build_dir / "flasher_args.json").write_text(json.dumps({
        "flash_files": {
            "0x0": "bootloader/bootloader.bin",
            "0x20000": "fof_badge_uplink.bin",
            "0x8000": "partition_table/partition-table.bin",
            "0xf000": "ota_data_initial.bin",
        },
        "app": {"offset": "0x20000", "file": "fof_badge_uplink.bin"},
        "partition-table": {
            "offset": "0x8000",
            "file": "partition_table/partition-table.bin",
        },
    }))


def _write_badge_scanner_build_inputs(
    build_dir: Path, *, app_offset: int = 0x20000
) -> None:
    build_dir.mkdir(parents=True, exist_ok=True)
    (build_dir / "bootloader.bin").write_bytes(b"bootloader")
    _write_partition_table(build_dir / "partitions.bin")
    (build_dir / "firmware.bin").write_bytes(b"application")
    (build_dir / "ota_data_initial.bin").write_bytes(b"otadata")
    rendered_offset = f"0x{app_offset:x}"
    common = "\n".join((
        "--flash_mode dio --flash_freq 80m --flash_size 8MB",
        "0x0 bootloader/bootloader.bin",
        f"{rendered_offset} fof_badge_scanner.bin",
        "0x8000 partition_table/partition-table.bin",
        "0xf000 ota_data_initial.bin",
    ))
    (build_dir / "flash_args").write_text(common)
    (build_dir / "flash_project_args").write_text(common)
    (build_dir / "flash_app_args").write_text(
        f"--flash_mode dio\n{rendered_offset} fof_badge_scanner.bin\n"
    )
    (build_dir / "app-flash_args").write_text(
        f"--flash_mode dio\n{rendered_offset} fof_badge_scanner.bin\n"
    )
    (build_dir / "flasher_args.json").write_text(json.dumps({
        "flash_files": {
            "0x0": "bootloader/bootloader.bin",
            rendered_offset: "fof_badge_scanner.bin",
            "0x8000": "partition_table/partition-table.bin",
            "0xf000": "ota_data_initial.bin",
        },
        "app": {
            "offset": rendered_offset,
            "file": "fof_badge_scanner.bin",
        },
        "partition-table": {
            "offset": "0x8000",
            "file": "partition_table/partition-table.bin",
        },
    }))


def test_badge_scanner_env_pins_custom_partition_and_app_offset():
    ini = (REPO_ROOT / "esp32" / "scanner" / "platformio.ini").read_text()
    badge_env = ini[ini.index("[env:scanner-s3-combo-fof_badge]") :]
    assert "board_upload.offset_address = 0x20000" in badge_env
    assert "board_build.partitions = partitions_s3_scanner_8mb.csv" in badge_env
    assert (
        'board_build.cmake_extra_args = '
        '-DSDKCONFIG_DEFAULTS="sdkconfig.scanner-s3-fof_badge.defaults"'
    ) in badge_env

    defaults = (
        REPO_ROOT / "esp32" / "scanner" /
        "sdkconfig.scanner-s3-fof_badge.defaults"
    ).read_text()
    assert "CONFIG_PARTITION_TABLE_CUSTOM=y" in defaults
    assert (
        'CONFIG_PARTITION_TABLE_CUSTOM_FILENAME='
        '"partitions_s3_scanner_8mb.csv"'
    ) in defaults
    assert "CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y" in defaults

    partitions = (
        REPO_ROOT / "esp32" / "scanner" /
        "partitions_s3_scanner_8mb.csv"
    ).read_text()
    ota0 = next(
        line for line in partitions.splitlines()
        if line.startswith("ota_0,")
    )
    assert int(ota0.split(",")[3].strip(), 0) == 0x20000


def test_badge_scanner_verifier_rejects_manifest_app_offset_not_ota0(
    tmp_path,
):
    module = _load_badge_scanner_build_verify_module()
    _write_badge_scanner_build_inputs(tmp_path, app_offset=0x10000)
    module.materialize_badge_scanner_aliases(tmp_path)

    errors = module.verify_badge_scanner_build(tmp_path)

    assert any("0x10000" in error for error in errors), errors
    assert any(
        "application must be exactly 0x20000" in error
        for error in errors
    ), errors


def test_badge_scanner_verifier_guards_idf_app_flash_args(tmp_path):
    module = _load_badge_scanner_build_verify_module()
    _write_badge_scanner_build_inputs(tmp_path)
    module.materialize_badge_scanner_aliases(tmp_path)
    (tmp_path / "app-flash_args").write_text(
        "--flash_mode dio\n0x10000 fof_badge_scanner.bin\n"
    )

    errors = module.verify_badge_scanner_build(tmp_path)

    assert any(
        "app-flash_args" in error and "0x20000" in error
        for error in errors
    ), errors


def test_badge_scanner_verifier_requires_rollback_enabled_build_config(
    tmp_path,
):
    module = _load_badge_scanner_build_verify_module()
    sdkconfig = tmp_path / "sdkconfig.scanner"
    sdkconfig.write_text(
        "CONFIG_PARTITION_TABLE_CUSTOM=y\n"
        'CONFIG_PARTITION_TABLE_CUSTOM_FILENAME='
        '"partitions_s3_scanner_8mb.csv"\n'
        "# CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE is not set\n"
    )

    errors = module.verify_badge_scanner_sdkconfig(sdkconfig)

    assert errors == [
        "sdkconfig: CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE must be enabled"
    ]
    sdkconfig.write_text(
        "CONFIG_PARTITION_TABLE_CUSTOM=y\n"
        'CONFIG_PARTITION_TABLE_CUSTOM_FILENAME='
        '"partitions_s3_scanner_8mb.csv"\n'
        "CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y\n"
    )
    assert module.verify_badge_scanner_sdkconfig(sdkconfig) == []


def test_badge_scanner_post_build_verifier_materializes_and_accepts_ota0(
    tmp_path,
):
    module = _load_badge_scanner_build_verify_module()
    _write_badge_scanner_build_inputs(tmp_path)
    module.materialize_badge_scanner_aliases(tmp_path)

    assert module.verify_badge_scanner_build(tmp_path) == []
    for path in (
        "fof_badge_scanner.bin",
        "bootloader/bootloader.bin",
        "partition_table/partition-table.bin",
    ):
        assert (tmp_path / path).is_file(), path


def test_badge_scanner_strict_verifier_runs_post_build_and_in_ci():
    ini = (REPO_ROOT / "esp32" / "scanner" / "platformio.ini").read_text()
    workflow = (
        REPO_ROOT / ".github" / "workflows" / "esp32-web-flasher.yml"
    ).read_text()
    hook = (
        REPO_ROOT / "esp32" / "scripts" /
        "pio_verify_badge_scanner_build.py"
    ).read_text()

    assert "post:../scripts/pio_verify_badge_scanner_build.py" in ini
    assert "AddPostAction" in hook
    assert "verify_badge_scanner_build.py" in workflow
    assert (
        "--build-dir "
        "esp32/scanner/.pio/build/scanner-s3-combo-fof_badge"
    ) in workflow


def test_badge_uplink_post_build_verifier_materializes_every_manifest_path(tmp_path):
    module = _load_badge_build_verify_module()
    _write_badge_uplink_build_inputs(tmp_path)
    module.materialize_badge_uplink_aliases(tmp_path)
    assert module.verify_badge_uplink_build(tmp_path) == []
    for path in (
        "fof_badge_uplink.bin",
        "bootloader/bootloader.bin",
        "partition_table/partition-table.bin",
    ):
        assert (tmp_path / path).is_file(), path


def test_badge_uplink_canary_sdkconfig_verifier_fails_closed(tmp_path):
    module = _load_badge_build_verify_module()
    sdkconfig = tmp_path / "sdkconfig.canary"
    sdkconfig.write_text(
        "CONFIG_BT_ENABLED=y\n"
        "CONFIG_BT_CONTROLLER_ONLY=y\n"
        "CONFIG_BT_CONTROLLER_ENABLED=y\n"
        "CONFIG_BT_CTRL_HCI_MODE_VHCI=y\n"
        "CONFIG_BT_CTRL_BLE_MAX_ACT=1\n"
        "CONFIG_BT_CTRL_DTM_ENABLE=y\n"
        "CONFIG_BT_CTRL_BLE_ADV=y\n"
        "CONFIG_BT_CTRL_BLE_SCAN=y\n"
        "# CONFIG_BT_CTRL_BLE_MASTER is not set\n"
        "CONFIG_BT_CTRL_BLE_SECURITY_ENABLE=y\n"
        "# CONFIG_BT_BLUEDROID_ENABLED is not set\n"
        "# CONFIG_BT_NIMBLE_ENABLED is not set\n"
        "CONFIG_SPIRAM=y\n"
        "CONFIG_SPIRAM_USE_CAPS_ALLOC=n\n"
        "# CONFIG_SPIRAM_USE_MALLOC is not set\n"
    )

    errors = module.verify_badge_uplink_canary_sdkconfig(sdkconfig)

    assert any(
        "CONFIG_BT_CTRL_BLE_SCAN must be disabled" in error
        for error in errors
    ), errors
    assert any(
        "CONFIG_BT_CTRL_BLE_SECURITY_ENABLE must be disabled" in error
        for error in errors
    ), errors
    assert any(
        "CONFIG_SPIRAM_USE_CAPS_ALLOC must be enabled" in error
        for error in errors
    ), errors
    assert any(
        "CONFIG_BT_CTRL_RUN_IN_FLASH_ONLY must be enabled" in error
        for error in errors
    ), errors
    assert any(
        "CONFIG_BT_CTRL_DTM_ENABLE must be disabled" in error
        for error in errors
    ), errors

    sdkconfig.write_text(
        sdkconfig.read_text()
        .replace("CONFIG_BT_ENABLED=y",
                 "CONFIG_BT_ENABLED=y\n"
                 "CONFIG_BT_CTRL_RUN_IN_FLASH_ONLY=y")
        .replace("CONFIG_BT_CTRL_BLE_SCAN=y",
                 "# CONFIG_BT_CTRL_BLE_SCAN is not set")
        .replace("CONFIG_BT_CTRL_BLE_SECURITY_ENABLE=y",
                 "# CONFIG_BT_CTRL_BLE_SECURITY_ENABLE is not set")
        .replace("CONFIG_BT_CTRL_DTM_ENABLE=y",
                 "# CONFIG_BT_CTRL_DTM_ENABLE is not set")
        .replace("CONFIG_SPIRAM_USE_CAPS_ALLOC=n",
                 "CONFIG_SPIRAM_USE_CAPS_ALLOC=y")
    )
    assert module.verify_badge_uplink_canary_sdkconfig(sdkconfig) == []


def test_badge_uplink_verifier_rejects_non_ota0_partition_source_drift(
    tmp_path, monkeypatch
):
    module = _load_badge_build_verify_module()
    build_dir = tmp_path / "build"
    _write_badge_uplink_build_inputs(build_dir, ota1_offset=0x220000)
    module.materialize_badge_uplink_aliases(build_dir)

    partition_source = tmp_path / "partitions.csv"
    baseline_source = "\n".join((
        "ota_0,app,ota_0,0x20000,0x200000,",
        "ota_1,app,ota_1,0x220000,0x200000,",
    ))
    partition_source.write_text(baseline_source)
    generator = tmp_path / "gen_esp32part.py"
    generator.write_text(
        "import struct, sys\n"
        "from pathlib import Path\n"
        "source = Path(sys.argv[-2]).read_text()\n"
        "ota1 = 0x230000 if '0x230000' in source else 0x220000\n"
        "entry = lambda subtype, offset, label: struct.pack(\n"
        "    '<HBBII16sI', 0x50AA, 0, subtype, offset, 0x200000,\n"
        "    label.ljust(16, b'\\x00'), 0)\n"
        "Path(sys.argv[-1]).write_bytes(\n"
        "    entry(0x10, 0x20000, b'ota_0') +\n"
        "    entry(0x11, ota1, b'ota_1'))\n"
    )
    monkeypatch.setenv("ESP_IDF_PARTITION_GENERATOR", str(generator))

    assert module.verify_badge_uplink_build(
        build_dir, partition_source
    ) == []

    partition_source.write_text(
        baseline_source.replace("0x220000", "0x230000")
    )
    errors = module.verify_badge_uplink_build(build_dir, partition_source)

    assert any(
        "partition source does not reproduce partitions.bin" in error
        for error in errors
    ), errors


def test_badge_uplink_partition_generator_unavailable_fails_closed(
    tmp_path, monkeypatch
):
    module = _load_badge_build_verify_module()
    _write_partition_table(tmp_path / "partitions.bin")
    partition_source = tmp_path / "partitions.csv"
    partition_source.write_text(
        "ota_0,app,ota_0,0x20000,0x200000,\n"
    )
    monkeypatch.setenv(
        "ESP_IDF_PARTITION_GENERATOR",
        str(tmp_path / "missing-gen_esp32part.py"),
    )

    errors = module.verify_badge_uplink_build(tmp_path, partition_source)

    assert any(
        "ESP-IDF partition generator unavailable" in error
        for error in errors
    ), errors


def test_badge_uplink_post_build_verifier_is_strict_about_all_four_manifests(tmp_path):
    module = _load_badge_build_verify_module()
    (tmp_path / "bootloader.bin").write_bytes(b"bootloader")
    _write_partition_table(tmp_path / "partitions.bin", ota0_offset=0x10000)
    (tmp_path / "firmware.bin").write_bytes(b"application")

    errors = module.verify_badge_uplink_build(tmp_path)

    for manifest in (
        "flash_args", "flash_app_args", "flash_project_args", "flasher_args.json"
    ):
        assert any(manifest in error for error in errors), errors
    assert any("ota_0" in error and "0x20000" in error for error in errors)


def test_badge_uplink_strict_verifier_runs_as_post_build_action_and_in_ci():
    ini = (REPO_ROOT / "esp32" / "uplink" / "platformio.ini").read_text()
    workflow = (
        REPO_ROOT / ".github" / "workflows" / "esp32-web-flasher.yml"
    ).read_text()
    hook = (
        REPO_ROOT / "esp32" / "scripts" / "pio_verify_badge_uplink_build.py"
    ).read_text()

    assert "post:../scripts/pio_verify_badge_uplink_build.py" in ini
    assert "AddPostAction" in hook
    assert "verify_badge_uplink_canary_sdkconfig" in hook
    assert "verify_badge_uplink_build.py" in workflow
    assert "--build-dir esp32/uplink/.pio/build/uplink-s3-fof_badge" in workflow


def test_badge_uplink_post_build_rtc_gate_attests_frozen_elf_and_bin():
    hook = (
        REPO_ROOT / "esp32" / "scripts" / "pio_verify_badge_uplink_build.py"
    ).read_text()

    assert "UPLINK_PRODUCTION_RTC_NOINIT_BYTES" in hook
    assert "UPLINK_CANARY_RTC_NOINIT_BYTES" in hook
    assert "verify_frozen_badge_uplink_attestation(" in hook
    assert "unsafe badge uplink RTC ABI" in hook
    assert hook.index("frozen = snapshot.freeze_for_mutation()") < hook.index(
        "verify_frozen_badge_uplink_attestation("
    )
    assert "_verify_uplink_rtc_final_elf(" not in hook


def test_badge_uplink_text_manifest_rejects_mapping_hidden_on_option_line(tmp_path):
    module = _load_badge_build_verify_module()
    manifest = tmp_path / "flash_args"
    manifest.write_text(
        "--flash_mode dio --flash_freq 80m --flash_size 8MB "
        "0x10000 bad.bin\n"
    )

    entries, errors = module._parse_text_manifest(manifest)

    assert entries.get(0x10000) == "bad.bin" or any(
        "0x10000" in error or "unexpected" in error for error in errors
    )


def test_badge_uplink_json_manifest_rejects_duplicate_decoded_offsets(tmp_path):
    module = _load_badge_build_verify_module()
    manifest = tmp_path / "flasher_args.json"
    manifest.write_text(
        '{"flash_files":{"0":"unexpected.bin",'
        '"0x0":"bootloader/bootloader.bin"},'
        '"app":{"offset":"0x20000","file":"fof_badge_uplink.bin"}}'
    )

    _entries, errors = module._parse_json_manifest(manifest)

    assert any("duplicate decoded offset 0x0" in error for error in errors)


def test_badge_uplink_json_manifest_rejects_duplicate_raw_keys(tmp_path):
    module = _load_badge_build_verify_module()
    manifest = tmp_path / "flasher_args.json"
    manifest.write_text(
        '{"flash_files":{"0x0":"unexpected.bin",'
        '"0x0":"bootloader/bootloader.bin"},'
        '"app":{"offset":"0x20000","file":"fof_badge_uplink.bin"}}'
    )

    _entries, errors = module._parse_json_manifest(manifest)

    assert any("duplicate JSON key '0x0'" in error for error in errors)


def _write_canary_linker_map(
    path: Path,
    *,
    data_bytes: int,
    bss_bytes: int,
    markers: tuple[str, ...],
) -> None:
    path.write_text("\n".join((
        *markers,
        f".dram0.data     0x3fca2000     0x{data_bytes:x}",
        f".dram0.bss      0x3fca8000     0x{bss_bytes:x}",
        "",
    )))


def _write_elf32_symbols(
    path: Path,
    *,
    symbols: tuple[str, ...] = (),
    source_files: tuple[str, ...] = (),
    rtc_section_size: int = 0,
    rtc_section_count: int = 1,
    rtc_section_type: int = 8,
    rtc_section_flags: int = 0x3,
    rtc_section_alignment: int = 4,
    rtc_section_address: int = 0x50000000,
    rtc_objects: tuple[tuple[str, int, int], ...] = (),
    rtc_aliases: tuple[tuple[str, int], ...] = (),
    raw_symbols: tuple[
        tuple[str, int, int, int, int, int], ...
    ] = (),
) -> None:
    """Write a minimal ELF32 file with a real symbol table."""
    symbol_names = (
        *source_files,
        *symbols,
        *(name for name, _offset, _size in rtc_objects),
        *(name for name, _offset in rtc_aliases),
        *(name for name, _value, _size, _info, _other, _shndx
          in raw_symbols),
    )
    string_table = bytearray(b"\x00")
    string_offsets: dict[str, int] = {}
    for name in symbol_names:
        string_offsets[name] = len(string_table)
        string_table.extend(name.encode("ascii"))
        string_table.append(0)

    symbol_table = bytearray(b"\x00" * 16)
    for name in source_files:
        symbol_table.extend(struct.pack(
            "<IIIBBH",
            string_offsets[name],
            0,
            0,
            0x04,
            0,
            0xFFF1,
        ))
    rtc_section_index = 2 if rtc_section_size and rtc_section_count else 0
    rtc_base = rtc_section_address
    for name, offset, size in rtc_objects:
        symbol_table.extend(struct.pack(
            "<IIIBBH",
            string_offsets[name],
            rtc_base + offset,
            size,
            0x11,
            0,
            rtc_section_index,
        ))
    for name in symbols:
        symbol_table.extend(struct.pack(
            "<IIIBBH",
            string_offsets[name],
            0x1000,
            4,
            0x12,
            0,
            1,
        ))
    for name, offset in rtc_aliases:
        symbol_table.extend(struct.pack(
            "<IIIBBH",
            string_offsets[name],
            rtc_base + offset,
            0,
            0x10,
            0,
            rtc_section_index,
        ))
    for name, value, size, info, other, section_index in raw_symbols:
        symbol_table.extend(struct.pack(
            "<IIIBBH",
            string_offsets[name],
            value,
            size,
            info,
            other,
            section_index,
        ))

    section_name_list = [".text"]
    if rtc_section_size and rtc_section_count:
        section_name_list.append(".rtc_noinit")
    section_name_list.extend((".strtab", ".symtab", ".shstrtab"))
    section_names = bytearray(b"\x00")
    section_name_offsets: dict[str, int] = {}
    for name in section_name_list:
        section_name_offsets[name] = len(section_names)
        section_names.extend(name.encode("ascii"))
        section_names.append(0)

    payload = bytearray(b"\x00" * 52)

    def append_aligned(data: bytes, alignment: int = 4) -> tuple[int, int]:
        while len(payload) % alignment:
            payload.append(0)
        offset = len(payload)
        payload.extend(data)
        return offset, len(data)

    text_offset, text_size = append_aligned(b"\x00\x00\x00\x00")
    strtab_offset, strtab_size = append_aligned(bytes(string_table))
    symtab_offset, symtab_size = append_aligned(bytes(symbol_table))
    shstrtab_offset, shstrtab_size = append_aligned(bytes(section_names))
    while len(payload) % 4:
        payload.append(0)
    section_header_offset = len(payload)

    section_headers = [
        (0, 0, 0, 0, 0, 0, 0, 0, 0, 0),
        (
            section_name_offsets[".text"], 1, 0x6, 0x1000,
            text_offset, text_size, 0, 0, 4, 0,
        ),
    ]
    if rtc_section_size and rtc_section_count:
        for _index in range(rtc_section_count):
            section_headers.append((
                section_name_offsets[".rtc_noinit"],
                rtc_section_type,
                rtc_section_flags,
                rtc_base,
                0,
                rtc_section_size,
                0,
                0,
                rtc_section_alignment,
                0,
            ))
    strtab_section_index = len(section_headers)
    section_headers.append((
            section_name_offsets[".strtab"], 3, 0, 0,
            strtab_offset, strtab_size, 0, 0, 1, 0,
        ))
    symtab_section_index = len(section_headers)
    section_headers.append((
            section_name_offsets[".symtab"], 2, 0, 0,
            symtab_offset, symtab_size, strtab_section_index,
            1 + len(source_files), 4, 16,
        ))
    shstrtab_section_index = len(section_headers)
    section_headers.append((
            section_name_offsets[".shstrtab"], 3, 0, 0,
            shstrtab_offset, shstrtab_size, 0, 0, 1, 0,
        ))
    for section_header in section_headers:
        payload.extend(struct.pack("<IIIIIIIIII", *section_header))

    payload[:16] = (
        b"\x7fELF"
        b"\x01\x01\x01\x00"
        b"\x00\x00\x00\x00\x00\x00\x00\x00"
    )
    struct.pack_into(
        "<HHIIIIIHHHHHH",
        payload,
        16,
        2,
        94,
        1,
        0x1000,
        0,
        section_header_offset,
        0,
        52,
        0,
        0,
        40,
        len(section_headers),
        shstrtab_section_index,
    )
    path.write_bytes(payload)


def _valid_uplink_rtc_elf_kwargs(
    rtc_size: int,
) -> dict[str, object]:
    return {
        "rtc_section_size": rtc_size,
        "rtc_objects": (("g_fof_badge_rtc_state", 0, rtc_size),),
        "rtc_aliases": (
            ("fof_badge_rtc_usb_recovery_once_magic", 0),
            ("fof_badge_rtc_expected_reboot_generation", 4),
            ("fof_badge_rtc_expected_reboot_magic", 8),
        ),
    }


def _valid_uplink_rtc_raw_symbols(
    rtc_size: int,
) -> tuple[tuple[str, int, int, int, int, int], ...]:
    return (
        ("g_fof_badge_rtc_state", 0x50000000, rtc_size, 0x11, 0, 2),
        (
            "fof_badge_rtc_usb_recovery_once_magic",
            0x50000000,
            0,
            0x10,
            0,
            2,
        ),
        (
            "fof_badge_rtc_expected_reboot_generation",
            0x50000004,
            0,
            0x10,
            0,
            2,
        ),
        (
            "fof_badge_rtc_expected_reboot_magic",
            0x50000008,
            0,
            0x10,
            0,
            2,
        ),
    )


def _write_uplink_rtc_elf(
    path: Path,
    *,
    rtc_size: int,
    canary: bool,
    **rtc_overrides,
) -> None:
    kwargs: dict[str, object] = _valid_uplink_rtc_elf_kwargs(rtc_size)
    kwargs.update(rtc_overrides)
    _write_elf32_symbols(
        path,
        source_files=(("badge_con_vhci.c",) if canary else ()),
        symbols=(
            (
                "badge_con_radio_runtime_poll",
                "badge_con_vhci_init",
                "esp_vhci_host_send_packet",
                "esp_vhci_host_register_callback",
            )
            if canary else ()
        ),
        **kwargs,
    )


def _uplink_rtc_acceptance_errors(
    tmp_path: Path,
    *,
    target: str,
    rtc_overrides: dict[str, object],
) -> list[str]:
    production, canary, sdkconfig = _write_uplink_acceptance_fixture(
        tmp_path
    )
    is_canary = target == "canary"
    rtc_size = 200 if is_canary else 20
    _write_uplink_rtc_elf(
        canary / "firmware.elf" if is_canary
        else production / "firmware.elf",
        rtc_size=rtc_size,
        canary=is_canary,
        **rtc_overrides,
    )
    module = _load_badge_build_verify_module()
    return module.verify_badge_uplink_canary_acceptance(
        canary, sdkconfig, production
    )


def _write_production_build_evidence(
    build_dir: Path,
    *,
    project: str,
    version: str,
    runtime_target: str,
    hardware: str,
    sdkconfig_defaults: str,
    rtc_size: int = 0,
) -> None:
    (build_dir / "firmware.bin").write_bytes(_esp_image(
        project,
        version,
        runtime_target,
        hardware,
    ))
    (build_dir / "CMakeCache.txt").write_text(
        f"CMAKE_PROJECT_NAME:STATIC={project}\n"
        f"SDKCONFIG_DEFAULTS:UNINITIALIZED={sdkconfig_defaults}\n"
    )
    _write_elf32_symbols(
        build_dir / "firmware.elf",
        **(_valid_uplink_rtc_elf_kwargs(rtc_size) if rtc_size else {}),
    )


def _write_uplink_canary_sdkconfig(path: Path) -> None:
    path.write_text(
        "CONFIG_BT_ENABLED=y\n"
        "CONFIG_BT_CTRL_RUN_IN_FLASH_ONLY=y\n"
        "CONFIG_BT_CONTROLLER_ONLY=y\n"
        "CONFIG_BT_CONTROLLER_ENABLED=y\n"
        "CONFIG_BT_CTRL_HCI_MODE_VHCI=y\n"
        "CONFIG_BT_CTRL_BLE_MAX_ACT=1\n"
        "# CONFIG_BT_CTRL_DTM_ENABLE is not set\n"
        "CONFIG_BT_CTRL_BLE_ADV=y\n"
        "# CONFIG_BT_CTRL_BLE_SCAN is not set\n"
        "# CONFIG_BT_CTRL_BLE_MASTER is not set\n"
        "# CONFIG_BT_CTRL_BLE_SECURITY_ENABLE is not set\n"
        "# CONFIG_BT_BLUEDROID_ENABLED is not set\n"
        "# CONFIG_BT_NIMBLE_ENABLED is not set\n"
        "CONFIG_SPIRAM=y\n"
        "CONFIG_SPIRAM_USE_CAPS_ALLOC=y\n"
        "# CONFIG_SPIRAM_USE_MALLOC is not set\n"
    )


def _write_scanner_canary_sdkconfig(path: Path) -> None:
    path.write_text(
        "CONFIG_PARTITION_TABLE_CUSTOM=y\n"
        'CONFIG_PARTITION_TABLE_CUSTOM_FILENAME='
        '"partitions_s3_scanner_8mb.csv"\n'
        "CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y\n"
        "CONFIG_BT_ENABLED=y\n"
        "CONFIG_BT_NIMBLE_ENABLED=y\n"
        "# CONFIG_BT_CONTROLLER_ONLY is not set\n"
        "CONFIG_BT_CTRL_BLE_SCAN=y\n"
        "CONFIG_ESP_WIFI_ENABLED=y\n"
        "CONFIG_SPIRAM=y\n"
        "CONFIG_SPIRAM_USE_CAPS_ALLOC=y\n"
        "# CONFIG_SPIRAM_USE_MALLOC is not set\n"
    )


def _write_uplink_acceptance_fixture(
    tmp_path: Path,
) -> tuple[Path, Path, Path]:
    production = tmp_path / "uplink-s3-fof_badge"
    canary = tmp_path / "uplink-s3-fof_badge-con-crud-canary"
    production.mkdir()
    canary.mkdir()
    _write_production_build_evidence(
        production,
        project="fof_badge_uplink",
        version="0.64.78-badge-defcon34",
        runtime_target="uplink-s3-fof_badge",
        hardware="seeed_xiao_esp32s3",
        sdkconfig_defaults="sdkconfig.esp32s3-fof_badge.defaults",
        rtc_size=20,
    )
    (canary / "firmware.bin").write_bytes(b"canary-uplink")
    _write_elf32_symbols(
        canary / "firmware.elf",
        source_files=("badge_con_vhci.c",),
        symbols=(
            "badge_con_radio_runtime_poll",
            "badge_con_vhci_init",
            "esp_vhci_host_send_packet",
            "esp_vhci_host_register_callback",
        ),
        **_valid_uplink_rtc_elf_kwargs(200),
    )
    sdkconfig = tmp_path / "sdkconfig.uplink-canary"
    _write_uplink_canary_sdkconfig(sdkconfig)
    _write_canary_linker_map(
        canary / "fof_badge_uplink.map",
        data_bytes=23_448,
        bss_bytes=185_488,
        markers=(
            "main/game/badge_con_vhci.c.o",
            "badge_con_radio_runtime",
            "esp_vhci_host_send_packet",
            "esp_vhci_host_register_callback",
        ),
    )
    return production, canary, sdkconfig


def _write_scanner_acceptance_fixture(
    tmp_path: Path,
) -> tuple[Path, Path, Path]:
    production = tmp_path / "scanner-s3-combo-fof_badge"
    canary = tmp_path / "scanner-s3-combo-fof_badge-con-crud-canary"
    production.mkdir()
    canary.mkdir()
    _write_production_build_evidence(
        production,
        project="fof_badge_scanner",
        version="0.64.78-badge-defcon34",
        runtime_target="scanner-s3-combo-fof_badge",
        hardware="seeed_xiao_esp32s3",
        sdkconfig_defaults="sdkconfig.scanner-s3-fof_badge.defaults",
    )
    (canary / "firmware.bin").write_bytes(b"canary-scanner")
    _write_elf32_symbols(
        canary / "firmware.elf",
        source_files=(
            "badge_con_observer.c",
            "ble_remote_id.c",
            "esp_nimble_hci.c",
        ),
        symbols=(
            "badge_con_observer_init",
            "ble_remote_id_init",
            "esp_wifi_set_promiscuous",
            "esp_nimble_hci_init",
            "nimble_port_init",
            "ble_gap_ext_disc",
        ),
    )
    sdkconfig = tmp_path / "sdkconfig.scanner-canary"
    _write_scanner_canary_sdkconfig(sdkconfig)
    _write_canary_linker_map(
        canary / "fof_badge_scanner.map",
        data_bytes=23_012,
        bss_bytes=135_592,
        markers=(
            "main/detection/badge_con_observer.c.o",
            "ble_remote_id.c.o",
            "esp_wifi_set_promiscuous",
            "esp_nimble_hci",
            "nimble_port_init",
            "ble_gap_ext_disc",
        ),
    )
    return production, canary, sdkconfig


def test_badge_uplink_canary_acceptance_enforces_radio_memory_and_isolation(
    tmp_path,
):
    module = _load_badge_build_verify_module()
    production, canary, sdkconfig = _write_uplink_acceptance_fixture(
        tmp_path
    )
    linker_map = canary / "fof_badge_uplink.map"

    assert module.verify_badge_uplink_canary_acceptance(
        canary, sdkconfig, production
    ) == []

    _write_canary_linker_map(
        linker_map,
        data_bytes=23_448,
        bss_bytes=module.UPLINK_CANARY_MAX_INTERNAL_RAM_BYTES,
        markers=(),
    )
    _write_elf32_symbols(
        canary / "firmware.elf",
        source_files=("badge_con_vhci.c",),
        symbols=(
            "badge_con_radio_runtime_poll",
            "badge_con_vhci_init",
            "esp_vhci_host_send_packet",
            "esp_vhci_host_register_callback",
            "nimble_port_init",
        ),
    )
    errors = module.verify_badge_uplink_canary_acceptance(
        canary, sdkconfig, production
    )

    assert any("internal RAM" in error and "budget" in error for error in errors)
    assert any(
        "forbidden linked symbol" in error
        and "nimble_port_init" in error
        for error in errors
    )

    _write_canary_linker_map(
        linker_map,
        data_bytes=23_448,
        bss_bytes=185_488,
        markers=(),
    )
    _write_elf32_symbols(
        canary / "firmware.elf",
        source_files=("badge_con_vhci.c",),
        symbols=(
            "badge_con_radio_runtime_poll",
            "badge_con_vhci_init",
            "esp_vhci_host_send_packet",
            "esp_vhci_host_register_callback",
        ),
    )
    (canary / "firmware.bin").write_bytes(
        b"x" * (module.UPLINK_CANARY_MAX_APP_BYTES + 1)
    )
    errors = module.verify_badge_uplink_canary_acceptance(
        canary, sdkconfig, production
    )
    assert any("image size" in error and "budget" in error for error in errors)

    (canary / "firmware.bin").write_bytes(
        (production / "firmware.bin").read_bytes()
    )
    errors = module.verify_badge_uplink_canary_acceptance(
        canary, sdkconfig, production
    )
    assert any("must differ from production" in error for error in errors)

    (canary / "firmware.bin").unlink()
    (canary / "firmware.bin").symlink_to(
        production / "firmware.bin"
    )
    errors = module.verify_badge_uplink_canary_acceptance(
        canary, sdkconfig, production
    )
    assert any(
        "canary firmware" in error and "non-symlink" in error
        for error in errors
    )


def test_badge_uplink_canary_acceptance_allows_only_one_aligned_budget_block():
    module = _load_badge_build_verify_module()

    assert module.UPLINK_CANARY_MAX_APP_BYTES == 1_468_464


def test_badge_uplink_rtc_source_has_one_fixed_owner_and_no_side_blocks():
    main_dir = REPO_ROOT / "esp32" / "uplink" / "main"
    occurrences: list[tuple[Path, int]] = []
    for source in sorted(main_dir.rglob("*.c")):
        count = source.read_text(encoding="utf-8").count("RTC_NOINIT_ATTR")
        if count:
            occurrences.append((source.relative_to(main_dir), count))

    assert occurrences == [(Path("core/badge_runtime.c"), 1)]
    runtime = (main_dir / "core" / "badge_runtime.c").read_text(
        encoding="utf-8"
    )
    assert (
        "RTC_NOINIT_ATTR badge_runtime_rtc_state_t "
        "g_fof_badge_rtc_state"
    ) in runtime
    assert "s_badge_con_rtc_record" not in (
        main_dir / "core" / "badge_con_runtime.c"
    ).read_text(encoding="utf-8")


def test_badge_uplink_canary_acceptance_rejects_shifted_or_split_rtc_layout(
    tmp_path,
):
    module = _load_badge_build_verify_module()
    production, canary, sdkconfig = _write_uplink_acceptance_fixture(
        tmp_path
    )
    _write_elf32_symbols(
        canary / "firmware.elf",
        source_files=("badge_con_vhci.c",),
        symbols=(
            "badge_con_radio_runtime_poll",
            "badge_con_vhci_init",
            "esp_vhci_host_send_packet",
            "esp_vhci_host_register_callback",
        ),
        rtc_section_size=200,
        rtc_objects=(
            ("g_fof_badge_rtc_state", 0, 180),
            ("s_badge_con_rtc_record", 180, 20),
        ),
        rtc_aliases=(
            ("fof_badge_rtc_usb_recovery_once_magic", 0),
            ("fof_badge_rtc_expected_reboot_generation", 8),
            ("fof_badge_rtc_expected_reboot_magic", 4),
        ),
    )

    errors = module.verify_badge_uplink_canary_acceptance(
        canary, sdkconfig, production
    )

    assert any(
        "canary firmware ELF" in error
        and "RTC" in error
        and (
            "exactly one" in error
            or "expected_reboot_generation" in error
        )
        for error in errors
    ), errors


def test_badge_uplink_canary_acceptance_rejects_shifted_production_rtc_layout(
    tmp_path,
):
    module = _load_badge_build_verify_module()
    production, canary, sdkconfig = _write_uplink_acceptance_fixture(
        tmp_path
    )
    _write_elf32_symbols(
        production / "firmware.elf",
        rtc_section_size=20,
        rtc_objects=(("g_fof_badge_rtc_state", 0, 20),),
        rtc_aliases=(
            ("fof_badge_rtc_usb_recovery_once_magic", 0),
            ("fof_badge_rtc_expected_reboot_generation", 8),
            ("fof_badge_rtc_expected_reboot_magic", 4),
        ),
    )

    errors = module.verify_badge_uplink_canary_acceptance(
        canary, sdkconfig, production
    )

    assert any(
        "production firmware ELF" in error
        and "RTC" in error
        and "expected_reboot_generation" in error
        for error in errors
    ), errors


@pytest.mark.parametrize(
    ("target", "rtc_overrides", "expected_fragment"),
    (
        ("production", {"rtc_section_count": 0}, "exactly one"),
        ("canary", {"rtc_section_count": 2}, "exactly one"),
        ("canary", {"rtc_section_type": 1}, "SHT_NOBITS"),
        ("canary", {"rtc_section_flags": 0x1}, "allocatable"),
        ("canary", {"rtc_section_flags": 0x2}, "writable"),
        ("canary", {"rtc_section_flags": 0x7}, "flags"),
        ("canary", {"rtc_section_alignment": 1}, "alignment"),
        ("canary", {"rtc_section_address": 0x50000002}, "base address"),
        ("production", {"rtc_section_size": 19}, "20 bytes"),
        ("canary", {"rtc_section_size": 199}, "200 bytes"),
    ),
)
def test_badge_uplink_acceptance_rejects_invalid_rtc_section_contract(
    tmp_path,
    target,
    rtc_overrides,
    expected_fragment,
):
    errors = _uplink_rtc_acceptance_errors(
        tmp_path,
        target=target,
        rtc_overrides=rtc_overrides,
    )

    label = f"{target} firmware ELF"
    assert any(
        label in error
        and "RTC" in error
        and expected_fragment in error
        for error in errors
    ), errors


@pytest.mark.parametrize(
    ("mutation", "expected_fragment"),
    (
        ("missing", "exactly one"),
        ("duplicate", "exactly one"),
        ("anonymous_extra", "exactly one"),
        ("local", "GLOBAL OBJECT"),
        ("notype", "GLOBAL OBJECT"),
        ("absolute", "same section"),
        ("undefined", "same section"),
        ("wrong_section", "same section"),
        ("wrong_address", "section base"),
        ("wrong_size", "200 bytes"),
        ("hidden", "default visibility"),
    ),
)
def test_badge_uplink_acceptance_rejects_invalid_unified_rtc_object(
    tmp_path,
    mutation,
    expected_fragment,
):
    entries = list(_valid_uplink_rtc_raw_symbols(200))
    if mutation == "missing":
        entries.pop(0)
    elif mutation == "duplicate":
        entries.insert(1, entries[0])
    elif mutation == "anonymous_extra":
        entries.append(("", 0x50000000, 4, 0x11, 0, 2))
    elif mutation == "local":
        entries[0] = (
            "g_fof_badge_rtc_state",
            0x50000000,
            200,
            0x01,
            0,
            2,
        )
    elif mutation == "notype":
        entries[0] = (
            "g_fof_badge_rtc_state",
            0x50000000,
            200,
            0x10,
            0,
            2,
        )
    elif mutation == "absolute":
        entries[0] = (
            "g_fof_badge_rtc_state",
            0x50000000,
            200,
            0x11,
            0,
            0xFFF1,
        )
    elif mutation == "undefined":
        entries[0] = (
            "g_fof_badge_rtc_state",
            0,
            200,
            0x11,
            0,
            0,
        )
    elif mutation == "wrong_section":
        entries[0] = (
            "g_fof_badge_rtc_state",
            0x1000,
            200,
            0x11,
            0,
            1,
        )
    elif mutation == "wrong_address":
        entries[0] = (
            "g_fof_badge_rtc_state",
            0x50000004,
            200,
            0x11,
            0,
            2,
        )
    elif mutation == "wrong_size":
        entries[0] = (
            "g_fof_badge_rtc_state",
            0x50000000,
            196,
            0x11,
            0,
            2,
        )
    elif mutation == "hidden":
        entries[0] = (
            "g_fof_badge_rtc_state",
            0x50000000,
            200,
            0x11,
            2,
            2,
        )
    else:
        raise AssertionError(f"unknown mutation {mutation}")

    errors = _uplink_rtc_acceptance_errors(
        tmp_path,
        target="canary",
        rtc_overrides={
            "rtc_objects": (),
            "rtc_aliases": (),
            "raw_symbols": tuple(entries),
        },
    )

    assert any(
        "canary firmware ELF" in error
        and "RTC" in error
        and "g_fof_badge_rtc_state" in error
        and expected_fragment in error
        for error in errors
    ), errors


@pytest.mark.parametrize("target", ("production", "canary"))
@pytest.mark.parametrize(
    ("mutation", "expected_fragment"),
    (
        ("missing", "exactly one"),
        ("duplicate", "exactly one"),
        ("local", "GLOBAL NOTYPE"),
        ("object_type", "GLOBAL NOTYPE"),
        ("absolute", "same section"),
        ("undefined", "same section"),
        ("wrong_section", "same section"),
        ("wrong_address", "offset +0"),
        ("nonzero_size", "zero size"),
        ("hidden", "default visibility"),
    ),
)
def test_badge_uplink_acceptance_rejects_invalid_rtc_alias(
    tmp_path,
    target,
    mutation,
    expected_fragment,
):
    rtc_size = 200 if target == "canary" else 20
    entries = list(_valid_uplink_rtc_raw_symbols(rtc_size))
    alias = entries[1]
    if mutation == "missing":
        entries.pop(1)
    elif mutation == "duplicate":
        entries.insert(2, alias)
    elif mutation == "local":
        entries[1] = (*alias[:3], 0x00, alias[4], alias[5])
    elif mutation == "object_type":
        entries[1] = (*alias[:3], 0x11, alias[4], alias[5])
    elif mutation == "absolute":
        entries[1] = (*alias[:5], 0xFFF1)
    elif mutation == "undefined":
        entries[1] = (alias[0], 0, *alias[2:5], 0)
    elif mutation == "wrong_section":
        entries[1] = (alias[0], 0x1000, *alias[2:5], 1)
    elif mutation == "wrong_address":
        entries[1] = (alias[0], 0x50000004, *alias[2:])
    elif mutation == "nonzero_size":
        entries[1] = (alias[0], alias[1], 4, *alias[3:])
    elif mutation == "hidden":
        entries[1] = (*alias[:4], 2, alias[5])
    else:
        raise AssertionError(f"unknown mutation {mutation}")

    errors = _uplink_rtc_acceptance_errors(
        tmp_path,
        target=target,
        rtc_overrides={
            "rtc_objects": (),
            "rtc_aliases": (),
            "raw_symbols": tuple(entries),
        },
    )

    assert any(
        f"{target} firmware ELF" in error
        and "RTC" in error
        and alias[0] in error
        and expected_fragment in error
        for error in errors
    ), errors


@pytest.mark.parametrize(
    ("mutation", "expected_fragment"),
    (
        ("shstr_index", "section-name table index"),
        ("shstr_type", "section-name table"),
        ("shstr_bounds", "section-name string table"),
        ("rtc_name_bounds", "section name offset"),
    ),
)
def test_badge_uplink_acceptance_rejects_malformed_elf_section_names(
    tmp_path,
    mutation,
    expected_fragment,
):
    production, canary, sdkconfig = _write_uplink_acceptance_fixture(
        tmp_path
    )
    elf_path = canary / "firmware.elf"
    elf = bytearray(elf_path.read_bytes())
    section_offset = struct.unpack_from("<I", elf, 32)[0]
    section_entry_size = struct.unpack_from("<H", elf, 46)[0]
    section_count = struct.unpack_from("<H", elf, 48)[0]
    shstr_index = struct.unpack_from("<H", elf, 50)[0]
    shstr_header = section_offset + shstr_index * section_entry_size
    if mutation == "shstr_index":
        struct.pack_into("<H", elf, 50, section_count)
    elif mutation == "shstr_type":
        struct.pack_into("<I", elf, shstr_header + 4, 1)
    elif mutation == "shstr_bounds":
        struct.pack_into("<I", elf, shstr_header + 16, len(elf) + 1)
    elif mutation == "rtc_name_bounds":
        shstr_size = struct.unpack_from("<I", elf, shstr_header + 20)[0]
        rtc_header = section_offset + 2 * section_entry_size
        struct.pack_into("<I", elf, rtc_header, shstr_size)
    else:
        raise AssertionError(f"unknown mutation {mutation}")
    elf_path.write_bytes(elf)

    module = _load_badge_build_verify_module()
    errors = module.verify_badge_uplink_canary_acceptance(
        canary, sdkconfig, production
    )

    assert any(
        "canary firmware ELF" in error
        and expected_fragment in error
        for error in errors
    ), errors


def test_badge_uplink_canary_acceptance_rejects_canary_token_in_production(
    tmp_path,
):
    module = _load_badge_build_verify_module()
    production, canary, sdkconfig = _write_uplink_acceptance_fixture(
        tmp_path
    )
    with (production / "firmware.bin").open("ab") as firmware:
        firmware.write(b"\x00FoF-DC34-CONCRUD\x00")

    errors = module.verify_badge_uplink_canary_acceptance(
        canary, sdkconfig, production
    )

    assert any(
        "production firmware" in error and "FoF-DC34-CONCRUD" in error
        for error in errors
    )


def test_badge_uplink_canary_acceptance_rejects_canary_cmake_production(
    tmp_path,
):
    module = _load_badge_build_verify_module()
    production, canary, sdkconfig = _write_uplink_acceptance_fixture(
        tmp_path
    )
    with (production / "CMakeCache.txt").open("a") as cache:
        cache.write("FOF_DC34_GAME_CANARY:UNINITIALIZED=1\n")

    errors = module.verify_badge_uplink_canary_acceptance(
        canary, sdkconfig, production
    )

    assert any(
        "production CMake cache" in error
        and "FOF_DC34_GAME_CANARY" in error
        for error in errors
    )


def test_badge_uplink_canary_acceptance_rejects_unbound_production_cache(
    tmp_path,
):
    module = _load_badge_build_verify_module()
    production, canary, sdkconfig = _write_uplink_acceptance_fixture(
        tmp_path
    )
    (production / "CMakeCache.txt").write_text("# generated cache\n")

    errors = module.verify_badge_uplink_canary_acceptance(
        canary, sdkconfig, production
    )

    assert any(
        "production CMake cache" in error
        and "CMAKE_PROJECT_NAME" in error
        for error in errors
    )
    assert any(
        "production CMake cache" in error
        and "SDKCONFIG_DEFAULTS" in error
        for error in errors
    )


def test_badge_uplink_canary_acceptance_rejects_canary_track_production(
    tmp_path,
):
    module = _load_badge_build_verify_module()
    production, canary, sdkconfig = _write_uplink_acceptance_fixture(
        tmp_path
    )
    (production / "firmware.bin").write_bytes(_esp_image(
        "fof_badge_uplink",
        "0.64.90-badge-defcon34",
        "uplink-s3-fof_badge",
        "seeed_xiao_esp32s3",
    ))

    errors = module.verify_badge_uplink_canary_acceptance(
        canary, sdkconfig, production
    )

    assert any(
        "production firmware identity" in error
        and "0.64.78-badge-defcon34" in error
        for error in errors
    )


def test_badge_uplink_canary_acceptance_rejects_canary_production_elf_symbol(
    tmp_path,
):
    module = _load_badge_build_verify_module()
    production, canary, sdkconfig = _write_uplink_acceptance_fixture(
        tmp_path
    )
    _write_elf32_symbols(
        production / "firmware.elf",
        symbols=("badge_con_runtime_init",),
    )

    errors = module.verify_badge_uplink_canary_acceptance(
        canary, sdkconfig, production
    )

    assert any(
        "production firmware ELF" in error
        and "badge_con_runtime_init" in error
        for error in errors
    )


def test_badge_uplink_canary_acceptance_rejects_non_xtensa_production_elf(
    tmp_path,
):
    module = _load_badge_build_verify_module()
    production, canary, sdkconfig = _write_uplink_acceptance_fixture(
        tmp_path
    )
    elf = bytearray((production / "firmware.elf").read_bytes())
    struct.pack_into("<H", elf, 18, 62)
    (production / "firmware.elf").write_bytes(elf)

    errors = module.verify_badge_uplink_canary_acceptance(
        canary, sdkconfig, production
    )

    assert any(
        "production firmware ELF" in error
        and "Xtensa executable" in error
        for error in errors
    )


def test_badge_uplink_canary_acceptance_requires_final_linked_radio_evidence(
    tmp_path,
):
    module = _load_badge_build_verify_module()
    production, canary, sdkconfig = _write_uplink_acceptance_fixture(
        tmp_path
    )
    _write_elf32_symbols(canary / "firmware.elf")

    errors = module.verify_badge_uplink_canary_acceptance(
        canary, sdkconfig, production
    )

    assert any(
        "canary firmware ELF" in error
        and "badge_con_vhci.c" in error
        for error in errors
    )
    assert any(
        "canary firmware ELF" in error
        and "badge_con_vhci_init" in error
        for error in errors
    )


def test_badge_uplink_canary_acceptance_uses_final_elf_not_raw_map_markers(
    tmp_path,
):
    module = _load_badge_build_verify_module()
    production, canary, sdkconfig = _write_uplink_acceptance_fixture(
        tmp_path
    )
    _write_canary_linker_map(
        canary / "fof_badge_uplink.map",
        data_bytes=23_448,
        bss_bytes=185_488,
        markers=("nimble_port_init",),
    )

    assert module.verify_badge_uplink_canary_acceptance(
        canary, sdkconfig, production
    ) == []


def test_badge_uplink_canary_acceptance_rejects_forbidden_final_elf_radio(
    tmp_path,
):
    module = _load_badge_build_verify_module()
    production, canary, sdkconfig = _write_uplink_acceptance_fixture(
        tmp_path
    )
    _write_elf32_symbols(
        canary / "firmware.elf",
        source_files=("badge_con_vhci.c",),
        symbols=(
            "badge_con_radio_runtime_poll",
            "badge_con_vhci_init",
            "esp_vhci_host_send_packet",
            "esp_vhci_host_register_callback",
            "nimble_port_init",
        ),
    )

    errors = module.verify_badge_uplink_canary_acceptance(
        canary, sdkconfig, production
    )

    assert any(
        "canary firmware ELF" in error
        and "nimble_port_init" in error
        and "forbidden" in error
        for error in errors
    )


def test_badge_uplink_canary_acceptance_fails_closed_on_missing_evidence(
    tmp_path,
):
    module = _load_badge_build_verify_module()
    production = tmp_path / "uplink-s3-fof_badge"
    canary = tmp_path / "uplink-s3-fof_badge-con-crud-canary"
    production.mkdir()
    canary.mkdir()

    errors = module.verify_badge_uplink_canary_acceptance(
        canary,
        tmp_path / "missing-sdkconfig",
        production,
    )

    assert any("sdkconfig: cannot read" in error for error in errors)
    assert any("linker map" in error and "required" in error for error in errors)
    assert any(
        "canary firmware" in error and "regular" in error
        for error in errors
    )
    assert any(
        "production firmware" in error and "regular" in error
        for error in errors
    )


def test_badge_uplink_canary_cli_cannot_skip_acceptance_evidence(
    tmp_path, monkeypatch, capsys,
):
    module = _load_badge_build_verify_module()
    canary = tmp_path / "uplink-s3-fof_badge-con-crud-canary"
    _write_badge_uplink_build_inputs(canary)
    module.materialize_badge_uplink_aliases(canary)
    monkeypatch.setattr(sys, "argv", [
        "verify_badge_uplink_build.py",
        "--build-dir",
        str(canary),
    ])

    assert module.main() == 1
    output = capsys.readouterr().out
    assert "canary verification requires --sdkconfig" in output
    assert "--canary-production-build-dir" in output


def test_badge_scanner_canary_acceptance_enforces_radio_memory_and_isolation(
    tmp_path,
):
    module = _load_badge_scanner_build_verify_module()
    production, canary, sdkconfig = _write_scanner_acceptance_fixture(
        tmp_path
    )
    linker_map = canary / "fof_badge_scanner.map"

    assert module.verify_badge_scanner_canary_acceptance(
        canary, sdkconfig, production
    ) == []

    _write_canary_linker_map(
        linker_map,
        data_bytes=23_012,
        bss_bytes=module.SCANNER_CANARY_MAX_INTERNAL_RAM_BYTES,
        markers=(),
    )
    _write_elf32_symbols(
        canary / "firmware.elf",
        source_files=(
            "badge_con_observer.c",
            "ble_remote_id.c",
            "esp_nimble_hci.c",
            "badge_con_vhci.c",
        ),
        symbols=(
            "badge_con_observer_init",
            "ble_remote_id_init",
            "esp_wifi_set_promiscuous",
            "esp_nimble_hci_init",
            "nimble_port_init",
            "ble_gap_ext_disc",
            "badge_con_vhci_init",
        ),
    )
    errors = module.verify_badge_scanner_canary_acceptance(
        canary, sdkconfig, production
    )

    assert any("internal RAM" in error and "budget" in error for error in errors)
    assert any(
        "forbidden linked symbol" in error
        and "badge_con_vhci_init" in error
        for error in errors
    )

    _write_canary_linker_map(
        linker_map,
        data_bytes=23_012,
        bss_bytes=135_592,
        markers=(),
    )
    _write_elf32_symbols(
        canary / "firmware.elf",
        source_files=(
            "badge_con_observer.c",
            "ble_remote_id.c",
            "esp_nimble_hci.c",
        ),
        symbols=(
            "badge_con_observer_init",
            "ble_remote_id_init",
            "esp_wifi_set_promiscuous",
            "esp_nimble_hci_init",
            "nimble_port_init",
            "ble_gap_ext_disc",
        ),
    )
    (canary / "firmware.bin").write_bytes(
        b"x" * (module.SCANNER_CANARY_MAX_APP_BYTES + 1)
    )
    errors = module.verify_badge_scanner_canary_acceptance(
        canary, sdkconfig, production
    )
    assert any("image size" in error and "budget" in error for error in errors)

    (canary / "firmware.bin").write_bytes(
        (production / "firmware.bin").read_bytes()
    )
    errors = module.verify_badge_scanner_canary_acceptance(
        canary, sdkconfig, production
    )
    assert any("must differ from production" in error for error in errors)


def test_badge_scanner_canary_acceptance_requires_final_linked_radio_evidence(
    tmp_path,
):
    module = _load_badge_scanner_build_verify_module()
    production, canary, sdkconfig = _write_scanner_acceptance_fixture(
        tmp_path
    )
    _write_elf32_symbols(canary / "firmware.elf")

    errors = module.verify_badge_scanner_canary_acceptance(
        canary, sdkconfig, production
    )

    assert any(
        "canary firmware ELF" in error
        and "badge_con_observer.c" in error
        for error in errors
    )
    assert any(
        "canary firmware ELF" in error
        and "badge_con_observer_init" in error
        for error in errors
    )


def test_badge_scanner_canary_acceptance_uses_final_elf_not_raw_map_markers(
    tmp_path,
):
    module = _load_badge_scanner_build_verify_module()
    production, canary, sdkconfig = _write_scanner_acceptance_fixture(
        tmp_path
    )
    _write_canary_linker_map(
        canary / "fof_badge_scanner.map",
        data_bytes=23_012,
        bss_bytes=135_592,
        markers=("main/game/badge_con_vhci.c.o",),
    )

    assert module.verify_badge_scanner_canary_acceptance(
        canary, sdkconfig, production
    ) == []


def test_badge_scanner_canary_acceptance_rejects_forbidden_final_elf_radio(
    tmp_path,
):
    module = _load_badge_scanner_build_verify_module()
    production, canary, sdkconfig = _write_scanner_acceptance_fixture(
        tmp_path
    )
    _write_elf32_symbols(
        canary / "firmware.elf",
        source_files=(
            "badge_con_observer.c",
            "ble_remote_id.c",
            "esp_nimble_hci.c",
            "badge_con_vhci.c",
        ),
        symbols=(
            "badge_con_observer_init",
            "ble_remote_id_init",
            "esp_wifi_set_promiscuous",
            "esp_nimble_hci_init",
            "nimble_port_init",
            "ble_gap_ext_disc",
            "badge_con_vhci_init",
        ),
    )

    errors = module.verify_badge_scanner_canary_acceptance(
        canary, sdkconfig, production
    )

    assert any(
        "canary firmware ELF" in error
        and "badge_con_vhci_init" in error
        and "forbidden" in error
        for error in errors
    )
    assert any(
        "canary firmware ELF" in error
        and "badge_con_vhci.c" in error
        and "forbidden" in error
        for error in errors
    )


def test_badge_scanner_canary_cli_cannot_skip_acceptance_evidence(
    tmp_path, monkeypatch, capsys,
):
    module = _load_badge_scanner_build_verify_module()
    canary = tmp_path / "scanner-s3-combo-fof_badge-con-crud-canary"
    _write_badge_scanner_build_inputs(canary)
    module.materialize_badge_scanner_aliases(canary)
    monkeypatch.setattr(sys, "argv", [
        "verify_badge_scanner_build.py",
        "--build-dir",
        str(canary),
    ])

    assert module.main() == 1
    output = capsys.readouterr().out
    assert "canary verification requires --sdkconfig" in output
    assert "--canary-production-build-dir" in output


def test_badge_scanner_canary_sdkconfig_requires_observer_wifi_and_psram(
    tmp_path,
):
    module = _load_badge_scanner_build_verify_module()
    sdkconfig = tmp_path / "sdkconfig.scanner-canary"
    _write_scanner_canary_sdkconfig(sdkconfig)
    sdkconfig.write_text(
        sdkconfig.read_text()
        .replace("CONFIG_BT_CTRL_BLE_SCAN=y",
                 "# CONFIG_BT_CTRL_BLE_SCAN is not set")
        .replace("CONFIG_ESP_WIFI_ENABLED=y",
                 "# CONFIG_ESP_WIFI_ENABLED is not set")
        .replace("CONFIG_SPIRAM=y", "# CONFIG_SPIRAM is not set")
    )

    errors = module.verify_badge_scanner_canary_sdkconfig(sdkconfig)

    assert any("CONFIG_BT_CTRL_BLE_SCAN" in error for error in errors)
    assert any("CONFIG_ESP_WIFI_ENABLED" in error for error in errors)
    assert any("CONFIG_SPIRAM" in error for error in errors)


def _workflow_named_step(workflow: str, name: str) -> str:
    start = workflow.index(f"      - name: {name}")
    end = workflow.find("\n      - name:", start + 1)
    return workflow[start:] if end < 0 else workflow[start:end]


def test_esp32_workflow_builds_firmware_with_non_writable_group_mode():
    workflow = (
        REPO_ROOT / ".github" / "workflows" / "esp32-web-flasher.yml"
    ).read_text()

    for build_step in (
        "Build Scanner firmware (ESP32-S3 combo)",
        "Build Scanner firmware (ESP32-S3 combo seed)",
        "Build Uplink firmware (ESP32-S3)",
        "Build Badge Scanner firmware (XIAO ESP32-S3)",
        "Build private Badge Scanner canary",
        "Build Badge Uplink firmware (XIAO ESP32-S3)",
        "Build private Badge Uplink canary",
        "Build Badge factory topology probe (XIAO ESP32-S3)",
    ):
        step = _workflow_named_step(workflow, build_step)
        assert "umask 0022" in step, build_step
        assert step.index("umask 0022") < step.index("pio run"), build_step


def test_esp32_workflow_keeps_canary_artifacts_private_and_production_clean():
    workflow = (
        REPO_ROOT / ".github" / "workflows" / "esp32-web-flasher.yml"
    ).read_text()

    scanner_build = _workflow_named_step(
        workflow, "Build private Badge Scanner canary"
    )
    uplink_build = _workflow_named_step(
        workflow, "Build private Badge Uplink canary"
    )
    scanner_verify = _workflow_named_step(
        workflow, "Verify private Badge Scanner canary"
    )
    uplink_verify = _workflow_named_step(
        workflow, "Verify private Badge Uplink canary"
    )
    private_upload = _workflow_named_step(
        workflow, "Upload private CON CRUD canary artifact"
    )

    assert "scanner-s3-combo-fof_badge-con-crud-canary" in scanner_build
    assert "uplink-s3-fof_badge-con-crud-canary" in uplink_build
    assert "--canary-production-build-dir" in scanner_verify
    assert "--canary-production-build-dir" in uplink_verify
    assert "private-canary/" in private_upload
    assert "con-crud-canary-${{ github.sha }}" in private_upload

    for production_step in (
        "Package firmware binaries",
        "Upload firmware artifact",
        "Build validated badge factory bundle",
        "Attach firmware to release",
        "Assemble site",
        "Download firmware artifact",
        "Upload Pages artifact",
    ):
        assert "con-crud-canary" not in _workflow_named_step(
            workflow, production_step
        ), production_step


def test_con_crud_acceptance_ledger_records_local_factory_promotion():
    acceptance = (
        REPO_ROOT / "docs" / "badge" / "con-crud-canary-acceptance.md"
    ).read_text()

    assert "Overall acceptance: **PENDING**" in acceptance
    assert "Two-badge BLE propagation" in acceptance
    assert "Scanner receive-only parity" in acceptance
    assert "USB maintenance and OTA retry" in acceptance
    assert "PENDING" in acceptance
    assert "private-canary/" in acceptance
    for pending_row in (
        "Live canary normal-mode memory",
        "Exact .78 updater baseline comparison",
        "Three scanner update cycles",
        "Two-badge game behavior",
        "Dual-button reset",
        "RF/power/soak",
    ):
        assert f"| {pending_row} |" in acceptance
        row = next(
            line for line in acceptance.splitlines()
            if line.startswith(f"| {pending_row} |")
        )
        assert row.endswith("| PENDING |")
    for footer in (
        "LOCAL EMBEDDED FACTORY PROMOTION: APPROVED on 2026-07-29",
        "OVERALL FORMAL PHYSICAL MATRIX: PENDING",
        "FACTORY BUNDLE: local embedded bundle promoted to "
        "`0.67.2-badge-defcon34`",
        "PUBLIC GITHUB RELEASE: not created; assets unchanged",
    ):
        assert footer in acceptance
