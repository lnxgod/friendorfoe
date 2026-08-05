import hashlib
import json
import os
from html.parser import HTMLParser
from pathlib import Path, PurePosixPath
import shutil
import subprocess
import sys
import textwrap
import zlib

import pytest


BACKEND_FW = Path(__file__).resolve().parents[2]
REPO_ROOT = BACKEND_FW.parent
FLASHER = BACKEND_FW / "web-flasher"
RELEASE_INDEX = BACKEND_FW / "release/backend-release-index.json"
EXPECTED_OFFSETS = [0, 32768, 61440, 131072]
TARGETS = {
    "uplink-s3-backend": {
        "kind": "uplink",
        "project": "fof_backend_uplink",
        "manifest": "manifest-uplink-s3-backend.json",
        "title": "Friend or Foe Backend Uplink (XIAO ESP32-S3)",
    },
}
LOGICAL_PARTS = (
    "bootloader",
    "partition-table",
    "ota-data-initial",
    "firmware",
)
FORBIDDEN_REFERENCES = (
    "esp32/web-flasher",
    "esp32/scanner",
    "esp32/uplink",
    "fof_badge",
    "badge-scanner",
    "badge-uplink",
    "scanner-s3-combo-seed",
    "scanner-s3-combo-backend",
    "fof_backend_scanner",
    "manifest-scanner",
    "uplink-s3/",
)
PACKAGE_CHILD = os.environ.get("FOF_BUILD_SCRIPT_CHILD") == "1"
PACKAGE_CHILD_EXPECTED = (
    os.environ.get("FOF_EXPECT_BUILD_SCRIPT_CHILD") == "1"
)


def _expected_names(target: str) -> list[str]:
    return [f"{target}-{logical}.bin" for logical in LOGICAL_PARTS]


def _text_assets(root: Path) -> str:
    paths = sorted(
        path
        for path in root.rglob("*")
        if path.is_file() and path.suffix in {".html", ".json", ".sh"}
    )
    return "\n".join(path.read_text(encoding="utf-8") for path in paths)


def _assert_safe_part_path(relative: str, target: str) -> None:
    path = PurePosixPath(relative)
    assert not path.is_absolute()
    assert "\\" not in relative
    assert relative == path.as_posix()
    assert "." not in path.parts
    assert ".." not in path.parts
    assert path.parts[:2] == ("firmware", target)
    assert len(path.parts) == 3
    expected_root = (FLASHER / "firmware" / target).resolve()
    resolved = (FLASHER / Path(*path.parts)).resolve()
    assert resolved.parent == expected_root


@pytest.mark.parametrize(
    ("target", "spec"),
    TARGETS.items(),
)
def test_manifest_is_backend_named_and_complete(target: str, spec: dict):
    manifest = json.loads(
        (FLASHER / spec["manifest"]).read_text(encoding="utf-8")
    )

    assert manifest["name"] == spec["title"]
    assert manifest["version"] == "0.2.0-backend"
    assert len(manifest["builds"]) == 1
    assert manifest["builds"][0]["chipFamily"] == "ESP32-S3"
    parts = manifest["builds"][0]["parts"]
    assert [part["offset"] for part in parts] == EXPECTED_OFFSETS
    assert [Path(part["path"]).name for part in parts] == _expected_names(
        target
    )
    for part in parts:
        _assert_safe_part_path(part["path"], target)
        assert "backend" in part["path"]


def test_backend_flasher_never_references_protected_firmware():
    assert FLASHER.is_dir()
    text = _text_assets(FLASHER)

    for forbidden in FORBIDDEN_REFERENCES:
        assert forbidden not in text


def test_backend_flasher_text_scan_ignores_binary_artifacts(tmp_path: Path):
    copied = tmp_path / "web-flasher"
    shutil.copytree(FLASHER, copied)
    for target in TARGETS:
        firmware = (
            copied
            / "firmware"
            / target
            / f"{target}-firmware.bin"
        )
        firmware.parent.mkdir(parents=True, exist_ok=True)
        firmware.write_bytes(b"\xff\xfe\x80not-utf8")

    text = _text_assets(copied)

    for forbidden in FORBIDDEN_REFERENCES:
        assert forbidden not in text


@pytest.mark.skipif(
    PACKAGE_CHILD or PACKAGE_CHILD_EXPECTED,
    reason="the child package run has materialized the uplink target directory",
)
def test_source_tree_keeps_only_the_parent_firmware_placeholder():
    firmware = FLASHER / "firmware"

    assert (firmware / ".gitkeep").is_file()
    assert [
        path.relative_to(firmware).as_posix()
        for path in firmware.rglob(".gitkeep")
    ] == [".gitkeep"]


