package com.friendorfoe.domain.model

/**
 * Category classification for detected sky objects.
 *
 * Used for color-coding in the AR overlay and list/detail views.
 */
enum class ObjectCategory {
    /** Commercial airline flight */
    COMMERCIAL,

    /** General aviation / private aircraft */
    GENERAL_AVIATION,

    /** Military aircraft */
    MILITARY,

    /** Rotorcraft (ADS-B category A7) */
    HELICOPTER,

    /** Government / law enforcement aircraft */
    GOVERNMENT,

    /** Emergency — squawk 7500/7600/7700 or MEDEVAC callsigns */
    EMERGENCY,

    /** Cargo carriers (FedEx, UPS, Atlas, etc.) */
    CARGO,

    /** Drone / UAS */
    DRONE,

    /** ADS-B surface vehicles (category B) */
    GROUND_VEHICLE,

    /** Unidentified or insufficient data */
    UNKNOWN
}
