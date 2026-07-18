import importlib.util
import json
import struct
import sys
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
MODULE_PATH = REPO_ROOT / "esp32" / "scripts" / "firmware_version.py"
VERIFY_MODULE_PATH = (
    REPO_ROOT / "esp32" / "scripts" / "verify_firmware_versions.py"
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
        "0.64.69-badge-defcon34",
    ),
    "uplink-s3": (
        "fof_uplink",
        "esp32-s3-devkitc-1",
        "0.64.68-live-follow",
    ),
    "uplink-s3-fof_badge": (
        "fof_badge_uplink",
        "seeed_xiao_esp32s3",
        "0.64.69-badge-defcon34",
    ),
}


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
        "0.64.69-badge-defcon34"
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
        json.dumps({"project_version": "0.64.69-badge-defcon34"})
    )

    changed = module.invalidate_stale_cmake_cache(
        build_dir,
        "0.64.69-badge-defcon34",
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
        _esp_image("fof_badge_uplink", "0.64.69-badge-defcon34")
    )

    assert info.project == "fof_badge_uplink"
    assert info.version == "0.64.69-badge-defcon34"


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
        "serial_config_start_control_task()",
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
    assert "badge_ble_control_init();" not in main
    assert 'strcmp(cmd, "badge_theme")' in serial
    assert 'strcmp(cmd, "badge_display_policy")' in serial


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
    serial_header = (
        REPO_ROOT / "esp32" / "uplink" / "main" / "core" / "serial_config.h"
    ).read_text()
    serial_source = (
        REPO_ROOT / "esp32" / "uplink" / "main" / "core" / "serial_config.c"
    ).read_text()

    assert "bool uart_rx_start(void);" in uart_header
    assert "bool uart_rx_start(void)" in uart_source
    assert "bool http_upload_start(void);" in upload_header
    assert "bool http_upload_start(void)" in upload_source
    assert "bool serial_config_start_control_task(void);" in serial_header
    assert "bool serial_config_start_control_task(void)" in serial_source
    assert "BaseType_t display_task_ok = xTaskCreate(" in main
    assert "if (display_task_ok != pdPASS)" in main

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
        "        if (!uart_rx_start())"
    )
    badge_worker_else = main.index("#else", badge_worker_start)
    badge_branch = main[badge_worker_start:badge_worker_else]
    standard_branch = main[
        badge_worker_else:main.index("#endif", badge_worker_else)
    ]
    assert "http_upload_start()" not in badge_branch
    assert "if (!http_upload_start())" in standard_branch
    assert "USB control and UART firmware relay remain active" in main


def test_usb_status_frame_holds_stdio_lock_until_newline_is_flushed():
    serial = (
        REPO_ROOT / "esp32" / "uplink" / "main" / "core" / "serial_config.c"
    ).read_text()
    start = serial.index("static void send_badge_status_response(void)")
    end = serial.index("static void send_control_ok", start)
    status = serial[start:end]

    lock = status.index("flockfile(stdout);")
    first_byte = status.index('printf("FOF_STATUS:')
    final_newline = status.rindex('printf("]}\\n");')
    flush = status.rindex("fflush(stdout);")
    unlock = status.rindex("funlockfile(stdout);")
    assert lock < first_byte < final_newline < flush < unlock
    assert status.count("flockfile(stdout);") == 1
    assert status.count("funlockfile(stdout);") == 1

    # Every known blocking or heap-locking snapshot happens before stdout is
    # held, preventing stdout -> subsystem-lock inversions with ESP_LOG tasks.
    for capture in (
        "oled_badge_get_display_state(&display_state)",
        "badge_ble_investigation_status_json(investigation_status",
        "badge_display_policy_runtime_json(policy_json",
        "badge_theme_runtime_json(theme_json",
        "heap_caps_get_free_size(MALLOC_CAP_INTERNAL)",
        "uart_rx_get_scanner_uart_diag(0, &ble_uart_diag)",
    ):
        assert status.index(capture) < lock

    locked_render = status[lock:unlock]
    for forbidden_getter in (
        "oled_badge_get_",
        "badge_ble_investigation_status_json(",
        "badge_display_policy_runtime_json(",
        "badge_theme_runtime_json(",
        "heap_caps_get_",
        "uart_rx_get_scanner_uart_diag(",
    ):
        assert forbidden_getter not in locked_render

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
        image.write_bytes(_esp_image(project, version, target, hardware))
        images[target] = image

    assert module.verify_firmware_images(header, images) == []

    badge = images["scanner-s3-combo-fof_badge"]
    badge.write_bytes(_esp_image(
        "fof_scanner",
        "0.64.69-badge-defcon34",
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
        "0.64.69-badge-defcon34",
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
