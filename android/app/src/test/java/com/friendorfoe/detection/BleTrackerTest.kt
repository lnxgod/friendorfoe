package com.friendorfoe.detection

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.time.Instant

class BleTrackerTest {

    @Test
    fun `old two minute fifty meter profile no longer alerts`() {
        val tracker = BleTracker()
        val start = Instant.parse("2026-07-16T12:00:00Z")

        listOf(0L to 37.0000, 60L to 37.0004, 121L to 37.0008).forEach { (seconds, lat) ->
            record(tracker, start.plusSeconds(seconds), lat)
        }

        assertTrue(tracker.checkForFollowersAt(start.plusSeconds(122)).isEmpty())
    }

    @Test
    fun `poor gps accuracy does not alert`() {
        val tracker = BleTracker()
        val start = Instant.parse("2026-07-16T12:00:00Z")

        repeat(6) { index ->
            record(
                tracker = tracker,
                timestamp = start.plusSeconds(index * 61L),
                latitude = FOLLOWING_LATITUDES[index],
                accuracy = 80f,
            )
        }

        assertTrue(tracker.checkForFollowersAt(start.plusSeconds(306)).isEmpty())
    }

    @Test
    fun `bonded device never alerts`() {
        val tracker = BleTracker()
        val start = Instant.parse("2026-07-16T12:00:00Z")

        recordFollowerPath(tracker, start, bonded = true)

        assertTrue(tracker.checkForFollowersAt(start.plusSeconds(301)).isEmpty())
    }

    @Test
    fun `stationary and noncandidate categories never alert when misrouted`() {
        val start = Instant.parse("2026-07-16T12:00:00Z")
        val excluded = listOf(
            PrivacyCategory.VENUE_BEACON,
            PrivacyCategory.HIDDEN_CAMERA,
            PrivacyCategory.INFORMATIONAL,
        )

        excluded.forEach { category ->
            val tracker = BleTracker()
            recordFollowerPath(tracker, start, category = category)
            assertTrue(category.name, tracker.checkForFollowersAt(start.plusSeconds(301)).isEmpty())
        }
    }

    @Test
    fun `six sightings across three clusters and five minutes alert`() {
        val tracker = BleTracker()
        val start = Instant.parse("2026-07-16T12:00:00Z")

        recordFollowerPath(tracker, start, strongestRssi = -55)

        val alert = tracker.checkForFollowersAt(start.plusSeconds(301)).single()
        assertEquals("following", alert.reason)
        assertEquals(2, alert.threatLevel)
        assertTrue(alert.device.isFollowing)
        assertTrue(alert.device.isStalker)
        assertEquals(300_000L, alert.evidence.durationMs)
        assertEquals(6, alert.evidence.qualifyingSightings)
        assertEquals(3, alert.evidence.clusterCount)
        assertTrue(alert.evidence.movementMeters >= 150.0)
        assertEquals(setOf(0, 1, 2), alert.evidence.temporalBands)
        assertEquals(-55, alert.evidence.strongestRssi)
    }

    @Test
    fun `two sequential clusters do not alert despite enough displacement`() {
        val tracker = BleTracker()
        val start = Instant.parse("2026-07-16T12:00:00Z")
        val latitudes = listOf(37.0000, 37.0000, 37.0000, 37.0017, 37.0017, 37.0017)

        latitudes.forEachIndexed { index, latitude ->
            record(tracker, start.plusSeconds(index * 60L), latitude)
        }

        assertTrue(tracker.checkForFollowersAt(start.plusSeconds(301)).isEmpty())
    }

    @Test
    fun `following requires first middle and final temporal thirds`() {
        val tracker = BleTracker()
        val start = Instant.parse("2026-07-16T12:00:00Z")
        val seconds = listOf(0L, 10L, 20L, 30L, 290L, 300L)

        seconds.forEachIndexed { index, offset ->
            record(tracker, start.plusSeconds(offset), FOLLOWING_LATITUDES[index])
        }

        assertTrue(tracker.checkForFollowersAt(start.plusSeconds(301)).isEmpty())
    }

