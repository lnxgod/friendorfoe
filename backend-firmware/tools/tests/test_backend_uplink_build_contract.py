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


def _sdkconfig() -> dict[str, str]:
    values: dict[str, str] = {}
    for raw_line in (UPLINK / "sdkconfig.defaults").read_text(
        encoding="utf-8"
    ).splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        key, separator, value = line.partition("=")
        assert separator, f"malformed sdkconfig line: {raw_line}"
        assert key not in values, f"duplicate sdkconfig key: {key}"
        values[key] = value
    return values


def _partition_rows() -> list[tuple[str, str, str, int, int]]:
    rows: list[tuple[str, str, str, int, int]] = []
    path = UPLINK / "partitions_backend_uplink_8mb.csv"
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
    assert parser.sections() == ["platformio", "env:uplink-s3-backend"]
    assert "FOF_BADGE_VARIANT" not in text
    assert "fof_badge" not in text.lower()
    for script in environment.get("extra_scripts", "").split():
        script_path = script.split(":", 1)[-1]
        assert not Path(script_path).is_absolute()
        assert ".." not in Path(script_path).parts


def test_uplink_project_uses_backend_version_and_explicit_local_sources() -> None:
    top = (UPLINK / "CMakeLists.txt").read_text(encoding="utf-8")
    component = (UPLINK / "main/CMakeLists.txt").read_text(encoding="utf-8")

    assert "project(fof_backend_uplink)" in top
    assert "FOF_VERSION_BACKEND" in top
    assert "backend_version.h" in top
    assert 'STRINGS "../shared/backend_version.h" PROJECT_VER' in top
    assert 'set(PROJECT_VER "' not in top
    forbidden_build_features = (
        "SRC_DIRS",
        "EXTRA_COMPONENT_DIRS",
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
        "hw/backend_yellow_led.c",
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
        "backend_yellow_led_set_state(",
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

    assert 'FOF_BACKEND_BOOT {\\"target\\":\\"%s\\"' in source
    assert 'FOF_BACKEND_HEALTH {\\"target\\":\\"%s\\"' in source
    assert 'strcmp(line, "FOF_BACKEND_STATUS") == 0' in source
    assert (
        "fof_backend_embedded_identity.image_kind != "
        "BACKEND_IMAGE_UPLINK"
    ) in source
