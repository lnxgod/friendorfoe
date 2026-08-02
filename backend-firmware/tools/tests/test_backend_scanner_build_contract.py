from configparser import ConfigParser
import csv
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SCANNER = ROOT / "scanner"


def load_ini(path: Path) -> ConfigParser:
    parser = ConfigParser(interpolation=None)
    parser.read_string(path.read_text(encoding="utf-8"))
    assert parser.sections()
    return parser


def sdkconfig_values(filename: str) -> dict[str, str]:
    values: dict[str, str] = {}
    for raw_line in (SCANNER / filename).read_text(
        encoding="utf-8"
    ).splitlines():
        line = raw_line.strip()
        if line.startswith("CONFIG_") and "=" in line:
            key, value = line.split("=", 1)
            values[key] = value
    return values


def test_scanner_platformio_environment_is_backend_only() -> None:
    parser = load_ini(SCANNER / "platformio.ini")
    assert parser.get("platformio", "src_dir") == "main"

    section = parser["env:scanner-s3-combo-backend"]
    assert section["platform"] == "espressif32@6.13.0"
    assert section["board"] == "seeed_xiao_esp32s3"
    assert section["framework"] == "espidf"
    assert section.getint("monitor_speed") == 921600
    assert section["board_upload.flash_size"] == "8MB"
    assert section["board_upload.offset_address"] == "0x20000"
    assert section["board_build.flash_mode"] == "dio"
    assert section["board_build.f_flash"] == "80000000L"
    assert section["board_build.partitions"] == (
        "partitions_backend_scanner_8mb.csv"
    )

    flags = section["build_flags"]
    assert "-DFOF_BACKEND_FIRMWARE=1" in flags
    assert "-DFOF_BACKEND_SCANNER=1" in flags
    assert "-DBOARD_HAS_PSRAM" in flags
    assert "-DFOF_BACKEND_PROFILE_BADGE_LITE=1" in flags
    text = (SCANNER / "platformio.ini").read_text(encoding="utf-8")
    assert "FOF_BADGE_VARIANT" not in text
    assert "fof_badge" not in text.lower()
    assert parser.get("platformio", "default_envs") == "scanner-s3-combo-backend"


def test_fullsize_scanner_environment_selects_its_16mb_generated_config_input() -> None:
    parser = load_ini(SCANNER / "platformio.ini")
    section = parser["env:scanner-s3-combo-fullsize-backend"]
    assert section["board"] == "esp32-s3-devkitc-1"
    assert section["board_upload.flash_size"] == "16MB"
    assert section["board_build.partitions"] == (
        "partitions_backend_scanner_fullsize_16mb.csv"
    )
    assert section["board_build.flash_mode"] == "qio"
    assert section["board_build.psram_type"] == "opi"
    assert '-DSDKCONFIG_DEFAULTS="sdkconfig.fullsize.defaults"' in section[
        "board_build.cmake_extra_args"
    ]

    config = sdkconfig_values("sdkconfig.fullsize.defaults")
    expected = {
        "CONFIG_ESPTOOLPY_FLASHSIZE_16MB": "y",
        "CONFIG_ESPTOOLPY_FLASHSIZE": '"16MB"',
        "CONFIG_ESPTOOLPY_FLASHMODE_QIO": "y",
        # ESP-IDF flashes a QIO bootloader over DIO, then the bootloader
        # switches the application to the selected QIO mode.
        "CONFIG_ESPTOOLPY_FLASHMODE": '"dio"',
        "CONFIG_ESPTOOLPY_FLASHFREQ_80M": "y",
        "CONFIG_PARTITION_TABLE_CUSTOM_FILENAME": (
            '"partitions_backend_scanner_fullsize_16mb.csv"'
        ),
        "CONFIG_SPIRAM": "y",
        "CONFIG_SPIRAM_MODE_OCT": "y",
    }
    for key, value in expected.items():
        assert config.get(key) == value

