from configparser import ConfigParser
from pathlib import Path
import subprocess


ROOT = Path(__file__).resolve().parents[2]


def test_operation_id_header_fails_closed_without_exactly_one_profile(
    tmp_path: Path,
) -> None:
    header = ROOT / "shared/backend_ota_operation_id.h"
    source = tmp_path / "operation_id_fixture.c"
    source.write_text(
        '#include "backend_ota_operation_id.h"\nint main(void) { return 0; }\n',
        encoding="utf-8",
    )

    header_text = header.read_text(encoding="utf-8")
    assert '#include "backend_hardware_profile.h"' in header_text
    for flags in ((), ("-DFOF_BACKEND_PROFILE_BADGE_LITE=1", "-DFOF_BACKEND_PROFILE_S3_FULLSIZE=1")):
        result = subprocess.run(
            ["cc", "-std=c11", "-I", str(ROOT / "shared"), *flags, "-c", str(source),
             "-o", str(tmp_path / "operation_id_fixture.o")],
            text=True,
            capture_output=True,
            check=False,
        )
        assert result.returncode != 0
        assert "select exactly one backend hardware profile" in result.stderr


def test_fullsize_native_receipt_fixture_path_is_explicit_and_fixture_exists() -> None:
    platformio = (ROOT / "platformio.ini").read_text(encoding="utf-8")
    fixture = ROOT / "test/fixtures/backend_ota_receipt_v1.json"

    assert fixture.is_file()
    assert "BACKEND_OTA_RECEIPT_FIXTURE_PATH" in platformio
    fullsize = platformio.split("[env:backend-native-fullsize]", 1)[1]
    assert "$PROJECT_DIR/test/fixtures/backend_ota_receipt_v1.json" in fullsize
    assert "BACKEND_OTA_RECEIPT_FIXTURE_PATH" not in platformio.split(
        "[env:backend-native-fullsize]", 1
    )[0]


def test_fullsize_probe_api_is_absent_from_lite_headers(tmp_path: Path) -> None:
    source = tmp_path / "fullsize_probe_fixture.c"
    source.write_text(
        '#include "backend_ota_maintenance.h"\n'
        'int main(void) { return backend_ota_maintenance_run_fullsize_probe('
        '0, 0, 0, 0, 0, 0, 0, 0, 0); }\n',
        encoding="utf-8",
    )
    include_args = [
        "-I", str(ROOT / "shared"),
        "-I", str(ROOT / "uplink/main/ota"),
        "-I", str(ROOT / "uplink/main/storage"),
    ]
    lite = subprocess.run(
        ["cc", "-std=c11", "-Werror=implicit-function-declaration", *include_args,
         "-DFOF_BACKEND_PROFILE_BADGE_LITE=1", "-c", str(source),
         "-o", str(tmp_path / "lite_fullsize_probe_fixture.o")],
        text=True, capture_output=True, check=False,
    )
    assert lite.returncode != 0
    fullsize = subprocess.run(
        ["cc", "-std=c11", "-Werror=implicit-function-declaration", *include_args,
         "-DFOF_BACKEND_PROFILE_S3_FULLSIZE=1", "-c", str(source),
         "-o", str(tmp_path / "fullsize_probe_fixture.o")],
        text=True, capture_output=True, check=False,
    )
    assert fullsize.returncode == 0, fullsize.stderr


def test_command_client_public_api_is_guarded_from_lite_at_compile_time(
    tmp_path: Path,
) -> None:
    source = tmp_path / "command_client_fixture.c"
    source.write_text(
        '#include "backend_ota_command_client.h"\n'
        "int main(void) { return (int)(sizeof(backend_ota_accepted_probe_t) + "
        "sizeof(backend_ota_terminal_evidence_t) + "
        "sizeof(backend_ota_built_end_t)); }\n",
        encoding="utf-8",
    )
    include_args = [
        "-I", str(ROOT / "shared"),
        "-I", str(ROOT / "uplink/main/network"),
        "-I", str(ROOT / "uplink/main/ota"),
        "-I", str(ROOT / "uplink/main/storage"),
    ]
    lite = subprocess.run(
        ["cc", "-std=c11", *include_args, "-DFOF_BACKEND_PROFILE_BADGE_LITE=1",
         "-c", str(source), "-o", str(tmp_path / "lite_command_client_fixture.o")],
        text=True, capture_output=True, check=False,
    )
    assert lite.returncode != 0
    assert "backend_ota_command_client is available only on the Fullsize profile" in lite.stderr

    fullsize = subprocess.run(
        ["cc", "-std=c11", *include_args, "-DFOF_BACKEND_PROFILE_S3_FULLSIZE=1",
         "-c", str(source), "-o", str(tmp_path / "fullsize_command_client_fixture.o")],
        text=True, capture_output=True, check=False,
    )
    assert fullsize.returncode == 0, fullsize.stderr

    header = (ROOT / "uplink/main/network/backend_ota_command_client.h").read_text(
        encoding="utf-8"
    )
    assert "backend_ota_event_end_build" in header
    assert "backend_ota_end_event_t" not in header
    assert "backend_ota_event_end_encode" not in header


