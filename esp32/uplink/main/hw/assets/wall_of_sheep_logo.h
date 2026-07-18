/* Generated badge asset. Do not edit by hand.
 * Official PHV source: https://www.phvillage.io/wp-content/uploads/2022/10/wall-of-sheep_1-1.png
 * Verified source SHA-256: 41e4d1641b14464a08496a975f7e073bb5692817a65b3eb1052f83e87da4cbe0
 * Conversion command: python3 esp32/scripts/convert_badge_logo.py --input esp32/uplink/assets/wall-of-sheep.png --header esp32/uplink/main/hw/assets/wall_of_sheep_logo.h --source esp32/uplink/main/hw/assets/wall_of_sheep_logo.c --max-size 72
 */
#pragma once

#ifdef FOF_BADGE_VARIANT

#include <stdint.h>

#define WALL_OF_SHEEP_LOGO_WIDTH 72u
#define WALL_OF_SHEEP_LOGO_HEIGHT 72u
#define WALL_OF_SHEEP_LOGO_PALETTE_SIZE 8u
#define WALL_OF_SHEEP_LOGO_TRANSPARENT_INDEX 0u

extern const uint16_t wall_of_sheep_logo_palette[WALL_OF_SHEEP_LOGO_PALETTE_SIZE];
extern const uint8_t wall_of_sheep_logo_pixels[
    WALL_OF_SHEEP_LOGO_WIDTH * WALL_OF_SHEEP_LOGO_HEIGHT];

#endif /* FOF_BADGE_VARIANT */
