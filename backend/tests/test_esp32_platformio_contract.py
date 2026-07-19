from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
PRODUCTION_PLATFORMIO_FILES = (
    REPO_ROOT / "esp32" / "scanner" / "platformio.ini",
    REPO_ROOT / "esp32" / "uplink" / "platformio.ini",
)
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
