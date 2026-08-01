package com.friendorfoe.presentation.privacy

import com.friendorfoe.detection.GlassesDetection
import com.friendorfoe.detection.PrivacyCategory
import java.time.Instant
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertSame
import org.junit.Test

class PrivacyFindingNormalizerTest {

    @Test
    fun appleBackendListeningWordingBecomesInformational() {
        val normalized = PrivacyFindingNormalizer.normalize(
            detection(
                manufacturer = "Apple",
                deviceType = "Possible Remote Listening",
                matchReason = "backend:REMOTE_LISTENING",
                category = PrivacyCategory.REMOTE_LISTENING,
                bleCompanyId = 0x004C,
                details = mapOf("apple_activity" to "2", "apple_flags" to "1"),
            )
        )

        assertEquals(PrivacyCategory.APPLE_CONTINUITY, normalized.category)
        assertEquals("AirPods connection/activity nearby", normalized.deviceType)
        assertFalse(normalized.hasCamera)
        assertFalse(normalized.deviceType.contains("listening", ignoreCase = true))
    }

    @Test
    fun unrelatedNonAppleListeningCategoryIsPreserved() {
        val original = detection(
            manufacturer = "Other",
            category = PrivacyCategory.REMOTE_LISTENING,
        )

        assertSame(original, PrivacyFindingNormalizer.normalize(original))
    }

    @Test
    fun appleListeningDetailsAreScrubbedAndReplacedWithLimitations() {
        val normalized = PrivacyFindingNormalizer.normalize(
            detection(
                deviceName = "Possible listening device",
                deviceType = "Eavesdrop warning",
                manufacturer = "Apple",
                hasCamera = true,
                matchReason = "apple eavesdrop listening",
                details = mapOf(
                    "listening_signal" to "microphone suspected",
                    "legacy" to "possible eavesdrop device",
                    "source" to "backend",
                ),
                category = PrivacyCategory.REMOTE_LISTENING,
                bleCompanyId = 0x004C,
                bleAppleFlags = 0,
            )
        )

        assertEquals("Apple device activity nearby", normalized.deviceType)
        assertNull(normalized.deviceName)
        assertEquals("apple_activity", normalized.matchReason)
        assertEquals(mapOf(
            "source" to "backend",
            "evidence" to "An Apple device reports a nearby activity state; the specific activity is unavailable.",
            "limitation" to "Live Listen and microphone use cannot be determined from BLE.",
        ), normalized.details)
        assertFalse(normalized.hasCamera)
    }

    @Test
    fun bondedAppleDevicePreservesSafeOwnedName() {
        val normalized = PrivacyFindingNormalizer.normalize(
            detection(
                deviceName = "Bill's AirPods Pro",
                deviceType = "Possible Listening",
                manufacturer = "Apple",
                matchReason = "apple_remote_listening:legacy",
                category = PrivacyCategory.REMOTE_LISTENING,
                isBonded = true,
            )
        )

        assertEquals("Bill's AirPods Pro", normalized.deviceName)
        assertEquals("Bill's AirPods Pro", normalized.deviceType)
    }

    @Test
    fun unbondedAppleDeviceDoesNotPreserveAdvertisedName() {
        val normalized = PrivacyFindingNormalizer.normalize(
            detection(
                deviceName = "Stranger's AirPods",
                deviceType = "Possible Listening",
                manufacturer = "Apple",
                matchReason = "apple_remote_listening:legacy",
                category = PrivacyCategory.REMOTE_LISTENING,
                isBonded = false,
            )
        )

        assertNull(normalized.deviceName)
        assertEquals("AirPods connection/activity nearby", normalized.deviceType)
    }

    @Test
    fun localAppleActivityIsAssignedInformationalCategory() {
        val normalized = PrivacyFindingNormalizer.normalize(
            detection(
                deviceName = null,
                deviceType = "AirPods connection/activity nearby",
                manufacturer = "Apple",
                matchReason = "apple_activity",
                category = PrivacyCategory.INFORMATIONAL,
                bleCompanyId = 0x004C,
            )
        )

        assertEquals(PrivacyCategory.APPLE_CONTINUITY, normalized.category)
        assertEquals("apple_activity", normalized.matchReason)
        assertEquals(0, normalized.category.threatLevel)
    }

