from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import stat
import struct
import subprocess
import sys

import pytest

from tools import pio_verify_backend_build as pio_hook
from tools.tests.test_firmware_identity import fake_app_image
from tools.pio_verify_backend_build import verify_post_build_image
from tools.verify_backend_build import (
    BACKEND_RELEASES,
    BuildVerificationError,
    EXPECTED_PART_OFFSETS,
    expected_packaged_parts,
    verify_artifact_set,
    verify_packaged_target,
    verify_release_pair,
)


VERSION = "0.1.0-backend"
FLASH_SIZE = 0x800000
PARTITION_BINARY_SIZE = 0xC00
PARTITION_ENTRY = struct.Struct("<HBBII16sI")

EXPECTED_COMMON_PARTITIONS = {
    "nvs": ("data", "nvs", 0x9000, 0x6000),
    "otadata": ("data", "ota", 0xF000, 0x2000),
    "phy_init": ("data", "phy", 0x11000, 0x1000),
    "ota_0": ("app", "ota_0", 0x20000, 0x200000),
    "ota_1": ("app", "ota_1", 0x220000, 0x200000),
}
EXPECTED_SCANNER_TAIL = {
    "storage": ("data", "spiffs", 0x420000, 0x100000),
    "reserved": ("data", "fat", 0x520000, 0x2E0000),
}
EXPECTED_UPLINK_TAIL = {
    "fw_scanner_be": ("data", "0x40", 0x420000, 0x200000),
    "storage": ("data", "spiffs", 0x620000, 0x100000),
    "reserved": ("data", "fat", 0x720000, 0x0E0000),
}


TYPE_VALUES = {"app": 0x00, "data": 0x01}
SUBTYPE_VALUES = {
    ("data", "ota"): 0x00,
    ("data", "phy"): 0x01,
    ("data", "nvs"): 0x02,
    ("data", "fat"): 0x81,
    ("data", "spiffs"): 0x82,
    ("app", "ota_0"): 0x10,
    ("app", "ota_1"): 0x11,
    ("data", "0x40"): 0x40,
}


def spec_for(kind: str) -> dict[str, object]:
    if kind == "scanner":
        return {
            "environment": "scanner-s3-combo-backend",
            "project": "fof_backend_scanner",
            "partitions": {
                **EXPECTED_COMMON_PARTITIONS,
                **EXPECTED_SCANNER_TAIL,
            },
            "partition_csv": "partitions_backend_scanner_8mb.csv",
            "image_kind": 1,
        }
    if kind == "uplink":
        return {
            "environment": "uplink-s3-backend",
            "project": "fof_backend_uplink",
            "partitions": {
                **EXPECTED_COMMON_PARTITIONS,
                **EXPECTED_UPLINK_TAIL,
            },
            "partition_csv": "partitions_backend_uplink_8mb.csv",
            "image_kind": 0,
        }
    raise AssertionError(kind)


def encode_partition_table(partitions: dict[str, tuple[str, str, int, int]]) -> bytes:
    entries = bytearray()
    for label, (part_type, subtype, offset, size) in partitions.items():
        label_bytes = label.encode("ascii")
        entries += PARTITION_ENTRY.pack(
            0x50AA,
            TYPE_VALUES[part_type],
            SUBTYPE_VALUES[(part_type, subtype)],
            offset,
            size,
            label_bytes + bytes(16 - len(label_bytes)),
            0,
        )
    md5_record = b"\xeb\xeb" + b"\xff" * 14 + hashlib.md5(entries).digest()
    table = entries + md5_record
    return bytes(table + b"\xff" * (PARTITION_BINARY_SIZE - len(table)))


def write_partition_csv(
    path: Path, partitions: dict[str, tuple[str, str, int, int]]
) -> None:
    lines = [
        f"{label},{part_type},{subtype},{offset:#x},{size:#x},"
        for label, (part_type, subtype, offset, size) in partitions.items()
    ]
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def sdkconfig_text(kind: str, partition_csv: str) -> str:
    common = [
        "CONFIG_ESPTOOLPY_FLASHSIZE_8MB=y",
        'CONFIG_ESPTOOLPY_FLASHSIZE="8MB"',
        "CONFIG_PARTITION_TABLE_CUSTOM=y",
        f'CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="{partition_csv}"',
        "CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y",
        "CONFIG_SPIRAM=y",
        "CONFIG_SPIRAM_MODE_OCT=y",
        "CONFIG_ESP_WIFI_ENABLED=y",
    ]
    if kind == "scanner":
        common += [
            "CONFIG_FOF_BACKEND_GLASSES_DETECTION=y",
            "CONFIG_BT_ENABLED=y",
            "CONFIG_BT_NIMBLE_ENABLED=y",
            "CONFIG_BT_NIMBLE_ROLE_OBSERVER=y",
            "CONFIG_BT_NIMBLE_ROLE_CENTRAL=y",
        ]
    else:
        common += [
            "CONFIG_BT_ENABLED=n",
            "CONFIG_BT_BLUEDROID_ENABLED=n",
            "CONFIG_BT_NIMBLE_ENABLED=n",
        ]
    return "\n".join(common) + "\n"


