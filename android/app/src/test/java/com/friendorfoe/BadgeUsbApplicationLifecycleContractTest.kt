package com.friendorfoe

import java.io.File
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class BadgeUsbApplicationLifecycleContractTest {

    @Test
    fun badge_usb_lifecycle_is_owned_by_the_application() {
        val application = source("FriendOrFoeApplication.kt")

        assertTrue(application.contains("import com.friendorfoe.data.badge.BadgeUsbRepository"))
        assertTrue(application.contains("lateinit var badgeUsbRepository: BadgeUsbRepository"))
        assertTrue(application.contains("override fun onStart(owner: LifecycleOwner)"))
        assertTrue(application.contains("badgeUsbRepository.start()"))
        assertTrue(application.contains("override fun onStop(owner: LifecycleOwner)"))
        assertTrue(application.contains("badgeUsbRepository.stop()"))

        for (screen in listOf("presentation/list/ListViewScreen.kt", "presentation/privacy/PrivacyScreen.kt")) {
            val contents = source(screen)
            assertFalse(screen, contents.contains("startBadgeUsb()"))
            assertFalse(screen, contents.contains("stopBadgeUsb()"))
        }

        val listScreen = source("presentation/list/ListViewScreen.kt")
        assertFalse(listScreen.contains("onConnect = viewModel::connectBadgeUsb"))
        assertFalse(listScreen.contains("Button(onClick = onConnect)"))

        val privacyScreen = source("presentation/privacy/PrivacyScreen.kt")
        assertTrue(privacyScreen.contains("BadgeUsbStatus.PERMISSION_NEEDED -> \"Grant USB access\""))
        assertTrue(privacyScreen.contains("BadgeUsbStatus.DISCONNECTED -> null"))
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
