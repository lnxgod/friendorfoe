#!/usr/bin/env python3
"""Convert the verified PHV Wall of Sheep mark to an indexed badge asset."""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path

from PIL import Image


EXPECTED_SHA256 = "41e4d1641b14464a08496a975f7e073bb5692817a65b3eb1052f83e87da4cbe0"
SOURCE_URL = "https://www.phvillage.io/wp-content/uploads/2022/10/wall-of-sheep_1-1.png"
CONVERSION_COMMAND = (
    "python3 esp32/scripts/convert_badge_logo.py "
    "--input esp32/uplink/assets/wall-of-sheep.png "
    "--header esp32/uplink/main/hw/assets/wall_of_sheep_logo.h "
    "--source esp32/uplink/main/hw/assets/wall_of_sheep_logo.c "
    "--max-size 72"
)

# Transparent plus a deliberately small black-to-white ultraviolet ramp.
# Keeping every chromatic entry purple makes the circular lettering and sheep
# silhouette survive aggressive quantization without carrying the source PNG
# decoder or a large RGB565 bitmap into firmware.
PALETTE_RGB = (
    (0x00, 0x00, 0x00),
    (0x00, 0x00, 0x00),
    (0x16, 0x00, 0x20),
    (0x36, 0x00, 0x52),
    (0x66, 0x2D, 0x91),
    (0x9B, 0x5D, 0xE5),
    (0xD8, 0xB4, 0xFE),
    (0xFF, 0xFF, 0xFF),
)


def rgb565(rgb: tuple[int, int, int]) -> int:
    red, green, blue = rgb
    return ((red & 0xF8) << 8) | ((green & 0xFC) << 3) | (blue >> 3)


def nearest_palette_index(red: int, green: int, blue: int) -> int:
    best_index = 1
    best_distance: int | None = None
    for index, (pr, pg, pb) in enumerate(PALETTE_RGB[1:], start=1):
        # Perceptual channel weighting preserves white type and black outlines.
        distance = 30 * (red - pr) ** 2 + 59 * (green - pg) ** 2 + 11 * (blue - pb) ** 2
        if best_distance is None or distance < best_distance:
            best_index = index
            best_distance = distance
    return best_index


def format_values(values: list[int], width: int, formatter) -> str:
    lines = []
    for start in range(0, len(values), width):
        chunk = ", ".join(formatter(value) for value in values[start : start + width])
        lines.append(f"    {chunk},")
    return "\n".join(lines)


def write_header(path: Path, width: int, height: int) -> None:
    path.write_text(
        f"""/* Generated badge asset. Do not edit by hand.
 * Official PHV source: {SOURCE_URL}
 * Verified source SHA-256: {EXPECTED_SHA256}
 * Conversion command: {CONVERSION_COMMAND}
 */
#pragma once

#ifdef FOF_BADGE_VARIANT

#include <stdint.h>

#define WALL_OF_SHEEP_LOGO_WIDTH {width}u
#define WALL_OF_SHEEP_LOGO_HEIGHT {height}u
#define WALL_OF_SHEEP_LOGO_PALETTE_SIZE {len(PALETTE_RGB)}u
#define WALL_OF_SHEEP_LOGO_TRANSPARENT_INDEX 0u

extern const uint16_t wall_of_sheep_logo_palette[WALL_OF_SHEEP_LOGO_PALETTE_SIZE];
extern const uint8_t wall_of_sheep_logo_pixels[
    WALL_OF_SHEEP_LOGO_WIDTH * WALL_OF_SHEEP_LOGO_HEIGHT];

#endif /* FOF_BADGE_VARIANT */
""",
        encoding="utf-8",
    )


def write_source(path: Path, indices: list[int]) -> None:
    palette = [rgb565(color) for color in PALETTE_RGB]
    path.write_text(
        f"""/* Generated from the verified official PHV Wall of Sheep source.
 * Indexed pixels and palette are const so the linker keeps them in flash.
 */
#ifdef FOF_BADGE_VARIANT

#include "wall_of_sheep_logo.h"

const uint16_t wall_of_sheep_logo_palette[WALL_OF_SHEEP_LOGO_PALETTE_SIZE] = {{
{format_values(palette, 8, lambda value: f"0x{value:04X}")}
}};

const uint8_t wall_of_sheep_logo_pixels[
    WALL_OF_SHEEP_LOGO_WIDTH * WALL_OF_SHEEP_LOGO_HEIGHT] = {{
{format_values(indices, 24, str)}
}};

#endif /* FOF_BADGE_VARIANT */
""",
        encoding="utf-8",
    )


def write_preview(path: Path, width: int, height: int, indices: list[int]) -> None:
    preview = Image.new("P", (width, height))
    flat_palette = [channel for rgb in PALETTE_RGB for channel in rgb]
    preview.putpalette(flat_palette + [0] * (768 - len(flat_palette)))
    preview.putdata(indices)
    preview.info["transparency"] = 0
    preview.save(path)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--header", required=True, type=Path)
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--max-size", type=int, default=72)
    parser.add_argument("--preview", type=Path)
    args = parser.parse_args()

    source_bytes = args.input.read_bytes()
    actual_hash = hashlib.sha256(source_bytes).hexdigest()
    if actual_hash != EXPECTED_SHA256:
        raise SystemExit(
            f"refusing unverified logo: expected {EXPECTED_SHA256}, got {actual_hash}"
        )
    if args.max_size < 1 or args.max_size > 72:
        raise SystemExit("--max-size must be between 1 and 72")

    with Image.open(args.input) as source_image:
        image = source_image.convert("RGBA")
    image.thumbnail((args.max_size, args.max_size), Image.Resampling.LANCZOS)

    indices: list[int] = []
    pixels = image.load()
    for y in range(image.height):
        for x in range(image.width):
            red, green, blue, alpha = pixels[x, y]
            indices.append(
                0 if alpha < 32 else nearest_palette_index(red, green, blue)
            )

    args.header.parent.mkdir(parents=True, exist_ok=True)
    args.source.parent.mkdir(parents=True, exist_ok=True)
    write_header(args.header, image.width, image.height)
    write_source(args.source, indices)
    if args.preview:
        write_preview(args.preview, image.width, image.height, indices)

    print(
        f"verified {actual_hash}; wrote {image.width}x{image.height}, "
        f"{len(PALETTE_RGB)} colors, {len(indices)} indexed pixels"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
