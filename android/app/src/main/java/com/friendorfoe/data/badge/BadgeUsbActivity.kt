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

internal fun badgeUsbActivityForLine(
    line: String,
    receivedAtElapsedMs: Long,
    detection: BadgeUsbDetection? = null,
    status: BadgeControlStatus? = null,
    firmwareProgress: BadgeFirmwareProgress? = null,
    investigationHandled: Boolean = false,
): BadgeUsbActivity? {
    val trimmed = line.trim()
    return when {
        trimmed.startsWith("FOF_DET:") && detection != null -> BadgeUsbActivity(
            kind = BadgeUsbActivityKind.DETECTION,
            key = detection.stableKey,
            title = detection.badgeLabel.ifBlank {
                detection.manufacturer.ifBlank { "Badge detection" }
            },
            detail = listOf(detection.badgeClass, detection.id)
                .filter { it.isNotBlank() }
                .joinToString(" · "),
            receivedAtElapsedMs = detection.receivedAtElapsedMs,
        )
        trimmed.startsWith("FOF_FW_") && firmwareProgress != null -> BadgeUsbActivity(
            kind = BadgeUsbActivityKind.FIRMWARE,
            key = "firmware:${firmwareProgress.kind}:${firmwareProgress.stage}",
            title = "Firmware ${firmwareProgress.kind} ${firmwareProgress.stage}",
            detail = firmwareProgress.error.ifBlank { "${firmwareProgress.percent}%" },
            receivedAtElapsedMs = receivedAtElapsedMs,
        )
        trimmed.startsWith("FOF_STATUS:") && status != null -> BadgeUsbActivity(
            kind = BadgeUsbActivityKind.STATUS,
            key = "status:${status.mode}:${status.version}",
            title = "Badge status updated",
            detail = listOf(status.modeLabel, status.version)
                .filter { it.isNotBlank() }
                .joinToString(" · "),
            receivedAtElapsedMs = receivedAtElapsedMs,
        )
        trimmed.startsWith("FOF_INV:") && investigationHandled -> BadgeUsbActivity(
            kind = BadgeUsbActivityKind.STATUS,
            key = "investigation:${trimmed.take(160)}",
            title = "Badge investigation updated",
            detail = trimmed.removePrefix("FOF_INV:").take(160),
            receivedAtElapsedMs = receivedAtElapsedMs,
        )
        trimmed.startsWith("FOF_CTL_OK:") || trimmed.startsWith("FOF_CTL_ERROR:") ->
            BadgeUsbActivity(
                kind = if (trimmed.startsWith("FOF_CTL_ERROR:")) {
                    BadgeUsbActivityKind.ERROR
                } else {
                    BadgeUsbActivityKind.COMMAND
                },
                key = "control:${trimmed.take(160)}",
                title = if (trimmed.startsWith("FOF_CTL_ERROR:")) {
                    "Badge command failed"
                } else {
                    "Badge command accepted"
                },
                detail = trimmed.substringAfter(':').take(160),
                receivedAtElapsedMs = receivedAtElapsedMs,
            )
        else -> null
    }
}
