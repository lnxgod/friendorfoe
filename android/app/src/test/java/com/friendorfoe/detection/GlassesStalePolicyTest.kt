package com.friendorfoe.detection

import java.time.Instant
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Test

class GlassesStalePolicyTest {

    private fun detection(
        manufacturer: String,
        deviceType: String,
        lastSeen: Instant,
    ) = GlassesDetection(
        mac = "AA:BB:CC:DD:EE:FF",
        deviceName = null,
        deviceType = deviceType,
        manufacturer = manufacturer,
        hasCamera = false,
        rssi = -55,
        confidence = 0.65f,
        matchReason = "test",
        firstSeen = lastSeen,
        lastSeen = lastSeen,
    )

    @Test
    fun uses_long_ttl_for_meta_and_default_ttl_for_ordinary_rows() {
        val now = Instant.parse("2026-07-20T12:00:00Z")
        assertEquals(300L, GlassesStalePolicy.ttlSeconds(detection("Meta", "Meta Device", now)))
        assertEquals(60L, GlassesStalePolicy.ttlSeconds(detection("Axon", "Privacy Infrastructure", now)))
    }

    @Test
    fun expires_at_deadline_and_computes_next_wakeup_without_new_advertisements() {
        val now = Instant.parse("2026-07-20T12:00:00Z")
        val fresh = detection("Axon", "Privacy Infrastructure", now.minusSeconds(30))
        val expired = detection("Axon", "Privacy Infrastructure", now.minusSeconds(60))

        assertFalse(GlassesStalePolicy.isStale(fresh, now))
        assertTrue(GlassesStalePolicy.isStale(expired, now))
        assertEquals(30_000L, GlassesStalePolicy.nextExpiryDelayMillis(listOf(fresh), now))
        assertNotNull(GlassesStalePolicy.nextExpiryDelayMillis(listOf(expired), now))
    }

    @Test
    fun generic_meta_company_ids_are_not_camera_or_glasses_claims() {
        val meta = GlassesDetector.manufacturerClassification(BleSignatures.CID_META)
        val metaTech = GlassesDetector.manufacturerClassification(BleSignatures.CID_META_TECH)
        val luxottica = GlassesDetector.manufacturerClassification(BleSignatures.CID_LUXOTTICA)

        assertEquals("Meta Device", meta?.deviceType)
        assertFalse(meta!!.hasCamera)
        assertEquals("VR Headset", metaTech?.deviceType)
        assertFalse(metaTech!!.hasCamera)
        assertEquals("Smart Glasses", luxottica?.deviceType)
        assertTrue(luxottica!!.hasCamera)
    }
}
