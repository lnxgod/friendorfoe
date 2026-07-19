#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int16_t x;
    int16_t y;
    int8_t vx;
    int8_t vy;
    uint8_t color_index;
} badge_easter_egg_animation_t;

void badge_easter_egg_animation_init(badge_easter_egg_animation_t *animation);

bool badge_easter_egg_animation_step(
    badge_easter_egg_animation_t *animation,
    int16_t screen_width,
    int16_t screen_height,
    int16_t sprite_width,
    int16_t sprite_height,
    uint8_t color_count);

#ifdef __cplusplus
}
#endif
