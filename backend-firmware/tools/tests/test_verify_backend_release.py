from __future__ import annotations

import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import zipfile
import zlib

import pytest
import yaml

from tools.verify_backend_release import (
    PINNED_VENDOR_BASE,
    ReleaseVerificationError,
    _porcelain_changed_paths,
    audit_protected,
    main,
    protected_changes,
    resolve_audit_base,
    verify_release,
    verify_source_isolation,
    verify_vendor_base,
)


VERSION = "0.1.0-backend"
HARDWARE = "seeed_xiao_esp32s3"
TARGETS = {
    "scanner-s3-combo-backend": {
        "kind": "scanner",
        "project": "fof_backend_scanner",
        "manifest_title": "Friend or Foe Backend Scanner (XIAO ESP32-S3)",
        "identity_crc32": 0x9DD382FF,
    },
    "uplink-s3-backend": {
        "kind": "uplink",
        "project": "fof_backend_uplink",
        "manifest_title": "Friend or Foe Backend Uplink (XIAO ESP32-S3)",
        "identity_crc32": 0xF08BCDE4,
    },
}
BACKEND_FW = Path(__file__).resolve().parents[2]
REPO_ROOT = BACKEND_FW.parent
WORKFLOW_PATH = REPO_ROOT / ".github/workflows/backend-firmware.yml"
WORKFLOW_PATHS = [
    "backend-firmware/**",
    "backend/app/models/**",
    "backend/app/routers/detections.py",
    "backend/app/routers/nodes.py",
    "backend/app/services/backend_node_status.py",
    "backend/app/services/database.py",
    "backend/app/services/firmware_manager.py",
    "backend/app/services/node_commands.py",
    "backend/tests/**",
    ".github/workflows/backend-firmware.yml",
]
PARTS = (
    ("bootloader", 0x0000),
    ("partition-table", 0x8000),
    ("ota-data-initial", 0xF000),
    ("firmware", 0x20000),
)


def _c_field(value: str, size: int) -> bytes:
    encoded = value.encode("ascii")
    assert len(encoded) < size
    return encoded + bytes(size - len(encoded))


def _backend_app_image(target: str, spec: dict) -> bytes:
    image_kind = 1 if spec["kind"] == "scanner" else 0
    identity_prefix = struct.pack(
        "<IHH40s40s40s32s",
        0x42464F46,
        1,
        image_kind,
        _c_field(target, 40),
        _c_field(spec["project"], 40),
        _c_field(HARDWARE, 40),
        _c_field(VERSION, 32),
    )
    identity_crc32 = zlib.crc32(identity_prefix) & 0xFFFFFFFF
    assert identity_crc32 == spec["identity_crc32"]
    identity = identity_prefix + struct.pack("<I", identity_crc32)

    segment = bytearray(768)
    segment[0:4] = (0xABCD5432).to_bytes(4, "little")
    segment[16:48] = _c_field(VERSION, 32)
    segment[48:80] = _c_field(spec["project"], 32)
    segment[256 : 256 + len(identity)] = identity

    common = struct.pack("<BBBBI", 0xE9, 1, 2, 0x3F, 0)
    extended = struct.pack(
        "<BBBBHBHHBBBBB",
        0xEE,
        0,
        0,
        0,
        9,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        1,
    )
    image = bytearray(common + extended)
    image += struct.pack("<II", 0x3FC88000, len(segment))
    image += segment
    checksum = 0xEF
    for value in segment:
        checksum ^= value
    while len(image) % 16 != 15:
        image.append(0)
    image.append(checksum)
    image += hashlib.sha256(image).digest()
    return bytes(image)


def _partition_entry(
    partition_type: int,
    subtype: int,
    offset: int,
    size: int,
    label: str,
) -> bytes:
    label_bytes = label.encode("ascii")
    assert len(label_bytes) < 16
    return struct.pack(
        "<HBBII16sI",
        0x50AA,
        partition_type,
        subtype,
        offset,
        size,
        label_bytes + bytes(16 - len(label_bytes)),
        0,
    )


