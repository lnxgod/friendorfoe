package com.friendorfoe.data.badge

internal enum class BadgeControlTransport {
    USB,
}

/**
 * Crunch-mode badge commands are intentionally USB-only. Phone-side BLE
 * detection and read-only badge status over HTTP are separate capabilities.
 */
internal object BadgeControlTransportPolicy {
    private val androidControlCommands = setOf(
        "set_mode",
        "reboot",
        "badge_display_policy",
        "badge_display_policy_reset",
        "badge_theme",
        "badge_theme_reset",
        "display_nav",
    )
    private val displayOnlyCommands = setOf(
        "badge_display_policy",
        "badge_display_policy_reset",
        "badge_theme",
        "badge_theme_reset",
        "display_nav",
    )

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

    fun allowsAndroidControlCommand(
        command: String,
        controlStatus: BadgeControlStatus? = null,
    ): Boolean = command in androidControlCommands &&
        (command !in displayOnlyCommands || badgeDisplayControlsAvailable(controlStatus))

    fun controlConnectionGuidance(): String =
        "Attach a FoF badge over USB-C to send controls"
}