def test_lite_operation_id_abi_and_client_source_contract_are_exact(
    tmp_path: Path,
) -> None:
    source = tmp_path / "lite_operation_id_abi.c"
    source.write_text(
        '#include "backend_ota_operation_id.h"\n'
        "_Static_assert(sizeof(backend_ota_operation_id_t) == 4, \"Lite operation ID ABI\");\n"
        "_Static_assert(_Generic((backend_ota_operation_id_t)0, uint32_t: 1, default: 0), "
        "\"Lite operation ID type\");\n"
        "int main(void) { return 0; }\n",
        encoding="utf-8",
    )
    result = subprocess.run(
        ["cc", "-std=c11", "-I", str(ROOT / "shared"),
         "-DFOF_BACKEND_PROFILE_BADGE_LITE=1", "-c", str(source),
         "-o", str(tmp_path / "lite_operation_id_abi.o")],
        text=True, capture_output=True, check=False,
    )
    assert result.returncode == 0, result.stderr

    platformio = (ROOT / "platformio.ini").read_text(encoding="utf-8")
    lite_contract = platformio.split("[env:backend-native-fullsize]", 1)[0]
    command_test = (
        ROOT / "test/test_backend_ota_command_client/test_main.c"
    ).read_text(encoding="utf-8")
    guarded_body = command_test.split(
        "#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)", 2
    )[2]
    lite_test_body = guarded_body.split("\n#else\n", 1)[1].split(
        "\n#endif\n", 1
    )[0]
    for fullsize_only in (
        "backend_ota_command_client.h",
        "backend_ota_command_client.c",
        "BACKEND_OTA_RECEIPT_FIXTURE_PATH",
        "backend_ota_receipt_v1_preimage",
        "backend_ota_receipt_end_t",
        "backend_ota_accepted_probe_t",
        "backend_ota_accepted_probe_capture",
        "backend_ota_apply_matches_accepted_probe",
        "backend_ota_terminal_evidence_t",
        "backend_ota_event_end_build",
        "fof-backend-ota-end-receipt-v1",
    ):
        assert fullsize_only not in lite_contract
        assert fullsize_only not in lite_test_body


def test_fullsize_outbox_sources_are_profile_scoped() -> None:
    platformio = (ROOT / "platformio.ini").read_text(encoding="utf-8")
    lite_native, fullsize_native = platformio.split(
        "[env:backend-native-fullsize]", 1
    )
    assert "backend_ota_event_outbox.c" not in lite_native
    assert "+<uplink/main/storage/backend_ota_event_outbox.c>" in fullsize_native

    cmake = (ROOT / "uplink/main/CMakeLists.txt").read_text(encoding="utf-8")
    fullsize_branch = cmake.split(
        'elseif("${BACKEND_STATUS_PROJECT_NAME}" STREQUAL '
        '"fof_backend_uplink_fullsize")',
        1,
    )[1].split("else()", 1)[0]
    lite_branch = cmake.split(
        'if("${BACKEND_STATUS_PROJECT_NAME}" STREQUAL "fof_backend_uplink")',
        1,
    )[1].split("elseif", 1)[0]
    for source in (
        "network/backend_ota_command_client.c",
        "network/backend_ota_workflow.c",
        "storage/backend_ota_event_outbox.c",
    ):
        assert source in fullsize_branch
        assert source not in lite_branch
    assert "${BACKEND_OTA_FULLSIZE_SRCS}" in cmake


def test_terminal_startup_classifies_progress_and_terminal_tombstone_cuts() -> None:
    main = (ROOT / "uplink/main/main.c").read_text(encoding="utf-8")
    startup = main.split("static bool ota_startup_restore(", 1)[1]
    terminal = startup.split("if (terminal) {", 1)[1].split(
        "uplink_ota_work_item_t work", 1
    )[0]

    assert "ota_running_version(" in terminal
    assert "copy_text(result.running_version" in terminal
    assert "terminal_matches" in terminal
    assert "ota_restore_pending_progress(" in terminal
    assert terminal.index("terminal_matches") < terminal.index(
        "ota_restore_pending_progress("
    )
    assert "terminal_tombstoned" in terminal
    assert "pending.body_sha256" in terminal
    assert "backend_ota_workflow_note_terminal_ack(" in terminal


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
            "BACKEND_IMAGE_SCANNER",
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