def _partition_table(kind: str) -> bytes:
    common = [
        (1, 2, 0x9000, 0x6000, "nvs"),
        (1, 0, 0xF000, 0x2000, "otadata"),
        (1, 1, 0x11000, 0x1000, "phy_init"),
        (0, 0x10, 0x20000, 0x200000, "ota_0"),
        (0, 0x11, 0x220000, 0x200000, "ota_1"),
    ]
    tails = {
        "scanner": [
            (1, 0x82, 0x420000, 0x100000, "storage"),
            (1, 0x81, 0x520000, 0x2E0000, "reserved"),
        ],
        "uplink": [
            (1, 0x40, 0x420000, 0x200000, "fw_scanner_be"),
            (1, 0x82, 0x620000, 0x100000, "storage"),
            (1, 0x81, 0x720000, 0x0E0000, "reserved"),
        ],
    }
    entries = b"".join(_partition_entry(*entry) for entry in common + tails[kind])
    md5_record = b"\xeb\xeb" + (b"\xff" * 14) + hashlib.md5(entries).digest()
    table = entries + md5_record
    return table + (b"\xff" * (0x1000 - len(table)))


def make_valid_release(tmp_path: Path) -> tuple[Path, Path]:
    flasher = tmp_path / "web-flasher"
    firmware_root = flasher / "firmware"
    targets: dict[str, dict] = {}
    for target, spec in TARGETS.items():
        directory = firmware_root / target
        directory.mkdir(parents=True)
        payloads = {
            "bootloader": (b"BACKEND-BOOT\x00" * 5),
            "partition-table": _partition_table(spec["kind"]),
            "ota-data-initial": b"\xff" * 0x2000,
            "firmware": _backend_app_image(target, spec),
        }
        parts: list[dict] = []
        manifest_parts: list[dict] = []
        for logical, offset in PARTS:
            name = f"{target}-{logical}.bin"
            data = payloads[logical]
            (directory / name).write_bytes(data)
            parts.append(
                {
                    "name": name,
                    "path": f"{target}/{name}",
                    "offset": offset,
                    "size": len(data),
                    "sha256": hashlib.sha256(data).hexdigest(),
                    "crc32": zlib.crc32(data) & 0xFFFFFFFF,
                }
            )
            manifest_parts.append(
                {
                    "path": f"firmware/{target}/{name}",
                    "offset": offset,
                }
            )
        targets[target] = {
            "kind": spec["kind"],
            "target": target,
            "project": spec["project"],
            "hardware": HARDWARE,
            "identity_crc32": spec["identity_crc32"],
            "partition_capacity": 0x200000,
            "parts": parts,
        }
        manifest_name = (
            "manifest-scanner-s3-combo-backend.json"
            if spec["kind"] == "scanner"
            else "manifest-uplink-s3-backend.json"
        )
        (flasher / manifest_name).write_text(
            json.dumps(
                {
                    "name": spec["manifest_title"],
                    "version": VERSION,
                    "builds": [
                        {"chipFamily": "ESP32-S3", "parts": manifest_parts}
                    ],
                }
            ),
            encoding="utf-8",
        )
    index = tmp_path / "backend-release-index.json"
    index.write_text(
        json.dumps(
            {
                "schema": 1,
                "version": VERSION,
                "targets": targets,
            }
        ),
        encoding="utf-8",
    )
    return index, flasher


def _read_index(index: Path) -> dict:
    return json.loads(index.read_text(encoding="utf-8"))


def _write_index(index: Path, body: dict) -> None:
    index.write_text(json.dumps(body), encoding="utf-8")


def _part(body: dict, target: str, logical: str) -> dict:
    name = f"{target}-{logical}.bin"
    return next(part for part in body["targets"][target]["parts"] if part["name"] == name)


def _rehash_artifact(index: Path, flasher: Path, target: str, logical: str) -> None:
    body = _read_index(index)
    part = _part(body, target, logical)
    artifact = flasher / "firmware" / part["path"]
    data = artifact.read_bytes()
    part["size"] = len(data)
    part["sha256"] = hashlib.sha256(data).hexdigest()
    part["crc32"] = zlib.crc32(data) & 0xFFFFFFFF
    _write_index(index, body)


def _git(repository: Path, *arguments: str) -> str:
    result = subprocess.run(
        ["git", *arguments],
        cwd=repository,
        check=True,
        text=True,
        capture_output=True,
    )
    return result.stdout.strip()


def _commit_all(repository: Path, message: str) -> str:
    _git(repository, "add", "-A")
    _git(repository, "commit", "-m", message)
    return _git(repository, "rev-parse", "HEAD")


