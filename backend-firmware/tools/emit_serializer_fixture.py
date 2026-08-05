#!/usr/bin/env python3
"""Compile the real backend serializer and emit its canonical host fixture."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile


PRODUCTION_SOURCES = (
    "shared/backend_upload_batch.c",
    "shared/backend_detection_codec.c",
    "shared/backend_json_writer.c",
)

PRODUCTION_DEPENDENCIES = (
    "shared/backend_identity.c",
    "shared/backend_scanner_topology.c",
    "shared/backend_json_reader.c",
)

PROFILE_BADGE_LITE = "FOF_BACKEND_PROFILE_BADGE_LITE"
PROFILE_S3_FULLSIZE = "FOF_BACKEND_PROFILE_S3_FULLSIZE"


def emit_fixture(
    repo_root: Path,
    *,
    compiler: str,
    build_dir: Path,
    profile: str = PROFILE_BADGE_LITE,
) -> bytes:
    backend_fw = repo_root / "backend-firmware"
    build_dir.mkdir(parents=True, exist_ok=True)
    executable = build_dir / "backend-serializer-fixture"
    command = [
        compiler,
        "-std=c11",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-DUNIT_TESTING",
        f"-D{profile}=1",
        f"-I{backend_fw / 'shared'}",
        f"-I{backend_fw / 'test/stubs'}",
        str(backend_fw / "test/support/backend_serializer_fixture.c"),
        *(str(backend_fw / source) for source in PRODUCTION_SOURCES),
        *(str(backend_fw / source) for source in PRODUCTION_DEPENDENCIES),
        "-o",
        str(executable),
    ]
    subprocess.run(command, check=True, cwd=repo_root)
    return subprocess.run(
        [str(executable)],
        check=True,
        cwd=repo_root,
        capture_output=True,
    ).stdout


def _atomic_write(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary: Path | None = None
    try:
        with tempfile.NamedTemporaryFile(
            mode="wb",
            dir=path.parent,
            prefix=f".{path.name}.",
            delete=False,
        ) as handle:
            temporary = Path(handle.name)
            handle.write(data)
            handle.flush()
            os.fsync(handle.fileno())
        temporary.chmod(0o644)
        os.replace(temporary, path)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def _write_temp(path: Path, data: bytes, label: str) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(
        mode="wb", dir=path.parent, prefix=f".{path.name}.{label}-",
        delete=False,
    ) as handle:
        temporary = Path(handle.name)
        handle.write(data)
        handle.flush()
        os.fsync(handle.fileno())
    temporary.chmod(0o644)
    return temporary


def write_fixture_pair(
    lite_path: Path,
    lite_data: bytes,
    fullsize_path: Path,
    fullsize_data: bytes,
) -> None:
    """Publish a fixture pair with rollback if either replacement fails."""
    new_lite = _write_temp(lite_path, lite_data, "new")
    new_fullsize = _write_temp(fullsize_path, fullsize_data, "new")
    backups: list[tuple[Path, Path]] = []
    published: list[tuple[Path, Path]] = []
    try:
        for destination in (lite_path, fullsize_path):
            if destination.exists():
                backup = _write_temp(destination, destination.read_bytes(), "backup")
                backups.append((destination, backup))
        for destination, temporary in ((lite_path, new_lite),
                                       (fullsize_path, new_fullsize)):
            os.replace(temporary, destination)
            published.append((destination, temporary))
    except Exception:
        for destination, backup in backups:
            os.replace(backup, destination)
        for destination, temporary in published:
            if not any(saved == destination for saved, _ in backups):
                destination.unlink(missing_ok=True)
            temporary.unlink(missing_ok=True)
        raise
    finally:
        new_lite.unlink(missing_ok=True)
        new_fullsize.unlink(missing_ok=True)
        for _destination, backup in backups:
            backup.unlink(missing_ok=True)


def _parse_args(argv: list[str] | None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Emit or verify the canonical backend serializer fixture.",
    )
    action = parser.add_mutually_exclusive_group(required=True)
    action.add_argument("--output", type=Path)
    action.add_argument("--check", type=Path)
    parser.add_argument("--fullsize-output", type=Path)
    parser.add_argument("--fullsize-check", type=Path)
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = _parse_args(argv)
    if args.fullsize_output is not None and args.output is None:
        raise SystemExit("--fullsize-output requires --output")
    if args.fullsize_check is not None and args.check is None:
        raise SystemExit("--fullsize-check requires --check")
    repo_root = Path(__file__).resolve().parents[2]
    with tempfile.TemporaryDirectory(prefix="fof-backend-serializer-") as raw:
        build_dir = Path(raw)
        payload = emit_fixture(
            repo_root,
            compiler="cc",
            build_dir=build_dir / "badge-lite",
            profile=PROFILE_BADGE_LITE,
        )
    canonical = payload + b"\n"
    if args.output is not None:
        fullsize_canonical = None
        if args.fullsize_output is not None:
            with tempfile.TemporaryDirectory(
                prefix="fof-backend-serializer-fullsize-",
            ) as raw:
                fullsize_canonical = emit_fixture(
                    repo_root,
                    compiler="cc",
                    build_dir=Path(raw),
                    profile=PROFILE_S3_FULLSIZE,
                ) + b"\n"
        if fullsize_canonical is None:
            _atomic_write(args.output, canonical)
        else:
            write_fixture_pair(args.output, canonical,
                               args.fullsize_output, fullsize_canonical)
        return 0

    try:
        checked = args.check.read_bytes()
    except OSError as exc:
        print(f"cannot read serializer fixture: {exc}", file=sys.stderr)
        return 1
    if checked != canonical:
        print(
            f"serializer fixture differs: regenerate {args.check}",
            file=sys.stderr,
        )
        return 1
    if args.fullsize_check is not None:
        with tempfile.TemporaryDirectory(
            prefix="fof-backend-serializer-fullsize-",
        ) as raw:
            fullsize_canonical = emit_fixture(
                repo_root,
                compiler="cc",
                build_dir=Path(raw),
                profile=PROFILE_S3_FULLSIZE,
            ) + b"\n"
        try:
            fullsize_checked = args.fullsize_check.read_bytes()
        except OSError as exc:
            print(f"cannot read serializer fixture: {exc}", file=sys.stderr)
            return 1
        if fullsize_checked != fullsize_canonical:
            print(
                f"serializer fixture differs: regenerate {args.fullsize_check}",
                file=sys.stderr,
            )
            return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
