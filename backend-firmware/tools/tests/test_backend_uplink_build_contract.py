from configparser import ConfigParser
import csv
from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[2]
UPLINK = ROOT / "uplink"


def _load_environment() -> tuple[ConfigParser, str]:
    path = UPLINK / "platformio.ini"
    text = path.read_text(encoding="utf-8")
    parser = ConfigParser(interpolation=None)
    parser.read_string(text)
    assert parser.has_section("env:uplink-s3-backend")
    return parser, text


def _sdkconfig(filename: str = "sdkconfig.defaults") -> dict[str, str]:
    values: dict[str, str] = {}
    for raw_line in (UPLINK / filename).read_text(
        encoding="utf-8"
    ).splitlines():
        line = raw_line.strip()
        if line.startswith("# CONFIG_") and line.endswith(" is not set"):
            key = line[len("# ") : -len(" is not set")]
            assert key not in values, f"duplicate sdkconfig key: {key}"
            values[key] = "n"
            continue
        if not line or line.startswith("#"):
            continue
        key, separator, value = line.partition("=")
        assert separator, f"malformed sdkconfig line: {raw_line}"
        assert key not in values, f"duplicate sdkconfig key: {key}"
        values[key] = value
    return values


def _partition_rows(filename: str = "partitions_backend_uplink_8mb.csv") -> list[tuple[str, str, str, int, int]]:
    rows: list[tuple[str, str, str, int, int]] = []
    path = UPLINK / filename
    with path.open(encoding="utf-8", newline="") as handle:
        for row in csv.reader(handle):
            if not row or row[0].strip().startswith("#"):
                continue
            assert len(row) >= 5
            rows.append(
                (
                    row[0].strip(),
                    row[1].strip(),
                    row[2].strip(),
                    int(row[3].strip(), 0),
                    int(row[4].strip(), 0),
                )
            )
    return rows


def test_uplink_environment_drives_one_exact_backend_image() -> None:
    parser, text = _load_environment()
    environment = parser["env:uplink-s3-backend"]

    assert environment["platform"] == "espressif32@6.13.0"
    assert environment["board"] == "seeed_xiao_esp32s3"
    assert environment["framework"] == "espidf"
    assert environment.getint("monitor_speed") == 921600
    assert environment["board_upload.flash_size"] == "8MB"
    assert environment["board_upload.offset_address"] == "0x20000"
    assert environment["board_build.partitions"] == (
        "partitions_backend_uplink_8mb.csv"
    )
    assert environment["board_build.flash_mode"] == "dio"
    assert environment["board_build.f_flash"] == "80000000L"
    flags = set(environment["build_flags"].split())
    assert {"-DFOF_BACKEND_FIRMWARE=1", "-DFOF_BACKEND_UPLINK=1", "-DBOARD_HAS_PSRAM"} <= flags
    assert "-DFOF_BACKEND_PROFILE_BADGE_LITE=1" in flags
    assert parser.get("platformio", "default_envs") == "uplink-s3-backend"
    assert "FOF_BADGE_VARIANT" not in text
    assert "fof_badge" not in text.lower()
    for script in environment.get("extra_scripts", "").split():
        script_path = script.split(":", 1)[-1]
        assert not Path(script_path).is_absolute()
        resolved = (UPLINK / script_path).resolve()
        assert resolved.is_relative_to(ROOT)
        assert resolved.is_file()


def test_fullsize_uplink_environment_selects_its_16mb_generated_config_input() -> None:
    parser, _ = _load_environment()
    environment = parser["env:uplink-s3-fullsize-backend"]
    assert environment["board"] == "esp32-s3-devkitc-1"
    assert environment["board_upload.flash_size"] == "16MB"
    assert environment["board_build.partitions"] == (
        "partitions_backend_uplink_fullsize_16mb.csv"
    )
    assert environment["board_build.flash_mode"] == "qio"
    assert environment["board_build.psram_type"] == "opi"
    assert '-DSDKCONFIG_DEFAULTS="sdkconfig.fullsize.defaults"' in environment[
        "board_build.cmake_extra_args"
    ]

    config = _sdkconfig("sdkconfig.fullsize.defaults")
    expected = {
        "CONFIG_ESPTOOLPY_FLASHSIZE_16MB": "y",
        "CONFIG_ESPTOOLPY_FLASHSIZE": '"16MB"',
        "CONFIG_ESPTOOLPY_FLASHMODE_QIO": "y",
        # ESP-IDF flashes a QIO bootloader over DIO, then the bootloader
        # switches the application to the selected QIO mode.
        "CONFIG_ESPTOOLPY_FLASHMODE": '"dio"',
        "CONFIG_ESPTOOLPY_FLASHFREQ_80M": "y",
        "CONFIG_PARTITION_TABLE_CUSTOM_FILENAME": (
            '"partitions_backend_uplink_fullsize_16mb.csv"'
        ),
        "CONFIG_SPIRAM": "y",
        "CONFIG_SPIRAM_MODE_OCT": "y",
    }
    for key, value in expected.items():
        assert config.get(key) == value