def make_fake_build(tmp_path: Path, *, kind: str) -> Path:
    spec = spec_for(kind)
    environment = str(spec["environment"])
    project = str(spec["project"])
    partition_csv_name = str(spec["partition_csv"])
    project_root = tmp_path / "repo" / "backend-firmware" / kind
    build = project_root / ".pio" / "build" / environment
    build.mkdir(parents=True)

    partitions = spec["partitions"]
    assert isinstance(partitions, dict)
    write_partition_csv(project_root / partition_csv_name, partitions)
    (project_root / f"sdkconfig.{environment}").write_text(
        sdkconfig_text(kind, partition_csv_name), encoding="utf-8"
    )

    firmware = fake_app_image(
        project,
        image_kind=int(spec["image_kind"]),
        target=environment,
    )
    (build / "bootloader.bin").write_bytes(b"boot" * 64)
    (build / "partitions.bin").write_bytes(encode_partition_table(partitions))
    (build / "ota_data_initial.bin").write_bytes(b"\xff" * 0x2000)
    (build / "firmware.bin").write_bytes(firmware)
    (build / f"{project}.bin").write_bytes(firmware)

    description = {
        "project_name": project,
        "project_version": VERSION,
        "project_path": str(project_root.resolve()),
        "build_dir": str(build.resolve()),
        "target": "esp32s3",
        "app_bin": f"{project}.bin",
        "build_components": ["main", "freertos"],
        "build_component_paths": [
            str((project_root / "main").resolve()),
            "/opt/platformio/framework-espidf/components/freertos",
        ],
        "all_component_info": {
            "main": {"dir": str((project_root / "main").resolve())},
            "freertos": {
                "dir": "/opt/platformio/framework-espidf/components/freertos"
            },
        },
    }
    (build / "project_description.json").write_text(
        json.dumps(description), encoding="utf-8"
    )

    flash_files = {
        "0x0": "bootloader/bootloader.bin",
        "0x8000": "partition_table/partition-table.bin",
        "0xf000": "ota_data_initial.bin",
        "0x20000": f"{project}.bin",
    }
    flasher = {
        "write_flash_args": [
            "--flash_mode",
            "dio",
            "--flash_size",
            "8MB",
            "--flash_freq",
            "80m",
        ],
        "flash_settings": {
            "flash_mode": "dio",
            "flash_size": "8MB",
            "flash_freq": "80m",
        },
        "flash_files": flash_files,
        "extra_esptool_args": {"chip": "esp32s3"},
    }
    (build / "flasher_args.json").write_text(json.dumps(flasher), encoding="utf-8")
    return build


def project_root(build: Path) -> Path:
    return build.parents[2]


def update_json(path: Path, callback) -> None:
    value = json.loads(path.read_text(encoding="utf-8"))
    callback(value)
    path.write_text(json.dumps(value), encoding="utf-8")


def refresh_partition_md5(table: bytearray, entry_count: int) -> None:
    entries_end = entry_count * PARTITION_ENTRY.size
    md5_offset = entries_end + 16
    table[md5_offset : md5_offset + 16] = hashlib.md5(table[:entries_end]).digest()


