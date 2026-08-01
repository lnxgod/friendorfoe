package com.friendorfoe.detection

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertSame
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
    fun `wall clock rollback does not move last seen backward`() {
        val tracker = BleTracker()
        val start = Instant.parse("2026-07-16T12:00:00Z")

        record(tracker, start.plusSeconds(300), latitudeAtMeters(0.0))
        record(tracker, start.plusSeconds(180), latitudeAtMeters(80.0))

        assertEquals(start.plusSeconds(300), tracker.getDevice(DEFAULT_MAC)?.lastSeen)
    }

    @Test
    fun `rollback timestamps cannot create a false third cluster`() {
        val tracker = BleTracker()
        val start = Instant.parse("2026-07-16T12:00:00Z")
        val observations = listOf(
            0L to 0.0,
            60L to 0.0,
            240L to 80.0,
            300L to 80.0,
            120L to 160.0,
            180L to 160.0,
        )

        observations.forEach { (seconds, meters) ->
            record(tracker, start.plusSeconds(seconds), latitudeAtMeters(meters))
        }

        assertTrue(tracker.checkForFollowersAt(start.plusSeconds(301)).isEmpty())
    }

    @Test
    fun `future timestamps rejected early cannot mature into follower movement`() {
        val tracker = BleTracker()
        val start = Instant.parse("2026-07-16T12:00:00Z")
        val observations = listOf(
            0L to 0.0,
            60L to 0.0,
            300L to 80.0,
            300L to 80.0,
            900L to 160.0,
            900L to 160.0,
        )

        observations.forEach { (seconds, meters) ->
            record(tracker, start.plusSeconds(seconds), latitudeAtMeters(meters))
        }

        assertTrue(tracker.checkForFollowersAt(start.plusSeconds(301)).isEmpty())
        assertTrue(tracker.checkForFollowersAt(start.plusSeconds(901)).isEmpty())
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
    fun `exact gps cluster and movement boundaries are inclusive`() {
        val tracker = BleTracker()
        val start = Instant.parse("2026-07-16T12:00:00Z")
        val meters = listOf(0.0, 0.0, 75.0, 75.0, 150.0, 150.0)

        recordMeterPath(tracker, start, meters, accuracy = 50f)

        val alert = tracker.checkForFollowersAt(start.plusSeconds(301)).single()
        assertEquals("following", alert.reason)
        assertEquals(3, alert.evidence.clusterCount)
        assertEquals(150.0, alert.evidence.movementMeters, DISTANCE_TOLERANCE_M)
    }

    @Test
    fun `follower distances one centimeter below boundaries do not alert`() {
        val tracker = BleTracker()
        val start = Instant.parse("2026-07-16T12:00:00Z")

        recordMeterPath(
            tracker = tracker,
            start = start,
            meters = listOf(0.0, 0.0, 75.0, 75.0, 149.99, 149.99),
        )

        assertTrue(tracker.checkForFollowersAt(start.plusSeconds(301)).isEmpty())
    }

    @Test
    fun `follower distances one centimeter above boundaries alert`() {
        val tracker = BleTracker()
        val start = Instant.parse("2026-07-16T12:00:00Z")

        recordMeterPath(
            tracker = tracker,
            start = start,
            meters = listOf(0.0, 0.0, 75.01, 75.01, 150.02, 150.02),
        )

        val alert = tracker.checkForFollowersAt(start.plusSeconds(301)).single()
        assertEquals(3, alert.evidence.clusterCount)
        assertTrue(alert.evidence.movementMeters > 150.0)
    }

    @Test
    fun `gps accuracy boundary distinguishes one centimeter sides`() {
        val start = Instant.parse("2026-07-16T12:00:00Z")
        val below = BleTracker()
        val above = BleTracker()
        val meters = listOf(0.0, 0.0, 80.0, 80.0, 160.0, 160.0)

        recordMeterPath(below, start, meters, accuracy = 49.99f)
        recordMeterPath(above, start, meters, accuracy = 50.01f)

        assertEquals("following", below.checkForFollowersAt(start.plusSeconds(301)).single().reason)
        assertTrue(above.checkForFollowersAt(start.plusSeconds(301)).isEmpty())
    }

    @Test
    fun `exact twenty five meter lingering span is inclusive`() {
        val tracker = BleTracker()
        val start = Instant.parse("2026-07-16T12:00:00Z")

        recordLingeringSpan(tracker, start, spanMeters = 25.0)

        val alert = tracker.checkForFollowersAt(start.plusSeconds(601)).single()
        assertEquals("lingering", alert.reason)
        assertEquals(25.0, alert.evidence.movementMeters, DISTANCE_TOLERANCE_M)
    }

    @Test
    fun `lingering span distinguishes one centimeter sides`() {
        val start = Instant.parse("2026-07-16T12:00:00Z")
        val below = BleTracker()
        val above = BleTracker()

        recordLingeringSpan(below, start, spanMeters = 24.99)
        recordLingeringSpan(above, start, spanMeters = 25.01)

        assertEquals("lingering", below.checkForFollowersAt(start.plusSeconds(601)).single().reason)
        assertTrue(above.checkForFollowersAt(start.plusSeconds(601)).isEmpty())
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
    fun `name enrichment preserves device identity through reset and clear`() {
        val tracker = BleTracker()
        val start = Instant.parse("2026-07-16T12:00:00Z")

        record(
            tracker = tracker,
            timestamp = start,
            latitude = FOLLOWING_LATITUDES.first(),
            deviceName = null,
        )
        val device = requireNotNull(tracker.getDevice(DEFAULT_MAC))
        FOLLOWING_LATITUDES.drop(1).forEachIndexed { offset, latitude ->
            val index = offset + 1
            record(
                tracker = tracker,
                timestamp = start.plusSeconds(index * 60L),
                latitude = latitude,
            )
        }
        val alert = tracker.checkForFollowersAt(start.plusSeconds(301)).single()
        assertSame(device, alert.device)
        assertEquals("Test Tag", device.deviceName)

        record(
            tracker = tracker,
            timestamp = start.plusSeconds(302),
            latitude = FOLLOWING_LATITUDES.last(),
            bonded = true,
        )
        assertTrue(tracker.checkForFollowersAt(start.plusSeconds(303)).isEmpty())
        assertFalse(device.isFollowing)
        assertFalse(device.isStalker)

        tracker.clear()
        assertFalse(device.isFollowing)
        assertFalse(device.isStalker)
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
    fun `remove evidence evicts only matching canonical device identities`() {
        val tracker = BleTracker()
        val start = Instant.parse("2026-07-16T12:00:00Z")
        val retainedMac = "AA:BB:CC:00:00:02"
        record(tracker, start, FOLLOWING_LATITUDES.first(), mac = DEFAULT_MAC)
        record(tracker, start, FOLLOWING_LATITUDES.first(), mac = retainedMac)
        tracker.startDirectionScan(DEFAULT_MAC)

        assertEquals(
            1,
            tracker.removeEvidenceForIdentities(setOf("MAC:aa:bb:cc:00:00:01")),
        )

        assertNull(tracker.getDevice(DEFAULT_MAC))
        assertNotNull(tracker.getDevice(retainedMac))
        assertFalse(tracker.isDirectionScanActive())
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

    @Test
    fun `multi device alerts have deterministic severity and mac order`() {
        val tracker = BleTracker()
        val start = Instant.parse("2026-07-16T12:00:00Z")
        recordFollowerPath(tracker, start, mac = "AA:BB:CC:00:00:02")
        recordFollowerPath(tracker, start, mac = "AA:BB:CC:00:00:01")
        recordFollowerPath(
            tracker = tracker,
            start = start,
            mac = "AA:BB:CC:00:00:03",
            durationSeconds = 600L,
            hasCamera = true,
            strongestRssi = -70,
        )

        val alerts = tracker.checkForFollowersAt(start.plusSeconds(601))

        assertEquals(listOf(3, 2, 2), alerts.map { it.threatLevel })
        assertEquals(
            listOf(
                "AA:BB:CC:00:00:03",
                "AA:BB:CC:00:00:01",
                "AA:BB:CC:00:00:02",
            ),
            alerts.map { it.device.mac },
        )
    }

    private fun recordFollowerPath(
        tracker: BleTracker,
        start: Instant,
        durationSeconds: Long = 300L,
        bonded: Boolean = false,
        category: PrivacyCategory = PrivacyCategory.BLE_TRACKER,
        hasCamera: Boolean = false,
        strongestRssi: Int = -60,
        mac: String = DEFAULT_MAC,
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
                mac = mac,
            )
        }
    }

    private fun recordMeterPath(
        tracker: BleTracker,
        start: Instant,
        meters: List<Double>,
        accuracy: Float = 5f,
    ) {
        meters.forEachIndexed { index, offsetMeters ->
            record(
                tracker = tracker,
                timestamp = start.plusSeconds(index * 60L),
                latitude = latitudeAtMeters(offsetMeters),
                accuracy = accuracy,
            )
        }
    }

    private fun recordLingeringSpan(
        tracker: BleTracker,
        start: Instant,
        spanMeters: Double,
    ) {
        repeat(10) { index ->
            record(
                tracker = tracker,
                timestamp = start.plusSeconds(index * 600L / 9L),
                latitude = latitudeAtMeters(if (index % 2 == 0) 0.0 else spanMeters),
                accuracy = 50f,
                rssi = -70,
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
        mac: String = DEFAULT_MAC,
        deviceName: String? = "Test Tag",
    ) = tracker.recordSightingAt(
        mac = mac,
        rssi = rssi,
        deviceName = deviceName,
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

    private fun latitudeAtMeters(meters: Double): Double =
        BASE_LATITUDE + Math.toDegrees(meters / EARTH_RADIUS_M)

    companion object {
        private const val DEFAULT_MAC = "AA:BB:CC:00:00:01"
        private const val BASE_LATITUDE = 37.0
        private const val EARTH_RADIUS_M = 6_371_000.0
        private const val DISTANCE_TOLERANCE_M = 0.001
        private val FOLLOWING_LATITUDES = listOf(
            37.0000,
            37.0000,
            37.0008,
            37.0008,
            37.0017,
            37.0017,
        )
    }

    @Test
    fun unknownZeroCoordinateSightingsCannotCreateASecondFollowerLocation() {
        val tracker = BleTracker()
        val start = Instant.parse("2026-06-11T12:00:00Z")
        tracker.recordUserLocation(37.0000, -122.0000, start)
        tracker.recordUserLocation(37.0010, -122.0000, start.plusSeconds(200))

        listOf(
            Triple(0.0, 0.0, 0L),
            Triple(0.0, 0.0, 80L),
            Triple(37.0010, -122.0000, 160L),
        ).forEach { (lat, lon, seconds) ->
            tracker.recordSightingAt(
                mac = "AA:BB:CC:00:00:03",
                rssi = -60,
                deviceName = "Unlocated device",
                deviceType = "BLE device",
                manufacturer = null,
                hasCamera = false,
                userLat = lat,
                userLon = lon,
                compassBearing = 0f,
                timestamp = start.plusSeconds(seconds),
            )
        }

        assertTrue(tracker.checkForFollowersAt(start.plusSeconds(201)).isEmpty())
    }

    @Test
    fun directionOnlySamplesFeedAnActiveSweepWithoutCreatingMovementHistory() {
        val tracker = BleTracker()
        val mac = "AA:BB:CC:00:00:04"
        tracker.startDirectionScan(mac)

        repeat(8) { index ->
            tracker.recordDirectionSample(
                mac = mac,
                rssi = -70 + index,
                compassBearing = index * 45f,
            )
        }

        assertEquals(null, tracker.getDevice(mac))
        assertEquals(8, tracker.getDirectionSampleCount())
        assertEquals(8, tracker.finishDirectionScan()?.samples?.size)
    }
}