    @Test
    fun unrelatedAppleContinuityFindingIsUnchanged() {
        val original = detection(
            deviceType = "Apple Handoff",
            manufacturer = "Apple",
            matchReason = "apple_continuity:Handoff",
            category = PrivacyCategory.APPLE_CONTINUITY,
            bleCompanyId = 0x004C,
            bleAppleFlags = 0,
        )

        assertSame(original, PrivacyFindingNormalizer.normalize(original))
    }

    @Test
    fun wifiPrivacyRowsKeepTheirProvenanceAndFingerprint() {
        val wifi = detection(
            mac = "11:22:33:44:55:66",
            deviceName = "CamHi_1234",
            deviceType = "Hidden Camera",
            manufacturer = "CamHi",
            hasCamera = true,
            rssi = -52,
            confidence = 0.82f,
            matchReason = "wifi_ssid:CamHi",
            details = mapOf("source" to "wifi_privacy_scanner"),
            category = PrivacyCategory.HIDDEN_CAMERA,
            bleAppleFlags = null,
            fingerprintKey = "wifi:11:22:33:44:55:66",
            seenMacs = setOf("11:22:33:44:55:66"),
        )

        val normalizedRows = listOf(wifi).map(PrivacyFindingNormalizer::normalize)

        assertEquals(1, normalizedRows.size)
        assertSame(wifi, normalizedRows.single())
        assertEquals("wifi_ssid:CamHi", normalizedRows.single().matchReason)
        assertEquals("wifi:11:22:33:44:55:66", normalizedRows.single().fingerprintKey)
    }

    private fun detection(
        mac: String = "AA:BB:CC:00:00:01",
        deviceName: String? = "Apple Device",
        deviceType: String = "Privacy Signal",
        manufacturer: String = "Unknown",
        hasCamera: Boolean = false,
        rssi: Int = -45,
        confidence: Float = 0.82f,
        matchReason: String = "test",
        firstSeen: Instant = Instant.parse("2026-07-31T12:00:00Z"),
        lastSeen: Instant = Instant.parse("2026-07-31T12:00:10Z"),
        details: Map<String, String> = emptyMap(),
        category: PrivacyCategory = PrivacyCategory.INFORMATIONAL,
        isBonded: Boolean = false,
        bleCompanyId: Int? = null,
        bleAppleType: Int? = null,
        bleAppleFlags: Int? = 1,
        bleAppleAction: Int? = null,
        bleAppleIosVersion: Int? = null,
        bleAdvFlags: Int? = null,
        bleDualModeHost: Boolean = false,
        bleJa3Hash: UInt? = null,
        bleServiceUuids: List<Int> = emptyList(),
        bleAppearance: Int? = null,
        bleLocalName: String? = null,
        fingerprintKey: String = "fp:apple-test",
        seenMacs: Set<String> = setOf("AA:BB:CC:00:00:01"),
    ) = GlassesDetection(
        mac = mac,
        deviceName = deviceName,
        deviceType = deviceType,
        manufacturer = manufacturer,
        hasCamera = hasCamera,
        rssi = rssi,
        confidence = confidence,
        matchReason = matchReason,
        firstSeen = firstSeen,
        lastSeen = lastSeen,
        details = details,
        category = category,
        isBonded = isBonded,
        bleCompanyId = bleCompanyId,
        bleAppleType = bleAppleType,
        bleAppleFlags = bleAppleFlags,
        bleAppleAction = bleAppleAction,
        bleAppleIosVersion = bleAppleIosVersion,
        bleAdvFlags = bleAdvFlags,
        bleDualModeHost = bleDualModeHost,
        bleJa3Hash = bleJa3Hash,
        bleServiceUuids = bleServiceUuids,
        bleAppearance = bleAppearance,
        bleLocalName = bleLocalName,
        fingerprintKey = fingerprintKey,
        seenMacs = seenMacs,
    )
}