def mutate_fake_build(build: Path, mutation: str, *, kind: str = "uplink") -> None:
    spec = spec_for(kind)
    root = project_root(build)
    project = str(spec["project"])
    sdkconfig = root / f"sdkconfig.{spec['environment']}"
    if mutation in {"partition_offset", "partition_label"}:
        table_path = build / "partitions.bin"
        table = bytearray(table_path.read_bytes())
        if mutation == "partition_offset":
            struct.pack_into("<I", table, 3 * PARTITION_ENTRY.size + 4, 0x21000)
        else:
            label_offset = 3 * PARTITION_ENTRY.size + 12
            table[label_offset : label_offset + 16] = b"bad_ota\0" + bytes(8)
        partition_count = len(spec["partitions"])  # type: ignore[arg-type]
        refresh_partition_md5(table, partition_count)
        table_path.write_bytes(table)
    elif mutation == "rollback_disabled":
        sdkconfig.write_text(
            sdkconfig.read_text().replace(
                "CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y",
                "CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=n",
            ),
            encoding="utf-8",
        )
    elif mutation == "wrong_flash_size":
        sdkconfig.write_text(
            sdkconfig.read_text().replace(
                "CONFIG_ESPTOOLPY_FLASHSIZE_8MB=y",
                "CONFIG_ESPTOOLPY_FLASHSIZE_8MB=n",
            ),
            encoding="utf-8",
        )
    elif mutation == "wrong_project":
        update_json(
            build / "project_description.json",
            lambda data: data.__setitem__("project_name", "fof_badge_scanner"),
        )
    elif mutation == "wrong_flasher_offset":
        def wrong_offset(data):
            app = data["flash_files"].pop("0x20000")
            data["flash_files"]["0x21000"] = app

        update_json(build / "flasher_args.json", wrong_offset)
    elif mutation == "wrong_flash_mode":
        update_json(
            build / "flasher_args.json",
            lambda data: data["flash_settings"].__setitem__("flash_mode", "qio"),
        )
    elif mutation == "app_alias_differs":
        (build / f"{project}.bin").write_bytes(b"different")
    elif mutation == "ota_data_not_initial":
        ota = bytearray((build / "ota_data_initial.bin").read_bytes())
        ota[0] = 0
        (build / "ota_data_initial.bin").write_bytes(ota)
    elif mutation == "bootloader_crosses_partition_table":
        (build / "bootloader.bin").write_bytes(bytes(0x8001))
    elif mutation == "partition_table_wrong_size":
        (build / "partitions.bin").write_bytes(
            (build / "partitions.bin").read_bytes()[:-1]
        )
    elif mutation == "part_intersects_nvs":
        (build / "bootloader.bin").write_bytes(bytes(0x9001))
    elif mutation == "ota_data_wrong_size":
        (build / "ota_data_initial.bin").write_bytes(b"\xff" * 0x1FFF)
    elif mutation == "application_exceeds_slot":
        oversized = bytes(0x200001)
        (build / "firmware.bin").write_bytes(oversized)
        (build / f"{project}.bin").write_bytes(oversized)
    elif mutation == "part_overlap":
        def overlap(data):
            ota = data["flash_files"].pop("0xf000")
            data["flash_files"]["0x8000"] = ota

        update_json(build / "flasher_args.json", overlap)
    elif mutation == "range_end_overflow":
        def overflow(data):
            boot = data["flash_files"].pop("0x0")
            data["flash_files"][str(1 << 130)] = boot

        update_json(build / "flasher_args.json", overflow)
    elif mutation == "flash_end_exceeds_8mb":
        def past_flash(data):
            boot = data["flash_files"].pop("0x0")
            data["flash_files"]["0x7fffff"] = boot

        update_json(build / "flasher_args.json", past_flash)
    elif mutation == "component_outside_backend_firmware":
        def outside(data):
            repo_root = root.parent.parent
            data["build_component_paths"].append(str(repo_root / "esp32" / "shared"))

        update_json(build / "project_description.json", outside)
    else:
        raise AssertionError(f"unknown mutation: {mutation}")


def verifier_args(build: Path, kind: str) -> dict[str, object]:
    spec = spec_for(kind)
    root = project_root(build)
    return {
        "kind": kind,
        "version": VERSION,
        "partition_csv": root / str(spec["partition_csv"]),
        "sdkconfig": root / f"sdkconfig.{spec['environment']}",
    }


def test_release_specs_and_offsets_are_exact() -> None:
    assert EXPECTED_PART_OFFSETS == {
        "bootloader": 0x0000,
        "partition-table": 0x8000,
        "ota-data-initial": 0xF000,
        "firmware": 0x20000,
    }
    assert set(BACKEND_RELEASES) == {"scanner", "uplink"}
    assert BACKEND_RELEASES["scanner"].target == "scanner-s3-combo-backend"
    assert BACKEND_RELEASES["uplink"].target == "uplink-s3-backend"


