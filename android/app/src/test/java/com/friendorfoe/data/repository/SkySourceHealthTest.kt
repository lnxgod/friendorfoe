package com.friendorfoe.data.repository

import com.friendorfoe.domain.model.Aircraft
import com.friendorfoe.domain.model.DetectionSource
import com.friendorfoe.domain.model.Drone
import com.friendorfoe.domain.model.ObjectCategory
import com.friendorfoe.domain.model.Position
import com.friendorfoe.domain.model.SkyObject
import java.time.Instant
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class SkySourceHealthTest {
    @Test
    fun oneLiveAndOneFailedSourceResolveWithoutErasingEitherSourcesRows() {
        val aircraft = aircraft("aircraft")
        val cachedDrone = drone("cached-drone")
        val resolution = reduceSkySources(
            snapshots = mapOf(
                SkySourceKind.BLE_REMOTE_ID to SkySourceSnapshot(
                    source = SkySourceKind.BLE_REMOTE_ID,
                    enabled = true,
                    resolved = true,
                    lastSuccessElapsedMs = 80L,
                    failure = "Scanner unavailable",
                    rows = listOf(cachedDrone),
                ),
                SkySourceKind.ADS_B to SkySourceSnapshot(
                    source = SkySourceKind.ADS_B,
                    enabled = true,
                    resolved = true,
                    lastSuccessElapsedMs = 100L,
                    failure = null,
                    rows = listOf(aircraft),
                ),
            ),
            expectedEnabledSources = setOf(
                SkySourceKind.ADS_B,
                SkySourceKind.BLE_REMOTE_ID,
            ),
        )

        assertTrue(resolution.resolved)
        assertEquals(listOf(aircraft, cachedDrone), resolution.rows)
        assertEquals(
            listOf(
                SkySourceFailure(
                    source = SkySourceKind.BLE_REMOTE_ID,
                    message = "Scanner unavailable",
                    lastSuccessElapsedMs = 80L,
                    cachedRowCount = 1,
                ),
            ),
            resolution.failures,
        )
        assertEquals("Remote ID · Bluetooth: Scanner unavailable", resolution.failure)
        assertEquals(100L, resolution.latestSuccessElapsedMs)
    }

    @Test
    fun allDisabledSourcesResolveImmediatelyAndContributeNothing() {
        val disabled = SkySourceSnapshot(
            source = SkySourceKind.ADS_B,
            enabled = false,
            resolved = false,
            lastSuccessElapsedMs = 10L,
            failure = "Ignored while disabled",
            rows = listOf(aircraft("disabled")),
        )

        val resolution = reduceSkySources(
            snapshots = mapOf(SkySourceKind.ADS_B to disabled),
            expectedEnabledSources = emptySet(),
        )

        assertTrue(resolution.resolved)
        assertTrue(resolution.rows.isEmpty())
        assertTrue(resolution.failures.isEmpty())
        assertNull(resolution.failure)
        assertNull(resolution.latestSuccessElapsedMs)
    }

    @Test
    fun successfulEmptyResponseIsResolvedAndRetainsItsMonotonicSuccessTime() {
        val resolution = reduceSkySources(
            snapshots = mapOf(
                SkySourceKind.WIFI_REMOTE_ID to SkySourceSnapshot(
                    source = SkySourceKind.WIFI_REMOTE_ID,
                    enabled = true,
                    resolved = true,
                    lastSuccessElapsedMs = 250L,
                    failure = null,
                    rows = emptyList(),
                ),
            ),
            expectedEnabledSources = setOf(SkySourceKind.WIFI_REMOTE_ID),
        )

        assertTrue(resolution.resolved)
        assertTrue(resolution.rows.isEmpty())
        assertEquals(250L, resolution.latestSuccessElapsedMs)
    }

    @Test
    fun unresolvedEnabledSourceKeepsFirstResolutionPending() {
        val resolution = reduceSkySources(
            snapshots = mapOf(
                SkySourceKind.PHONE_DERIVED to SkySourceSnapshot(
                    source = SkySourceKind.PHONE_DERIVED,
                    enabled = true,
                    resolved = false,
                    lastSuccessElapsedMs = null,
                    failure = null,
                    rows = emptyList(),
                ),
            ),
            expectedEnabledSources = setOf(SkySourceKind.PHONE_DERIVED),
        )

        assertFalse(resolution.resolved)
    }

    @Test
    fun emptyExpectedSourceSetResolvesImmediatelyWithoutSnapshots() {
        val resolution = reduceSkySources(
            snapshots = emptyMap(),
            expectedEnabledSources = emptySet(),
        )

        assertTrue(resolution.resolved)
        assertTrue(resolution.rows.isEmpty())
        assertTrue(resolution.failures.isEmpty())
        assertNull(resolution.latestSuccessElapsedMs)
    }

    @Test
    fun missingExpectedSourceKeepsFirstResolutionPending() {
        val aircraft = aircraft("ready-adsb")
        val resolution = reduceSkySources(
            snapshots = mapOf(
                SkySourceKind.ADS_B to SkySourceSnapshot(
                    source = SkySourceKind.ADS_B,
                    enabled = true,
                    resolved = true,
                    lastSuccessElapsedMs = 100L,
                    failure = null,
                    rows = listOf(aircraft),
                ),
            ),
            expectedEnabledSources = setOf(
                SkySourceKind.ADS_B,
                SkySourceKind.BLE_REMOTE_ID,
            ),
        )

        assertFalse(resolution.resolved)
        assertEquals(listOf(aircraft), resolution.rows)
    }

    @Test
    fun mismatchedSnapshotRegistrationDoesNotSatisfyAnExpectedSource() {
        val resolution = reduceSkySources(
            snapshots = mapOf(
                SkySourceKind.ADS_B to SkySourceSnapshot(
                    source = SkySourceKind.BLE_REMOTE_ID,
                    enabled = true,
                    resolved = true,
                    lastSuccessElapsedMs = 100L,
                    failure = null,
                    rows = listOf(drone("wrong-source")),
                ),
            ),
            expectedEnabledSources = setOf(SkySourceKind.ADS_B),
        )

        assertFalse(resolution.resolved)
        assertTrue(resolution.rows.isEmpty())
        assertTrue(resolution.failures.isEmpty())
        assertNull(resolution.latestSuccessElapsedMs)
    }

    private fun aircraft(id: String): Aircraft = Aircraft(
        id = id,
        position = Position(32.7, -117.1, 1_000.0),
        source = DetectionSource.ADS_B,
        category = ObjectCategory.COMMERCIAL,
        firstSeen = Instant.EPOCH,
        lastUpdated = Instant.EPOCH,
        icaoHex = id,
    )

    private fun drone(id: String): Drone = Drone(
        id = id,
        position = Position(32.7, -117.1, 100.0),
        source = DetectionSource.REMOTE_ID,
        confidence = 0.9f,
        firstSeen = Instant.EPOCH,
        lastUpdated = Instant.EPOCH,
        droneId = id,
    )
}