    @Test
    fun `unrelated global user movement cannot create follower evidence`() {
        val tracker = BleTracker()
        val start = Instant.parse("2026-07-16T12:00:00Z")
        tracker.recordUserLocation(36.0, -122.0, start)

        repeat(6) { index ->
            record(
                tracker = tracker,
                timestamp = start.plusSeconds(index * 60L),
                latitude = 37.0 + (index % 2) * 0.0001,
            )
        }
        tracker.recordUserLocation(38.0, -122.0, start.plusSeconds(301))

        assertTrue(tracker.checkForFollowersAt(start.plusSeconds(301)).isEmpty())
    }

    @Test
    fun `camera reaches level three only after ten minutes with strong rssi`() {
        val start = Instant.parse("2026-07-16T12:00:00Z")
        val qualifying = BleTracker().also {
            recordFollowerPath(it, start, durationSeconds = 600L, hasCamera = true, strongestRssi = -70)
        }
        val tooShort = BleTracker().also {
            recordFollowerPath(it, start, durationSeconds = 599L, hasCamera = true, strongestRssi = -60)
        }
        val tooWeak = BleTracker().also {
            recordFollowerPath(it, start, durationSeconds = 600L, hasCamera = true, strongestRssi = -71)
        }

        assertEquals(3, qualifying.checkForFollowersAt(start.plusSeconds(601)).single().threatLevel)
        assertEquals(2, tooShort.checkForFollowersAt(start.plusSeconds(600)).single().threatLevel)
        assertEquals(2, tooWeak.checkForFollowersAt(start.plusSeconds(601)).single().threatLevel)
    }

    @Test
    fun `lingering remains level one after ten minutes`() {
        val tracker = BleTracker()
        val start = Instant.parse("2026-07-16T12:00:00Z")

        repeat(10) { index ->
            record(
                tracker = tracker,
                timestamp = start.plusSeconds(index * 600L / 9L),
                latitude = 37.0 + (index % 2) * 0.00005,
                rssi = if (index == 4) -70 else -75,
            )
        }

        val alert = tracker.checkForFollowersAt(start.plusSeconds(601)).single()
        assertEquals("lingering", alert.reason)
        assertEquals(1, alert.threatLevel)
        assertFalse(alert.device.isFollowing)
        assertFalse(alert.device.isStalker)
        assertEquals(600_000L, alert.evidence.durationMs)
        assertEquals(10, alert.evidence.qualifyingSightings)
        assertTrue(alert.evidence.movementMeters <= 25.0)
        assertEquals(setOf(0, 1, 2), alert.evidence.temporalBands)
        assertEquals(-70, alert.evidence.strongestRssi)
    }

    @Test
    fun `lingering rejects weak sparse or moving evidence`() {
        val start = Instant.parse("2026-07-16T12:00:00Z")
        val weak = BleTracker()
        val sparse = BleTracker()
        val moving = BleTracker()

        repeat(10) { index ->
            val timestamp = start.plusSeconds(index * 600L / 9L)
            record(weak, timestamp, 37.0, rssi = -71)
            record(moving, timestamp, 37.0 + index * 0.00003, rssi = -60)
            if (index < 9) record(sparse, timestamp, 37.0, rssi = -60)
        }

        assertTrue(weak.checkForFollowersAt(start.plusSeconds(601)).isEmpty())
        assertTrue(sparse.checkForFollowersAt(start.plusSeconds(601)).isEmpty())
        assertTrue(moving.checkForFollowersAt(start.plusSeconds(601)).isEmpty())
    }