@pytest.mark.parametrize("kind", ["scanner", "uplink"])
def test_release_set_names_offsets_and_identity_are_exact(
    tmp_path: Path, kind: str
) -> None:
    build = make_fake_build(tmp_path, kind=kind)
    release = verify_artifact_set(build, **verifier_args(build, kind))
    target = str(spec_for(kind)["environment"])

    assert {part.name: part.offset for part in release.parts} == (
        expected_packaged_parts(target)
    )
    assert release.target == target
    assert release.project == spec_for(kind)["project"]
    assert release.hardware == "seeed_xiao_esp32s3"
    assert release.artifact_directory == target
    assert release.partition_capacity == 0x200000
    assert [part.offset for part in release.parts] == sorted(
        part.offset for part in release.parts
    )
    assert next(
        part.size
        for part in release.parts
        if part.name.endswith("-partition-table.bin")
    ) == 0x1000
    assert all(part.source_path.parent == build for part in release.parts)
    assert all(part.name != part.source_path.name for part in release.parts)


@pytest.mark.parametrize(
    ("filename", "message"),
    [
        ("bootloader.bin", "bootloader"),
        ("partitions.bin", "partition table"),
        ("ota_data_initial.bin", "ota data"),
        ("firmware.bin", "firmware"),
    ],
)
def test_release_set_refuses_each_missing_build_input(
    tmp_path: Path, filename: str, message: str
) -> None:
    build = make_fake_build(tmp_path, kind="scanner")
    (build / filename).unlink()
    with pytest.raises(BuildVerificationError, match=message):
        verify_artifact_set(build, **verifier_args(build, "scanner"))


@pytest.mark.parametrize(
    "mutation",
    [
        "partition_offset",
        "partition_label",
        "rollback_disabled",
        "wrong_flash_size",
        "wrong_project",
        "wrong_flasher_offset",
        "wrong_flash_mode",
        "app_alias_differs",
        "ota_data_not_initial",
        "bootloader_crosses_partition_table",
        "partition_table_wrong_size",
        "part_intersects_nvs",
        "ota_data_wrong_size",
        "application_exceeds_slot",
        "part_overlap",
        "range_end_overflow",
        "flash_end_exceeds_8mb",
        "component_outside_backend_firmware",
    ],
)
def test_release_set_rejects_layout_or_build_metadata_drift(
    tmp_path: Path, mutation: str
) -> None:
    build = make_fake_build(tmp_path, kind="uplink")
    mutate_fake_build(build, mutation)
    with pytest.raises(BuildVerificationError):
        verify_artifact_set(build, **verifier_args(build, "uplink"))


def test_release_set_rejects_wrong_environment_directory(tmp_path: Path) -> None:
    build = make_fake_build(tmp_path, kind="scanner")
    wrong = build.with_name("scanner")
    build.rename(wrong)
    with pytest.raises(BuildVerificationError, match="environment"):
        verify_artifact_set(wrong, **verifier_args(wrong, "scanner"))


def test_release_set_rejects_badge_or_unknown_kind(tmp_path: Path) -> None:
    build = make_fake_build(tmp_path, kind="scanner")
    for kind in ("badge", "generic", ""):
        with pytest.raises(BuildVerificationError, match="kind"):
            verify_artifact_set(build, kind=kind, version=VERSION)


def test_release_set_accepts_idf_trailing_empty_component_sentinel(
    tmp_path: Path,
) -> None:
    build = make_fake_build(tmp_path, kind="scanner")

    def add_sentinel(data):
        data["build_components"].append("")
        data["build_component_paths"].append("")

    update_json(build / "project_description.json", add_sentinel)

    release = verify_artifact_set(build, **verifier_args(build, "scanner"))

    assert release.target == "scanner-s3-combo-backend"


def pair_args(tmp_path: Path) -> tuple[Path, Path, dict[str, object]]:
    scanner = make_fake_build(tmp_path, kind="scanner")
    uplink = make_fake_build(tmp_path, kind="uplink")
    scanner_spec = spec_for("scanner")
    uplink_spec = spec_for("uplink")
    args: dict[str, object] = {
        "scanner_build_dir": scanner,
        "scanner_partition_csv": project_root(scanner)
        / str(scanner_spec["partition_csv"]),
        "scanner_sdkconfig": project_root(scanner)
        / f"sdkconfig.{scanner_spec['environment']}",
        "uplink_build_dir": uplink,
        "uplink_partition_csv": project_root(uplink)
        / str(uplink_spec["partition_csv"]),
        "uplink_sdkconfig": project_root(uplink)
        / f"sdkconfig.{uplink_spec['environment']}",
        "output_dir": tmp_path / "release",
        "index_path": tmp_path / "backend-release-index.json",
        "version": VERSION,
    }
    return scanner, uplink, args


