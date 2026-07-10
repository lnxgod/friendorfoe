package com.friendorfoe.detection

import java.time.Instant
import org.junit.Assert.assertEquals
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
    fun `serial signal keeps target MAC for GATT investigation`() {
        val detection = GlassesDetector.behavioralDetection(serialSignal(), Instant.EPOCH)

        assertEquals("C0:98:E5:00:00:01", detection.investigationTarget?.mac)
        assertEquals(BleInvestigationMode.GATT, detection.investigationTarget?.mode)
    }

    private fun serialSignal() = BleThreatSignal.SerialSkimmer(
        entityKey = "ble:serial-skimmer:C0:98:E5:00:00:01",
        targetMac = "C0:98:E5:00:00:01",
        serialServiceUuid = 0xFFE0,
        confidence = 0.75f,
        evidence = setOf(
            BleSerialEvidence.SERIAL_UUID,
            BleSerialEvidence.SPARSE_PROFILE,
            BleSerialEvidence.PERSISTENT,
            BleSerialEvidence.CLOSE,
            BleSerialEvidence.UNTRUSTED,
        ),
    )
}
