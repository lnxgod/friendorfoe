from configparser import ConfigParser
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def _environment(project: str, name: str) -> tuple[ConfigParser, str]:
    path = ROOT / project / "platformio.ini"
    text = path.read_text(encoding="utf-8")
    parser = ConfigParser(interpolation=None)
    parser.read_string(text)
    assert parser["platformio"]["default_envs"] == name
    return parser, text


def _cmake_values(environment: ConfigParser) -> dict[str, str]:
    values: dict[str, str] = {}
    for argument in environment["board_build.cmake_extra_args"].split():
        assert argument.startswith("-D") and "=" in argument
        key, value = argument[2:].split("=", 1)
        values[key] = value.strip('"')
    return values


def test_platformio_defaults_are_explicit_backend_images_only() -> None:
    scanner, scanner_text = _environment(
        "scanner", "scanner-s3-combo-backend"
    )
    uplink, uplink_text = _environment("uplink", "uplink-s3-backend")

    for parser, name, project in (
        (scanner, "scanner-s3-combo-backend", "fof_backend_scanner"),
        (uplink, "uplink-s3-backend", "fof_backend_uplink"),
    ):
        environment = parser[f"env:{name}"]
        assert environment["board"] == "seeed_xiao_esp32s3"
        assert environment["framework"] == "espidf"
        assert environment["board_upload.offset_address"] == "0x20000"
        assert environment["board_build.flash_mode"] == "dio"
        assert environment["board_build.f_flash"] == "80000000L"
        assert "-DFOF_BACKEND_PROFILE_BADGE_LITE=1" in environment["build_flags"]
        assert _cmake_values(environment) == {
            "FOF_BACKEND_PROJECT_NAME": project,
            "FOF_BACKEND_PROFILE_NAME": "badge_lite",
            "SDKCONFIG_DEFAULTS": "sdkconfig.defaults",
        }

    fullsize = (
        (scanner, "scanner-s3-combo-fullsize-backend", "fof_backend_scanner_fullsize"),
        (uplink, "uplink-s3-fullsize-backend", "fof_backend_uplink_fullsize"),
    )
    for parser, name, project in fullsize:
        environment = parser[f"env:{name}"]
        assert environment["board"] == "esp32-s3-devkitc-1"
        assert environment["framework"] == "espidf"
        assert environment["board_upload.flash_size"] == "16MB"
        assert environment["board_upload.offset_address"] == "0x20000"
        assert environment["board_build.flash_mode"] == "qio"
        assert environment["board_build.f_flash"] == "80000000L"
        assert environment["board_build.psram_type"] == "opi"
        assert "-DBOARD_HAS_PSRAM" in environment["build_flags"]
        assert "-DFOF_BACKEND_PROFILE_S3_FULLSIZE=1" in environment["build_flags"]
        assert _cmake_values(environment) == {
            "FOF_BACKEND_PROJECT_NAME": project,
            "FOF_BACKEND_PROFILE_NAME": "s3_fullsize",
            "SDKCONFIG_DEFAULTS": "sdkconfig.fullsize.defaults",
        }

    build_text = scanner_text + uplink_text
    assert "FOF_BACKEND_FIRMWARE=1" in build_text
    assert "fof_badge" not in build_text.lower()
    assert "FOF_BADGE_VARIANT" not in build_text


def test_projects_reject_identity_pairs_that_could_mislabel_app_descriptors() -> None:
    expected = {
        "scanner": (
            "fof_backend_scanner",
            "scanner-s3-combo-backend",
            "scanner-s3-combo-backend",
        ),
        "uplink": (
            "fof_backend_uplink",
            "uplink-s3-backend",
            "BACKEND_IMAGE_UPLINK",
        ),
    }
    identity_header = (ROOT / "shared/backend_hardware_profile.h").read_text(
        encoding="utf-8"
    )
    for project, (project_name, target, runtime_marker) in expected.items():
        cmake = (ROOT / project / "CMakeLists.txt").read_text(
            encoding="utf-8"
        )
        main = (ROOT / project / "main/main.c").read_text(encoding="utf-8")
        assert "project(${FOF_BACKEND_PROJECT_NAME})" in cmake
        assert f'"{project_name}"' in cmake
        assert f'"{project_name}_fullsize"' in cmake
        assert "FOF_BACKEND_PROFILE_NAME" in cmake
        assert "badge_lite" in cmake
        assert "s3_fullsize" in cmake
        assert "FATAL_ERROR" in cmake
        assert "FOF_VERSION_BACKEND" in cmake
        assert "backend_version.h" in cmake
        assert "file(" in cmake
        assert 'set(PROJECT_VER "0.1.0-backend")' not in cmake
        assert "void app_main(void)" in main
        assert target in identity_header
        assert runtime_marker in main
        assert "FOF_BACKEND_BOOT" in main
        assert "FOF_BACKEND_HEALTH" in main
        assert "FOF_BACKEND_STATUS" in main


def test_both_application_source_lists_are_headless_and_local() -> None:
    combined = ""
    for project in ("scanner", "uplink"):
        combined += (ROOT / project / "main/CMakeLists.txt").read_text(
            encoding="utf-8"
        )
    for forbidden in (
        "SRC_DIRS",
        "EXTRA_COMPONENT_DIRS",
        "GLOB",
        "../../../",
        "vendor/",
        "display_st7735",
        "oled_display",
        "badge_easter",
        "ws2812",
    ):
        assert forbidden not in combined


def test_profiles_share_backend_modules_without_protected_source_paths() -> None:
    scanner = (ROOT / "scanner/main/CMakeLists.txt").read_text(encoding="utf-8")
    uplink = (ROOT / "uplink/main/CMakeLists.txt").read_text(encoding="utf-8")

    for required in (
        "detection/backend_feature_adapter.c",
        "detection/ble_threat_detector.c",
        "../../shared/detection_policy.c",
    ):
        assert required in scanner
    for required in (
        "storage/backend_firmware_buffer.c",
        "../../shared/backend_ap_policy.c",
        "../../shared/backend_http_policy.c",
        "network/backend_command_client.c",
        "ota/backend_ota_maintenance.c",
        "ota/backend_scanner_relay.c",
    ):
        assert required in uplink

    assert "esp_http_server" not in scanner
    assert "backend_ap_policy" not in scanner
    assert "backend_http_policy" not in scanner
    combined = scanner + uplink
    for forbidden in (
        "esp32/",
        "oled",
        "display_task",
        "display_assets",
    ):
        assert forbidden not in combined.lower()


def test_embedded_projects_limit_idf_to_main_and_declared_dependencies() -> None:
    for project in ("scanner", "uplink"):
        cmake = (ROOT / project / "CMakeLists.txt").read_text(encoding="utf-8")
        components = cmake.index("set(COMPONENTS main __pio_env)")
        idf_include = cmake.index("include($ENV{IDF_PATH}/tools/cmake/project.cmake)")
        descriptor = cmake.index("project(${FOF_BACKEND_PROJECT_NAME})")
        assert components < idf_include < descriptor
