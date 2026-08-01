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
        "scanner-s3-combo-fof_badge-con-crud-canary",
    ),
    PRODUCTION_PLATFORMIO_FILES[1]: (
        "uplink-s3",
        "uplink-s3-fof_badge",
        "uplink-s3-fof_badge-con-crud-canary",
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


def test_con_crud_canary_environments_are_explicit_and_isolated():
    scanner = PRODUCTION_PLATFORMIO_FILES[0].read_text()
    uplink = PRODUCTION_PLATFORMIO_FILES[1].read_text()

    scanner_canary = scanner.split(
        "[env:scanner-s3-combo-fof_badge-con-crud-canary]", 1
    )[1].split("[env:", 1)[0]
    uplink_canary = uplink.split(
        "[env:uplink-s3-fof_badge-con-crud-canary]", 1
    )[1].split("[env:", 1)[0]
    scanner_production = scanner.split(
        "[env:scanner-s3-combo-fof_badge]", 1
    )[1].split("[env:", 1)[0]
    uplink_production = uplink.split(
        "[env:uplink-s3-fof_badge]", 1
    )[1].split("[env:", 1)[0]

    for section in (scanner_canary, uplink_canary):
        assert "-DFOF_BADGE_VARIANT" in section
        assert "-DFOF_DC34_GAME_CANARY=1" in section
    assert "FOF_DC34_GAME_CANARY" not in scanner_production
    assert "FOF_DC34_GAME_CANARY" not in uplink_production
    assert (
        'sdkconfig.scanner-s3-fof_badge-con-crud-canary.defaults'
        in scanner_canary
    )
    assert (
        'sdkconfig.esp32s3-fof_badge-con-crud-canary.defaults'
        in uplink_canary
    )

    uplink_defaults = (
        REPO_ROOT / "esp32" / "uplink" /
        "sdkconfig.esp32s3-fof_badge-con-crud-canary.defaults"
    ).read_text()
    required_controller_only = {
        "CONFIG_BT_ENABLED=y",
        "CONFIG_BT_CTRL_RUN_IN_FLASH_ONLY=y",
        "CONFIG_BT_CTRL_DTM_ENABLE=n",
        "CONFIG_BT_CONTROLLER_ONLY=y",
        "CONFIG_BT_CONTROLLER_ENABLED=y",
        "CONFIG_BT_CTRL_HCI_MODE_VHCI=y",
        "CONFIG_BT_CTRL_BLE_MAX_ACT=1",
        "CONFIG_BT_CTRL_BLE_ADV=y",
        "CONFIG_BT_CTRL_BLE_SCAN=n",
        "CONFIG_BT_CTRL_BLE_MASTER=n",
        "CONFIG_BT_CTRL_BLE_SECURITY_ENABLE=n",
        "CONFIG_BT_BLUEDROID_ENABLED=n",
        "CONFIG_BT_NIMBLE_ENABLED=n",
    }
    assert required_controller_only <= set(uplink_defaults.splitlines())

    uplink_hook = (
        REPO_ROOT / "esp32" / "scripts" /
        "pio_verify_badge_uplink_build.py"
    ).read_text()
    scanner_hook = (
        REPO_ROOT / "esp32" / "scripts" /
        "pio_verify_badge_scanner_build.py"
    ).read_text()
    assert "uplink-s3-fof_badge-con-crud-canary" in uplink_hook
    assert "scanner-s3-combo-fof_badge-con-crud-canary" in scanner_hook


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


def test_pull_request_firmware_checks_do_not_deploy_github_pages():
    workflow = (
        REPO_ROOT / ".github" / "workflows" / "esp32-web-flasher.yml"
    ).read_text()
    deploy_job = workflow[workflow.index("  deploy:") :]
    condition = next(
        line.strip()
        for line in deploy_job.splitlines()
        if line.strip().startswith("if:")
    )

    assert condition == (
        "if: ${{ needs.build.result == 'success' && "
        "github.event_name != 'pull_request' && "
        "github.event_name != 'release' && "
        "!startsWith(github.ref, 'refs/tags/') }}"
    )
