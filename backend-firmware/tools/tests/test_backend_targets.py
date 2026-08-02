"""Target-matrix contracts for the isolated backend firmware family."""

from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[2]
TARGETS_PATH = ROOT / "tools/backend_targets.py"
PIO = Path.home() / ".platformio/penv/bin/pio"


def _targets_module():
    spec = importlib.util.spec_from_file_location("backend_targets", TARGETS_PATH)
    assert spec is not None and spec.loader is not None, "missing backend target registry"
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def test_target_matrix_has_only_exact_disjoint_backend_identities() -> None:
    """Catches a target typo or a backend target that can collide with Badge."""
    targets = _targets_module().BACKEND_TARGETS

    assert set(targets) == {
        "uplink-s3-backend",
        "scanner-s3-combo-backend",
        "uplink-s3-fullsize-backend",
        "scanner-s3-combo-fullsize-backend",
    }
    assert targets == {
        "uplink-s3-backend": {
            "family": "badge_lite", "component": "uplink",
            "project": "fof_backend_uplink", "hardware": "seeed_xiao_esp32s3",
            "flash_bytes": 8 * 1024 * 1024, "app_capacity": 2 * 1024 * 1024,
            "cache_capacity": 2 * 1024 * 1024, "default": True,
        },
        "scanner-s3-combo-backend": {
            "family": "badge_lite", "component": "scanner",
            "project": "fof_backend_scanner", "hardware": "seeed_xiao_esp32s3",
            "flash_bytes": 8 * 1024 * 1024, "app_capacity": 2 * 1024 * 1024,
            "cache_capacity": None, "default": True,
        },
        "uplink-s3-fullsize-backend": {
            "family": "s3_fullsize", "component": "uplink",
            "project": "fof_backend_uplink_fullsize", "hardware": "esp32s3_n16r8_fullsize",
            "flash_bytes": 16 * 1024 * 1024, "app_capacity": 2 * 1024 * 1024,
            "cache_capacity": 3 * 1024 * 1024, "default": False,
        },
        "scanner-s3-combo-fullsize-backend": {
            "family": "s3_fullsize", "component": "scanner",
            "project": "fof_backend_scanner_fullsize", "hardware": "esp32s3_n16r8_fullsize",
            "flash_bytes": 16 * 1024 * 1024, "app_capacity": 3 * 1024 * 1024,
            "cache_capacity": None, "default": False,
        },
    }
    assert len({(target, spec["project"]) for target, spec in targets.items()}) == 4
    assert all("fof_badge" not in target for target in targets)
    assert all(
        not value.replace("\\", "/").startswith("esp32/")
        for spec in targets.values()
        for value in spec.values()
        if isinstance(value, str)
    )


def test_platformio_native_profiles_select_exactly_one_hardware_profile() -> None:
    """Catches inherited PlatformIO flags that select zero or both board profiles."""
    assert PIO.is_file(), f"PlatformIO executable not found: {PIO}"
    result = subprocess.run(
        [str(PIO), "project", "config", "--json-output"],
        cwd=ROOT,
        check=True,
        capture_output=True,
        text=True,
    )
    config = {section: dict(entries) for section, entries in json.loads(result.stdout)}
    expected = {
        "env:backend-native": "-DFOF_BACKEND_PROFILE_BADGE_LITE=1",
        "env:backend-native-fullsize": "-DFOF_BACKEND_PROFILE_S3_FULLSIZE=1",
    }
    for environment, selected_profile in expected.items():
        flags = config[environment]["build_flags"]
        profiles = [flag for flag in flags if flag.startswith("-DFOF_BACKEND_PROFILE_")]
        assert profiles == [selected_profile]


def _profile_macros(profile_define: str) -> dict[str, str]:
    compiler = shutil.which("cc")
    assert compiler is not None, "C compiler not found"
    result = subprocess.run(
        [
            compiler,
            "-dM",
            "-E",
            "-I",
            str(ROOT / "shared"),
            f"-D{profile_define}",
            "-include",
            "backend_hardware_profile.h",
            "-",
        ],
        input="",
        capture_output=True,
        text=True,
        check=True,
    )
    return {
        parts[1]: " ".join(parts[2:])
        for line in result.stdout.splitlines()
        if line.startswith("#define ")
        if len(parts := line.split()) >= 3
    }


def test_c_hardware_profiles_match_host_target_registry() -> None:
    """Catches a C profile identity or capacity drifting from management metadata."""
    targets = _targets_module().BACKEND_TARGETS
    profile_targets = {
        "FOF_BACKEND_PROFILE_BADGE_LITE=1": [
            "uplink-s3-backend", "scanner-s3-combo-backend"
        ],
        "FOF_BACKEND_PROFILE_S3_FULLSIZE=1": [
            "uplink-s3-fullsize-backend", "scanner-s3-combo-fullsize-backend"
        ],
    }
    for profile, names in profile_targets.items():
        macros = _profile_macros(profile)
        uplink, scanner = (targets[name] for name in names)
        assert macros["FOF_BACKEND_PRODUCT_FAMILY"] == f'"{uplink["family"]}"'
        assert macros["FOF_BACKEND_HARDWARE"] == f'"{uplink["hardware"]}"'
        assert macros["FOF_BACKEND_UPLINK_TARGET"] == f'"{names[0]}"'
        assert macros["FOF_BACKEND_UPLINK_PROJECT"] == f'"{uplink["project"]}"'
        assert macros["FOF_BACKEND_SCANNER_TARGET"] == f'"{names[1]}"'
        assert macros["FOF_BACKEND_SCANNER_PROJECT"] == f'"{scanner["project"]}"'
        assert macros["FOF_BACKEND_FLASH_SIZE_BYTES"] == f"UINT32_C(0x{uplink['flash_bytes']:x})"
        assert macros["FOF_BACKEND_UPLINK_APP_CAPACITY"] == f"UINT32_C(0x{uplink['app_capacity']:x})"
        assert macros["FOF_BACKEND_SCANNER_OTA_CAPACITY"] == f"UINT32_C(0x{scanner['app_capacity']:x})"
        assert macros["FOF_BACKEND_SCANNER_CACHE_CAPACITY"] == f"UINT32_C(0x{uplink['cache_capacity']:x})"
