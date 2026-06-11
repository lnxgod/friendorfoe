package com.friendorfoe.presentation.privacy

import com.friendorfoe.data.badge.BadgeThreatEntity
import com.friendorfoe.detection.PrivacyCategory
import java.time.Instant
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
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
}