class _FlasherPageParser(HTMLParser):
    def __init__(self) -> None:
        super().__init__()
        self.script_sources: list[str] = []
        self.manifests: list[str] = []
        self.text: list[str] = []

    def handle_starttag(self, tag: str, attrs: list[tuple[str, str | None]]):
        values = dict(attrs)
        if tag == "script" and values.get("src"):
            self.script_sources.append(values["src"] or "")
        if tag == "esp-web-install-button":
            self.manifests.append(values.get("manifest") or "")

    def handle_data(self, data: str):
        self.text.append(data)


def test_static_page_has_only_the_pinned_backend_uplink_choice():
    parser = _FlasherPageParser()
    parser.feed((FLASHER / "index.html").read_text(encoding="utf-8"))
    visible_text = " ".join(" ".join(parser.text).split())

    assert parser.script_sources == [
        "https://unpkg.com/esp-web-tools@10.4.0/dist/web/install-button.js?module"
    ]
    assert parser.manifests == ["manifest-uplink-s3-backend.json"]
    assert "Backend Uplink" in visible_text
    assert "Backend Scanner" not in visible_text
    assert (
        "UNPUBLISHED BACKEND RECOVERY/MAINTENANCE TOOL — NOT THE INITIAL "
        "CANARY PATH."
    ) in visible_text
    assert (
        "This page flashes only the screenless Lite uplink. It cannot "
        "distinguish that board from a badge because both use XIAO ESP32-S3."
    ) in visible_text
    assert (
        "Do not connect or select a badge or scanner. Production ComboFO "
        "scanner firmware is intentionally not offered here."
    ) in visible_text


def _validate_release_index(index: dict, *, require_artifacts: bool) -> None:
    assert index["schema"] == 1
    assert index["version"] == "0.2.0-backend"
    assert set(index["targets"]) == set(TARGETS)
    for target, spec in TARGETS.items():
        release = index["targets"][target]
        assert release["kind"] == spec["kind"]
        assert release["target"] == target
        assert release["project"] == spec["project"]
        assert release["hardware"] == "seeed_xiao_esp32s3"
        assert isinstance(release["identity_crc32"], int)
        assert release["partition_capacity"] == 2_097_152
        parts = release["parts"]
        assert [part["offset"] for part in parts] == EXPECTED_OFFSETS
        assert [part["name"] for part in parts] == _expected_names(target)
        for part in parts:
            relative = PurePosixPath(part["path"])
            assert relative.parts == (target, part["name"])
            artifact = FLASHER / "firmware" / Path(*relative.parts)
            if not artifact.exists():
                assert not require_artifacts
                continue
            data = artifact.read_bytes()
            assert part["size"] == len(data) > 0
            assert part["sha256"] == hashlib.sha256(data).hexdigest()
            assert part["crc32"] == zlib.crc32(data) & 0xFFFFFFFF


def test_release_index_is_strictly_backend_only_when_present():
    required = os.environ.get("FOF_REQUIRE_BACKEND_RELEASE_INDEX") == "1"
    if not RELEASE_INDEX.exists():
        assert not required, "backend release index is required"
        return

    _validate_release_index(
        json.loads(RELEASE_INDEX.read_text(encoding="utf-8")),
        require_artifacts=required,
    )


@pytest.mark.skipif(
    not PACKAGE_CHILD_EXPECTED,
    reason="only the copied package harness expects a child-phase marker",
)
def test_build_script_marks_the_packaged_child_test_phase():
    assert PACKAGE_CHILD


@pytest.mark.skipif(
    PACKAGE_CHILD or PACKAGE_CHILD_EXPECTED,
    reason="child package test runs the remaining flasher contract",
)
def test_backend_binary_outputs_are_ignored_but_index_is_trackable():
    binary = (
        "backend-firmware/web-flasher/firmware/uplink-s3-backend/"
        "uplink-s3-backend-firmware.bin"
    )
    ignored = subprocess.run(
        ["git", "check-ignore", "--quiet", "--", binary],
        cwd=REPO_ROOT,
    )
    index = subprocess.run(
        [
            "git",
            "check-ignore",
            "--quiet",
            "--",
            "backend-firmware/release/backend-release-index.json",
        ],
        cwd=REPO_ROOT,
    )

    assert ignored.returncode == 0
    assert index.returncode == 1


