from __future__ import annotations

import hashlib
import json
import warnings
import zipfile
from pathlib import Path

import pytest

from tools.lite_factory_flasher.bundles import (
    BundleError,
    is_trusted_release_bundle,
    load_bundle,
)
from tools import build_lite_factory_bundle as bundle_builder


RESOURCE = (
    Path(__file__).resolve().parents[1]
    / "lite_factory_flasher"
    / "resources"
    / "lite-factory-flasher-embedded.zip"
)
EXPECTED_BUNDLE_SHA256 = (
    "6d39ff58f5d9030b40efb80cb2e1aa62e901c230e15f4b3f2fee5854b31d9536"
)


def _archive_members() -> dict[str, bytes]:
    with zipfile.ZipFile(RESOURCE) as archive:
        return {name: archive.read(name) for name in archive.namelist()}


def _write_archive(path: Path, members: dict[str, bytes]) -> Path:
    with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        for name, value in sorted(members.items()):
            archive.writestr(name, value)
    return path


def _manifest(members: dict[str, bytes]) -> dict[str, object]:
    return json.loads(members["manifest.json"])


def _store_manifest(
    members: dict[str, bytes],
    manifest: dict[str, object],
) -> None:
    members["manifest.json"] = json.dumps(
        manifest,
        sort_keys=True,
        separators=(",", ":"),
    ).encode("utf-8")


def _update_part(
    members: dict[str, bytes],
    manifest: dict[str, object],
    role: str,
    relative: str,
) -> None:
    layouts = manifest["layouts"]
    assert isinstance(layouts, dict)
    layout = layouts[role]
    assert isinstance(layout, dict)
    parts = layout["parts"]
    assert isinstance(parts, list)
    part = next(item for item in parts if item["path"] == relative)
    value = members[relative]
    part["size"] = len(value)
    part["sha256"] = hashlib.sha256(value).hexdigest()


def test_embedded_lite_factory_bundle_is_exact_and_self_consistent() -> None:
    assert hashlib.sha256(RESOURCE.read_bytes()).hexdigest() == EXPECTED_BUNDLE_SHA256

    bundle = load_bundle(RESOURCE, source="test-embedded")

    assert bundle.bundle_sha256 == EXPECTED_BUNDLE_SHA256
    assert is_trusted_release_bundle(bundle)
    assert bundle.version == "0.2.0-backend"
    assert bundle.scanner_version == "0.67.2-badge-defcon34"
    assert bundle.manifest["family"] == "badge_lite"
    assert bundle.manifest["assembly"] == {
        "board_count": 3,
        "layouts": {
            "scanner0": "scanner",
            "scanner1": "scanner",
            "uplink": "uplink",
        },
        "flash_order": ["scanner0", "scanner1", "uplink"],
    }
    assert bundle.layout("probe")["identity"] == {
        "project": "fof_badge_factory_probe",
        "target": "factory-probe-s3",
        "version": "1.0.0",
    }
    assert bundle.layout("scanner")["identity"] == {
        "project": "fof_badge_scanner",
        "target": "scanner-s3-combo-fof_badge",
        "version": "0.67.2-badge-defcon34",
    }
    assert bundle.layout("uplink")["identity"] == {
        "project": "fof_backend_uplink",
        "target": "uplink-s3-backend",
        "version": "0.2.0-backend",
    }


def test_bundle_rejects_a_part_digest_mutation(tmp_path: Path) -> None:
    members = _archive_members()
    firmware = bytearray(members["scanner/firmware.bin"])
    firmware[-1] ^= 0x01
    members["scanner/firmware.bin"] = bytes(firmware)

    with pytest.raises(BundleError, match="digest mismatch"):
        load_bundle(_write_archive(tmp_path / "digest.zip", members))


def test_bundle_rejects_mutable_directory_input(tmp_path: Path) -> None:
    extracted = tmp_path / "extracted"
    extracted.mkdir()
    for name, value in _archive_members().items():
        target = extracted / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(value)

    with pytest.raises(BundleError, match="immutable ZIP archives"):
        load_bundle(extracted)


