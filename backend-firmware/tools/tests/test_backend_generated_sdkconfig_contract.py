from pathlib import Path
import subprocess
import sys

import pytest


ROOT = Path(__file__).resolve().parents[2]
VERIFIER = ROOT / "tools/verify_generated_sdkconfigs.py"

VALID_GENERATED = {
    "scanner/sdkconfig.scanner-s3-combo-backend": {
        "CONFIG_ESPTOOLPY_FLASHSIZE_8MB": "y",
        "CONFIG_ESPTOOLPY_FLASHSIZE": '"8MB"',
        "CONFIG_ESPTOOLPY_FLASHMODE_DIO": "y",
        "CONFIG_ESPTOOLPY_FLASHMODE": '"dio"',
        "CONFIG_ESPTOOLPY_FLASHFREQ_80M": "y",
        "CONFIG_PARTITION_TABLE_CUSTOM": "y",
        "CONFIG_PARTITION_TABLE_CUSTOM_FILENAME": (
            '"partitions_backend_scanner_8mb.csv"'
        ),
        "CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE": "y",
        "CONFIG_SPIRAM_MODE_OCT": "y",
        "CONFIG_BT_ENABLED": "y",
        "CONFIG_BT_NIMBLE_ENABLED": "y",
        "CONFIG_ESP_WIFI_ENABLED": "y",
    },
    "scanner/sdkconfig.scanner-s3-combo-fullsize-backend": {
        "CONFIG_ESPTOOLPY_FLASHSIZE_16MB": "y",
        "CONFIG_ESPTOOLPY_FLASHSIZE": '"16MB"',
        "CONFIG_ESPTOOLPY_FLASHMODE_QIO": "y",
        "CONFIG_ESPTOOLPY_FLASHMODE": '"dio"',
        "CONFIG_ESPTOOLPY_FLASHFREQ_80M": "y",
        "CONFIG_PARTITION_TABLE_CUSTOM": "y",
        "CONFIG_PARTITION_TABLE_CUSTOM_FILENAME": (
            '"partitions_backend_scanner_fullsize_16mb.csv"'
        ),
        "CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE": "y",
        "CONFIG_SPIRAM_MODE_OCT": "y",
        "CONFIG_BT_ENABLED": "y",
        "CONFIG_BT_NIMBLE_ENABLED": "y",
        "CONFIG_ESP_WIFI_ENABLED": "y",
    },
    "uplink/sdkconfig.uplink-s3-backend": {
        "CONFIG_ESPTOOLPY_FLASHSIZE_8MB": "y",
        "CONFIG_ESPTOOLPY_FLASHSIZE": '"8MB"',
        "CONFIG_ESPTOOLPY_FLASHMODE_DIO": "y",
        "CONFIG_ESPTOOLPY_FLASHMODE": '"dio"',
        "CONFIG_ESPTOOLPY_FLASHFREQ_80M": "y",
        "CONFIG_PARTITION_TABLE_CUSTOM": "y",
        "CONFIG_PARTITION_TABLE_CUSTOM_FILENAME": (
            '"partitions_backend_uplink_8mb.csv"'
        ),
        "CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE": "y",
        "CONFIG_SPIRAM_MODE_OCT": "y",
        "CONFIG_BT_ENABLED": "n",
        "CONFIG_BT_NIMBLE_ENABLED": "n",
        "CONFIG_ESP_WIFI_ENABLED": "y",
    },
    "uplink/sdkconfig.uplink-s3-fullsize-backend": {
        "CONFIG_ESPTOOLPY_FLASHSIZE_16MB": "y",
        "CONFIG_ESPTOOLPY_FLASHSIZE": '"16MB"',
        "CONFIG_ESPTOOLPY_FLASHMODE_QIO": "y",
        "CONFIG_ESPTOOLPY_FLASHMODE": '"dio"',
        "CONFIG_ESPTOOLPY_FLASHFREQ_80M": "y",
        "CONFIG_PARTITION_TABLE_CUSTOM": "y",
        "CONFIG_PARTITION_TABLE_CUSTOM_FILENAME": (
            '"partitions_backend_uplink_fullsize_16mb.csv"'
        ),
        "CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE": "y",
        "CONFIG_SPIRAM_MODE_OCT": "y",
        "CONFIG_BT_ENABLED": "n",
        "CONFIG_BT_NIMBLE_ENABLED": "n",
        "CONFIG_ESP_WIFI_ENABLED": "y",
    },
}


def _write_generated_configs(
    root: Path,
    override: tuple[str, str, str] | None = None,
    omit_disabled: bool = False,
) -> None:
    for relative_path, values in VALID_GENERATED.items():
        path = root / relative_path
        path.parent.mkdir(parents=True, exist_ok=True)
        lines = []
        for key, original in values.items():
            value = (
                override[2]
                if override is not None
                and override[:2] == (relative_path, key)
                else original
            )
            if omit_disabled and value == "n":
                continue
            lines.append(
                f"# {key} is not set" if value == "n" else f"{key}={value}"
            )
        path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def _run_verifier(root: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, str(VERIFIER), "--root", str(root)],
        check=False,
        capture_output=True,
        text=True,
    )


def test_verifier_rejects_a_clean_tree_without_all_generated_sdkconfigs(
    tmp_path: Path,
) -> None:
    result = _run_verifier(tmp_path)

    assert result.returncode == 1
    for relative_path in (
        "scanner/sdkconfig.scanner-s3-combo-backend",
        "scanner/sdkconfig.scanner-s3-combo-fullsize-backend",
        "uplink/sdkconfig.uplink-s3-backend",
        "uplink/sdkconfig.uplink-s3-fullsize-backend",
    ):
        assert f"missing generated sdkconfig: {relative_path}" in result.stderr


@pytest.mark.parametrize(
    ("relative_path", "key"),
    [
        (relative_path, key)
        for relative_path, values in VALID_GENERATED.items()
        for key in values
    ],
)
def test_verifier_rejects_every_wrong_generated_sdkconfig_value(
    tmp_path: Path,
    relative_path: str,
    key: str,
) -> None:
    _write_generated_configs(tmp_path, (relative_path, key, "wrong"))

    result = _run_verifier(tmp_path)

    assert result.returncode == 1
    assert (
        f"generated sdkconfig mismatch: {relative_path}: {key}: "
        f"expected {VALID_GENERATED[relative_path][key]}, got wrong"
    ) in result.stderr


def test_verifier_accepts_all_four_exact_generated_sdkconfigs(
    tmp_path: Path,
) -> None:
    _write_generated_configs(tmp_path)

    result = _run_verifier(tmp_path)

    assert result.returncode == 0, result.stderr


def test_verifier_accepts_omitted_kconfig_keys_as_disabled_only_when_expected(
    tmp_path: Path,
) -> None:
    _write_generated_configs(tmp_path, omit_disabled=True)

    result = _run_verifier(tmp_path)

    assert result.returncode == 0, result.stderr
