#!/usr/bin/env python3
"""Convert the verified official GameChangersAI logo to a badge sprite."""

from __future__ import annotations

import argparse
import hashlib
import io
from pathlib import Path

from PIL import Image


EXPECTED_SHA256 = "903d20f0b3d52c8b5b785686680cbb5e884ea17a5636fdf381e9752ade92efce"
SOURCE_URL = "https://gamechangersai.org/assets/gamechangers-128.png"
PALETTE_SIZE = 8


def verify_source(source_bytes: bytes) -> str:
    actual = hashlib.sha256(source_bytes).hexdigest()
    if actual != EXPECTED_SHA256:
        raise ValueError(
            f"refusing unverified logo: expected {EXPECTED_SHA256}, got {actual}"
        )
    return actual


def level_for_pixel(red: int, green: int, blue: int, alpha: int) -> int:
    luminance = (30 * red + 59 * green + 11 * blue) // 100
    if alpha < 32 or luminance < 70:
        return 0
    return min(7, 1 + ((luminance - 70) * 6 // 185))


def format_values(values: list[int], width: int = 24) -> str:
    lines = []
    for start in range(0, len(values), width):
        lines.append("    " + ", ".join(str(v) for v in values[start:start + width]) + ",")
    return "\n".join(lines)


def generated_header(size: int) -> str:
    command = (
        "python3 esp32/scripts/convert_gamechangersai_logo.py "
        "--input esp32/uplink/assets/gamechangersai-logo.png "
        "--header esp32/uplink/main/hw/assets/gamechangersai_logo.h "
        "--source esp32/uplink/main/hw/assets/gamechangersai_logo.c --size 64"
    )
    return f"""/* Generated badge asset. Do not edit by hand.
 * Official GameChangersAI source: {SOURCE_URL}
 * Verified source SHA-256: {EXPECTED_SHA256}
 * Conversion command: {command}
 */
#pragma once

#ifdef FOF_BADGE_VARIANT

#include <stdint.h>

#define GAMECHANGERSAI_LOGO_WIDTH {size}u
#define GAMECHANGERSAI_LOGO_HEIGHT {size}u
#define GAMECHANGERSAI_LOGO_PALETTE_SIZE {PALETTE_SIZE}u
#define GAMECHANGERSAI_LOGO_TRANSPARENT_INDEX 0u

extern const uint8_t gamechangersai_logo_levels[
    GAMECHANGERSAI_LOGO_WIDTH * GAMECHANGERSAI_LOGO_HEIGHT];

#endif /* FOF_BADGE_VARIANT */
"""


def generated_source(levels: list[int]) -> str:
    return f"""/* Generated from the verified official GameChangersAI source.
 * Indexed luminance levels are const so the linker keeps them in flash.
 */
#ifdef FOF_BADGE_VARIANT

#include "gamechangersai_logo.h"

const uint8_t gamechangersai_logo_levels[
    GAMECHANGERSAI_LOGO_WIDTH * GAMECHANGERSAI_LOGO_HEIGHT] = {{
{format_values(levels)}
}};

#endif /* FOF_BADGE_VARIANT */
"""


def generate(input_path: Path, header_path: Path, source_path: Path,
             size: int) -> tuple[int, int, int]:
    if size < 1 or size > 64:
        raise ValueError("size must be between 1 and 64")

    source_bytes = input_path.read_bytes()
    verify_source(source_bytes)
    with Image.open(io.BytesIO(source_bytes)) as source_image:
        image = source_image.convert("RGBA").resize(
            (size, size), Image.Resampling.LANCZOS
        )
        rgba = image.tobytes()
        levels = [
            level_for_pixel(*rgba[offset:offset + 4])
            for offset in range(0, len(rgba), 4)
        ]

    header_text = generated_header(size)
    source_text = generated_source(levels)
    header_path.parent.mkdir(parents=True, exist_ok=True)
    source_path.parent.mkdir(parents=True, exist_ok=True)
    header_tmp = header_path.with_suffix(header_path.suffix + ".tmp")
    source_tmp = source_path.with_suffix(source_path.suffix + ".tmp")
    header_tmp.write_text(header_text, encoding="utf-8")
    source_tmp.write_text(source_text, encoding="utf-8")
    header_tmp.replace(header_path)
    source_tmp.replace(source_path)
    return image.width, image.height, len(levels)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--header", required=True, type=Path)
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--size", type=int, default=64)
    args = parser.parse_args()

    width, height, pixels = generate(
        args.input, args.header, args.source, args.size
    )
    print(
        f"verified {EXPECTED_SHA256}; wrote {width}x{height}, "
        f"{PALETTE_SIZE} levels, {pixels} indexed pixels"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