def test_pair_release_is_exact_deterministic_and_contains_no_generic_names(
    tmp_path: Path,
) -> None:
    scanner, uplink, args = pair_args(tmp_path)
    release_index = verify_release_pair(**args)
    output_dir = args["output_dir"]
    index_path = args["index_path"]
    assert isinstance(output_dir, Path)
    assert isinstance(index_path, Path)

    assert release_index["schema"] == 1
    assert release_index["version"] == VERSION
    assert list(release_index["targets"]) == [
        "scanner-s3-combo-backend",
        "uplink-s3-backend",
    ]
    assert stat.S_IMODE(index_path.stat().st_mode) == 0o600
    rendered = index_path.read_text(encoding="utf-8")
    assert rendered == json.dumps(release_index, indent=2, sort_keys=True) + "\n"
    assert "firmware.bin\"" not in rendered.replace(
        "scanner-s3-combo-backend-firmware.bin\"",
        "",
    ).replace("uplink-s3-backend-firmware.bin\"", "")

    builds = {
        "scanner-s3-combo-backend": scanner,
        "uplink-s3-backend": uplink,
    }
    for target, target_data in release_index["targets"].items():
        assert set(target_data) == {
            "kind",
            "target",
            "project",
            "hardware",
            "identity_crc32",
            "partition_capacity",
            "parts",
        }
        target_dir = output_dir / target
        assert sorted(path.name for path in target_dir.iterdir()) == sorted(
            expected_packaged_parts(target)
        )
        parts = target_data["parts"]
        assert [part["offset"] for part in parts] == sorted(
            part["offset"] for part in parts
        )
        for part in parts:
            assert Path(part["path"]).name == part["name"]
            assert Path(part["path"]).parent == Path(target)
            assert part["name"].startswith(f"{target}-")
            packaged = output_dir / part["path"]
            logical = next(
                logical
                for logical in EXPECTED_PART_OFFSETS
                if part["name"] == f"{target}-{logical}.bin"
            )
            source_name = {
                "bootloader": "bootloader.bin",
                "partition-table": "partitions.bin",
                "ota-data-initial": "ota_data_initial.bin",
                "firmware": "firmware.bin",
            }[logical]
            source = (builds[target] / source_name).read_bytes()
            if logical == "partition-table":
                source = source.ljust(0x1000, b"\xff")
            assert packaged.read_bytes() == source
            assert part["size"] == len(source)


def test_pair_check_only_performs_no_writes(tmp_path: Path) -> None:
    _scanner, _uplink, args = pair_args(tmp_path)
    args["check_only"] = True

    release_index = verify_release_pair(**args)

    assert release_index["schema"] == 1
    assert not Path(args["output_dir"]).exists()
    assert not Path(args["index_path"]).exists()


def test_pair_validates_both_targets_before_copying_either(tmp_path: Path) -> None:
    _scanner, uplink, args = pair_args(tmp_path)
    (uplink / "ota_data_initial.bin").write_bytes(b"bad")

    with pytest.raises(BuildVerificationError):
        verify_release_pair(**args)

    assert not Path(args["output_dir"]).exists()
    assert not Path(args["index_path"]).exists()


def test_pair_index_is_byte_deterministic_across_output_roots(tmp_path: Path) -> None:
    _scanner, _uplink, args = pair_args(tmp_path)
    args["check_only"] = True
    first = verify_release_pair(**args)
    second = verify_release_pair(**args)

    assert json.dumps(first, indent=2, sort_keys=True) == json.dumps(
        second, indent=2, sort_keys=True
    )


@pytest.mark.parametrize("mutation", ["non_ff_tail", "wrong_published_size"])
def test_published_partition_table_requires_exact_ff_padded_flash_span(
    tmp_path: Path, mutation: str
) -> None:
    scanner, _uplink, args = pair_args(tmp_path)
    verify_release_pair(**args)
    release = verify_artifact_set(scanner, **verifier_args(scanner, "scanner"))
    target_dir = Path(args["output_dir"]) / release.target
    partition_path = target_dir / f"{release.target}-partition-table.bin"
    payload = bytearray(partition_path.read_bytes())
    assert len(payload) == 0x1000
    assert payload[0xC00:] == b"\xff" * 0x400
    if mutation == "non_ff_tail":
        payload[0xC00] = 0
    else:
        payload.pop()
    partition_path.write_bytes(payload)

    with pytest.raises(BuildVerificationError, match="partition table"):
        verify_packaged_target(target_dir, release)


