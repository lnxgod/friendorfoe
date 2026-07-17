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
    fun `serial signal keeps target MAC for GATT investigation`() {
        val detection = GlassesDetector.behavioralDetection(serialSignal(), Instant.EPOCH)

        assertEquals("C0:98:E5:00:00:01", detection.investigationTarget?.mac)
        assertEquals(BleInvestigationMode.GATT, detection.investigationTarget?.mode)
    }

    @Test
    fun `serial detection uses strongest observed RSSI`() {
        val detection = GlassesDetector.behavioralDetection(
            serialSignal(strongestRssi = -61),
            Instant.EPOCH,
        )

        assertEquals(-61, detection.rssi)
    }

    @Test
    fun `bonded or recognized nonserial product identity is trusted`() {
        assertTrue(
            GlassesDetector.isTrustedSerialProductIdentity(
                isBonded = true,
                confidence = 0f,
                manufacturer = "",
                deviceType = "",
                matchReason = "",
            )
        )
        assertTrue(
            GlassesDetector.isTrustedSerialProductIdentity(
                isBonded = false,
                confidence = 0.60f,
                manufacturer = "Meta",
                deviceType = "Smart Glasses",
                matchReason = "mfr_cid:0x01AB",
            )
        )
        assertFalse(
            GlassesDetector.isTrustedSerialProductIdentity(
                isBonded = false,
                confidence = 0.59f,
                manufacturer = "Meta",
                deviceType = "Smart Glasses",
                matchReason = "mfr_cid:0x01AB",
            )
        )
    }

    @Test
    fun `public unknown serial UUID and generic UART identities are not trusted`() {
        assertFalse(
            GlassesDetector.isTrustedSerialProductIdentity(
                isBonded = false,
                confidence = 0.95f,
                manufacturer = "",
                deviceType = "",
                matchReason = "address_type:public",
            )
        )
        assertFalse(
            GlassesDetector.isTrustedSerialProductIdentity(
                isBonded = false,
                confidence = 0.95f,
                manufacturer = "Unknown",
                deviceType = "Unknown Product",
                matchReason = "address_type:public",
            )
        )
        listOf(
            "uuid16:0xFFE0",
            "uuid16:0xFFF0",
            "svc_data:0xFFE0",
            "svc_data:0xFFF0",
        ).forEach { reason ->
            assertFalse(
                GlassesDetector.isTrustedSerialProductIdentity(
                    isBonded = false,
                    confidence = 0.95f,
                    manufacturer = "Generic",
                    deviceType = "Serial Device",
                    matchReason = reason,
                )
            )
        }
        assertFalse(
            GlassesDetector.isTrustedSerialProductIdentity(
                isBonded = false,
                confidence = 0.95f,
                manufacturer = "Generic",
                deviceType = "UART Device",
                matchReason = "name:BT",
            )
        )
    }

    @Test
    fun `behavioral ignore checks physical MAC and stable entity key across rotation`() {
        val serialDetection = GlassesDetector.behavioralDetection(serialSignal(), Instant.EPOCH)
        assertTrue(
            GlassesDetector.behavioralDetectionIsIgnored(
                serialDetection,
                setOf("MAC:c0:98:e5:00:00:01"),
            )
        )

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

    private fun serialSignal(strongestRssi: Int = -62) = BleThreatSignal.SerialSkimmer(
        entityKey = "ble:serial-skimmer:C0:98:E5:00:00:01",
        targetMac = "C0:98:E5:00:00:01",
        serialServiceUuid = 0xFFE0,
        strongestRssi = strongestRssi,
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
