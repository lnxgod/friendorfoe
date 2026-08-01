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
        assertEquals(
            "{\"cmd\":\"badge_theme\",\"persist\":true,\"theme\":{" +
                "\"version\":1,\"palette\":\"field\",\"background\":\"dark\"," +
                "\"brightness\":100,\"accents\":{" +
                "\"drone\":65184,\"meta\":63539,\"tracker\":63519," +
                "\"flock\":43039,\"wifi_attack\":2047,\"clear\":12133}}}",
            badgeThemeCommandJson(BadgeTheme.firmwareDefaults()).toString()
        )
        assertEquals(
            "{\"cmd\":\"badge_display_policy\",\"persist\":true,\"policy\":{" +
                "\"version\":1,\"classes\":{" +
                "\"drone\":{\"enabled\":true,\"lane\":\"both\",\"min_proximity\":\"present\",\"priority\":100}," +
                "\"meta\":{\"enabled\":true,\"lane\":\"both\",\"min_proximity\":\"present\",\"priority\":95}," +
                "\"tracker\":{\"enabled\":true,\"lane\":\"lower\",\"min_proximity\":\"near\",\"priority\":70}," +
                "\"wifi_attack\":{\"enabled\":true,\"lane\":\"both\",\"min_proximity\":\"present\",\"priority\":90}," +
                "\"skimmer\":{\"enabled\":true,\"lane\":\"both\",\"min_proximity\":\"near\",\"priority\":88}," +
                "\"camera\":{\"enabled\":true,\"lane\":\"lower\",\"min_proximity\":\"near\",\"priority\":65}," +
                "\"flock\":{\"enabled\":true,\"lane\":\"both\",\"min_proximity\":\"present\",\"priority\":85}," +
                "\"lock\":{\"enabled\":true,\"lane\":\"lower\",\"min_proximity\":\"near\",\"priority\":55}," +
                "\"hid\":{\"enabled\":true,\"lane\":\"lower\",\"min_proximity\":\"close\",\"priority\":45}," +
                "\"beacon\":{\"enabled\":true,\"lane\":\"lower\",\"min_proximity\":\"near\",\"priority\":30}," +
                "\"event_badge\":{\"enabled\":true,\"lane\":\"lower\",\"min_proximity\":\"near\",\"priority\":35}," +
                "\"auracast\":{\"enabled\":true,\"lane\":\"lower\",\"min_proximity\":\"near\",\"priority\":20}," +
                "\"scanner_status\":{\"enabled\":true,\"lane\":\"lower\",\"min_proximity\":\"present\",\"priority\":10}}}}",
            badgeDisplayPolicyCommandJson(BadgeDisplayPolicy.firmwareDefaults()).toString()
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

        assertNull(tracker.acceptSerialLine("FOF_BOOTLOADER:OK"))
        assertEquals(BadgeRecoveryCommand.REBOOT, tracker.pendingCommand)
        assertNull(tracker.acceptSerialLine("FOF_CTL_OK:{\"cmd\":\"reboot\"}"))
        assertEquals(BadgeRecoveryCommand.REBOOT, tracker.pendingCommand)
        assertEquals(
            BadgeRecoveryAcknowledgement.REBOOT_OK,
            tracker.acceptSerialLine("FOF_REBOOT:OK")
        )
        assertNull(tracker.pendingCommand)
    }

    @Test
    fun serialLineBoundaryLeavesWhitespaceAcknowledgementsPending() {
        val prefixTracker = BadgeRecoveryTracker()
        assertTrue(prefixTracker.begin(BadgeRecoveryCommand.REBOOT))
        assertNull(prefixTracker.acceptSerialLine(" FOF_REBOOT:OK"))
        assertEquals(BadgeRecoveryCommand.REBOOT, prefixTracker.pendingCommand)

        val suffixTracker = BadgeRecoveryTracker()
        assertTrue(suffixTracker.begin(BadgeRecoveryCommand.BOOTLOADER))
        assertNull(suffixTracker.acceptSerialLine("FOF_BOOTLOADER:OK "))
        assertEquals(BadgeRecoveryCommand.BOOTLOADER, suffixTracker.pendingCommand)
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
