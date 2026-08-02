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


def emit_fixture(
    repo_root: Path,
    *,
    compiler: str,
    build_dir: Path,
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


def _parse_args(argv: list[str] | None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Emit or verify the canonical backend serializer fixture.",
    )
    action = parser.add_mutually_exclusive_group(required=True)
    action.add_argument("--output", type=Path)
    action.add_argument("--check", type=Path)
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = _parse_args(argv)
    repo_root = Path(__file__).resolve().parents[2]
    with tempfile.TemporaryDirectory(prefix="fof-backend-serializer-") as raw:
        payload = emit_fixture(
            repo_root,
            compiler="cc",
            build_dir=Path(raw),
        )
    canonical = payload + b"\n"
    if args.output is not None:
        _atomic_write(args.output, canonical)
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
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
