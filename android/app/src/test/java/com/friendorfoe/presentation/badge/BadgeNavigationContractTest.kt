package com.friendorfoe.presentation.badge

import java.io.File
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class BadgeNavigationContractTest {

    @Test
    fun `bottom navigation uses exact seven item order with badge after privacy`() {
        val source = source("com/friendorfoe/presentation/MainActivity.kt")
        val labels = Regex("BottomNavItem\\(\\\"([^\\\"]+)\\\"")
            .findAll(source)
            .map { it.groupValues[1] }
            .toList()

        assertEquals(
            listOf("AR", "Map", "List", "Privacy", "Badge", "History", "Info"),
            labels,
        )
        assertTrue(source.contains("Screen.Badge.route"))
        assertTrue(source.contains("BadgeMarkIcon"))
        assertTrue(source.contains("BadgeMarkGold"))
        assertFalse(source.contains("BottomNavItem(\"Cal\""))
    }

    @Test
    fun `badge root and focused routes are registered while info keeps calibration`() {
        val screen = source("com/friendorfoe/presentation/navigation/Screen.kt")
        val graph = source("com/friendorfoe/presentation/navigation/FriendOrFoeNavGraph.kt")

        assertTrue(screen.contains("data object Badge : Screen(\"badge\")"))
        assertTrue(screen.contains("data object BadgeFocus : Screen(\"badge/{focusKey}\")"))
        assertTrue(screen.contains("Uri.encode(focusKey)"))
        assertTrue(graph.contains("composable(Screen.Badge.route)"))
        assertTrue(graph.contains("route = Screen.BadgeFocus.route"))
        assertTrue(graph.contains("BadgeControlScreen(initialFocusKey = focusKey)"))
        assertTrue(graph.contains("onNavigateToCalibrate"))
        assertTrue(graph.contains("navigate(Screen.Calibrate.route)"))
    }

    @Test
    fun `dedicated badge workspace has bounded tagged sections and command gating`() {
        val source = source("com/friendorfoe/presentation/badge/BadgeControlScreen.kt")
        listOf(
            "badge_status",
            "badge_live_feed",
            "badge_lcd_remote",
            "badge_appearance",
            "badge_filters",
            "badge_operations",
        ).forEach { tag -> assertTrue("missing $tag", source.contains("testTag(\"$tag\")")) }

        assertTrue(source.contains("LazyColumn"))
        assertTrue(source.contains("take(MAX_BADGE_LIVE_FEED_ITEMS)"))
        assertTrue(source.contains("initialFocusKey"))
        assertTrue(source.contains("BadgeControlTransportPolicy.allowsCommandSurface(state.status)"))
        assertTrue(source.contains("BadgeControlTransportPolicy.allowsStatusRefresh(state.status)"))
        assertTrue(source.contains("BadgeUsbStatus.PERMISSION_NEEDED"))
        assertTrue(source.contains("Grant USB access"))
        assertFalse(source.contains("Text(\"Connect\")"))
        assertFalse(source.contains("flashScannerFirmware"))
    }

    @Test
    fun `every remote command family has explicit disabled state and danger confirmation`() {
        val screen = source("com/friendorfoe/presentation/badge/BadgeControlScreen.kt")
        val appearance = source("com/friendorfoe/presentation/badge/BadgeAppearanceSection.kt")
        val filters = source("com/friendorfoe/presentation/badge/BadgeDisplayFiltersSection.kt")

        assertTrue(screen.contains("commandsEnabled = commandsEnabled"))
        assertTrue(screen.contains("remoteActionsEnabled = commandsEnabled"))
        assertTrue(appearance.contains("commandsEnabled: Boolean = true"))
        assertTrue(appearance.contains("enabled = commandsEnabled"))
        assertTrue(filters.contains("remoteActionsEnabled: Boolean = true"))
        assertTrue(filters.contains("enabled = remoteActionsEnabled"))
        BadgeDangerAction.entries.forEach { action ->
            assertTrue(screen.contains("BadgeDangerAction.${action.name}"))
        }
        listOf(
            "Reboot the badge now?",
            "Enter badge bootloader now?",
            "Recover scanner slot 0 now?",
            "Recover scanner slot 1 now?",
        ).forEach { prompt -> assertTrue("missing $prompt", screen.contains(prompt)) }
    }

    private fun source(relativePath: String): String {
        val candidates = listOf(
            File("src/main/java/$relativePath"),
            File("app/src/main/java/$relativePath"),
            File("android/app/src/main/java/$relativePath"),
        )
        return candidates.firstOrNull(File::isFile)?.readText()
            ?: error("Unable to locate source: $relativePath")
    }
}
