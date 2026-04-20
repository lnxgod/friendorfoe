package com.friendorfoe.detection

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Unit tests for GlassesDetector.computeFingerprintKey.
 *
 * The fingerprint key is the primary mechanism for collapsing BT Private
 * Resolvable Address (RPA) rotations back into a single logical device
 * for MAC-rotating high-risk classes (Meta Glasses, Ray-Ban, Oakley, Quest).
 * Stable-MAC devices (AirTags, generic BLE peripherals) fall back to `mac:`
 * so two distinct physical devices never collapse into one row.
 */
class GlassesDetectorFingerprintTest {

    @Test
    fun `three Meta ads from same pair produce the same fingerprint key`() {
        val mac1 = "AA:BB:CC:00:00:01"
        val mac2 = "AA:BB:CC:00:00:02"
        val mac3 = "AA:BB:CC:00:00:03"
        val ja3 = 0xDEADBEEFu
        val uuids = listOf(0xFD5F)
        val name = "RB Meta abc"

        val k1 = GlassesDetector.computeFingerprintKey(mac1, "Meta", "Ray-Ban Meta", ja3, uuids, name)
        val k2 = GlassesDetector.computeFingerprintKey(mac2, "Meta", "Ray-Ban Meta", ja3, uuids, name)
        val k3 = GlassesDetector.computeFingerprintKey(mac3, "Meta", "Ray-Ban Meta", ja3, uuids, name)

        assertEquals("MAC rotation must not change fingerprint", k1, k2)
        assertEquals("MAC rotation must not change fingerprint", k2, k3)
        assertTrue("High-risk devices use fp: prefix", k1.startsWith("fp:"))
    }

    @Test
    fun `different JA3 hashes produce different fingerprint keys`() {
        val mac = "AA:BB:CC:00:00:01"
        val uuids = listOf(0xFD5F)
        val name = "RB Meta xyz"

        val k1 = GlassesDetector.computeFingerprintKey(mac, "Meta", "Ray-Ban Meta", 0x11111111u, uuids, name)
        val k2 = GlassesDetector.computeFingerprintKey(mac, "Meta", "Ray-Ban Meta", 0x22222222u, uuids, name)

        assertNotEquals("different JA3 = different fingerprint", k1, k2)
    }

    @Test
    fun `two different Meta pairs with same JA3 but different names stay separate`() {
        // Same advertisement structure can still have different local names
        // (e.g. "RB Meta-AB12" vs "RB Meta-CD34") — that disambiguates pairs.
        val mac = "AA:BB:CC:00:00:01"
        val ja3 = 0xDEADBEEFu
        val uuids = listOf(0xFD5F)

        val pairA = GlassesDetector.computeFingerprintKey(mac, "Meta", "Ray-Ban Meta", ja3, uuids, "RB Meta-AB12")
        val pairB = GlassesDetector.computeFingerprintKey(mac, "Meta", "Ray-Ban Meta", ja3, uuids, "RB Meta-CD34")

        assertNotEquals("different names disambiguate two pairs", pairA, pairB)
    }

    @Test
    fun `stable-MAC devices fall back to mac key`() {
        val mac = "DD:EE:FF:00:00:99"
        val key = GlassesDetector.computeFingerprintKey(
            mac = mac,
            bestMfr = "Apple",
            bestType = "AirTag (Separated)",
            ja3 = 0xCAFEBABEu,
            serviceUuids16 = null,
            deviceName = null
        )
        assertEquals("AirTags must key on MAC so two trackers never collapse", "mac:$mac", key)
    }

    @Test
    fun `unknown manufacturer falls back to mac key`() {
        val mac = "11:22:33:44:55:66"
        val key = GlassesDetector.computeFingerprintKey(
            mac = mac,
            bestMfr = "Samsung",
            bestType = "Smart Speaker",
            ja3 = 0x12345678u,
            serviceUuids16 = listOf(0xFE2C),
            deviceName = "Galaxy Home"
        )
        assertTrue("non-rotating classes use mac: prefix", key.startsWith("mac:"))
    }

    @Test
    fun `Oakley Meta HSTN is recognized as rotating`() {
        val mac = "AA:BB:CC:00:00:01"
        val key = GlassesDetector.computeFingerprintKey(
            mac = mac,
            bestMfr = "Luxottica",
            bestType = "Oakley HSTN",
            ja3 = 0xABCDEF01u,
            serviceUuids16 = listOf(0xFD5F),
            deviceName = "Oakley HSTN 001"
        )
        assertTrue("Luxottica-manufactured frames rotate", key.startsWith("fp:"))
    }

    @Test
    fun `Meta Quest headset is recognized as rotating`() {
        val mac = "AA:BB:CC:00:00:01"
        val key = GlassesDetector.computeFingerprintKey(
            mac = mac,
            bestMfr = "Meta",
            bestType = "Meta Quest 3",
            ja3 = 0xABCDEF01u,
            serviceUuids16 = listOf(0xFEB7),
            deviceName = "Quest 3"
        )
        assertTrue("Meta Quest uses fingerprint key", key.startsWith("fp:"))
    }

    @Test
    fun `null JA3 and null UUIDs still produce stable key`() {
        val mac1 = "AA:BB:CC:00:00:01"
        val mac2 = "AA:BB:CC:00:00:02"
        val k1 = GlassesDetector.computeFingerprintKey(mac1, "Meta", "Ray-Ban Meta", null, null, "RB Meta")
        val k2 = GlassesDetector.computeFingerprintKey(mac2, "Meta", "Ray-Ban Meta", null, null, "RB Meta")
        assertEquals("Missing optional fields must not break determinism", k1, k2)
    }

    @Test
    fun `service UUID order does not affect key`() {
        val mac = "AA:BB:CC:00:00:01"
        val k1 = GlassesDetector.computeFingerprintKey(mac, "Meta", "Ray-Ban Meta", 0u, listOf(0xFD5F, 0xFEB7), "RB Meta")
        val k2 = GlassesDetector.computeFingerprintKey(mac, "Meta", "Ray-Ban Meta", 0u, listOf(0xFEB7, 0xFD5F), "RB Meta")
        assertEquals("UUID ordering must not change the key (we sort them)", k1, k2)
    }
}
