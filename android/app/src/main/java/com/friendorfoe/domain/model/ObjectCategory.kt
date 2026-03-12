package com.friendorfoe.domain.model

/**
 * Category classification for detected sky objects.
 *
 * Used for color-coding in the AR overlay:
 * - COMMERCIAL -> Green
 * - GENERAL_AVIATION -> Yellow
 * - MILITARY -> Red
 * - DRONE -> Blue
 * - UNKNOWN -> Gray
 */
enum class ObjectCategory {
    /** Commercial airline flight */
    COMMERCIAL,

    /** General aviation / private aircraft */
    GENERAL_AVIATION,

    /** Military aircraft */
    MILITARY,

    /** Drone / UAS */
    DRONE,

    /** Unidentified or insufficient data */
    UNKNOWN
}
