from app.services.firmware_manager import FIRMWARE_TYPES, _repo_fof_version_for_name


def test_live_fleet_firmware_targets_are_present():
    assert set(FIRMWARE_TYPES) == {
        "scanner-s3-combo",
        "scanner-s3-combo-fof_badge",
        "scanner-s3-combo-seed",
        "uplink-s3",
        "uplink-s3-fof_badge",
    }


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
    assert str(FIRMWARE_TYPES["scanner-s3-combo-fof_badge"]["local_bin"]).endswith(
        "/esp32/scanner/.pio/build/scanner-s3-combo-fof_badge/firmware.bin"
    )
    assert str(FIRMWARE_TYPES["uplink-s3-fof_badge"]["local_bin"]).endswith(
        "/esp32/uplink/.pio/build/uplink-s3-fof_badge/firmware.bin"
    )


def test_release_asset_patterns_match_current_bin_asset_names():
    for target, info in FIRMWARE_TYPES.items():
        assert info["asset_pattern"] == target


def test_badge_targets_use_badge_version_track():
    prod_version = _repo_fof_version_for_name("scanner-s3-combo")
    badge_scanner_version = _repo_fof_version_for_name("scanner-s3-combo-fof_badge")
    badge_uplink_version = _repo_fof_version_for_name("uplink-s3-fof_badge")

    assert prod_version
    assert badge_scanner_version == badge_uplink_version
    assert badge_scanner_version != prod_version
    assert "badge" in badge_scanner_version
