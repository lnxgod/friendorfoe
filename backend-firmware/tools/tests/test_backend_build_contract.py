from configparser import ConfigParser
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def _environment(project: str, name: str) -> tuple[ConfigParser, str]:
    path = ROOT / project / "platformio.ini"
    text = path.read_text(encoding="utf-8")
    parser = ConfigParser(interpolation=None)
    parser.read_string(text)
    assert parser.sections() == ["platformio", f"env:{name}"]
    assert parser["platformio"]["default_envs"] == name
    return parser, text


def test_platformio_defaults_are_explicit_backend_images_only() -> None:
    scanner, scanner_text = _environment(
        "scanner", "scanner-s3-combo-backend"
    )
    uplink, uplink_text = _environment("uplink", "uplink-s3-backend")

    for parser, name in (
        (scanner, "scanner-s3-combo-backend"),
        (uplink, "uplink-s3-backend"),
    ):
        environment = parser[f"env:{name}"]
        assert environment["board"] == "seeed_xiao_esp32s3"
        assert environment["framework"] == "espidf"
        assert environment["board_upload.offset_address"] == "0x20000"
        assert environment["board_build.flash_mode"] == "dio"
        assert environment["board_build.f_flash"] == "80000000L"

    build_text = scanner_text + uplink_text
    assert "FOF_BACKEND_FIRMWARE=1" in build_text
    assert "fof_badge" not in build_text.lower()
    assert "FOF_BADGE_VARIANT" not in build_text


def test_projects_and_apps_have_exact_backend_identity_and_status_ingress() -> None:
    expected = {
        "scanner": (
            "project(fof_backend_scanner)",
            "scanner-s3-combo-backend",
            "scanner-s3-combo-backend",
        ),
        "uplink": (
            "project(fof_backend_uplink)",
            "uplink-s3-backend",
            "BACKEND_IMAGE_UPLINK",
        ),
    }
    identity_header = (ROOT / "shared/backend_identity.h").read_text(
        encoding="utf-8"
    )
    for project, (project_marker, target, runtime_marker) in expected.items():
        cmake = (ROOT / project / "CMakeLists.txt").read_text(
            encoding="utf-8"
        )
        main = (ROOT / project / "main/main.c").read_text(encoding="utf-8")
        assert project_marker in cmake
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
