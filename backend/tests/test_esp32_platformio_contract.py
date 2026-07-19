from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
PRODUCTION_PLATFORMIO_FILES = (
    REPO_ROOT / "esp32" / "scanner" / "platformio.ini",
    REPO_ROOT / "esp32" / "uplink" / "platformio.ini",
)
PRODUCTION_ENVIRONMENTS = {
    PRODUCTION_PLATFORMIO_FILES[0]: (
        "scanner-s3-combo",
        "scanner-s3-combo-seed",
        "scanner-s3-combo-fof_badge",
    ),
    PRODUCTION_PLATFORMIO_FILES[1]: (
        "uplink-s3",
        "uplink-s3-fof_badge",
    ),
}
PINNED_PLATFORM = "platform = espressif32@6.13.0"


def test_production_firmware_uses_reviewed_esp_idf_5_toolchain():
    for path in PRODUCTION_PLATFORMIO_FILES:
        config = path.read_text()
        platform_lines = [
            line.strip()
            for line in config.splitlines()
            if line.strip().startswith("platform =")
        ]

        assert platform_lines
        assert set(platform_lines) == {PINNED_PLATFORM}
        assert "platform = espressif32\n" not in config

        for environment in PRODUCTION_ENVIRONMENTS[path]:
            section = config.split(f"[env:{environment}]", 1)[1]
            section = section.split("[env:", 1)[0]
            assert section.count("platform =") == 1
            assert PINNED_PLATFORM in section


def test_badge_excluded_http_route_descriptors_are_not_compiled():
    route_descriptors = {
        REPO_ROOT / "esp32" / "uplink" / "main" / "network" / "http_status.c": (
            "uri_ota_post",
            "uri_ota_relay",
        ),
        REPO_ROOT / "esp32" / "uplink" / "main" / "network" / "fw_store.c": (
            "uri_fw_upload",
            "uri_fw_relay",
            "uri_fw_trigger",
        ),
    }

    for path, descriptors in route_descriptors.items():
        source = path.read_text()
        for descriptor in descriptors:
            declaration = source.index(f"static const httpd_uri_t {descriptor}")
            preceding = source[:declaration]
            assert preceding.rfind("#ifndef FOF_BADGE_VARIANT") > preceding.rfind(
                "#endif"
            )
            assert source.index("#endif", declaration) > declaration
