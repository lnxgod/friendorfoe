#!/usr/bin/env python3
"""Reject EXIF GPS directories and XMP coordinate tags in bundled reference photos."""

import argparse
import re
import struct
import sys
from pathlib import Path
from zipfile import ZipFile

ASSET = re.compile(r"(?:[^/]+/)?assets/(?:aircraft|drones)/[^/]+\.(?:jpg|jpeg|png)$", re.I)
XMP_GPS = re.compile(br"(?:exif|Iptc4xmpExt):GPS(?:Latitude|Longitude)", re.I)


def gps_ifd(tiff: bytes) -> bool:
    endian = {b"II": "<", b"MM": ">"}.get(tiff[:2])
    if not endian or len(tiff) < 8 or struct.unpack_from(endian + "H", tiff, 2)[0] != 42:
        raise ValueError("invalid EXIF header")
    start = struct.unpack_from(endian + "I", tiff, 4)[0]
    if start + 2 > len(tiff):
        raise ValueError("invalid EXIF directory offset")
    count = struct.unpack_from(endian + "H", tiff, start)[0]
    if start + 2 + 12 * count > len(tiff):
        raise ValueError("truncated EXIF directory")
    return any(struct.unpack_from(endian + "H", tiff, start + 2 + i * 12)[0] == 0x8825 for i in range(count))


def contains_gps(photo: bytes) -> bool:
    if XMP_GPS.search(photo):
        return True
    # JPEG APP1 EXIF segments use this marker. Public JPEGs may contain no EXIF.
    for m in re.finditer(b"\\xff\\xe1..Exif\\x00\\x00", photo, re.S):
        length = int.from_bytes(photo[m.start() + 2:m.start() + 4], "big")
        if gps_ifd(photo[m.end():m.start() + 2 + length]):
            return True
    # Some references have a .jpg extension but are actually PNG files.
    if photo.startswith(b"\x89PNG\r\n\x1a\n"):
        p = 8
        while p + 12 <= len(photo):
            length = int.from_bytes(photo[p:p + 4], "big")
            if photo[p + 4:p + 8] == b"eXIf" and gps_ifd(photo[p + 8:p + 8 + length]):
                return True
            p += length + 12
    return False


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", type=Path, help="APK or AAB")
    args = parser.parse_args()
    count = 0
    with ZipFile(args.archive) as archive:
        for name in archive.namelist():
            if not ASSET.fullmatch(name):
                continue
            count += 1
            if contains_gps(archive.read(name)):
                print(f"Bundled reference image contains GPS metadata: {name}", file=sys.stderr)
                return 1
    if not count:
        print("No bundled reference images found", file=sys.stderr)
        return 1
    print(f"Checked {count} packaged reference images for EXIF/XMP GPS metadata")
    return 0


if __name__ == "__main__":
    sys.exit(main())