def _new_repository(path: Path) -> tuple[Path, str]:
    path.mkdir()
    _git(path, "init", "-b", "main")
    _git(path, "config", "user.email", "release-audit@example.invalid")
    _git(path, "config", "user.name", "Release Audit Test")
    (path / "backend-firmware").mkdir()
    (path / "backend-firmware/README.md").write_text("backend\n", encoding="utf-8")
    return path, _commit_all(path, "base")


def test_release_index_and_flasher_are_identical(tmp_path: Path) -> None:
    index, flasher = make_valid_release(tmp_path)

    result = verify_release(index=index, flasher=flasher)

    assert result.targets == (
        "scanner-s3-combo-backend",
        "uplink-s3-backend",
    )
    tampered = (
        flasher
        / "firmware/uplink-s3-backend/uplink-s3-backend-firmware.bin"
    )
    tampered.write_bytes(tampered.read_bytes() + b"x")
    with pytest.raises(ReleaseVerificationError, match="digest"):
        verify_release(index=index, flasher=flasher)


def test_release_audit_rejects_cross_target_app_after_index_rehash(
    tmp_path: Path,
) -> None:
    index, flasher = make_valid_release(tmp_path)
    scanner = flasher / (
        "firmware/scanner-s3-combo-backend/"
        "scanner-s3-combo-backend-firmware.bin"
    )
    uplink = flasher / (
        "firmware/uplink-s3-backend/uplink-s3-backend-firmware.bin"
    )
    scanner.write_bytes(uplink.read_bytes())
    _rehash_artifact(index, flasher, "scanner-s3-combo-backend", "firmware")

    with pytest.raises(ReleaseVerificationError, match="identity"):
        verify_release(index=index, flasher=flasher)


def test_release_audit_rejects_native_badge_app_after_index_rehash(
    tmp_path: Path,
) -> None:
    index, flasher = make_valid_release(tmp_path)
    bundle = (
        REPO_ROOT
        / "tools/badge_flasher/resources/badge-factory-flasher-embedded.zip"
    )
    with zipfile.ZipFile(bundle) as archive:
        badge_image = archive.read("uplink/firmware.bin")
    artifact = flasher / (
        "firmware/uplink-s3-backend/uplink-s3-backend-firmware.bin"
    )
    artifact.write_bytes(badge_image)
    _rehash_artifact(index, flasher, "uplink-s3-backend", "firmware")

    with pytest.raises(ReleaseVerificationError, match="identity"):
        verify_release(index=index, flasher=flasher)


def test_release_audit_rejects_arbitrary_app_after_index_rehash(
    tmp_path: Path,
) -> None:
    index, flasher = make_valid_release(tmp_path)
    artifact = flasher / (
        "firmware/uplink-s3-backend/uplink-s3-backend-firmware.bin"
    )
    artifact.write_bytes(b"not an ESP32-S3 backend application")
    _rehash_artifact(index, flasher, "uplink-s3-backend", "firmware")

    with pytest.raises(ReleaseVerificationError, match="identity"):
        verify_release(index=index, flasher=flasher)


def test_protected_path_audit_reports_exact_changes() -> None:
    changed = [
        "backend/app/x.py",
        "esp32/scanner/main/main.c",
        "esp32/rid-simulator/main/main.c",
        "tools/badge_flasher/flash.py",
        ".github/workflows/esp32-web-flasher.yml",
    ]

    assert protected_changes(changed) == [
        ".github/workflows/esp32-web-flasher.yml",
        "esp32/rid-simulator/main/main.c",
        "esp32/scanner/main/main.c",
        "tools/badge_flasher/flash.py",
    ]


@pytest.mark.parametrize("target", TARGETS)
@pytest.mark.parametrize("logical", [logical for logical, _ in PARTS])
def test_release_audit_rejects_each_indexed_size_mutation(
    tmp_path: Path, target: str, logical: str
) -> None:
    index, flasher = make_valid_release(tmp_path)
    body = _read_index(index)
    _part(body, target, logical)["size"] += 1
    _write_index(index, body)

    with pytest.raises(ReleaseVerificationError):
        verify_release(index=index, flasher=flasher)


