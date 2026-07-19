package com.friendorfoe.data.badge

enum class BadgeUsbActivityKind { DETECTION, COMMAND, FIRMWARE, STATUS, ERROR }

data class BadgeUsbActivity(
    val kind: BadgeUsbActivityKind,
    val key: String,
    val title: String,
    val detail: String,
    val receivedAtElapsedMs: Long,
)

internal const val MAX_BADGE_USB_ACTIVITY = 64

val BadgeUsbDetection.stableKey: String
    get() = when {
        badgeEntityKey.isNotBlank() -> "entity:$badgeEntityKey"
        id.isNotBlank() -> "id:$id"
        else -> "source:$source:${badgeClass}:${manufacturer}:${rssi}"
    }

internal fun pushBadgeUsbActivity(
    current: List<BadgeUsbActivity>,
    next: BadgeUsbActivity,
    limit: Int = MAX_BADGE_USB_ACTIVITY,
): List<BadgeUsbActivity> = buildList {
    add(next)
    current.asSequence()
        .filterNot { it.key == next.key }
        .take((limit - 1).coerceAtLeast(0))
        .forEach(::add)
}