FAKE_VERIFIER = r'''#!/usr/bin/env python3
import argparse
import hashlib
import json
import os
from pathlib import Path
import zlib

parser = argparse.ArgumentParser()
parser.add_argument("command")
parser.add_argument("--uplink-build-dir")
parser.add_argument("--uplink-partition-csv")
parser.add_argument("--uplink-sdkconfig")
parser.add_argument("--output-dir", type=Path, required=True)
parser.add_argument("--index", type=Path, required=True)
args = parser.parse_args()
assert args.command == "uplink"
assert args.uplink_sdkconfig == (
    "uplink/.pio/build/uplink-s3-backend/config/sdkconfig.h"
)
mode = os.environ.get("FAKE_RELEASE_MODE", "valid")
if mode == "missing":
    raise SystemExit(0)
args.index.parent.mkdir(parents=True, exist_ok=True)
if mode == "invalid":
    args.index.write_text("{}\n", encoding="utf-8")
    raise SystemExit(0)

offsets = [0, 32768, 61440, 131072]
logical = ["bootloader", "partition-table", "ota-data-initial", "firmware"]
specs = {
    "uplink-s3-backend": ("uplink", "fof_backend_uplink"),
}
targets = {}
for target, (kind, project) in specs.items():
    directory = args.output_dir / target
    if directory.exists():
        raise RuntimeError(f"publisher refuses preexisting target: {target}")
    directory.mkdir(parents=True)
    parts = []
    for name_part, offset in zip(logical, offsets):
        name = f"{target}-{name_part}.bin"
        data = f"{target}:{name_part}\n".encode("ascii")
        (directory / name).write_bytes(data)
        parts.append({
            "name": name,
            "path": f"{target}/{name}",
            "offset": offset,
            "size": len(data),
            "sha256": hashlib.sha256(data).hexdigest(),
            "crc32": zlib.crc32(data) & 0xffffffff,
        })
    targets[target] = {
        "kind": kind,
        "target": target,
        "project": project,
        "hardware": "seeed_xiao_esp32s3",
        "identity_crc32": 1,
        "partition_capacity": 2097152,
        "parts": parts,
    }
args.index.write_text(json.dumps({
    "schema": 1,
    "version": "0.2.0-backend",
    "targets": targets,
}, indent=2, sort_keys=True) + "\n", encoding="utf-8")
'''


def _package_fixture(tmp_path: Path) -> tuple[Path, Path]:
    backend_fw = tmp_path / "backend-firmware"

    def ignore_generated_targets(directory: str, names: list[str]) -> list[str]:
        if Path(directory) == FLASHER / "firmware":
            return [name for name in names if name in TARGETS]
        return []

    shutil.copytree(
        FLASHER,
        backend_fw / "web-flasher",
        ignore=ignore_generated_targets,
    )
    for directory in (
        "uplink",
        "tools/tests",
        "release",
    ):
        (backend_fw / directory).mkdir(parents=True, exist_ok=True)
    shutil.copy2(
        Path(__file__),
        backend_fw / "tools/tests/test_backend_web_flasher.py",
    )
    verifier = backend_fw / "tools/verify_backend_build.py"
    verifier.write_text(textwrap.dedent(FAKE_VERIFIER), encoding="utf-8")
    verifier.chmod(0o755)
    pio_log = tmp_path / "pio.log"
    fake_pio = tmp_path / "pio"
    fake_pio.write_text(
        "#!/bin/sh\nprintf '%s|%s\\n' \"$PWD\" \"$*\" >> \"$PIO_LOG\"\n",
        encoding="utf-8",
    )
    fake_pio.chmod(0o755)
    return backend_fw, pio_log


def _run_package(tmp_path: Path, mode: str) -> subprocess.CompletedProcess[str]:
    backend_fw, pio_log = _package_fixture(tmp_path)
    environment = os.environ.copy()
    environment.update({
        "FAKE_RELEASE_MODE": mode,
        "FOF_EXPECT_BUILD_SCRIPT_CHILD": "1",
        "PIO": str(tmp_path / "pio"),
        "PIO_LOG": str(pio_log),
        "PATH": (
            str(Path(sys.executable).parent)
            + os.pathsep
            + environment.get("PATH", "")
        ),
    })
    result = subprocess.run(
        ["bash", "web-flasher/build.sh"],
        cwd=backend_fw,
        env=environment,
        text=True,
        capture_output=True,
    )
    result.pio_log = pio_log  # type: ignore[attr-defined]
    result.backend_fw = backend_fw  # type: ignore[attr-defined]
    return result


@pytest.mark.skipif(
    PACKAGE_CHILD or PACKAGE_CHILD_EXPECTED,
    reason="avoid recursively exercising build orchestration",
)
def test_build_script_packages_and_checks_only_the_backend_uplink(tmp_path: Path):
    result = _run_package(tmp_path, "valid")

    assert result.returncode == 0, result.stdout + result.stderr
    assert result.pio_log.read_text(encoding="utf-8").splitlines() == [
        f"{result.backend_fw / 'uplink'}|run -e uplink-s3-backend",
    ]
    assert "scanner-s3-combo-backend" not in result.stdout
    assert "uplink-s3-backend-firmware.bin" in result.stdout
    assert "sha256=" in result.stdout


@pytest.mark.parametrize("mode", ["missing", "invalid"])
@pytest.mark.skipif(
    PACKAGE_CHILD or PACKAGE_CHILD_EXPECTED,
    reason="avoid recursively exercising build orchestration",
)
def test_build_script_fails_closed_without_valid_release_index(
    tmp_path: Path,
    mode: str,
):
    result = _run_package(tmp_path, mode)

    assert result.returncode != 0
