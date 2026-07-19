package com.friendorfoe.presentation.badge

import com.friendorfoe.data.badge.BadgeControlStatus
import com.friendorfoe.data.badge.BadgeScannerStatus
import com.friendorfoe.data.badge.BadgeUsbState
import com.friendorfoe.data.badge.BadgeUsbStatus
import java.util.Locale

internal fun badgeStatusIdentityLines(state: BadgeUsbState): List<String> = buildList {
    state.deviceName?.trim()?.takeIf { it.isNotEmpty() }?.let(::add)
    state.message.trim()
        .takeIf { it.isNotEmpty() && it != state.deviceName?.trim() }
        ?.let(::add)
}

internal fun badgeStatusIsStale(state: BadgeUsbState): Boolean =
    state.controlStatus != null && state.status !in setOf(
        BadgeUsbStatus.CONNECTED,
        BadgeUsbStatus.AP_CONNECTED,
        BadgeUsbStatus.DEBUG_BRIDGE_CONNECTED,
        BadgeUsbStatus.BLE_CONNECTED,
    )

internal fun badgeStatusHealthRows(status: BadgeControlStatus): List<Pair<String, String>> =
    buildList {
        add(
            "Uplink" to listOfNotNull(
                when {
                    status.safeMode ->
                        "SAFE MODE${status.safeReason.takeIf(String::isNotBlank)?.let { ": $it" }.orEmpty()}"
                    status.crashCount > 0 ->
                        "Attention: ${status.crashCount} recorded crash${if (status.crashCount == 1) "" else "es"}"
                    else -> "Healthy"
                },
                status.modeLabel.takeIf(String::isNotBlank),
                status.firmwareTarget.takeIf(String::isNotBlank),
            ).joinToString("  •  ")
        )

        if (status.scanners.isEmpty()) {
            add("Scanners" to "No scanner health reported")
        } else {
            status.scanners.sortedBy(BadgeScannerStatus::slot).forEach { scanner ->
                add("Scanner ${scanner.slot.coerceAtLeast(0)}" to scannerHealthText(scanner))
            }
        }

        add("USB age" to status.usbControlAgeSeconds?.let(::formatAge).orEmpty().ifBlank {
            "Not reported"
        })
        add(
            "Runtime" to listOfNotNull(
                "${status.crashCount} crash${if (status.crashCount == 1) "" else "es"}",
                status.resetReason.takeIf(String::isNotBlank)?.let { "reset $it" },
                status.recoveryMode.takeIf(String::isNotBlank)?.let { "recovery $it" },
            ).joinToString("  •  ")
        )
        add(
            "Heap" to memoryTriple(
                free = status.heapInternalFree,
                total = 0,
                minimum = status.heapInternalMinFree,
                largest = status.heapInternalLargest,
            )
        )
        add(
            "Stack" to listOf(
                "main ${formatBytes(status.stackMainFree.toLong())}",
                "LCD ${formatBytes(status.stackDisplayFree.toLong())}",
                "USB ${formatBytes(status.stackUsbFree.toLong())}",
                "BLE ${formatBytes(status.stackUartBleFree.toLong())}",
                "WiFi ${formatBytes(status.stackUartWifiFree.toLong())}",
            ).joinToString("  •  ")
        )
        add(
            "PSRAM" to memoryTriple(
                free = status.psramFree,
                total = status.psramTotal,
                minimum = 0,
                largest = status.psramLargest,
            )
        )
    }

private fun scannerHealthText(scanner: BadgeScannerStatus): String = listOfNotNull(
    if (scanner.connected) "Connected" else "Offline",
    scanner.slotRole.takeIf(String::isNotBlank)?.uppercase(Locale.US),
    scanner.scanProfile.takeIf(String::isNotBlank)?.let { "profile $it" },
    scanner.health.takeIf(String::isNotBlank),
    scanner.targetVersion.takeIf(String::isNotBlank)?.let { "v$it" },
    if (scanner.roleAcked) "role verified" else "role unverified",
).joinToString("  •  ")

private fun formatAge(seconds: Long): String = when {
    seconds < 60 -> "${seconds.coerceAtLeast(0)}s"
    seconds < 3600 -> "${seconds / 60}m ${seconds % 60}s"
    else -> "${seconds / 3600}h ${(seconds % 3600) / 60}m"
}

private fun memoryTriple(
    free: Long,
    total: Long,
    minimum: Long,
    largest: Long,
): String {
    if (free <= 0 && total <= 0 && minimum <= 0 && largest <= 0) return "Not reported"
    val capacity = when {
        free > 0 && total > 0 -> "${formatBytes(free)} free / ${formatBytes(total)}"
        free > 0 -> "${formatBytes(free)} free"
        total > 0 -> formatBytes(total)
        else -> null
    }
    return listOfNotNull(
        capacity,
        minimum.takeIf { it > 0 }?.let { "min ${formatBytes(it)}" },
        largest.takeIf { it > 0 }?.let { "largest ${formatBytes(it)}" },
    ).joinToString("  •  ")
}

private fun formatBytes(bytes: Long): String = when {
    bytes <= 0 -> "n/a"
    bytes >= 1024L * 1024L -> String.format(Locale.US, "%.1f MiB", bytes / (1024.0 * 1024.0))
    bytes >= 1024L -> String.format(Locale.US, "%.1f KiB", bytes / 1024.0)
    else -> "$bytes B"
}
