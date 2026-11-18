#!/usr/bin/env python3
"""Check ELF LOAD segments in the 64-bit native libraries packaged in an APK/AAB.

This gate checks the actual prebuilt libraries, not their declared SDK versions.
APK ZIP entry alignment (zipalign) and AAB delivery alignment (bundletool) are
separate packaging checks; passing this gate does not prove runtime compatibility.
"""

import argparse
import re
import struct
import sys
import zipfile
from pathlib import Path

PAGE = 16384
MACHINES = {"arm64-v8a": 183, "x86_64": 62}
LIBRARY = re.compile(r"(?:[^/]+/)?lib/(arm64-v8a|x86_64)/[^/]+\.so")


def check_elf(data: bytes, abi: str) -> None:
    if len(data) < 64 or data[:7] != b"\x7fELF\x02\x01\x01":
        raise ValueError("expected a little-endian ELF64 library")
    if struct.unpack_from("<H", data, 18)[0] != MACHINES[abi]:
        raise ValueError(f"ELF machine does not match {abi}")
    offset = struct.unpack_from("<Q", data, 32)[0]
    stride, count = struct.unpack_from("<HH", data, 54)
    if stride < 56 or offset < 64 or offset + stride * count > len(data):
        raise ValueError("invalid ELF program header table")
    loads = 0
    for i in range(count):
        kind, _, file_off, vaddr, _, _, _, align = struct.unpack_from("<IIQQQQQQ", data, offset + stride * i)
        if kind == 1:  # PT_LOAD
            loads += 1
            if align < PAGE or align & (align - 1) or (file_off - vaddr) % PAGE:
                raise ValueError(f"PT_LOAD {i}: p_align={align}, offset={file_off:#x}, vaddr={vaddr:#x}")
    if not loads:
        raise ValueError("no PT_LOAD segments")


def check_archive(path: Path) -> tuple[int, list[str]]:
    errors = []
    count = 0
    has_arm64 = False
    with zipfile.ZipFile(path) as archive:
        for entry in archive.infolist():
            match = LIBRARY.fullmatch(entry.filename)
            if match:
                abi = match[1]
                count += 1
                has_arm64 |= abi == "arm64-v8a"
                try:
                    check_elf(archive.read(entry), abi)
                except ValueError as exc:
                    errors.append(f"{entry.filename}: {exc}")
    if not has_arm64:
        errors.append("expected packaged arm64-v8a libraries for this app")
    return count, errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", type=Path)
    args = parser.parse_args()
    try:
        count, errors = check_archive(args.archive)
    except (OSError, zipfile.BadZipFile) as exc:
        print(exc, file=sys.stderr)
        return 1
    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 1
    print(f"{args.archive.name}: all {count} 64-bit ELF libraries have 16 KiB-compatible LOAD alignment")
    return 0


if __name__ == "__main__":
    sys.exit(main())
