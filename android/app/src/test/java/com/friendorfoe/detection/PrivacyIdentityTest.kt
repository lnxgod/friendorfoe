package com.friendorfoe.detection

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import java.time.Instant

class PrivacyIdentityTest {

    @Test
    fun mac_case_and_optional_prefix_have_one_canonical_identity() {
        assertEquals(
            "mac:aa:bb:cc:dd:ee:ff",
            canonicalPrivacyIdentity("AA:BB:CC:DD:EE:FF"),
        )
        assertEquals(
            "mac:aa:bb:cc:dd:ee:ff",
            canonicalPrivacyIdentity("MAC:aa:bb:cc:dd:ee:ff"),
        )
    }

    @Test
    fun stable_fingerprint_with_commas_is_canonicalized_without_splitting() {
        assertEquals(
            "fp:meta|ray-ban,wayfarer",
            canonicalPrivacyIdentity("FP:Meta|Ray-Ban,Wayfarer"),
        )
    }

    @Test
    fun detection_matches_ignored_fingerprint_or_any_rotated_mac_alias() {
        val detection = GlassesDetection(
            mac = "AA:BB:CC:DD:EE:FF",
            deviceName = "Glasses",
            deviceType = "Smart Glasses",
            manufacturer = "Meta",
            hasCamera = true,
            rssi = -55,
            confidence = 0.9f,
            matchReason = "test",
            firstSeen = Instant.EPOCH,
            lastSeen = Instant.EPOCH,
            category = PrivacyCategory.SMART_GLASSES,
            fingerprintKey = "fp:Meta|Ray-Ban,Wayfarer",
            seenMacs = setOf(
                "AA:BB:CC:DD:EE:FF",
                "AA:BB:CC:DD:EE:00",
            ),
        )

        assertTrue(
            privacyIdentityIsIgnored(
                detection.canonicalPrivacyIdentityAliases(),
                setOf("FP:META|RAY-BAN,WAYFARER"),
            )
        )
        assertTrue(
            privacyIdentityIsIgnored(
                detection.canonicalPrivacyIdentityAliases(),
                setOf("mac:aa:bb:cc:dd:ee:00"),
            )
        )
        assertFalse(
            privacyIdentityIsIgnored(
                detection.canonicalPrivacyIdentityAliases(),
                setOf("mac:00:00:00:00:00:00"),
            )
        )
    }
}