@pytest.mark.parametrize("target", TARGETS)
@pytest.mark.parametrize("logical", [logical for logical, _ in PARTS])
def test_release_audit_rejects_each_indexed_offset_mutation(
    tmp_path: Path, target: str, logical: str
) -> None:
    index, flasher = make_valid_release(tmp_path)
    body = _read_index(index)
    _part(body, target, logical)["offset"] += 1
    _write_index(index, body)

    with pytest.raises(ReleaseVerificationError, match="offset"):
        verify_release(index=index, flasher=flasher)


@pytest.mark.parametrize(
    ("field", "value"),
    [
        ("kind", "badge"),
        ("project", "fof_badge_uplink"),
        ("hardware", "badge"),
        ("partition_capacity", 0x200001),
        ("identity_crc32", 1),
        ("identity_crc32", -1),
        ("identity_crc32", True),
    ],
)
def test_release_audit_rejects_target_identity_drift(
    tmp_path: Path, field: str, value: object
) -> None:
    index, flasher = make_valid_release(tmp_path)
    body = _read_index(index)
    body["targets"]["uplink-s3-backend"][field] = value
    _write_index(index, body)

    with pytest.raises(ReleaseVerificationError):
        verify_release(index=index, flasher=flasher)


def test_release_audit_rejects_generic_name_and_path_disagreement(
    tmp_path: Path,
) -> None:
    index, flasher = make_valid_release(tmp_path)
    body = _read_index(index)
    part = _part(body, "uplink-s3-backend", "firmware")
    part["name"] = "firmware.bin"
    _write_index(index, body)

    with pytest.raises(ReleaseVerificationError, match="generic"):
        verify_release(index=index, flasher=flasher)


def test_release_audit_rejects_extra_binary(tmp_path: Path) -> None:
    index, flasher = make_valid_release(tmp_path)
    (flasher / "firmware/extra.bin").write_bytes(b"not indexed")

    with pytest.raises(ReleaseVerificationError, match="extra"):
        verify_release(index=index, flasher=flasher)


def test_release_audit_rejects_manifest_drift(tmp_path: Path) -> None:
    index, flasher = make_valid_release(tmp_path)
    manifest_path = flasher / "manifest-uplink-s3-backend.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    manifest["builds"][0]["parts"][3]["offset"] = 0x30000
    manifest_path.write_text(json.dumps(manifest), encoding="utf-8")

    with pytest.raises(ReleaseVerificationError, match="manifest"):
        verify_release(index=index, flasher=flasher)


def test_release_audit_rejects_manifest_identity_drift(tmp_path: Path) -> None:
    index, flasher = make_valid_release(tmp_path)
    manifest_path = flasher / "manifest-uplink-s3-backend.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    manifest["name"] = "Badge Uplink"
    manifest_path.write_text(json.dumps(manifest), encoding="utf-8")

    with pytest.raises(ReleaseVerificationError, match="manifest name"):
        verify_release(index=index, flasher=flasher)


def test_release_audit_decodes_partition_table_independently(
    tmp_path: Path,
) -> None:
    index, flasher = make_valid_release(tmp_path)
    artifact = (
        flasher
        / "firmware/uplink-s3-backend/uplink-s3-backend-partition-table.bin"
    )
    table = bytearray(artifact.read_bytes())
    struct.pack_into("<I", table, (3 * 32) + 4, 0x30000)
    entries_end = 8 * 32
    table[entries_end + 16 : entries_end + 32] = hashlib.md5(
        table[:entries_end]
    ).digest()
    artifact.write_bytes(table)
    _rehash_artifact(index, flasher, "uplink-s3-backend", "partition-table")

    with pytest.raises(ReleaseVerificationError, match="partition layout"):
        verify_release(index=index, flasher=flasher)


def test_release_audit_rejects_application_beyond_partition(
    tmp_path: Path,
) -> None:
    index, flasher = make_valid_release(tmp_path)
    artifact = flasher / (
        "firmware/uplink-s3-backend/uplink-s3-backend-firmware.bin"
    )
    artifact.write_bytes(b"x" * (0x200000 + 1))
    _rehash_artifact(index, flasher, "uplink-s3-backend", "firmware")

    with pytest.raises(ReleaseVerificationError, match="OTA slot"):
        verify_release(index=index, flasher=flasher)