def test_scanner_project_has_exact_backend_descriptor_and_local_sources() -> None:
    project = (SCANNER / "CMakeLists.txt").read_text(encoding="utf-8")
    component = (SCANNER / "main/CMakeLists.txt").read_text(encoding="utf-8")
    identity_fragment = (SCANNER / "main/backend_identity.lf").read_text(
        encoding="utf-8"
    )

    assert "FOF_VERSION_BACKEND" in project
    assert "project(${FOF_BACKEND_PROJECT_NAME})" in project
    assert "EXTRA_COMPONENT_DIRS" not in project + component
    for forbidden in ("SRC_DIRS", "GLOB", "../../../", "vendor/"):
        assert forbidden not in component

    required_sources = (
        "main.c",
        "comms/backend_uart_rx.c",
        "comms/backend_uart_tx.c",
        "comms/uart_ota.c",
        "core/backend_detection_sink.c",
        "core/backend_investigation_sink.c",
        "core/backend_scanner_runtime.c",
        "core/scanner_rollback.c",
        "detection/ble_remote_id.c",
        "detection/ble_investigator.c",
        "detection/wifi_scanner.c",
        "hw/backend_yellow_led.c",
        "../../shared/backend_embedded_identity.c",
        "../../shared/backend_identity.c",
        "../../shared/backend_ota_identity.c",
        "../../shared/backend_scanner_status_codec.c",
    )
    for source in required_sources:
        assert f'"{source}"' in component
    assert component.count("../../shared/backend_embedded_identity.c") == 1
    assert 'LDFRAGMENTS "backend_identity.lf"' in component
    assert ".fof_backend_identity" in identity_fragment
    assert "flash_rodata KEEP()" in identity_fragment

    for dependency in (
        "bt",
        "esp_wifi",
        "nvs_flash",
        "esp_timer",
        "app_update",
        "mbedtls",
        "esp_psram",
        "driver",
    ):
        assert dependency in component
    for forbidden in (
        "display_st7735",
        "oled_display",
        "badge_easter",
        "ws2812",
        "cjson",
    ):
        assert forbidden not in component.lower()


def test_scanner_sdkconfig_enables_exact_headless_radio_and_rollback_contract() -> None:
    config = sdkconfig_values("sdkconfig.defaults")
    expected = {
        "CONFIG_ESPTOOLPY_FLASHSIZE_8MB": "y",
        "CONFIG_PARTITION_TABLE_CUSTOM": "y",
        "CONFIG_PARTITION_TABLE_CUSTOM_FILENAME": (
            '"partitions_backend_scanner_8mb.csv"'
        ),
        "CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG": "y",
        "CONFIG_FREERTOS_HZ": "1000",
        "CONFIG_ESP_TASK_WDT_TIMEOUT_S": "30",
        "CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE": "y",
        "CONFIG_BT_ENABLED": "y",
        "CONFIG_BT_NIMBLE_ENABLED": "y",
        "CONFIG_BT_NIMBLE_ROLE_OBSERVER": "y",
        "CONFIG_BT_NIMBLE_ROLE_CENTRAL": "y",
        "CONFIG_BT_NIMBLE_EXT_ADV": "y",
        "CONFIG_ESP_WIFI_ENABLED": "y",
        "CONFIG_SW_COEXIST_ENABLE": "y",
        "CONFIG_ESP_COEX_SW_COEXIST_ENABLE": "y",
        "CONFIG_SPIRAM": "y",
        "CONFIG_SPIRAM_MODE_OCT": "y",
        "CONFIG_SPIRAM_USE_CAPS_ALLOC": "y",
    }
    for key, value in expected.items():
        assert config.get(key) == value

def test_scanner_partition_table_has_two_exact_2mb_ota_slots_and_safe_nvs() -> None:
    with (SCANNER / "partitions_backend_scanner_8mb.csv").open(
        newline="", encoding="utf-8"
    ) as handle:
        rows = [row for row in csv.reader(handle) if row and not row[0].startswith("#")]

    normalized = [[field.strip() for field in row] for row in rows]
    assert normalized == [
        ["nvs", "data", "nvs", "0x9000", "0x6000", ""],
        ["otadata", "data", "ota", "0xf000", "0x2000", ""],
        ["phy_init", "data", "phy", "0x11000", "0x1000", ""],
        ["ota_0", "app", "ota_0", "0x20000", "0x200000", ""],
        ["ota_1", "app", "ota_1", "0x220000", "0x200000", ""],
        ["storage", "data", "spiffs", "0x420000", "0x100000", ""],
        ["reserved", "data", "fat", "0x520000", "0x2e0000", ""],
    ]
    assert int(normalized[-1][3], 16) + int(normalized[-1][4], 16) == 0x800000


