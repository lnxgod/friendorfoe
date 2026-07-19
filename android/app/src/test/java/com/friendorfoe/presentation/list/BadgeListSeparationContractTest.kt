package com.friendorfoe.presentation.list

import java.io.File
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class BadgeListSeparationContractTest {

    @Test
    fun `List observes badge detections read only and exposes no badge controls`() {
        val viewModel = source("presentation/list/ListViewModel.kt")
        val screen = source("presentation/list/ListViewScreen.kt")

        assertTrue(viewModel.contains("val badgeUsbState = badgeUsbRepository.state"))
        listOf(
            "connectBadgeUsb",
            "pingBadgeUsb",
            "refreshBadgeStatus",
            "setBadgeMode",
            "rebootBadge",
            "badgeBootloader",
            "relayBadgeScannerFirmware",
            "applyBadgeDisplayPolicy",
            "resetBadgeDisplayPolicy",
            "applyBadgeTheme",
            "resetBadgeTheme",
            "createBadgeThemeProfile",
            "renameBadgeThemeProfile",
            "replaceBadgeThemeProfile",
            "deleteBadgeThemeProfile",
        ).forEach { forbidden ->
            assertFalse("ListViewModel still owns $forbidden", viewModel.contains(forbidden))
        }
        assertFalse(viewModel.contains("BadgeThemeProfileStore"))
        assertFalse(screen.contains("BadgeUsbPanel"))
        assertFalse(screen.contains("BadgeAppearanceSection"))
        assertFalse(screen.contains("BadgeDisplayFiltersSection"))
        assertFalse(screen.contains("Badge Control"))
        assertFalse(screen.contains("Connect"))
    }

    @Test
    fun `List renders a unified feed with badge provenance and stable focus navigation`() {
        val screen = source("presentation/list/ListViewScreen.kt")
        val navigation = source("presentation/navigation/FriendOrFoeNavGraph.kt")

        assertTrue(screen.contains("onBadgeDetectionTapped: (String) -> Unit"))
        assertTrue(screen.contains("mergeListFeed(skyObjects, badgeUsbState.detections)"))
        assertTrue(screen.contains("resultCount = listFeed.size"))
        assertTrue(screen.contains("if (listFeed.isEmpty())"))
        assertTrue(screen.contains("key = { it.key }"))
        assertTrue(screen.contains("is ListFeedItem.Sky"))
        assertTrue(screen.contains("is ListFeedItem.Badge"))
        assertTrue(screen.contains("BadgeMarkIcon"))
        assertTrue(screen.contains("BadgeMarkGold"))
        assertTrue(screen.contains("onBadgeDetectionTapped(item.detection.stableKey)"))
        assertTrue(screen.contains("detectionSourceIcon(skyObject.source)"))
        assertTrue(navigation.contains("Screen.BadgeFocus.createRoute(stableKey)"))
    }

    private fun source(relativePath: String): String {
        val candidates = listOf(
            File("src/main/java/com/friendorfoe/$relativePath"),
            File("app/src/main/java/com/friendorfoe/$relativePath"),
            File("android/app/src/main/java/com/friendorfoe/$relativePath"),
        )
        return candidates.first { it.isFile }.readText()
    }
}
