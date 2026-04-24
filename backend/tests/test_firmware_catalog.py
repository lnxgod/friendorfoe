from app.services.firmware_manager import FIRMWARE_TYPES


def test_live_fleet_firmware_targets_are_present():
    assert "scanner-s3-combo" in FIRMWARE_TYPES
    assert "scanner-s3-combo-seed" in FIRMWARE_TYPES
    assert "uplink-s3" in FIRMWARE_TYPES


def test_live_fleet_targets_point_at_expected_local_builds():
    assert str(FIRMWARE_TYPES["scanner-s3-combo"]["local_bin"]).endswith(
        "/esp32/scanner/.pio/build/scanner-s3-combo/firmware.bin"
    )
    assert str(FIRMWARE_TYPES["scanner-s3-combo-seed"]["local_bin"]).endswith(
        "/esp32/scanner/.pio/build/scanner-s3-combo-seed/firmware.bin"
    )
    assert str(FIRMWARE_TYPES["uplink-s3"]["local_bin"]).endswith(
        "/esp32/uplink/.pio/build/uplink-s3/firmware.bin"
    )
