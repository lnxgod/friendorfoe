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

        val badgeScreen = source("presentation/badge/BadgeControlScreen.kt")
        assertTrue(badgeScreen.contains("state.status == BadgeUsbStatus.PERMISSION_NEEDED"))
        assertTrue(badgeScreen.contains("Grant USB access"))

        val privacyScreen = source("presentation/privacy/PrivacyScreen.kt")
        assertFalse(privacyScreen.contains("BadgeUsbStatus.PERMISSION_NEEDED"))
        assertFalse(privacyScreen.contains("Grant USB access"))

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
        assertTrue(repository.contains("PendingIntent.FLAG_MUTABLE"))
        assertFalse(repository.contains("PendingIntent.FLAG_IMMUTABLE"))
        assertTrue(repository.contains("putExtra(EXTRA_USB_PERMISSION_SESSION, lifecycleSession)"))
        assertTrue(repository.contains("EXTRA_USB_PERMISSION_ATTACHMENT_GENERATION"))
        assertTrue(repository.contains("attachmentToken.generation"))
        assertTrue(repository.contains("EXTRA_USB_PERMISSION_DEVICE_ID"))
        assertTrue(repository.contains("attachmentToken.identity.deviceId"))
        assertTrue(repository.contains("EXTRA_USB_PERMISSION_DEVICE_PATH"))
        assertTrue(repository.contains("attachmentToken.identity.devicePath"))
        assertTrue(repository.contains("intent.getLongExtra("))
        assertTrue(repository.contains("EXTRA_USB_PERMISSION_SESSION,"))
        assertTrue(repository.contains("attachmentGate.acceptsPermission"))
        assertTrue(repository.contains("expectedAttachmentToken"))
        assertTrue(repository.contains("expectedConnection"))
        assertTrue(repository.contains("devicePath = deviceName"))
        assertTrue(repository.contains("terminateUsbInvestigationLocked(disconnectedOwner)"))
        assertTrue(repository.contains("expectedOwner = owner"))
        assertTrue(repository.contains("handleUsbInvestigationLine(line, frameOwner)"))
        assertTrue(repository.contains("catch (cancelled: CancellationException)"))
        assertTrue(repository.contains("throw cancelled"))
        assertTrue(repository.contains("attachmentToken = attachmentToken"))
        assertTrue(repository.contains("connectionMutex.withBadgeUsbReaderOwner"))
        assertTrue(repository.contains("private fun rejectUsbIdentityLocked("))
        assertTrue(repository.contains("rejectUsbIdentityLocked("))
        val identityRejector = repository
            .substringAfter("private fun rejectUsbIdentityLocked(")
            .substringBefore("private fun startReader(")
        assertFalse(
            "Identity rejection must stay synchronous inside the reader's connection mutex",
            identityRejector.contains("scope.launch"),
        )

        val detachBlock = repository
            .substringAfter("UsbManager.ACTION_USB_DEVICE_DETACHED -> {")
            .take(2_000)
        assertTrue(
            "Every Espressif detach must rescan so an ambiguous pair can recover automatically",
            detachBlock.contains("requestConnection()"),
        )
        assertFalse(
            "A detach with no selected attachment token must not return before rescanning",
            detachBlock.substringBefore("requestConnection()").contains("?: return"),
        )

        val disconnectLocked = repository
            .substringAfter("private fun disconnectLocked()")
            .substringBefore("private fun setState")
        val revokeOwnerAt = disconnectLocked.indexOf("verifiedUsbOwnerKey = null")
        val terminateAt = disconnectLocked.indexOf(
            "terminateUsbInvestigationLocked(disconnectedOwner)",
        )
        assertTrue(revokeOwnerAt >= 0)
        assertTrue(terminateAt >= 0)
        assertTrue("USB owner must be revoked before investigation termination", revokeOwnerAt < terminateAt)
        assertTrue(repository.contains(
            "BadgeUsbHandshakeTimerAction.STOP -> {\n" +
                "                            if (usbHandshakeJob === ownJob) usbHandshakeJob = null",
        ))
        assertTrue(
            "Only the owning STOP path may clear the shared handshake job reference",
            Regex("usbHandshakeJob = null").findAll(repository).count() == 1,
        )
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
