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

        for (viewModel in listOf(
            "presentation/list/ListViewModel.kt",
            "presentation/privacy/PrivacyViewModel.kt",
        )) {
            val contents = source(viewModel)
            assertFalse(viewModel, contents.contains("fun startBadgeUsb()"))
            assertFalse(viewModel, contents.contains("fun stopBadgeUsb()"))
            assertFalse(viewModel, contents.contains("badgeUsbRepository.start()"))
            assertFalse(viewModel, contents.contains("badgeUsbRepository.stop()"))
        }

        val repository = source("data/badge/BadgeUsbRepository.kt")
        assertTrue(repository.contains("UsbManager.ACTION_USB_DEVICE_ATTACHED -> requestConnection()"))
        assertTrue(repository.contains("\"Attach a FoF badge over USB-C\""))
        assertFalse(repository.contains("\"Connect a FoF badge over USB-C\""))
        assertTrue(repository.contains("Attach only the badge over USB-C"))
        assertTrue(repository.contains("attach the badge over USB-C"))
        assertFalse(repository.contains("Connect only the badge over USB-C"))
        assertFalse(repository.contains("connect via USB-C"))
        assertTrue(repository.contains("Intent(ACTION_USB_PERMISSION)"))
        assertTrue(repository.contains("putExtra(EXTRA_USB_PERMISSION_SESSION, lifecycleSession)"))
        assertTrue(repository.contains("intent.getLongExtra("))
        assertTrue(repository.contains("EXTRA_USB_PERMISSION_SESSION,"))
        assertTrue(repository.contains("catch (cancelled: CancellationException)"))
        assertTrue(repository.contains("throw cancelled"))
        assertTrue(repository.contains("startReader(connection, port.inEndpoint, device.displayName(), lifecycleSession)"))
        assertTrue(repository.contains("connectionMutex.withBadgeUsbReaderOwner"))
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
