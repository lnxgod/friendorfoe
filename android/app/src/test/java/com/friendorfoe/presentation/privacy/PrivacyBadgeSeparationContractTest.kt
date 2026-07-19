package com.friendorfoe.presentation.privacy

import java.io.File
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class PrivacyBadgeSeparationContractTest {

    @Test
    fun `Privacy owns no badge repository state controls or presentation`() {
        val viewModel = source("presentation/privacy/PrivacyViewModel.kt")
        val screen = source("presentation/privacy/PrivacyScreen.kt")

        listOf(viewModel, screen).forEach { contents ->
            assertFalse(contents.contains("com.friendorfoe.data.badge"))
            assertFalse(contents.contains("com.friendorfoe.presentation.badge"))
            assertFalse(contents.contains("BadgeUsbRepository"))
            assertFalse(contents.contains("BadgeUsbState"))
            assertFalse(contents.contains("BadgeDetailPanel"))
            assertFalse(contents.contains("BadgeUsbStatusRow"))
            assertFalse(contents.contains("badgeUsbState"))
            assertFalse(contents.contains("toPrivacyDetections()"))
            assertFalse(contents.contains("BleInvestigationRoute.BADGE"))
            assertFalse(contents.contains("investigateBadgeEntity"))
        }
    }

    @Test
    fun `Privacy preserves phone backend wifi and sweep behavior`() {
        val viewModel = source("presentation/privacy/PrivacyViewModel.kt")
        val screen = source("presentation/privacy/PrivacyScreen.kt")

        assertTrue(viewModel.contains("skyObjectRepository.glassesDetections"))
        assertTrue(viewModel.contains("_backendPrivacyDetections"))
        assertTrue(viewModel.contains("_wifiAnomalies"))
        assertTrue(viewModel.contains("bleInvestigationCoordinator.investigatePhone"))
        assertTrue(viewModel.contains("skyObjectRepository.refreshPrivacyDetections()"))
        assertTrue(screen.contains("SweepToolsRow("))
        assertTrue(screen.contains("DirectionScanOverlay("))
        assertTrue(screen.contains("BleInvestigationDialog("))
    }

    @Test
    fun `USB permission retry is rendered only by dedicated badge screen`() {
        val privacy = source("presentation/privacy/PrivacyScreen.kt")
        val badge = source("presentation/badge/BadgeControlScreen.kt")

        assertFalse(privacy.contains("Grant USB access"))
        assertFalse(privacy.contains("BadgeUsbStatus.PERMISSION_NEEDED"))
        assertTrue(badge.contains("BadgeUsbStatus.PERMISSION_NEEDED"))
        assertTrue(badge.contains("Grant USB access"))
    }

    private fun source(relativePath: String): String {
        val candidates = listOf(
            File("src/main/java/com/friendorfoe/$relativePath"),
            File("app/src/main/java/com/friendorfoe/$relativePath"),
            File("android/app/src/main/java/com/friendorfoe/$relativePath"),
        )
        return candidates.firstOrNull(File::isFile)?.readText()
            ?: error("Unable to locate source: $relativePath")
    }
}