@pytest.mark.parametrize(
    ("mutation", "expected_status", "expected_path"),
    [
        ("delete", "D", "esp32/scanner/main/main.c"),
        ("protected-to-backend", "R", "esp32/scanner/main/main.c"),
        ("backend-to-protected", "R", "esp32/scanner/main/main.c"),
        ("type-change", "T", "esp32/scanner/main/main.c"),
    ],
)
def test_git_audit_catches_protected_deletion_rename_and_type_change(
    tmp_path: Path,
    mutation: str,
    expected_status: str,
    expected_path: str,
) -> None:
    repository, _ = _new_repository(tmp_path / "repository")
    protected = repository / expected_path
    if mutation != "backend-to-protected":
        protected.parent.mkdir(parents=True)
        protected.write_text("scanner\n", encoding="utf-8")
    backend_file = repository / "backend-firmware/move-me.c"
    backend_file.write_text("backend\n", encoding="utf-8")
    base = _commit_all(repository, "inputs")

    if mutation == "delete":
        protected.unlink()
    elif mutation == "protected-to-backend":
        _git(repository, "mv", expected_path, "backend-firmware/from-protected.c")
    elif mutation == "backend-to-protected":
        protected.parent.mkdir(parents=True)
        _git(repository, "mv", "backend-firmware/move-me.c", expected_path)
    elif mutation == "type-change":
        protected.unlink()
        protected.symlink_to("../../../backend-firmware/README.md")
    _commit_all(repository, mutation)

    violations = audit_protected(repository=repository, base=base)

    assert any(
        violation.status.startswith(expected_status)
        and violation.path == expected_path
        for violation in violations
    )


def test_git_audit_allows_unchanged_protected_source_copied_to_backend(
    tmp_path: Path,
) -> None:
    repository, _ = _new_repository(tmp_path / "repository")
    protected = repository / "esp32/scanner/main/main.c"
    protected.parent.mkdir(parents=True)
    protected.write_text("distinct protected source\n", encoding="utf-8")
    base = _commit_all(repository, "protected source")
    shutil.copyfile(protected, repository / "backend-firmware/copied.c")
    _commit_all(repository, "copy out")

    violations = audit_protected(repository=repository, base=base)

    assert violations == []


def test_git_audit_rejects_backend_source_copied_into_protected_path(
    tmp_path: Path,
) -> None:
    repository, _ = _new_repository(tmp_path / "repository")
    backend = repository / "backend-firmware/source.c"
    backend.write_text("backend source\n", encoding="utf-8")
    base = _commit_all(repository, "backend source")
    protected = repository / "esp32/scanner/main/main.c"
    protected.parent.mkdir(parents=True)
    shutil.copyfile(backend, protected)
    _commit_all(repository, "copy into protected path")

    violations = audit_protected(repository=repository, base=base)

    assert any(
        violation.status.startswith("C")
        and violation.path == "esp32/scanner/main/main.c"
        for violation in violations
    )


def test_git_audit_rejects_modified_protected_source_even_when_copied_out(
    tmp_path: Path,
) -> None:
    repository, _ = _new_repository(tmp_path / "repository")
    protected = repository / "esp32/scanner/main/main.c"
    protected.parent.mkdir(parents=True)
    protected.write_text("protected source\n", encoding="utf-8")
    base = _commit_all(repository, "protected source")
    protected.write_text("modified protected source\n", encoding="utf-8")
    shutil.copyfile(protected, repository / "backend-firmware/copied.c")
    _commit_all(repository, "modify protected and copy out")

    violations = audit_protected(repository=repository, base=base)

    assert any(
        violation.path == "esp32/scanner/main/main.c"
        for violation in violations
    )


def test_git_audit_allows_staged_copy_from_unchanged_protected_source(
    tmp_path: Path,
) -> None:
    repository, _ = _new_repository(tmp_path / "repository")
    protected = repository / "esp32/scanner/main/main.c"
    protected.parent.mkdir(parents=True)
    protected.write_text("protected source\n", encoding="utf-8")
    base = _commit_all(repository, "protected source")
    _git(repository, "config", "status.renames", "copies")
    shutil.copyfile(protected, repository / "backend-firmware/copied.c")
    _git(repository, "add", "backend-firmware/copied.c")

    assert audit_protected(repository=repository, base=base) == []