def test_uplink_project_uses_backend_version_and_explicit_local_sources() -> None:
    top = (UPLINK / "CMakeLists.txt").read_text(encoding="utf-8")
    component = (UPLINK / "main/CMakeLists.txt").read_text(encoding="utf-8")

    assert "project(${FOF_BACKEND_PROJECT_NAME})" in top
    assert "FOF_VERSION_BACKEND" in top
    assert "backend_version.h" in top
    assert 'STRINGS "../shared/backend_version.h" PROJECT_VER' in top
    assert 'set(PROJECT_VER "' not in top
    forbidden_build_features = (
        "SRC_DIRS",
        "GLOB",
        "../../../",
        "vendor/",
    )
    assert not any(value in top + component for value in forbidden_build_features)

    source_tokens = re.findall(r'"([^"\n]+\.c)"', component)
    assert source_tokens
    assert source_tokens.count("main.c") == 1
    assert source_tokens.count("../../shared/backend_embedded_identity.c") == 1
    assert 'LDFRAGMENTS\n        "backend_identity.lf"' in component
    linker_fragment = (UPLINK / "main/backend_identity.lf").read_text(
        encoding="utf-8"
    )
    assert ".fof_backend_identity" in linker_fragment
    assert "fof_backend_identity -> flash_rodata KEEP()" in linker_fragment
    expected_uplink_sources = {
        "comms/backend_uart_slot.c",
        "core/backend_coordinator.c",
        "core/backend_health.c",
        "../../shared/backend_status_led.c",
        "network/backend_command_client.c",
        "network/backend_config_portal.c",
        "network/backend_http_transport.c",
        "network/backend_time_sync.c",
        "network/backend_uploader.c",
        "network/backend_wifi_manager.c",
        "ota/backend_ota_maintenance.c",
        "ota/backend_scanner_relay.c",
        "ota/backend_self_ota.c",
        "storage/backend_firmware_buffer.c",
        "storage/backend_firmware_store.c",
        "storage/backend_nvs_config.c",
        "storage/backend_ota_journal.c",
    }
    assert expected_uplink_sources <= set(source_tokens)
    for source in source_tokens:
        source_path = (UPLINK / "main" / source).resolve()
        assert source_path.is_relative_to(ROOT)
        assert source_path.is_file(), f"missing explicit source: {source}"


def test_lite_runtime_sources_are_explicit_and_excluded_from_fullsize() -> None:
    component = (UPLINK / "main/CMakeLists.txt").read_text(encoding="utf-8")
    lite_start = component.index(
        'if("${BACKEND_STATUS_PROJECT_NAME}" STREQUAL "fof_backend_uplink")'
    )
    fullsize_start = component.index(
        'elseif("${BACKEND_STATUS_PROJECT_NAME}" STREQUAL '
        '"fof_backend_uplink_fullsize")',
        lite_start,
    )
    profile_end = component.index("else()", fullsize_start)
    lite_branch = component[lite_start:fullsize_start]
    fullsize_branch = component[fullsize_start:profile_end]
    lite_sources = {
        "core/backend_dashboard_event.c",
        "network/backend_dashboard_page.c",
        "storage/backend_event_ring.c",
        "usb/backend_usb_config.c",
        "usb/backend_usb_protocol.c",
        "usb/backend_usb_service.c",
        "usb/backend_usb_transport_core.c",
        "../../shared/backend_lite_ap_policy.c",
    }

    assert "${BACKEND_STATUS_LED_SRCS}" in component
    assert "esp_driver_usb_serial_jtag" in lite_branch
    assert "esp_driver_usb_serial_jtag" not in fullsize_branch
    for source in lite_sources:
        assert f'"{source}"' in lite_branch
        assert source not in fullsize_branch
        assert component.count(f'"{source}"') == 1


