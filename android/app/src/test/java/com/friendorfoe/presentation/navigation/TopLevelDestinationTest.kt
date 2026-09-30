package com.friendorfoe.presentation.navigation

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Test

class TopLevelDestinationTest {
    @Test
    fun destinationsStayInApprovedOrder() {
        assertEquals(
            listOf("Nearby", "Map", "Camera", "Privacy", "More"),
            primaryDestinations.map { it.label }
        )
        assertEquals(7, TopLevelDestination.entries.map { it.route }.distinct().size)
        assertFalse(TopLevelDestination.entries.any { it.route == "calibrate" })
    }

    @Test
    fun topLevelBackExitsAndSecondaryBackPops() {
        primaryDestinations.forEach {
            assertEquals(BackDisposition.EXIT_APP, backDisposition(it.route))
        }
        listOf(Screen.Badge.route, Screen.History.route).forEach {
            assertEquals(BackDisposition.POP_SECONDARY, backDisposition(it))
        }
        assertEquals(
            BackDisposition.POP_SECONDARY,
            backDisposition(Screen.IgnoredDevices.route)
        )
    }
}
