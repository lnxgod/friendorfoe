/* Generated badge asset. Do not edit by hand.
 * Official GameChangersAI source: https://gamechangersai.org/assets/gamechangers-128.png
 * Verified source SHA-256: 903d20f0b3d52c8b5b785686680cbb5e884ea17a5636fdf381e9752ade92efce
 * Conversion command: python3 esp32/scripts/convert_gamechangersai_logo.py --input esp32/uplink/assets/gamechangersai-logo.png --header esp32/uplink/main/hw/assets/gamechangersai_logo.h --source esp32/uplink/main/hw/assets/gamechangersai_logo.c --size 64
 */
#pragma once

#ifdef FOF_BADGE_VARIANT

#include <stdint.h>

#define GAMECHANGERSAI_LOGO_WIDTH 64u
#define GAMECHANGERSAI_LOGO_HEIGHT 64u
#define GAMECHANGERSAI_LOGO_PALETTE_SIZE 8u
#define GAMECHANGERSAI_LOGO_TRANSPARENT_INDEX 0u

extern const uint8_t gamechangersai_logo_levels[
    GAMECHANGERSAI_LOGO_WIDTH * GAMECHANGERSAI_LOGO_HEIGHT];

#endif /* FOF_BADGE_VARIANT */