def test_git_audit_rejects_staged_copy_into_protected_destination(
    tmp_path: Path,
) -> None:
    repository, _ = _new_repository(tmp_path / "repository")
    backend = repository / "backend-firmware/source.c"
    backend.write_text("backend source\n", encoding="utf-8")
    base = _commit_all(repository, "backend source")
    _git(repository, "config", "status.renames", "copies")
    protected = repository / "esp32/scanner/main/main.c"
    protected.parent.mkdir(parents=True)
    shutil.copyfile(backend, protected)
    _git(repository, "add", "esp32/scanner/main/main.c")

    violations = audit_protected(repository=repository, base=base)

    assert any(
        violation.path == "esp32/scanner/main/main.c"
        for violation in violations
    )


def test_porcelain_copy_semantics_select_destination_before_source() -> None:
    assert _porcelain_changed_paths(
        "C ",
        (
            "backend-firmware/copied.c",
            "esp32/scanner/main/main.c",
        ),
    ) == ("backend-firmware/copied.c",)
    assert _porcelain_changed_paths(
        " C",
        (
            "esp32/scanner/main/main.c",
            "backend-firmware/source.c",
        ),
    ) == ("esp32/scanner/main/main.c",)


def test_git_audit_catches_untracked_protected_file(tmp_path: Path) -> None:
    repository, base = _new_repository(tmp_path / "repository")
    untracked = repository / "tools/badge_flasher/new.py"
    untracked.parent.mkdir(parents=True)
    untracked.write_text("do not touch badge\n", encoding="utf-8")

    violations = audit_protected(repository=repository, base=base)

    assert [(item.status, item.path) for item in violations] == [
        ("??", "tools/badge_flasher/new.py")
    ]


def test_git_audit_catches_uncommitted_tracked_protected_change(
    tmp_path: Path,
) -> None:
    repository, _ = _new_repository(tmp_path / "repository")
    protected = repository / ".github/workflows/esp32-web-flasher.yml"
    protected.parent.mkdir(parents=True)
    protected.write_text("name: protected\n", encoding="utf-8")
    base = _commit_all(repository, "protected workflow")
    protected.write_text("name: modified\n", encoding="utf-8")

    violations = audit_protected(repository=repository, base=base)

    assert [(item.status, item.path) for item in violations] == [
        (" M", ".github/workflows/esp32-web-flasher.yml")
    ]


def test_git_audit_allows_backend_only_change(tmp_path: Path) -> None:
    repository, base = _new_repository(tmp_path / "repository")
    (repository / "backend-firmware/README.md").write_text(
        "backend changed\n", encoding="utf-8"
    )
    _commit_all(repository, "backend only")

    assert audit_protected(repository=repository, base=base) == []


def _repository_with_origin(tmp_path: Path) -> tuple[Path, str]:
    origin = tmp_path / "origin.git"
    origin.mkdir()
    _git(origin, "init", "--bare")
    repository, base = _new_repository(tmp_path / "work")
    _git(repository, "remote", "add", "origin", str(origin))
    _git(repository, "push", "-u", "origin", "main")
    (repository / "backend-firmware/README.md").write_text(
        "feature\n", encoding="utf-8"
    )
    _commit_all(repository, "feature")
    return repository, base


@pytest.mark.parametrize("event_name", ["pull_request", "push"])
def test_resolve_audit_base_uses_exact_event_sha(
    tmp_path: Path, event_name: str
) -> None:
    repository, base = _repository_with_origin(tmp_path)

    assert resolve_audit_base(
        repository=repository,
        event_name=event_name,
        event_base=base,
        default_branch="main",
    ) == base


def test_resolve_audit_base_fetches_default_tip_for_zero_before(
    tmp_path: Path,
) -> None:
    repository, base = _repository_with_origin(tmp_path)

    resolved = resolve_audit_base(
        repository=repository,
        event_name="push",
        event_base="0" * 40,
        default_branch="main",
    )

    assert resolved == base


@pytest.mark.parametrize(
    ("event_name", "event_base", "default_branch"),
    [
        ("pull_request", "", "main"),
        ("pull_request", "f" * 39, "main"),
        ("pull_request", "0" * 40, "main"),
        ("release", "a" * 40, "main"),
        ("push", "0" * 40, ""),
        ("push", "0" * 40, "main^{commit}"),
    ],
)
def test_resolve_audit_base_fails_closed_on_invalid_event_data(
    tmp_path: Path,
    event_name: str,
    event_base: str,
    default_branch: str,
) -> None:
    repository, _ = _repository_with_origin(tmp_path)

    with pytest.raises(ReleaseVerificationError):
        resolve_audit_base(
            repository=repository,
            event_name=event_name,
            event_base=event_base,
            default_branch=default_branch,
        )