def test_bundle_rejects_self_consistent_substitution_of_accepted_scanner_bytes(
    tmp_path: Path,
) -> None:
    members = _archive_members()
    manifest = _manifest(members)
    altered = bytearray(members["scanner/bootloader.bin"])
    altered[-1] ^= 0x01
    members["scanner/bootloader.bin"] = bytes(altered)
    _update_part(members, manifest, "scanner", "scanner/bootloader.bin")
    _store_manifest(members, manifest)

    with pytest.raises(BundleError, match="accepted factory release"):
        load_bundle(_write_archive(tmp_path / "substituted-scanner.zip", members))


@pytest.mark.parametrize(
    ("role", "relative", "forbidden", "message"),
    (
        (
            "scanner",
            "scanner/firmware.bin",
            b"scanner-s3-combo-backend",
            "backend scanner image is forbidden",
        ),
        (
            "uplink",
            "uplink/firmware.bin",
            b"fof_badge_uplink",
            "native badge uplink image is forbidden",
        ),
    ),
)
def test_bundle_rejects_cross_product_firmware_markers(
    tmp_path: Path,
    role: str,
    relative: str,
    forbidden: bytes,
    message: str,
) -> None:
    members = _archive_members()
    manifest = _manifest(members)
    members[relative] += b"\0" + forbidden + b"\0"
    _update_part(members, manifest, role, relative)
    _store_manifest(members, manifest)

    with pytest.raises(BundleError, match=message):
        load_bundle(_write_archive(tmp_path / f"{role}-confusion.zip", members))


def test_bundle_rejects_wrong_three_board_assembly(tmp_path: Path) -> None:
    members = _archive_members()
    manifest = _manifest(members)
    assembly = manifest["assembly"]
    assert isinstance(assembly, dict)
    assembly["board_count"] = 2
    _store_manifest(members, manifest)

    with pytest.raises(BundleError, match="assembly contract mismatch"):
        load_bundle(_write_archive(tmp_path / "wrong-assembly.zip", members))


def test_bundle_rejects_undeclared_files(tmp_path: Path) -> None:
    members = _archive_members()
    members["notes/operator.txt"] = b"not declared"

    with pytest.raises(BundleError, match="undeclared or missing files"):
        load_bundle(_write_archive(tmp_path / "extra-member.zip", members))


def test_bundle_rejects_duplicate_members(tmp_path: Path) -> None:
    path = tmp_path / "duplicate.zip"
    members = _archive_members()
    with warnings.catch_warnings():
        warnings.simplefilter("ignore", UserWarning)
        with zipfile.ZipFile(
            path,
            "w",
            compression=zipfile.ZIP_DEFLATED,
        ) as archive:
            for name, value in sorted(members.items()):
                archive.writestr(name, value)
            archive.writestr("manifest.json", members["manifest.json"])

    with pytest.raises(BundleError, match="unsafe bundle member"):
        load_bundle(path)


@pytest.mark.parametrize("member", ("../escape.bin", "/absolute.bin"))
def test_bundle_rejects_paths_outside_the_extract_root(
    tmp_path: Path,
    member: str,
) -> None:
    members = _archive_members()
    members[member] = b"unsafe"

    with pytest.raises(BundleError, match="unsafe bundle member"):
        load_bundle(_write_archive(tmp_path / "unsafe-path.zip", members))


def test_bundle_rejects_symlink_members(tmp_path: Path) -> None:
    path = tmp_path / "symlink.zip"
    members = _archive_members()
    with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        for name, value in sorted(members.items()):
            archive.writestr(name, value)
        link = zipfile.ZipInfo("scanner/escape-link")
        link.create_system = 3
        link.external_attr = 0o120777 << 16
        archive.writestr(link, b"../../outside")

    with pytest.raises(BundleError, match="unsafe bundle member"):
        load_bundle(path)


def test_builder_validation_failure_preserves_existing_output(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    source = tmp_path / "source"
    source.mkdir()
    (source / "candidate.txt").write_text("new candidate", encoding="utf-8")
    output = tmp_path / "factory.zip"
    output.write_bytes(b"known-good-output")

    def reject_candidate(*_args: object, **_kwargs: object) -> None:
        raise BundleError("candidate validation failed")

    monkeypatch.setattr(bundle_builder, "load_bundle", reject_candidate)

    with pytest.raises(BundleError, match="candidate validation failed"):
        bundle_builder._publish_validated_bundle(source, output)

    assert output.read_bytes() == b"known-good-output"
    assert not tuple(tmp_path.glob(".factory.zip.*.tmp"))
