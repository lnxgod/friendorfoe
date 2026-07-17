package com.friendorfoe.data.repository

import com.friendorfoe.detection.GlassesDetection
import com.friendorfoe.detection.PrivacyCategory
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import java.time.Instant

class FollowerCandidatePolicyTest {

    private val eligibleCategories = setOf(
        PrivacyCategory.FINDMY,
        PrivacyCategory.BLE_TRACKER,
        PrivacyCategory.GPS_TRACKER,
        PrivacyCategory.OBD_TRACKER,
        PrivacyCategory.SMART_GLASSES,
        PrivacyCategory.ACTION_CAMERA,
        PrivacyCategory.DASH_CAMERA,
        PrivacyCategory.VEHICLE_CAMERA,
        PrivacyCategory.BODY_CAMERA,
    )

    @Test
    fun only_mobile_privacy_categories_are_eligible() {
        PrivacyCategory.values().forEach { category ->
            assertEquals(
                category.name,
                category in eligibleCategories,
                detection(category = category).isFollowerCandidate(emptySet()),
            )
        }
    }

    @Test
    fun bonded_devices_are_excluded() {
        assertFalse(
            detection(
                category = PrivacyCategory.BLE_TRACKER,
                bonded = true,
            ).isFollowerCandidate(emptySet())
        )
    }

    @Test
    fun ignored_mac_fingerprint_and_rotated_mac_aliases_are_excluded() {
        val detection = detection(category = PrivacyCategory.SMART_GLASSES)
        val ignoredAliases = listOf(
            detection.mac,
            "mac:${detection.mac}",
            detection.fingerprintKey,
            "AA:BB:CC:DD:EE:00",
        )

        ignoredAliases.forEach { ignoredAlias ->
            assertFalse(
                ignoredAlias,
                detection.isFollowerCandidate(setOf(ignoredAlias)),
            )
        }
        assertTrue(detection.isFollowerCandidate(emptySet()))
    }

    private fun detection(
        category: PrivacyCategory,
        bonded: Boolean = false,
    ) = GlassesDetection(
        mac = "AA:BB:CC:DD:EE:FF",
        deviceName = "Test device",
        deviceType = category.label,
        manufacturer = "Test",
        hasCamera = category == PrivacyCategory.SMART_GLASSES,
        rssi = -55,
        confidence = 0.9f,
        matchReason = "test",
        firstSeen = Instant.EPOCH,
        lastSeen = Instant.EPOCH,
        category = category,
        isBonded = bonded,
        fingerprintKey = "fp:test-device",
        seenMacs = setOf(
            "AA:BB:CC:DD:EE:FF",
            "AA:BB:CC:DD:EE:00",
        ),
    )
}
