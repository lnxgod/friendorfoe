package com.friendorfoe.data.badge

sealed interface BadgeCommand {
    data class ApplyTheme(val theme: BadgeTheme) : BadgeCommand
    data class ApplyPolicy(val policy: BadgeDisplayPolicy) : BadgeCommand
    data class SetNetworkMode(val mode: BadgeNetworkMode) : BadgeCommand
    data class NavigateDisplay(val action: BadgeDisplayAction) : BadgeCommand
    data object Reboot : BadgeCommand
    data object EnterBootloader : BadgeCommand
}

sealed interface BadgeCommandOutcome {
    data class Acknowledged(
        val acknowledgement: BadgeControlAcknowledgement,
    ) : BadgeCommandOutcome

    data class Accepted(val message: String) : BadgeCommandOutcome
    data class Failed(val message: String) : BadgeCommandOutcome
    data class Unsupported(val reason: String) : BadgeCommandOutcome
    data object TimedOut : BadgeCommandOutcome
}

enum class BadgeRuntimeNetworkMode(val wireValue: String) {
    OFF("off"),
    LOCAL_AP("local_ap"),
    BACKEND("backend"),
}

fun BadgeNetworkMode.expectedRuntimeMode(): BadgeRuntimeNetworkMode = when (this) {
    BadgeNetworkMode.USB_ONLY -> BadgeRuntimeNetworkMode.OFF
    BadgeNetworkMode.LOCAL_AP -> BadgeRuntimeNetworkMode.LOCAL_AP
    BadgeNetworkMode.BACKEND -> BadgeRuntimeNetworkMode.BACKEND
}

data class BadgeControlAcknowledgement(
    val message: String,
    val themeHash: Long? = null,
    val policyHash: Long? = null,
    val networkApplied: Boolean? = null,
    val runtimeNetworkMode: BadgeRuntimeNetworkMode? = null,
)

data class BadgeRepositoryState(
    val connection: BadgeConnectionEvidence = BadgeConnectionEvidence(),
    val controlStatus: BadgeControlStatus? = null,
    val lastCommandOutcome: BadgeCommandOutcome? = null,
    val detections: List<BadgeUsbDetection> = emptyList(),
)

fun BadgeCommand.requiredCapability(): BadgeCapability = when (this) {
    is BadgeCommand.ApplyTheme -> BadgeCapability.THEME_V1
    is BadgeCommand.ApplyPolicy -> BadgeCapability.DISPLAY_POLICY_V1
    is BadgeCommand.SetNetworkMode -> BadgeCapability.NETWORK_MODE
    is BadgeCommand.NavigateDisplay -> BadgeCapability.DISPLAY_NAV
    BadgeCommand.Reboot -> BadgeCapability.REBOOT
    BadgeCommand.EnterBootloader -> BadgeCapability.BOOTLOADER
}

fun BadgeCommand.payloadSizeOrNull(): Int? = when (this) {
    is BadgeCommand.NavigateDisplay -> when (action) {
        BadgeDisplayAction.NEXT,
        BadgeDisplayAction.BACK,
        -> 37
        BadgeDisplayAction.DETAIL -> 39
    }
    else -> null
}

internal fun BadgeCommand.toControlJson() = when (this) {
    is BadgeCommand.ApplyTheme -> badgeThemeCommandJson(theme)
    is BadgeCommand.ApplyPolicy -> badgeDisplayPolicyCommandJson(policy)
    is BadgeCommand.SetNetworkMode -> badgeNetworkModeCommandJson(mode)
    is BadgeCommand.NavigateDisplay -> badgeDisplayNavCommandJson(action)
    BadgeCommand.Reboot -> badgeRebootCommandJson()
    BadgeCommand.EnterBootloader -> badgeBootloaderCommandJson()
}
