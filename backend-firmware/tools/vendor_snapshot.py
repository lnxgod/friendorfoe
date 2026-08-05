#!/usr/bin/env python3
"""Materialize the explicitly pinned donor files for manual adaptation."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import subprocess
import tempfile
from typing import Any, Sequence


GROUP_SOURCE_ROOTS = {
    "shared": PurePosixPath("esp32/shared"),
    "scanner_detection": PurePosixPath("esp32/scanner/main/detection"),
    "shared_reference": PurePosixPath("esp32/shared"),
    "scanner_reference": PurePosixPath("esp32/scanner/main"),
}
MANIFEST_KEYS = {"base", *GROUP_SOURCE_ROOTS}
COMMIT_PATTERN = re.compile(r"[0-9a-f]{40}")


def _git(repository: Path, *arguments: str, text: bool = False) -> subprocess.CompletedProcess:
    return subprocess.run(
        ["git", "-C", str(repository), *arguments],
        check=True,
        capture_output=True,
        text=text,
    )


def _validate_entry(value: Any) -> str:
    if not isinstance(value, str):
        raise ValueError("unsafe vendor path: entry must be a string")
    path = PurePosixPath(value)
    if not value or path.is_absolute() or ".." in path.parts or "." in path.parts:
        raise ValueError(f"unsafe vendor path: {value}")
    if "\\" in value or path.as_posix() != value:
        raise ValueError(f"unsafe vendor path: {value}")
    return value


def load_manifest(path: Path) -> dict[str, Any]:
    manifest = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(manifest, dict) or set(manifest) != MANIFEST_KEYS:
        raise ValueError("vendor manifest has unexpected keys")
    base = manifest.get("base")
    if not isinstance(base, str) or COMMIT_PATTERN.fullmatch(base) is None:
        raise ValueError("vendor manifest base must be a 40-character lowercase commit")
    for group in GROUP_SOURCE_ROOTS:
        entries = manifest[group]
        if not isinstance(entries, list):
            raise ValueError(f"vendor manifest group must be a list: {group}")
        manifest[group] = [_validate_entry(entry) for entry in entries]
        if len(set(manifest[group])) != len(manifest[group]):
            raise ValueError(f"vendor manifest group contains duplicates: {group}")
    return manifest


def _blob_at_commit(repository: Path, base: str, source: PurePosixPath) -> bytes:
    source_name = source.as_posix()
    tree_line = _git(repository, "ls-tree", base, "--", source_name, text=True).stdout
    if not tree_line.strip():
        raise ValueError(f"donor source is not a file: {source_name}")
    mode, object_type, _remainder = tree_line.split(maxsplit=2)
    if mode == "120000":
        raise ValueError(f"donor source is a symlink: {source_name}")
    if object_type != "blob" or not mode.startswith("100"):
        raise ValueError(f"donor source is not a regular file: {source_name}")
    return _git(repository, "show", f"{base}:{source_name}").stdout


def _reject_symlink_components(path: Path, boundary: Path) -> None:
    try:
        relative = path.relative_to(boundary)
    except ValueError as exc:
        raise ValueError(f"unsafe vendor destination outside output root: {path}") from exc
    current = boundary
    for part in relative.parts:
        current = current / part
        if current.is_symlink():
            raise ValueError(f"unsafe vendor destination symlink: {current}")


def _reject_existing_path_symlinks(path: Path) -> None:
    absolute = path.absolute()
    current = Path(absolute.anchor)
    for part in absolute.parts[1:]:
        current = current / part
        if current.is_symlink():
            raise ValueError(f"unsafe vendor output path symlink: {current}")


def _write_provenance_atomically(path: Path, payload: str, output_root: Path) -> None:
    _reject_symlink_components(path, output_root)
    temporary_name: str | None = None
    try:
        with tempfile.NamedTemporaryFile(
            mode="w",
            encoding="utf-8",
            dir=output_root,
            prefix=".VENDORED_SHA256.",
            suffix=".tmp",
            delete=False,
        ) as temporary:
            temporary.write(payload)
            temporary.flush()
            os.fsync(temporary.fileno())
            temporary_name = temporary.name
        os.replace(temporary_name, path)
        temporary_name = None
    finally:
        if temporary_name is not None:
            try:
                Path(temporary_name).unlink()
            except FileNotFoundError:
                pass


def snapshot(
    repository: Path,
    manifest_path: Path,
    output_root: Path,
) -> dict[str, str]:
    repository = repository.resolve(strict=True)
    manifest_path = manifest_path.resolve(strict=True)
    requested_output_root = output_root.expanduser()
    _reject_existing_path_symlinks(requested_output_root)
    output_root = requested_output_root.resolve(strict=False)
    output_root.mkdir(parents=True, exist_ok=True)
    manifest = load_manifest(manifest_path)
    base = manifest["base"]

    try:
        _git(repository, "cat-file", "-e", f"{base}^{{commit}}")
    except subprocess.CalledProcessError as exc:
        raise ValueError(f"vendor base is not an available commit: {base}") from exc

    hashes: dict[str, str] = {}
    for group, source_root in GROUP_SOURCE_ROOTS.items():
        for entry in manifest[group]:
            source = source_root / PurePosixPath(entry)
            data = _blob_at_commit(repository, base, source)
            relative_destination = Path("vendor") / group / Path(entry)
            group_root = output_root / "vendor" / group
            destination = output_root / relative_destination
            _reject_symlink_components(destination, output_root)
            canonical_group_root = group_root.resolve(strict=False)
            canonical_destination = destination.resolve(strict=False)
            try:
                canonical_destination.relative_to(canonical_group_root)
            except ValueError as exc:
                raise ValueError(f"unsafe vendor destination: {entry}") from exc
            destination.parent.mkdir(parents=True, exist_ok=True)
            _reject_symlink_components(destination, output_root)
            destination.write_bytes(data)
            hashes[relative_destination.as_posix()] = hashlib.sha256(data).hexdigest()

    provenance = output_root / "VENDORED_SHA256.json"
    _write_provenance_atomically(
        provenance,
        json.dumps(dict(sorted(hashes.items())), indent=2) + "\n",
        output_root,
    )
    return dict(sorted(hashes.items()))


def check_snapshot(
    repository: Path,
    manifest_path: Path,
    output_root: Path,
) -> dict[str, str]:
    """Verify the pinned snapshot without modifying any local file."""
    repository = repository.resolve(strict=True)
    manifest_path = manifest_path.resolve(strict=True)
    requested_output_root = output_root.expanduser()
    _reject_existing_path_symlinks(requested_output_root)
    output_root = requested_output_root.resolve(strict=True)
    manifest = load_manifest(manifest_path)
    base = manifest["base"]

    try:
        _git(repository, "cat-file", "-e", f"{base}^{{commit}}")
    except subprocess.CalledProcessError as exc:
        raise ValueError(f"vendor base is not an available commit: {base}") from exc

    vendor_base_path = output_root / "VENDOR_BASE"
    _reject_symlink_components(vendor_base_path, output_root)
    try:
        vendor_base = vendor_base_path.read_bytes()
    except OSError as exc:
        raise ValueError("VENDOR_BASE is missing or unreadable") from exc
    if vendor_base != (base + "\n").encode("ascii"):
        raise ValueError("VENDOR_BASE does not exactly match manifest base")

    expected_hashes: dict[str, str] = {}
    declared_files: set[Path] = set()
    for group, source_root in GROUP_SOURCE_ROOTS.items():
        for entry in manifest[group]:
            source = source_root / PurePosixPath(entry)
            expected_data = _blob_at_commit(repository, base, source)
            relative = Path("vendor") / group / Path(entry)
            destination = output_root / relative
            _reject_symlink_components(destination, output_root)
            try:
                actual_data = destination.read_bytes()
            except OSError as exc:
                raise ValueError(
                    f"vendored blob is missing or unreadable: {relative.as_posix()}"
                ) from exc
            if actual_data != expected_data:
                raise ValueError(f"vendored blob differs from donor: {relative.as_posix()}")
            expected_hashes[relative.as_posix()] = hashlib.sha256(
                expected_data
            ).hexdigest()
            declared_files.add(relative)

    expected_hashes = dict(sorted(expected_hashes.items()))
    provenance_path = output_root / "VENDORED_SHA256.json"
    _reject_symlink_components(provenance_path, output_root)
    try:
        provenance = json.loads(provenance_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ValueError("VENDORED_SHA256.json is missing or invalid") from exc
    if provenance != expected_hashes:
        raise ValueError("VENDORED_SHA256.json does not exactly match the snapshot")

    vendor_root = output_root / "vendor"
    _reject_symlink_components(vendor_root, output_root)
    try:
        vendor_entries = list(vendor_root.rglob("*"))
    except OSError as exc:
        raise ValueError("vendor snapshot is unreadable") from exc
    for path in vendor_entries:
        if path.is_symlink():
            raise ValueError(f"unsafe vendor destination symlink: {path}")
        if path.is_file():
            relative = path.relative_to(output_root)
            if relative not in declared_files:
                raise ValueError(
                    f"undeclared vendor file: {relative.as_posix()}"
                )

    return expected_hashes


def main(argv: Sequence[str] | None = None) -> int:
    firmware_root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--repo-root", "--repository", dest="repository", type=Path,
        default=firmware_root.parent,
        help="repository containing the pinned donor commit",
    )
    parser.add_argument(
        "--manifest", type=Path, default=firmware_root / "VENDOR_MANIFEST.json"
    )
    parser.add_argument("--output-root", type=Path, default=firmware_root)
    parser.add_argument(
        "--check", action="store_true",
        help="verify the existing snapshot without modifying it",
    )
    arguments = parser.parse_args(argv)

    if arguments.check:
        hashes = check_snapshot(
            arguments.repository, arguments.manifest, arguments.output_root
        )
        print(f"verified {len(hashes)} pinned donor files")
    else:
        hashes = snapshot(
            arguments.repository, arguments.manifest, arguments.output_root
        )
        print(f"snapshotted {len(hashes)} pinned donor files")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
