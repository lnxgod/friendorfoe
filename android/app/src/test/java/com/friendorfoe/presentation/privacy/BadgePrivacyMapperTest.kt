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
}
