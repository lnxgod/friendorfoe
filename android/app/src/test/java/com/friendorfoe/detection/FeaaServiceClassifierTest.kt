package com.friendorfoe.detection

import org.junit.Assert.*
import org.junit.Test

class FeaaServiceClassifierTest {
    @Test
    fun recognizesBothFindHubModesAndIdentifierSizesWithoutDecodingHashedFlags() {
        for (size in listOf(21, 22, 33, 34)) {
            for (frame in listOf(0x40, 0x41)) {
                val data = ByteArray(size) { 0x7F }.also { it[0] = frame.toByte() }
                val match = requireNotNull(FeaaServiceClassifier.classify(data))
                assertEquals("Find Hub accessory", match.deviceType)
                assertEquals(PrivacyCategory.BLE_TRACKER, GlassesDetector.categorizeDeviceType(match.deviceType))
                assertEquals(frame == 0x41, match.evidence.contains("protection mode"))
                assertTrue(match.limitation.contains("does not establish ownership"))
            }
        }
    }

    @Test
    fun rejectsTruncatedOverlongAndUnknownFindHubFrames() {
        for (size in 0..50) {
            if (size in setOf(21, 22, 33, 34)) continue
            val data = ByteArray(size).also { if (it.isNotEmpty()) it[0] = 0x40 }
            assertNull("length $size", FeaaServiceClassifier.classify(data))
        }
        assertNull(FeaaServiceClassifier.classify(ByteArray(22).also { it[0] = 0x42 }))
    }

    @Test
    fun keepsEddystoneSeparateAndRejectsUuidOnlyEvidence() {
        for ((frame, size) in listOf(0x00 to 20, 0x10 to 6, 0x20 to 14, 0x30 to 10)) {
            val match = requireNotNull(FeaaServiceClassifier.classify(ByteArray(size).also { it[0] = frame.toByte() }))
            assertEquals("Eddystone Beacon", match.deviceType)
            assertEquals(PrivacyCategory.VENUE_BEACON, GlassesDetector.categorizeDeviceType(match.deviceType))
            assertNull(FeaaServiceClassifier.classify(byteArrayOf(frame.toByte())))
        }
        assertNull(FeaaServiceClassifier.classify(byteArrayOf()))
    }

    @Test
    fun distinctFindHubAddressesAreNeverMergedBySharedProtocol() {
        val keys = listOf("AA:BB:CC:00:00:01", "AA:BB:CC:00:00:02").map { mac ->
            GlassesDetector.computeFingerprintKey(mac, "Find Hub network", "Find Hub accessory", 42u, listOf(0xFEAA), null)
        }
        assertNotEquals(keys[0], keys[1])
    }
}
