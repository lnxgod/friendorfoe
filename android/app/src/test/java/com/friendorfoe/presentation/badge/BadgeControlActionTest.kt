package com.friendorfoe.presentation.badge

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class BadgeControlActionTest {

    @Test
    fun `each dangerous action requires exact confirmation`() {
        BadgeDangerAction.entries.forEach { action ->
            val armed = reduceBadgeDangerConfirmation(
                pending = null,
                event = BadgeDangerEvent.Request(action),
            )

            assertEquals(action, armed.pending)
            assertNull(armed.confirmed)
            assertNull(
                reduceBadgeDangerConfirmation(
                    armed.pending,
                    BadgeDangerEvent.Cancel,
                ).confirmed,
            )
            assertEquals(
                action,
                reduceBadgeDangerConfirmation(
                    armed.pending,
                    BadgeDangerEvent.Confirm,
                ).confirmed,
            )
        }
    }

    @Test
    fun `confirmation without a pending action executes nothing`() {
        assertNull(
            reduceBadgeDangerConfirmation(
                pending = null,
                event = BadgeDangerEvent.Confirm,
            ).confirmed,
        )
    }

    @Test
    fun `lost command transport cancels pending action and reconnect cannot confirm it`() {
        val armed = reduceBadgeDangerCommand(
            pending = null,
            event = BadgeDangerEvent.Request(BadgeDangerAction.REBOOT),
            commandsEnabled = true,
        )
        assertEquals(BadgeDangerAction.REBOOT, armed.pending)

        val transportLost = reduceBadgeDangerCommand(
            pending = armed.pending,
            event = BadgeDangerEvent.Confirm,
            commandsEnabled = false,
        )
        assertNull(transportLost.pending)
        assertNull(transportLost.confirmed)

        val staleConfirmAfterReconnect = reduceBadgeDangerCommand(
            pending = transportLost.pending,
            event = BadgeDangerEvent.Confirm,
            commandsEnabled = true,
        )
        assertNull(staleConfirmAfterReconnect.confirmed)

        val freshlyArmed = reduceBadgeDangerCommand(
            pending = staleConfirmAfterReconnect.pending,
            event = BadgeDangerEvent.Request(BadgeDangerAction.REBOOT),
            commandsEnabled = true,
        )
        assertEquals(
            BadgeDangerAction.REBOOT,
            reduceBadgeDangerCommand(
                pending = freshlyArmed.pending,
                event = BadgeDangerEvent.Confirm,
                commandsEnabled = true,
            ).confirmed,
        )
    }
}