def test_lite_usb_service_owns_bounded_driver_and_strict_psram_queues() -> None:
    service = (UPLINK / "main/usb/backend_usb_service.c").read_text(
        encoding="utf-8"
    )

    assert ".rx_buffer_size = 8192" in service
    assert ".tx_buffer_size = 8192" in service
    assert "BACKEND_USB_DRIVER_WRITE_MAX 4096U" in service
    assert "psram_alloc_strict(" in service
    assert "BACKEND_USB_REQUIRED_QUEUE_CAPACITY" in service
    assert "BACKEND_USB_OPTIONAL_QUEUE_CAPACITY" in service
    assert "usb_serial_jtag_write_bytes(" in service


def test_lite_main_registers_http_and_canonical_sinks_with_psram_history() -> None:
    source = (UPLINK / "main/main.c").read_text(encoding="utf-8")

    assert "backend_coordinator_set_upload_sink(" in source
    assert "backend_coordinator_set_canonical_sink(" in source
    assert "psram_alloc_strict(" in source
    assert "128U * sizeof(backend_dashboard_event_t)" in source

    lite_status_sources = source + (UPLINK / "main/usb/backend_usb_service.c").read_text(
        encoding="utf-8"
    )
    assert "fof_badge_uplink" not in lite_status_sources
    assert "uplink-s3-fof_badge" not in lite_status_sources


def test_uplink_partition_table_is_exact_nonoverlapping_eight_megabytes() -> None:
    rows = _partition_rows()
    expected = [
        ("nvs", "data", "nvs", 0x9000, 0x6000),
        ("otadata", "data", "ota", 0xF000, 0x2000),
        ("phy_init", "data", "phy", 0x11000, 0x1000),
        ("ota_0", "app", "ota_0", 0x20000, 0x200000),
        ("ota_1", "app", "ota_1", 0x220000, 0x200000),
        ("fw_scanner_be", "data", "0x40", 0x420000, 0x200000),
        ("storage", "data", "spiffs", 0x620000, 0x100000),
        ("reserved", "data", "fat", 0x720000, 0xE0000),
    ]
    assert rows == expected
    for previous, current in zip(rows, rows[1:]):
        assert previous[3] + previous[4] <= current[3]
    assert rows[-1][3] + rows[-1][4] == 0x800000


def test_fullsize_uplink_partition_has_exact_3mb_scanner_cache() -> None:
    rows = _partition_rows("partitions_backend_uplink_fullsize_16mb.csv")
    assert rows == [
        ("nvs", "data", "nvs", 0x9000, 0x6000),
        ("otadata", "data", "ota", 0xF000, 0x2000),
        ("phy_init", "data", "phy", 0x11000, 0x1000),
        ("ota_0", "app", "ota_0", 0x20000, 0x200000),
        ("ota_1", "app", "ota_1", 0x220000, 0x200000),
        ("fw_scanner_be", "data", "0x40", 0x420000, 0x300000),
        ("storage", "data", "spiffs", 0x720000, 0x100000),
        ("reserved", "data", "fat", 0x820000, 0x7E0000),
    ]
    for previous, current in zip(rows, rows[1:]):
        assert previous[3] + previous[4] <= current[3]
    assert rows[-1][3] + rows[-1][4] == 0x1000000


def test_uplink_sdkconfig_enables_rollback_psram_usb_wifi_and_no_bluetooth() -> None:
    config = _sdkconfig()
    expected = {
        "CONFIG_ESPTOOLPY_FLASHSIZE_8MB": "y",
        "CONFIG_ESPTOOLPY_FLASHSIZE": '"8MB"',
        "CONFIG_PARTITION_TABLE_CUSTOM": "y",
        "CONFIG_PARTITION_TABLE_CUSTOM_FILENAME": (
            '"partitions_backend_uplink_8mb.csv"'
        ),
        "CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE": "y",
        "CONFIG_SPIRAM": "y",
        "CONFIG_SPIRAM_MODE_OCT": "y",
        "CONFIG_SPIRAM_USE_MALLOC": "y",
        "CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG": "y",
        "CONFIG_FREERTOS_HZ": "1000",
        "CONFIG_ESP_TASK_WDT_TIMEOUT_S": "30",
        "CONFIG_ESP_WIFI_ENABLED": "y",
        "CONFIG_BT_ENABLED": "n",
        "CONFIG_BT_BLUEDROID_ENABLED": "n",
        "CONFIG_BT_NIMBLE_ENABLED": "n",
    }
    for key, value in expected.items():
        assert config.get(key) == value