def test_resolve_audit_base_rejects_nonancestor(tmp_path: Path) -> None:
    repository, _ = _repository_with_origin(tmp_path)
    _git(repository, "switch", "--orphan", "unrelated")
    for child in repository.iterdir():
        if child.name != ".git":
            if child.is_dir():
                shutil.rmtree(child)
            else:
                child.unlink()
    (repository / "unrelated.txt").write_text("unrelated\n", encoding="utf-8")
    unrelated = _commit_all(repository, "unrelated")
    _git(repository, "switch", "main")

    with pytest.raises(ReleaseVerificationError, match="ancestor"):
        resolve_audit_base(
            repository=repository,
            event_name="push",
            event_base=unrelated,
            default_branch="main",
        )


def test_resolve_audit_base_rejects_missing_commit(tmp_path: Path) -> None:
    repository, _ = _repository_with_origin(tmp_path)

    with pytest.raises(ReleaseVerificationError):
        resolve_audit_base(
            repository=repository,
            event_name="push",
            event_base="f" * 40,
            default_branch="main",
        )


def test_resolve_audit_base_rejects_tag_object_instead_of_exact_commit(
    tmp_path: Path,
) -> None:
    repository, _ = _repository_with_origin(tmp_path)
    _git(repository, "tag", "-a", "audit-tag", "-m", "audit tag")
    tag_object = _git(repository, "rev-parse", "audit-tag")

    with pytest.raises(ReleaseVerificationError, match="exact commit"):
        resolve_audit_base(
            repository=repository,
            event_name="push",
            event_base=tag_object,
            default_branch="main",
        )


def test_zero_before_rejects_fetch_failure(tmp_path: Path) -> None:
    repository, _ = _new_repository(tmp_path / "repository")

    with pytest.raises(ReleaseVerificationError, match="Git command failed"):
        resolve_audit_base(
            repository=repository,
            event_name="push",
            event_base="0" * 40,
            default_branch="main",
        )


def test_resolve_audit_base_rejects_missing_repository(tmp_path: Path) -> None:
    with pytest.raises(ReleaseVerificationError, match="repository"):
        resolve_audit_base(
            repository=tmp_path / "missing",
            event_name="push",
            event_base="a" * 40,
            default_branch="main",
        )


def test_vendor_base_is_fixed_and_rejects_drift(tmp_path: Path) -> None:
    vendor_base = tmp_path / "VENDOR_BASE"
    vendor_base.write_text(PINNED_VENDOR_BASE + "\n", encoding="ascii")
    assert verify_vendor_base(vendor_base) == PINNED_VENDOR_BASE

    vendor_base.write_text("f" * 40 + "\n", encoding="ascii")
    with pytest.raises(ReleaseVerificationError, match="VENDOR_BASE"):
        verify_vendor_base(vendor_base)


def test_release_cli_source_isolation_wrapper_fails_closed(
    tmp_path: Path,
) -> None:
    backend_root = tmp_path / "backend-firmware"
    backend_root.mkdir()
    source = backend_root / "safe.c"
    source.write_text('#include "safe.h"\n', encoding="utf-8")
    (backend_root / "safe.h").write_text("#pragma once\n", encoding="utf-8")

    assert verify_source_isolation(backend_root) == []

    source.write_text('#include "../esp32/protected.h"\n', encoding="utf-8")
    with pytest.raises(ReleaseVerificationError, match="source-isolation"):
        verify_source_isolation(backend_root)


