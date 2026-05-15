package com.friendorfoe.detection

import org.junit.Assert.assertEquals
import org.junit.Test

class PrivacyCategoryMappingTest {

    @Test
    fun maps_new_backend_privacy_categories() {
        assertEquals(
            PrivacyCategory.VENUE_BEACON,
            GlassesDetector.categorizeDeviceType("Venue Beacon")
        )
        assertEquals(
            PrivacyCategory.EVENT_BADGE,
            GlassesDetector.categorizeDeviceType("Event Badge")
        )
        assertEquals(
            PrivacyCategory.MOBILE_KEY_LOCK,
            GlassesDetector.categorizeDeviceType("Mobile Key Lock")
        )
        assertEquals(
            PrivacyCategory.BLE_HID,
            GlassesDetector.categorizeDeviceType("BLE HID")
        )
        assertEquals(
            PrivacyCategory.AURACAST,
            GlassesDetector.categorizeDeviceType("Auracast")
        )
        assertEquals(
            PrivacyCategory.APPLE_CONTINUITY,
            GlassesDetector.categorizeDeviceType("Apple Continuity Nearby Info")
        )
    }
}