    @Test
    fun `each evaluation clears stale following flags`() {
        val tracker = BleTracker()
        val start = Instant.parse("2026-07-16T12:00:00Z")
        recordFollowerPath(tracker, start)
        val firstAlert = tracker.checkForFollowersAt(start.plusSeconds(301)).single()

        record(
            tracker = tracker,
            timestamp = start.plusSeconds(302),
            latitude = 37.0017,
            bonded = true,
        )

        assertTrue(tracker.checkForFollowersAt(start.plusSeconds(303)).isEmpty())
        assertFalse(firstAlert.device.isFollowing)
        assertFalse(firstAlert.device.isStalker)
    }

    @Test
    fun `clear removes follower evidence for a new detection session`() {
        val tracker = BleTracker()
        val start = Instant.parse("2026-07-16T12:00:00Z")
        recordFollowerPath(tracker, start)
        val device = tracker.checkForFollowersAt(start.plusSeconds(301)).single().device

        tracker.clear()

        assertTrue(tracker.getTrackedDevices().isEmpty())
        assertTrue(tracker.checkForFollowersAt(start.plusSeconds(302)).isEmpty())
        assertFalse(device.isFollowing)
        assertFalse(device.isStalker)
    }

    @Test
    fun `direction scan keeps samples and averages across north`() {
        val tracker = BleTracker()
        val start = Instant.parse("2026-07-16T12:00:00Z")
        val samples = listOf(
            350f to -40,
            0f to -41,
            10f to -42,
            90f to -60,
            135f to -62,
            180f to -64,
            225f to -66,
            270f to -68,
        )

        tracker.startDirectionScan(DEFAULT_MAC)
        samples.forEachIndexed { index, (bearing, rssi) ->
            record(
                tracker = tracker,
                timestamp = start.plusSeconds(index.toLong()),
                latitude = 37.0,
                compassBearing = bearing,
                rssi = rssi,
            )
        }

        val result = tracker.finishDirectionScan()
        assertNotNull(result)
        requireNotNull(result)
        assertFalse(tracker.isDirectionScanActive())
        assertEquals(8, result.samples.size)
        assertEquals(-40, result.peakRssi)
        assertTrue(result.estimatedBearing > 340f || result.estimatedBearing < 20f)
    }

    private fun recordFollowerPath(
        tracker: BleTracker,
        start: Instant,
        durationSeconds: Long = 300L,
        bonded: Boolean = false,
        category: PrivacyCategory = PrivacyCategory.BLE_TRACKER,
        hasCamera: Boolean = false,
        strongestRssi: Int = -60,
    ) {
        FOLLOWING_LATITUDES.forEachIndexed { index, latitude ->
            record(
                tracker = tracker,
                timestamp = start.plusSeconds(index * durationSeconds / (FOLLOWING_LATITUDES.size - 1)),
                latitude = latitude,
                bonded = bonded,
                category = category,
                hasCamera = hasCamera,
                rssi = if (index == 2) strongestRssi else strongestRssi - 5,
            )
        }
    }

    private fun record(
        tracker: BleTracker,
        timestamp: Instant,
        latitude: Double,
        longitude: Double = -122.0,
        accuracy: Float = 5f,
        bonded: Boolean = false,
        rssi: Int = -60,
        category: PrivacyCategory = PrivacyCategory.BLE_TRACKER,
        hasCamera: Boolean = false,
        compassBearing: Float = 0f,
    ) = tracker.recordSightingAt(
        mac = DEFAULT_MAC,
        rssi = rssi,
        deviceName = "Test Tag",
        deviceType = "BLE Tracker",
        manufacturer = "Generic",
        hasCamera = hasCamera,
        category = category,
        isBonded = bonded,
        userLat = latitude,
        userLon = longitude,
        locationAccuracyMeters = accuracy,
        compassBearing = compassBearing,
        timestamp = timestamp,
    )

    companion object {
        private const val DEFAULT_MAC = "AA:BB:CC:00:00:01"
        private val FOLLOWING_LATITUDES = listOf(
            37.0000,
            37.0000,
            37.0008,
            37.0008,
            37.0017,
            37.0017,
        )
    }
}
