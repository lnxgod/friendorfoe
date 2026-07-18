package com.friendorfoe.data.badge

internal enum class BadgeControlTransport {
    USB,
}

/**
 * Crunch-mode badge commands are intentionally USB-only. Phone-side BLE
 * detection and read-only badge status over HTTP are separate capabilities.
 */
internal object BadgeControlTransportPolicy {
    fun select(
        hasUsb: Boolean,
        @Suppress("UNUSED_PARAMETER") hasBle: Boolean,
        @Suppress("UNUSED_PARAMETER") hasHttp: Boolean,
    ): BadgeControlTransport? = if (hasUsb) BadgeControlTransport.USB else null

    fun allowsBleTether(): Boolean = false

    fun allowsReadOnlyHttpStatus(): Boolean = true

    fun allowsCommandSurface(status: BadgeUsbStatus): Boolean =
        status == BadgeUsbStatus.CONNECTED

    fun allowsStatusRefresh(status: BadgeUsbStatus): Boolean = status in setOf(
        BadgeUsbStatus.CONNECTED,
        BadgeUsbStatus.AP_CONNECTED,
        BadgeUsbStatus.DEBUG_BRIDGE_CONNECTED,
    )

    fun allowsAndroidFirmwareUpload(): Boolean = false

    fun controlConnectionGuidance(): String =
        "Connect a FoF badge over USB-C to send controls"

    fun scannerFirmwareStagingGuidance(): String =
        "Stage the shared scanner firmware from a laptop over USB. " +
            "The uplink automatically converges both scanner slots one at a time " +
            "when the staged version is newer."

    fun scannerFirmwareRecoveryHeading(): String =
        "Manual Per-Slot Relay (Recovery Only)"

    fun scannerFirmwareRecoveryActionLabel(uart: String): String = when (uart.lowercase()) {
        "ble" -> "Recover Slot 0"
        "wifi" -> "Recover Slot 1"
        else -> "Recover Scanner"
    }
}