def test_fullsize_scanner_partition_admits_3mb_images_without_overlap() -> None:
    path = SCANNER / "partitions_backend_scanner_fullsize_16mb.csv"
    with path.open(newline="", encoding="utf-8") as handle:
        rows = [row for row in csv.reader(handle) if row and not row[0].startswith("#")]
    normalized = [[field.strip() for field in row] for row in rows]
    assert normalized == [
        ["nvs", "data", "nvs", "0x9000", "0x6000", ""],
        ["otadata", "data", "ota", "0xf000", "0x2000", ""],
        ["phy_init", "data", "phy", "0x11000", "0x1000", ""],
        ["ota_0", "app", "ota_0", "0x20000", "0x300000", ""],
        ["ota_1", "app", "ota_1", "0x320000", "0x300000", ""],
        ["storage", "data", "spiffs", "0x620000", "0x100000", ""],
        ["reserved", "data", "fat", "0x720000", "0x8e0000", ""],
    ]
    assert int(normalized[-1][3], 16) + int(normalized[-1][4], 16) == 0x1000000


def test_scanner_main_composes_uart_radios_led_identity_ota_and_rollback() -> None:
    main = (SCANNER / "main/main.c").read_text(encoding="utf-8")
    required_markers = (
        "void app_main(void)",
        "FOF_BACKEND_BOOT",
        "FOF_BACKEND_HEALTH",
        "FOF_BACKEND_STATUS",
        "backend_detection_sink_register",
        "backend_investigation_sink_register",
        "backend_uart_rx_init",
        "backend_scanner_status_encode",
        "backend_yellow_led_init",
        "ble_remote_id_init",
        "wifi_scanner_init",
        "uart_ota_init",
        "esp_ota_mark_app_valid_cancel_rollback",
        "esp_ota_mark_app_invalid_rollback_and_reboot",
    )
    for marker in required_markers:
        assert marker in main

    # Detection and rollback evidence must come from their own monotonic
    # sources.  Status cadence cannot stamp detections, and the supervisor
    # cannot claim a radio worker merely because it polled radio state.
    assert "uint32_t detection_sequence;" in main
    assert "++s_app.detection_sequence" in main
    assert "ble_remote_id_get_stats" in main
    assert "ble_stats.ble_scan_start_ok > 0U" in main
    assert "wifi_scanner_get_stats" in main
    assert "wifi_stats.full_scan_count > 0U" in main
    uart_task = main[
        main.index("static void uart_rx_task") :
        main.index("static bool reset_was_crash")
    ]
    assert "backend_scanner_runtime_wdt_register_current" in uart_task

    lower = main.lower()
    for forbidden in (
        "display",
        "lcd",
        "oled",
        "theme",
        "easter",
        "game",
        "ws2812",
        "advertiser",
    ):
        assert forbidden not in lower


def test_scanner_kconfig_defines_only_backend_glasses_switch() -> None:
    text = (SCANNER / "main/Kconfig.projbuild").read_text(encoding="utf-8")
    configs = [
        line.split(maxsplit=1)[1]
        for line in text.splitlines()
        if line.strip().startswith("config ")
    ]
    assert configs == ["FOF_BACKEND_GLASSES_DETECTION"]
    assert "default y" in text
    assert "FOF_BADGE" not in text


def test_ble_feature_gates_import_generated_config_first() -> None:
    source = (SCANNER / "main/detection/ble_remote_id.c").read_text(
        encoding="utf-8"
    )
    config_include = source.index('#include "sdkconfig.h"')
    glasses_gate = source.index("#if CONFIG_FOF_BACKEND_GLASSES_DETECTION")
    assert config_include < glasses_gate
