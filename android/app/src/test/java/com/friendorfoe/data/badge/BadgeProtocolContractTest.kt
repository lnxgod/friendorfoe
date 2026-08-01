package com.friendorfoe.data.badge

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class BadgeProtocolContractTest {

    @Test
    fun serializersProduceExactFirmwareControlPayloads() {
        assertEquals(
            "{\"cmd\":\"reboot\"}",
            badgeRebootCommandJson().toString()
        )
        assertEquals(
            "{\"cmd\":\"bootloader\"}",
            badgeBootloaderCommandJson().toString()
        )
        assertEquals(
            "{\"cmd\":\"set_mode\",\"mode\":\"usb_only\",\"persist\":true}",
            badgeNetworkModeCommandJson(BadgeNetworkMode.USB_ONLY).toString()
        )
    }

    @Test
    fun navigationSerializerLengthsMatchMtuContract() {
        val expected = mapOf(
            BadgeDisplayAction.NEXT to "{\"cmd\":\"display_nav\",\"action\":\"next\"}",
            BadgeDisplayAction.DETAIL to "{\"cmd\":\"display_nav\",\"action\":\"detail\"}",
            BadgeDisplayAction.BACK to "{\"cmd\":\"display_nav\",\"action\":\"back\"}"
        )

        val payloads = BadgeDisplayAction.entries.associateWith { action ->
            badgeDisplayNavCommandJson(action).toString()
        }

        assertEquals(expected, payloads)
        assertEquals(
            listOf(37, 39, 37),
            BadgeDisplayAction.entries.map { payloads.getValue(it).encodeToByteArray().size }
        )
    }

    @Test
    fun directUsbRecoveryAcknowledgementsRequireMatchingPendingCommand() {
        assertEquals(
            BadgeRecoveryAcknowledgement.REBOOT_OK,
            parseBadgeRecoveryAcknowledgement(
                line = "FOF_REBOOT:OK",
                pendingCommand = BadgeRecoveryCommand.REBOOT
            )
        )
        assertEquals(
            BadgeRecoveryAcknowledgement.BOOTLOADER_OK,
            parseBadgeRecoveryAcknowledgement(
                line = "FOF_BOOTLOADER:OK",
                pendingCommand = BadgeRecoveryCommand.BOOTLOADER
            )
        )
    }

    @Test
    fun recoveryParserRejectsMismatchedMissingAndGenericAcknowledgements() {
        assertNull(
            parseBadgeRecoveryAcknowledgement(
                line = "FOF_BOOTLOADER:OK",
                pendingCommand = BadgeRecoveryCommand.REBOOT
            )
        )
        assertNull(
            parseBadgeRecoveryAcknowledgement(
                line = "FOF_REBOOT:OK",
                pendingCommand = BadgeRecoveryCommand.BOOTLOADER
            )
        )
        assertNull(
            parseBadgeRecoveryAcknowledgement(
                line = "FOF_REBOOT:OK",
                pendingCommand = null
            )
        )
        assertNull(
            parseBadgeRecoveryAcknowledgement(
                line = "FOF_CTL_OK:{\"cmd\":\"reboot\"}",
                pendingCommand = BadgeRecoveryCommand.REBOOT
            )
        )
        assertNull(
            parseBadgeRecoveryAcknowledgement(
                line = " FOF_REBOOT:OK",
                pendingCommand = BadgeRecoveryCommand.REBOOT
            )
        )
        assertNull(
            parseBadgeRecoveryAcknowledgement(
                line = "FOF_REBOOT:OK extra",
                pendingCommand = BadgeRecoveryCommand.REBOOT
            )
        )
    }

    @Test
    fun recoveryTrackerClearsOnlyAfterExactMatchingAcknowledgement() {
        val tracker = BadgeRecoveryTracker()
        assertTrue(tracker.begin(BadgeRecoveryCommand.REBOOT))

        assertNull(tracker.accept("FOF_BOOTLOADER:OK"))
        assertEquals(BadgeRecoveryCommand.REBOOT, tracker.pendingCommand)
        assertNull(tracker.accept("FOF_CTL_OK:{\"cmd\":\"reboot\"}"))
        assertEquals(BadgeRecoveryCommand.REBOOT, tracker.pendingCommand)
        assertEquals(
            BadgeRecoveryAcknowledgement.REBOOT_OK,
            tracker.accept("FOF_REBOOT:OK")
        )
        assertNull(tracker.pendingCommand)
    }

    @Test
    fun recoveryCommandsAreSupportedOnlyOnDirectUsb() {
        assertTrue(isDirectUsbRecoverySupported(BadgeUsbStatus.CONNECTED))
        assertEquals(
            emptyList<BadgeUsbStatus>(),
            listOf(
                BadgeUsbStatus.AP_CONNECTED,
                BadgeUsbStatus.BLE_CONNECTED,
                BadgeUsbStatus.DEBUG_BRIDGE_CONNECTED,
                BadgeUsbStatus.DISCONNECTED
            ).filter(::isDirectUsbRecoverySupported)
        )
    }

    @Test
    fun policyAndThemeCommandsValidateBeforeSerialization() {
        val invalidTheme = BadgeTheme.firmwareDefaults().copy(intensity = 24)
        val invalidPolicy = BadgeDisplayPolicy.firmwareDefaults().copy(version = 2)

        assertTrue(runCatching { badgeThemeCommandJson(invalidTheme) }.isFailure)
        assertTrue(runCatching { badgeDisplayPolicyCommandJson(invalidPolicy) }.isFailure)
    }
}