def test_release_cli_resolves_base_as_a_separate_fail_closed_mode(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    repository, base = _repository_with_origin(tmp_path)

    assert main(
        [
            "--resolve-audit-base",
            "--repository",
            str(repository),
            "--event-name",
            "push",
            "--event-base",
            base,
            "--default-branch",
            "main",
        ]
    ) == 0
    assert capsys.readouterr().out.strip() == base


def test_release_cli_rejects_cross_mode_arguments(tmp_path: Path) -> None:
    repository, base = _repository_with_origin(tmp_path)
    resolver_arguments = [
        "--resolve-audit-base",
        "--repository",
        str(repository),
        "--event-name",
        "push",
        "--event-base",
        base,
        "--default-branch",
        "main",
    ]
    with pytest.raises(SystemExit):
        main([*resolver_arguments, "--audit-protected", base])

    index, flasher = make_valid_release(tmp_path / "release")
    with pytest.raises(SystemExit):
        main(
            [
                "--index",
                str(index),
                "--flasher",
                str(flasher),
                "--repository",
                str(repository),
            ]
        )


def _workflow() -> dict:
    return yaml.load(
        WORKFLOW_PATH.read_text(encoding="utf-8"),
        Loader=yaml.BaseLoader,
    )


def test_workflow_is_read_only_non_deploying_and_backend_filtered() -> None:
    workflow = _workflow()

    assert workflow["permissions"] == {"contents": "read"}
    assert set(workflow["on"]) == {"pull_request", "push"}
    assert workflow["on"]["pull_request"]["paths"] == WORKFLOW_PATHS
    assert workflow["on"]["push"] == {
        "branches": ["main"],
        "paths": WORKFLOW_PATHS,
    }
    assert "esp32/**" not in workflow["on"]["push"]["paths"]
    assert "workflow_dispatch" not in workflow["on"]
    assert "release" not in workflow["on"]

    assert len(workflow["jobs"]) == 1
    job = next(iter(workflow["jobs"].values()))
    assert "permissions" not in job
    assert "environment" not in job
    assert job["runs-on"] == "ubuntu-latest"

    serialized = WORKFLOW_PATH.read_text(encoding="utf-8").lower()
    for forbidden in (
        "deploy-pages",
        "pages: write",
        "gh release",
        "actions/create-release",
        "git tag",
        "scanner-s3-combo-fof_badge",
        "uplink-s3-fof_badge",
        "esp32/web-flasher",
    ):
        assert forbidden not in serialized


def test_workflow_resolves_event_base_and_runs_exact_backend_gates_in_order() -> None:
    workflow = _workflow()
    assert workflow["env"] == {
        "BACKEND_AUDIT_EVENT_BASE": (
            "${{ github.event.pull_request.base.sha || github.event.before }}"
        ),
        "BACKEND_AUDIT_DEFAULT_BRANCH": (
            "${{ github.event.repository.default_branch }}"
        ),
    }
    job = next(iter(workflow["jobs"].values()))
    steps = job["steps"]
    checkout = next(step for step in steps if step.get("uses") == "actions/checkout@v4")
    assert checkout["with"] == {"fetch-depth": "0"}

    commands = "\n".join(step.get("run", "") for step in steps)
    required_in_order = [
        "--resolve-audit-base",
        "cd backend && pytest tests -q",
        "cd backend-firmware && python -m pytest tools/tests -q",
        "cd backend-firmware && pio test -e backend-native",
        "python tools/emit_serializer_fixture.py --check",
        "pio run -e scanner-s3-combo-backend -t clean",
        "pio run -e scanner-s3-combo-backend",
        "pio run -e uplink-s3-backend -t clean",
        "pio run -e uplink-s3-backend",
        "bash backend-firmware/web-flasher/build.sh",
        "python tools/verify_backend_release.py --index",
        "python tools/check_source_isolation.py --root .",
        "--audit-protected \"$BACKEND_AUDIT_BASE\"",
        "git diff --check",
    ]
    positions = [commands.index(fragment) for fragment in required_in_order]
    assert positions == sorted(positions)
    assert "scanner-s3-combo-backend" in commands
    assert "uplink-s3-backend" in commands
    assert "HEAD^" not in commands


def test_workflow_uploads_only_private_backend_package_for_seven_days() -> None:
    workflow = _workflow()
    job = next(iter(workflow["jobs"].values()))
    uploads = [
        step
        for step in job["steps"]
        if step.get("uses") == "actions/upload-artifact@v4"
    ]

    assert len(uploads) == 1
    upload = uploads[0]["with"]
    assert {key: value for key, value in upload.items() if key != "path"} == {
        "name": "friend-or-foe-backend-firmware-0.1.0-backend",
        "retention-days": "7",
        "if-no-files-found": "error",
    }
    assert upload["path"].splitlines() == [
        "backend-firmware/web-flasher",
        "backend-firmware/release/backend-release-index.json",
        "backend-firmware/test-logs",
    ]
