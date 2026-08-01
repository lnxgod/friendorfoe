package com.friendorfoe.presentation.privacy

import com.friendorfoe.data.badge.BadgeThreatEntity
import com.friendorfoe.data.badge.BadgeBleControlStatus
import com.friendorfoe.data.badge.BadgeConfigReadback
import com.friendorfoe.data.badge.BadgeControlStatus
import com.friendorfoe.data.badge.BadgeNetworkModeReadback
import com.friendorfoe.data.badge.BadgeReportingStatus
import com.friendorfoe.data.badge.BadgeThreatCounts
import com.friendorfoe.data.badge.BadgeUsbState
import com.friendorfoe.detection.PrivacyCategory
import java.time.Instant
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertSame
import org.junit.Test

class BadgePrivacyMapperTest {

    @Test
    fun mapsRichBadgeEntityIntoPrivacyDetection() {
        val entity = BadgeThreatEntity(
            label = "FLOCK CAM",
            detail = "camera oui b4:1e:52",
            evidence = "wifi oui match",
            threatClass = "flock",
            category = "FLOCK",
            code = "FLK",
            displayId = "B4:1E:52",
            source = "wifi_oui",
            sourceId = 7,
            score = 92,
            confidencePct = 88,
            ageSeconds = 9,
            lastSeenSeconds = 2,
            rssi = -57,
            bestRssi = -55,
            events = 3,
            seenCount = 4,
            groupCount = 1,
            operatorId = "OP-7"
        )

        val detection = entity.toPrivacyDetection(Instant.parse("2026-05-18T12:00:00Z"))

        assertNotNull(detection)
        detection!!
        assertEquals("Flock / ALPR Camera", detection.deviceType)
        assertEquals("FoF Badge", detection.manufacturer)
        assertEquals(PrivacyCategory.ALPR_CAMERA, detection.category)
        assertEquals(-57, detection.rssi)
        assertEquals(0.92f, detection.confidence, 0.0001f)
        assertEquals("camera oui b4:1e:52", detection.deviceName)
        assertEquals("usb_badge", detection.details.getValue("source"))
        assertEquals("flock", detection.details.getValue("class"))
        assertEquals("FLOCK", detection.details.getValue("category"))
        assertEquals("FLK", detection.details.getValue("code"))
        assertEquals("wifi oui match", detection.details.getValue("evidence"))
        assertEquals("wifi_oui", detection.details.getValue("badge_source"))
        assertEquals("OP-7", detection.details.getValue("operator_id"))
    }

    @Test
    fun staleBadgeEntityDoesNotMapIntoPrivacyList() {
        val entity = BadgeThreatEntity(
            label = "TRACKER",
            threatClass = "tracker",
            category = "TAG",
            score = 50,
            ageSeconds = 90,
            rssi = -70,
            events = 1,
            stale = true
        )

        assertNull(entity.toPrivacyDetection(Instant.parse("2026-05-18T12:00:00Z")))
    }

    @Test
    fun mapsBadgeEvilTwinIntoAttackToolWithEvidenceDetails() {
        val entity = BadgeThreatEntity(
            label = "Evil Twin",
            detail = "ssid CafeWiFi",
            evidence = "Evil Twin: open clone vs WPA2; ref 00:11:22:33:44:55 ch6",
            threatClass = "wifi_anomaly",
            category = "WIFI",
            code = "WIFI",
            displayId = "66:77:88:99:AA:BB",
            source = "wifi_assoc",
            sourceId = 7,
            ssid = "CafeWiFi",
            bssid = "66:77:88:99:AA:BB",
            authMode = 0,
            freqMhz = 2437,
            score = 88,
            confidencePct = 82,
            ageSeconds = 3,
            lastSeenSeconds = 1,
            rssi = -48,
            bestRssi = -48,
            events = 1,
        )

        val detection = entity.toPrivacyDetection(Instant.parse("2026-05-18T12:00:00Z"))

        assertNotNull(detection)
        detection!!
        assertEquals("Evil Twin", detection.deviceType)
        assertEquals(PrivacyCategory.ATTACK_TOOL, detection.category)
        assertEquals("ssid CafeWiFi", detection.deviceName)
        assertEquals("66:77:88:99:AA:BB", detection.details.getValue("display_id"))
        assertEquals("CafeWiFi", detection.details.getValue("ssid"))
        assertEquals("66:77:88:99:AA:BB", detection.details.getValue("bssid"))
        assertEquals("0", detection.details.getValue("auth_m"))
        assertEquals("2437", detection.details.getValue("freq_mhz"))
        assertEquals("wifi_assoc", detection.details.getValue("badge_source"))
        assertEquals("Evil Twin: open clone vs WPA2; ref 00:11:22:33:44:55 ch6",
            detection.details.getValue("evidence"))
    }

    @Test
    fun badgeAppleListeningEvidenceIsNormalizedFromTheSameEntity() {
        val entity = badgeEntity(
            label = "Apple AirPods",
            detail = "possible listening activity",
            category = "LISTEN",
            code = "LIS",
        )

        val normalized = entity.toPrivacyDetection(Instant.parse("2026-05-18T12:00:00Z"))!!

        assertEquals(PrivacyCategory.APPLE_CONTINUITY, normalized.category)
        assertEquals("AirPods connection/activity nearby", normalized.deviceType)
        assertEquals("apple_activity", normalized.matchReason)
        assertEquals("usb_badge", normalized.details.getValue("source"))
    }

