#include "badge_easter_egg_animation.h"

void badge_easter_egg_animation_init(badge_easter_egg_animation_t *animation)
{
    if (!animation) {
        return;
    }

    animation->x = 8;
    animation->y = 12;
    animation->vx = 3;
    animation->vy = 2;
    animation->color_index = 0;
}

bool badge_easter_egg_animation_step(
    badge_easter_egg_animation_t *animation,
    int16_t screen_width,
    int16_t screen_height,
    int16_t sprite_width,
    int16_t sprite_height,
    uint8_t color_count)
{
    if (!animation) {
        return false;
    }
    if (screen_width <= 0 || screen_height <= 0 ||
        sprite_width <= 0 || sprite_height <= 0 ||
        sprite_width > screen_width || sprite_height > screen_height ||
        color_count == 0) {
        animation->x = 0;
        animation->y = 0;
        animation->color_index = 0;
        return false;
    }

    const int16_t max_x = (int16_t)(screen_width - sprite_width);
    const int16_t max_y = (int16_t)(screen_height - sprite_height);
    int16_t next_x = (int16_t)(animation->x + animation->vx);
    int16_t next_y = (int16_t)(animation->y + animation->vy);
    bool collided = false;

    if (next_x < 0) {
        next_x = 0;
        if (animation->vx < 0) animation->vx = (int8_t)-animation->vx;
        collided = true;
    } else if (next_x > max_x) {
        next_x = max_x;
        if (animation->vx > 0) animation->vx = (int8_t)-animation->vx;
        collided = true;
    }

    if (next_y < 0) {
        next_y = 0;
        if (animation->vy < 0) animation->vy = (int8_t)-animation->vy;
        collided = true;
    } else if (next_y > max_y) {
        next_y = max_y;
        if (animation->vy > 0) animation->vy = (int8_t)-animation->vy;
        collided = true;
    }

    animation->x = next_x;
    animation->y = next_y;
    if (collided) {
        animation->color_index =
            (uint8_t)((animation->color_index + 1U) % color_count);
    }
    return collided;
}
