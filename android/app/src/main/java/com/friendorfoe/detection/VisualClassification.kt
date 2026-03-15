package com.friendorfoe.detection

/** Sky-specific classification for visual detections after heuristic filtering. */
enum class VisualClassification {
    /** Medium-to-large, fast, straight-line trajectory */
    LIKELY_AIRCRAFT,
    /** Small, slow/hovering movement pattern */
    LIKELY_DRONE,
    /** Small, erratic movement pattern */
    LIKELY_BIRD,
    /** Moving object that doesn't match other patterns */
    UNKNOWN_FLYING,
    /** Stationary — ground clutter, buildings, poles */
    LIKELY_STATIC
}

/** Alert escalation level for persistent unidentified objects. */
enum class AlertLevel {
    /** Default — recently detected or low interest */
    NORMAL,
    /** Tracked continuously for >3 seconds */
    INTEREST,
    /** Tracked continuously for >10 seconds with no radio match */
    ALERT
}