def test_uplink_main_has_no_badge_presentation_location_or_bluetooth_runtime() -> None:
    source = (UPLINK / "main/main.c").read_text(encoding="utf-8").lower()
    forbidden = (
        "fof_badge",
        "badge_con",
        "display_st7735",
        "oled",
        "lcd",
        "gps",
        "battery",
        "nimble",
        "bluedroid",
        "esp_bt_controller",
    )
    assert not any(symbol in source for symbol in forbidden)


def test_uplink_main_composes_the_backend_lite_runtime() -> None:
    source = (UPLINK / "main/main.c").read_text(encoding="utf-8")
    required_calls = {
        "backend_ap_policy_note_config_commit(",
        "backend_ap_policy_note_backend_success(",
        "backend_ap_policy_tick(",
        "backend_config_load_or_migrate(",
        "backend_config_portal_start(",
        "backend_identity_record_validate(",
        "backend_uart_slots_init(",
        "backend_uart_slot_driver_init(",
        "backend_detection_uart_decode(",
        "backend_observation_resolve(",
        "backend_coordinator_ingest_detection(",
        "backend_scanner_plan_compute(",
        "backend_scanner_control_encode(",
        "backend_threat_ingest(",
        "backend_status_led_set_state(",
        "backend_upload_builder_add(",
        "backend_upload_fifo_push(",
        "backend_http_post_json(",
        "backend_ingest_ack_validate(",
        "backend_time_select_source(",
        "backend_command_envelope_decode(",
        "backend_ota_maintenance_auto_poll(",
        "backend_ota_maintenance_parse_usb(",
        "backend_self_ota_mark_valid_if_healthy(",
    }
    missing = sorted(call for call in required_calls if call not in source)
    assert not missing, f"uplink runtime calls missing: {missing}"

    for task in (
        "uart_worker",
        "coordinator_worker",
        "network_worker",
        "uploader_worker",
        "time_worker",
        "command_worker",
        "ota_worker",
        "usb_worker",
    ):
        assert f"{task}(" in source

    assert 'FOF_BACKEND_BOOT {\\"product_family\\":\\"%s\\"' in source
    assert 'FOF_BACKEND_HEALTH {\\"product_family\\":\\"%s\\"' in source
    assert '\\"target\\":\\"%s\\",\\"project\\":\\"%s\\"' in source
    assert 'strcmp(line, "FOF_BACKEND_STATUS") == 0' in source
    assert (
        "fof_backend_embedded_identity.image_kind != "
        "BACKEND_IMAGE_UPLINK"
    ) in source


def test_uplink_led_profile_build_artifacts_select_only_the_requested_adapter() -> None:
    lite = UPLINK / ".pio/build/uplink-s3-backend"
    fullsize = UPLINK / ".pio/build/uplink-s3-fullsize-backend"
    lite_commands = (lite / "compile_commands.json").read_text(encoding="utf-8")
    lite_map = (lite / "fof_backend_uplink.map").read_text(
        encoding="utf-8", errors="replace"
    )
    lite_app = (lite / "firmware.elf").read_bytes().decode("latin-1")
    assert "backend_yellow_led.c" in lite_commands
    assert "backend_yellow_led.c.o" in lite_map
    for marker in (
        "backend_fullsize_rgb_led",
        "led_strip",
        "fullsize-components/backend_fullsize_led",
    ):
        assert marker not in lite_commands
        assert marker not in lite_map
        assert marker not in lite_app

    fullsize_map = (fullsize / "fof_backend_uplink_fullsize.map").read_text(
        encoding="utf-8", errors="replace"
    )
    assert "backend_fullsize_led" in fullsize_map
    assert "backend_fullsize_rgb_led.c.o" in fullsize_map
    assert "espressif__led_strip" in fullsize_map
    assert "backend_yellow_led.c.o" not in fullsize_map