def test_pair_refuses_existing_target_directory_without_overwriting(
    tmp_path: Path,
) -> None:
    _scanner, _uplink, args = pair_args(tmp_path)
    existing = Path(args["output_dir"]) / "scanner-s3-combo-backend"
    existing.mkdir(parents=True)
    sentinel = existing / "keep.txt"
    sentinel.write_text("keep", encoding="utf-8")

    with pytest.raises(BuildVerificationError, match="already exists"):
        verify_release_pair(**args)

    assert sentinel.read_text(encoding="utf-8") == "keep"
    assert not Path(args["index_path"]).exists()


def test_expected_packaged_parts_rejects_non_backend_target() -> None:
    for target in ("firmware", "badge", "scanner"):
        with pytest.raises(BuildVerificationError):
            expected_packaged_parts(target)


@pytest.mark.parametrize("kind", ["scanner", "uplink"])
def test_platformio_post_build_verifies_image_and_materializes_project_alias(
    tmp_path: Path, kind: str
) -> None:
    build = make_fake_build(tmp_path, kind=kind)
    spec = spec_for(kind)
    alias = build / f"{spec['project']}.bin"
    alias.unlink()

    verified = verify_post_build_image(
        build_dir=build,
        environment=str(spec["environment"]),
        project_dir=project_root(build),
        version=VERSION,
    )

    assert verified.target == spec["environment"]
    assert alias.read_bytes() == (build / "firmware.bin").read_bytes()


def test_platformio_post_build_does_not_alias_an_invalid_image(tmp_path: Path) -> None:
    build = make_fake_build(tmp_path, kind="scanner")
    alias = build / "fof_backend_scanner.bin"
    alias.unlink()
    image = bytearray((build / "firmware.bin").read_bytes())
    image[-1] ^= 1
    (build / "firmware.bin").write_bytes(image)

    with pytest.raises(BuildVerificationError, match="post-build identity"):
        verify_post_build_image(
            build_dir=build,
            environment="scanner-s3-combo-backend",
            project_dir=project_root(build),
            version=VERSION,
        )

    assert not alias.exists()


def test_platformio_post_build_rejects_non_backend_environment(tmp_path: Path) -> None:
    build = make_fake_build(tmp_path, kind="scanner")
    with pytest.raises(BuildVerificationError, match="environment"):
        verify_post_build_image(
            build_dir=build,
            environment="badge",
            project_dir=project_root(build),
            version=VERSION,
        )


def test_both_platformio_environments_install_only_the_local_post_build_hook() -> None:
    root = Path(__file__).resolve().parents[2]
    for kind in ("scanner", "uplink"):
        text = (root / kind / "platformio.ini").read_text(encoding="utf-8")
        assert text.count("post:../tools/pio_verify_backend_build.py") == 1
        assert "fof_badge" not in text.lower()


def test_platformio_hook_bootstraps_sibling_imports_in_scons_exec_context() -> None:
    root = Path(__file__).resolve().parents[2]
    script = root / "tools" / "pio_verify_backend_build.py"
    code = """
from pathlib import Path
import sys
import types

class Environment:
    def subst(self, value):
        if value == "$PROJECT_DIR":
            return sys.argv[2]
        return value

    def AddPostAction(self, *_args):
        pass

path = Path(sys.argv[1])
tools_namespace = types.ModuleType("tools")
tools_namespace.__path__ = []
sys.modules["tools"] = tools_namespace
scope = {"env": Environment()}
scope["Import"] = lambda _name: None
exec(compile(path.read_bytes(), str(path), "exec"), scope)
"""

    result = subprocess.run(
        [sys.executable, "-c", code, str(script), str(root / "scanner")],
        cwd=root / "scanner",
        capture_output=True,
        text=True,
    )

    assert result.returncode == 0, result.stderr


def test_platformio_callback_accepts_scons_keyword_contract(tmp_path: Path) -> None:
    build = make_fake_build(tmp_path, kind="scanner")

    class Environment:
        def subst(self, value: str) -> str:
            return {
                "$BUILD_DIR": str(build),
                "$PIOENV": "scanner-s3-combo-backend",
                "$PROJECT_DIR": str(project_root(build)),
            }[value]

    pio_hook._platformio_post_action(
        source=[build / "firmware.bin"],
        target=[build / "firmware.elf"],
        env=Environment(),
    )
