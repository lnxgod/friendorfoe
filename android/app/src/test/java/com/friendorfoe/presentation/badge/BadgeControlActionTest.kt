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
}
