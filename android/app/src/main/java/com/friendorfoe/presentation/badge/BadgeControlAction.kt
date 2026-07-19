package com.friendorfoe.presentation.badge

enum class BadgeDangerAction {
    REBOOT,
    BOOTLOADER,
    RECOVER_SLOT_0,
    RECOVER_SLOT_1,
}

sealed interface BadgeDangerEvent {
    data class Request(val action: BadgeDangerAction) : BadgeDangerEvent
    data object Confirm : BadgeDangerEvent
    data object Cancel : BadgeDangerEvent
}

data class BadgeDangerTransition(
    val pending: BadgeDangerAction? = null,
    val confirmed: BadgeDangerAction? = null,
)

internal fun reduceBadgeDangerConfirmation(
    pending: BadgeDangerAction?,
    event: BadgeDangerEvent,
): BadgeDangerTransition = when (event) {
    is BadgeDangerEvent.Request -> BadgeDangerTransition(pending = event.action)
    BadgeDangerEvent.Cancel -> BadgeDangerTransition()
    BadgeDangerEvent.Confirm -> BadgeDangerTransition(confirmed = pending)
}
