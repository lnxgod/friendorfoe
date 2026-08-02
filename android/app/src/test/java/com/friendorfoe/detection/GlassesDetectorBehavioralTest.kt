package com.friendorfoe.detection

import java.time.Instant
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class GlassesDetectorBehavioralTest {

    @Test
    fun `pairing flood becomes one attack tool with passive capture target`() {
        val signal = BleThreatSignal.PairingSpam(
            entityKey = "ble_anomaly:pairing_spam:1234",
            families = setOf(BlePromptFamily.MICROSOFT_SWIFT_PAIR),
            uniqueMacs = 12,
            observationCount = 24,
            strongestRssi = -44,
            rssiSpan = 8,
            windowMs = 8_000,
        )

        val detection = GlassesDetector.behavioralDetection(signal, now = Instant.EPOCH)

        assertEquals(PrivacyCategory.ATTACK_TOOL, detection.category)
        assertEquals("BLE Pairing Spam", detection.deviceType)
        assertEquals(BleInvestigationMode.PASSIVE_CAPTURE, detection.investigationTarget?.mode)
        assertEquals(signal.entityKey, detection.fingerprintKey)
    }

    @Test
    fun `behavioral ignore checks stable entity key across rotation`() {
        val pairingDetection = GlassesDetector.behavioralDetection(pairingSpamSignal(), Instant.EPOCH)
            .copy(mac = "02:00:00:00:00:FE")
        assertTrue(
            GlassesDetector.behavioralDetectionIsIgnored(
                pairingDetection,
                setOf("BLE:PAIRING-SPAM"),
            )
        )
        assertFalse(
            GlassesDetector.behavioralDetectionIsIgnored(
                pairingDetection,
                setOf("02:00:00:00:00:FF"),
            )
        )
    }

    private fun pairingSpamSignal() = BleThreatSignal.PairingSpam(
        entityKey = "ble:pairing-spam",
        families = setOf(BlePromptFamily.MICROSOFT_SWIFT_PAIR),
        uniqueMacs = 12,
        observationCount = 24,
        strongestRssi = -48,
        rssiSpan = 4,
        windowMs = 8_000,
    )
}
