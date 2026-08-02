package com.friendorfoe.presentation.map

import com.friendorfoe.data.remote.LocatedDroneDto
import com.friendorfoe.domain.model.Aircraft
import com.friendorfoe.domain.model.ObjectCategory
import com.friendorfoe.domain.model.Position
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.take
import kotlinx.coroutines.flow.toList
import kotlinx.coroutines.launch
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertSame
import org.junit.Assert.assertTrue
import org.junit.Test
import java.time.Instant

@OptIn(ExperimentalCoroutinesApi::class)
class MapOverlayPresentationTest {

    @Test
    fun `fresh coasting and stale alpha remain distinguishable`() {
        assertEquals(1f, mapTrackAlpha(1f), 0.001f)
        assertEquals(0.6f, mapTrackAlpha(0.5f), 0.001f)
        assertEquals(0.25f, mapTrackAlpha(0f), 0.001f)
    }

    @Test
    fun `map frame clock emits epoch timestamps while pacing on monotonic time`() = runTest {
        var epochNowMs = 1_700_000_000_000L
        var monotonicNowMs = 1_000L
        val delays = mutableListOf<Long>()

        val frames = mapFrameClock(
            epochNowMs = { epochNowMs },
            monotonicNowMs = { monotonicNowMs },
            waitForNextFrame = { delayMs ->
                delays += delayMs
                monotonicNowMs += delayMs
                epochNowMs += delayMs
            },
        ).take(3).toList()

        assertEquals(
            listOf(1_700_000_000_000L, 1_700_000_000_250L, 1_700_000_000_500L),
            frames,
        )
        assertEquals(listOf(250L, 250L), delays)
    }

    @Test
    fun `stabilized heading consumes raw values only while following and resets when disabled`() = runTest {
        var nowMs = 0L
        val follow = MutableStateFlow(false)
        val rawHeading = MutableStateFlow(12f)
        val emitted = mutableListOf<Float>()
        val collection = backgroundScope.launch {
            stabilizedMapHeadingFlow(follow, rawHeading) { nowMs }
                .toList(emitted)
        }
        runCurrent()

        follow.value = true
        runCurrent()
        assertEquals(12f, emitted.last(), 0.001f)

        nowMs = 250L
        rawHeading.value = 90f
        runCurrent()
        val followedHeading = emitted.last()
        assertTrue(followedHeading > 12f)

        follow.value = false
        runCurrent()
        assertEquals(0f, emitted.last(), 0.001f)

        nowMs = 500L
        rawHeading.value = 180f
        runCurrent()
        assertEquals(0f, emitted.last(), 0.001f)
        assertEquals(180f, rawHeading.value, 0.001f)

        follow.value = true
        runCurrent()
        assertEquals(180f, emitted.last(), 0.001f)
        collection.cancel()
    }

    @Test
    fun `aircraft presentation uses projected motion but raw object click identity`() {
        val raw = aircraft("raw-id")
        val track = MapTrack(
            skyObject = raw,
            position = raw.position.copy(latitude = 38.0, longitude = -121.0),
            ageSeconds = 5f,
            confidence = 0.5f,
            isExtrapolated = true,
            headingDegrees = 123f,
        )

        val presentation = track.toAircraftMarkerPresentation(visuallyConfirmed = true)

        assertEquals("raw-id", presentation.key)
        assertEquals(38.0, presentation.latitude, 0.0)
        assertEquals(-121.0, presentation.longitude, 0.0)
        assertEquals(123f, presentation.rotationDegrees, 0.001f)
        assertEquals(0.6f, presentation.alpha, 0.001f)
        assertTrue(presentation.visuallyConfirmed)
    }

    @Test
    fun `retained store updates existing instances and removes departed keys`() {
        data class Fixture(val key: String, val value: Int)
        data class Retained(var value: Int)

        val removed = mutableListOf<Retained>()
        val store = RetainedOverlayStore<Retained>()
        store.render(
            desired = listOf(Fixture("a", 1), Fixture("b", 2)),
            keyOf = Fixture::key,
            create = { Retained(it.value) },
            update = { retained, fixture -> retained.value = fixture.value },
            remove = removed::add,
        )
        val originalA = store["a"]

        store.render(
            desired = listOf(Fixture("a", 3)),
            keyOf = Fixture::key,
            create = { Retained(it.value) },
            update = { retained, fixture -> retained.value = fixture.value },
            remove = removed::add,
        )

        assertSame(originalA, store["a"])
        assertEquals(3, store["a"]?.value)
        assertEquals(setOf("a"), store.keys)
        assertEquals(1, removed.size)
        assertFalse(removed.single() === originalA)
    }

    @Test
    fun `distance rings change only with center or zoom bucket`() {
        val center = Position(37.0, -122.0, 0.0)
        val original = distanceRingKey(center, zoomLevel = 10.1, remoteCenter = null)

        assertEquals(original, distanceRingKey(center, zoomLevel = 10.9, remoteCenter = null))
        assertFalse(original == distanceRingKey(center, zoomLevel = 11.0, remoteCenter = null))
        assertFalse(
            original == distanceRingKey(
                center.copy(latitude = 37.0001),
                zoomLevel = 10.1,
                remoteCenter = null,
            )
        )
    }

    @Test
    fun `sensor shapes draw before their retained markers`() {
        val rangeDrone = LocatedDroneDto(
            droneId = "range",
            lat = 37.0,
            lon = -122.0,
            positionSource = "range_only",
            rangeM = 100.0,
        )
        val accurateDrone = LocatedDroneDto(
            droneId = "accurate",
            lat = 37.1,
            lon = -122.1,
            positionSource = "trilateration",
            accuracyM = 25.0,
        )

        assertEquals(
            listOf(
                SensorOverlayKey("range", SensorOverlayKind.RANGE),
                SensorOverlayKey("accurate", SensorOverlayKind.ACCURACY),
                SensorOverlayKey("range", SensorOverlayKind.MARKER),
                SensorOverlayKey("accurate", SensorOverlayKind.MARKER),
            ),
            sensorOverlayDrawOrder(listOf(rangeDrone, accurateDrone)),
        )
    }

    private fun aircraft(id: String) = Aircraft(
        id = id,
        position = Position(
            latitude = 37.0,
            longitude = -122.0,
            altitudeMeters = 1_000.0,
            heading = 90f,
            speedMps = 100f,
        ),
        category = ObjectCategory.GENERAL_AVIATION,
        firstSeen = Instant.ofEpochMilli(1_000),
        lastUpdated = Instant.ofEpochMilli(1_000),
        icaoHex = id,
    )
}