    @Test
    fun badgeAppleListeningEvidenceNormalizesWithoutListeningCategoryCode() {
        val normalized = badgeEntity(
            label = "Apple AirPods",
            detail = "nearby accessory",
            evidence = "possible listening activity",
            category = "AUDIO",
            code = "AUD",
        ).toPrivacyDetection(Instant.parse("2026-05-18T12:00:00Z"))!!

        assertEquals(PrivacyCategory.APPLE_CONTINUITY, normalized.category)
        assertEquals("AirPods connection/activity nearby", normalized.deviceType)
        assertEquals("apple_activity", normalized.matchReason)
        assertEquals("usb_badge", normalized.details.getValue("source"))
    }

    @Test
    fun underscoreDelimitedBadgeEvidenceIsNormalized() {
        val normalized = badgeEntity(
            label = "Unknown signal",
            detail = "nearby activity",
            category = "APPLE_REMOTE_LISTENING",
            code = "OTHER",
        ).toPrivacyDetection(Instant.parse("2026-05-18T12:00:00Z"))!!

        assertEquals(PrivacyCategory.APPLE_CONTINUITY, normalized.category)
        assertEquals("Apple device activity nearby", normalized.deviceType)
        assertEquals("apple_activity", normalized.matchReason)
    }

    @Test
    fun neighboringAppleAndListeningBadgeRowsAreNotCorrelated() {
        val appleOnly = badgeEntity(
            label = "Apple AirPods",
            detail = "nearby accessory",
            category = "AUDIO",
            code = "AUD",
        ).toPrivacyDetection(Instant.parse("2026-05-18T12:00:00Z"))!!
        val listeningOnly = badgeEntity(
            label = "Unknown signal",
            detail = "possible listening activity",
            category = "LISTEN",
            code = "LIS",
        ).toPrivacyDetection(Instant.parse("2026-05-18T12:00:00Z"))!!

        assertSame(appleOnly, PrivacyFindingNormalizer.normalize(appleOnly))
        assertSame(listeningOnly, PrivacyFindingNormalizer.normalize(listeningOnly))
        assertEquals(PrivacyCategory.REMOTE_LISTENING, listeningOnly.category)
    }

    @Test
    fun remappingCachedBadgeStatusDoesNotRejuvenateAbsoluteLastSeenTime() {
        val state = BadgeUsbState(
            controlStatus = statusFixture(
                receivedAtElapsedMs = 1_000,
                entity = badgeEntity(
                    label = "Venue beacon",
                    detail = "cached row",
                    category = "BEACON",
                    code = "BCN",
                ).copy(lastSeenSeconds = 2),
            ),
        )

        val first = state.toPrivacyDetections().single()
        val remappedThirtySecondsLater = state.toPrivacyDetections().single()

        assertEquals(Instant.parse("2026-05-18T11:59:58Z"), first.lastSeen)
        assertEquals(first.lastSeen, remappedThirtySecondsLater.lastSeen)
    }

    private fun badgeEntity(
        label: String,
        detail: String,
        evidence: String = "",
        category: String,
        code: String,
    ) = BadgeThreatEntity(
        label = label,
        detail = detail,
        evidence = evidence,
        threatClass = "privacy",
        category = category,
        code = code,
        displayId = "badge-row-1",
        source = "ble",
        sourceId = 1,
        ssid = "",
        bssid = "",
        authMode = -1,
        freqMhz = 0,
        score = 75,
        confidencePct = 70,
        evidenceQuality = 2,
        displayRank = 1,
        ageSeconds = 3,
        lastSeenSeconds = 1,
        rssi = -48,
        bestRssi = -45,
        events = 2,
        seenCount = 2,
        groupCount = 1,
        proximityLevel = 2,
        stale = false,
        lat = null,
        lon = null,
        altitudeM = null,
        operatorLat = null,
        operatorLon = null,
        operatorId = null,
    )

    private fun statusFixture(
        receivedAtElapsedMs: Long,
        entity: BadgeThreatEntity,
    ) = BadgeControlStatus(
        version = "0.64.65",
        receivedAtElapsedMs = receivedAtElapsedMs,
        receivedAtWallClock = Instant.parse("2026-05-18T12:00:00Z"),
        themeReadback = BadgeConfigReadback(null, null, "not included in fixture"),
        policyReadback = BadgeConfigReadback(null, null, "not included in fixture"),
        networkModeReadback = BadgeNetworkModeReadback(null, "not included in fixture"),
        entities = listOf(entity),
        scanners = emptyList(),
        displayState = null,
        debugBridge = null,
        reporting = BadgeReportingStatus(),
        counts = BadgeThreatCounts(),
        bleControl = BadgeBleControlStatus(),
        safeMode = false,
        safeReason = "",
        resetReason = "",
        crashCount = 0,
        recoveryMode = "",
        stackFreeBytes = emptyMap(),
        heapInternalFreeBytes = 0,
        heapInternalMinimumFreeBytes = 0,
        psramFreeBytes = 0,
    )
}
