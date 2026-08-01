package com.friendorfoe.presentation.navigation

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Test

class TopLevelDestinationTest {
    @Test
    fun destinationsStayInApprovedOrder() {
        assertEquals(
            listOf("AR", "Map", "List", "Privacy", "Badge", "History", "Info"),
            TopLevelDestination.entries.map { it.label }
        )
        assertEquals(7, TopLevelDestination.entries.map { it.route }.distinct().size)
        assertFalse(TopLevelDestination.entries.any { it.route == "calibrate" })
    }

    @Test
    fun topLevelBackExitsAndSecondaryBackPops() {
        TopLevelDestination.entries.forEach {
            assertEquals(BackDisposition.EXIT_APP, backDisposition(it.route))
        }
        assertEquals(
            BackDisposition.POP_SECONDARY,
            backDisposition(Screen.IgnoredDevices.route)
        )
    }
}
