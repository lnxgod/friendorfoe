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
from typing import Any


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
    if requested_output_root.is_symlink():
        raise ValueError(f"unsafe vendor output root symlink: {requested_output_root}")
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


def main() -> int:
    firmware_root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--repository", type=Path, default=firmware_root.parent,
        help="repository containing the pinned donor commit",
    )
    parser.add_argument(
        "--manifest", type=Path, default=firmware_root / "VENDOR_MANIFEST.json"
    )
    parser.add_argument("--output-root", type=Path, default=firmware_root)
    arguments = parser.parse_args()

    hashes = snapshot(arguments.repository, arguments.manifest, arguments.output_root)
    print(f"snapshotted {len(hashes)} pinned donor files")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
