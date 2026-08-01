package com.friendorfoe.presentation.privacy

import com.friendorfoe.data.DetectionSettings
import com.friendorfoe.data.remote.LivePrivacyDeviceDto
import com.friendorfoe.data.remote.LivePrivacyDevicesDto
import com.friendorfoe.data.time.MonotonicClock
import com.friendorfoe.detection.PrivacyCategory
import java.time.Instant
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.awaitCancellation
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.test.advanceTimeBy
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import kotlin.coroutines.Continuation
import kotlin.coroutines.resume
import kotlin.coroutines.suspendCoroutine

@OptIn(ExperimentalCoroutinesApi::class)
class BackendPrivacySourceAdapterTest {

    @Test
    fun endpointReplacementCancelsLateFetchAndPublishesOnlyTheNewNamespace() = runTest {
        val settings = MutableStateFlow(
            DetectionSettings.defaults().copy(backendUrl = "https://old.example/"),
        )
        val oldStarted = CompletableDeferred<Unit>()
        val oldCancelled = CompletableDeferred<Unit>()
        val adapter = BackendPrivacySourceAdapter(
            settings = settings,
            fetch = { endpoint ->
                if (endpoint.contains("old.example")) {
                    oldStarted.complete(Unit)
                    try {
                        awaitCancellation()
                    } finally {
                        oldCancelled.complete(Unit)
                    }
                }
                LivePrivacyDevicesDto(devices = listOf(device(fingerprint = "new-fp")))
            },
            clock = FakeClock(),
            scope = backgroundScope,
            pollIntervalMs = 100L,
        )
        runCurrent()
        assertTrue(oldStarted.isCompleted)

        settings.value = settings.value.copy(backendUrl = "https://new.example/api/")
        runCurrent()

        assertTrue(oldCancelled.isCompleted)
        val finding = adapter.snapshot().findings.single()
        assertTrue(finding.observationKey.sourceRecordId.contains("new.example"))
        assertTrue(finding.stableSourceId.orEmpty().contains("new.example"))
        assertEquals(SourceHealthState.LIVE, adapter.snapshot().health.state)
    }

    @Test
    fun nonCooperativeLateResponseCannotPublishIntoTheReplacementEndpoint() = runTest {
        val settings = MutableStateFlow(
            DetectionSettings.defaults().copy(backendUrl = "https://old.example/"),
        )
        val oldStarted = CompletableDeferred<Unit>()
        lateinit var oldContinuation: Continuation<LivePrivacyDevicesDto>
        val adapter = BackendPrivacySourceAdapter(
            settings = settings,
            fetch = { endpoint ->
                if (endpoint.contains("old.example")) {
                    suspendCoroutine { continuation ->
                        oldContinuation = continuation
                        oldStarted.complete(Unit)
                    }
                } else {
                    LivePrivacyDevicesDto(devices = listOf(device(fingerprint = "new-fp")))
                }
            },
            clock = FakeClock(),
            scope = backgroundScope,
            pollIntervalMs = 100L,
        )
        runCurrent()
        assertTrue(oldStarted.isCompleted)

        settings.value = settings.value.copy(backendUrl = "https://new.example/")
        runCurrent()
        oldContinuation.resume(
            LivePrivacyDevicesDto(devices = listOf(device(fingerprint = "late-old-fp"))),
        )
        runCurrent()

        val finding = adapter.snapshot().findings.single()
        assertTrue(finding.observationKey.sourceRecordId.contains("new.example"))
        assertTrue(finding.stableSourceId.orEmpty().contains("new-fp"))
        assertTrue(adapter.snapshot().findings.none {
            it.stableSourceId.orEmpty().contains("late-old-fp")
        })
    }

    @Test
    fun successfulEmptyResponseIsLiveAndAuthoritativelyClearsBackendRows() = runTest {
        val settings = MutableStateFlow(DetectionSettings.defaults())
        var call = 0
        val adapter = BackendPrivacySourceAdapter(
            settings = settings,
            fetch = {
                call += 1
                if (call == 1) {
                    LivePrivacyDevicesDto(devices = listOf(device(fingerprint = "one")))
                } else {
                    LivePrivacyDevicesDto(devices = emptyList())
                }
            },
            clock = FakeClock(),
            scope = backgroundScope,
            pollIntervalMs = 100L,
        )
        runCurrent()
        assertEquals(1, adapter.snapshot().findings.size)

        advanceTimeBy(100L)
        runCurrent()

        assertEquals(SourceHealthState.LIVE, adapter.snapshot().health.state)
        assertTrue(adapter.snapshot().findings.isEmpty())
    }

    @Test
    fun failureRetainsCachedRowsAndTheirSourceTimestamps() = runTest {
        val settings = MutableStateFlow(DetectionSettings.defaults())
        var call = 0
        val clock = FakeClock(elapsed = 10_000L, wall = 100_000L)
        val adapter = BackendPrivacySourceAdapter(
            settings = settings,
            fetch = {
                call += 1
                if (call == 1) {
                    LivePrivacyDevicesDto(
                        devices = listOf(device(fingerprint = "one", lastSeen = 95.0)),
                    )
                } else {
                    error("backend offline")
                }
            },
            clock = clock,
            scope = backgroundScope,
            pollIntervalMs = 100L,
        )
        runCurrent()
        val first = adapter.snapshot().findings.single()
        assertEquals(5_000L, first.lastObservedElapsedMs)

        clock.elapsed = 40_000L
        clock.wall = 900_000L
        advanceTimeBy(100L)
        runCurrent()

        assertEquals(SourceHealthState.FAILED, adapter.snapshot().health.state)
        assertEquals(first, adapter.snapshot().findings.single())
        assertEquals(10_000L, adapter.snapshot().health.lastSuccessElapsedMs)
    }

    @Test
    fun pauseAndResumeRetainObservationAndSourceSuccessTimestampsUntilARealResponse() = runTest {
        val settings = MutableStateFlow(DetectionSettings.defaults())
        val clock = FakeClock(elapsed = 10_000L, wall = 100_000L)
        val resumedFetchStarted = CompletableDeferred<Unit>()
        var call = 0
        val adapter = BackendPrivacySourceAdapter(
            settings = settings,
            fetch = {
                call += 1
                if (call == 1) {
                    LivePrivacyDevicesDto(
                        devices = listOf(device(fingerprint = "one", lastSeen = 95.0)),
                    )
                } else {
                    resumedFetchStarted.complete(Unit)
                    awaitCancellation()
                }
            },
            clock = clock,
            scope = backgroundScope,
            pollIntervalMs = 100L,
        )
        runCurrent()
        val row = adapter.snapshot().findings.single()
        val lastSuccessElapsed = adapter.snapshot().health.lastSuccessElapsedMs
        val lastSuccessWall = adapter.snapshot().health.lastSuccessWallMs

        clock.elapsed = 50_000L
        clock.wall = 500_000L
        settings.value = settings.value.copy(sensorBackendEnabled = false)
        runCurrent()
        assertEquals(SourceHealthState.PAUSED, adapter.snapshot().health.state)
        assertEquals(row, adapter.snapshot().findings.single())
        assertEquals(lastSuccessElapsed, adapter.snapshot().health.lastSuccessElapsedMs)
        assertEquals(lastSuccessWall, adapter.snapshot().health.lastSuccessWallMs)

        settings.value = settings.value.copy(sensorBackendEnabled = true)
        runCurrent()
        assertTrue(resumedFetchStarted.isCompleted)
        assertEquals(SourceHealthState.STALE, adapter.snapshot().health.state)
        assertEquals(row, adapter.snapshot().findings.single())
        assertEquals(lastSuccessElapsed, adapter.snapshot().health.lastSuccessElapsedMs)
        assertEquals(lastSuccessWall, adapter.snapshot().health.lastSuccessWallMs)
    }

    @Test
    fun onlyExplicitFingerprintCreatesDurableIdentity() {
        val clock = FakeClock(elapsed = 10_000L, wall = 100_000L)
        val noFingerprint = BackendPrivacySourceAdapter.mapDevice(
            dto = device(
                fingerprint = null,
                lastBssid = "AA:BB:CC:DD:EE:FF",
                bleJa3 = "ja3-alone",
            ),
            endpointNamespace = "https://backend.example/",
            responseSequence = 7L,
            rowIndex = 0,
            clock = clock,
        )
        val fingerprinted = BackendPrivacySourceAdapter.mapDevice(
            dto = device(fingerprint = "explicit-fp"),
            endpointNamespace = "https://backend.example/",
            responseSequence = 7L,
            rowIndex = 1,
            clock = clock,
        )

        assertNull(noFingerprint.stableSourceId)
        assertTrue(noFingerprint.observationKey.sourceRecordId.contains("ephemeral:7:0"))
        assertEquals(
            "https://backend.example/|fingerprint:explicit-fp",
            fingerprinted.stableSourceId,
        )
    }

    @Test
    fun sameRowBackendAppleListeningEvidenceNormalizesWithoutCrossRowCorrelation() {
        val clock = FakeClock()
        val appleListening = BackendPrivacySourceAdapter.mapDevice(
            dto = device(
                fingerprint = "apple",
                manufacturer = "Apple",
                deviceType = "AirPods",
                privacyKind = "REMOTE_LISTENING",
                displayLabel = "Possible listening",
                bleCompanyId = 0x004C,
            ),
            endpointNamespace = "backend",
            responseSequence = 1L,
            rowIndex = 0,
            clock = clock,
        )
        val appleOnly = BackendPrivacySourceAdapter.mapDevice(
            dto = device(
                fingerprint = "apple-only",
                manufacturer = "Apple",
                deviceType = "AirPods",
                privacyKind = "APPLE_CONTINUITY",
                displayLabel = "Nearby accessory",
                bleCompanyId = 0x004C,
            ),
            endpointNamespace = "backend",
            responseSequence = 1L,
            rowIndex = 1,
            clock = clock,
        )
        val listeningOnly = BackendPrivacySourceAdapter.mapDevice(
            dto = device(
                fingerprint = "listening-only",
                manufacturer = "Unknown",
                deviceType = "Possible Listening",
                privacyKind = "REMOTE_LISTENING",
                displayLabel = "Possible listening",
                bleCompanyId = null,
            ),
            endpointNamespace = "backend",
            responseSequence = 1L,
            rowIndex = 2,
            clock = clock,
        )

        assertEquals(PrivacyCategory.APPLE_CONTINUITY, appleListening.category)
        assertEquals(FindingSeverity.INFO, appleListening.severity)
        assertEquals(PrivacyCategory.APPLE_CONTINUITY, appleOnly.category)
        assertEquals("Nearby accessory", appleOnly.title)
        assertEquals(FindingSeverity.INFO, appleOnly.severity)
        assertTrue(reduced(appleOnly).alertEligible.isEmpty())
        assertEquals(PrivacyCategory.REMOTE_LISTENING, listeningOnly.category)
        assertEquals("Possible listening", listeningOnly.title)
    }

    @Test
    fun endpointNamespaceUsesCanonicalOriginAndEquivalentPathsDoNotRestartFetch() = runTest {
        val settings = MutableStateFlow(
            DetectionSettings.defaults().copy(
                backendUrl = " https://same.example:8443/api?q=1#status ",
            ),
        )
        var fetches = 0
        val adapter = BackendPrivacySourceAdapter(
            settings = settings,
            fetch = {
                fetches += 1
                LivePrivacyDevicesDto(devices = listOf(device(fingerprint = "one")))
            },
            clock = FakeClock(),
            scope = backgroundScope,
            pollIntervalMs = 5_000L,
        )
        runCurrent()
        assertEquals(1, fetches)
        assertEquals(
            "https://same.example:8443/|fingerprint:one",
            adapter.snapshot().findings.single().stableSourceId,
        )

        settings.value = settings.value.copy(
            backendUrl = "https://same.example:8443/a/different/path",
        )
        runCurrent()

        assertEquals(1, fetches)
        assertEquals(SourceHealthState.LIVE, adapter.snapshot().health.state)
    }

    @Test
    fun invalidReplacementClearsPriorRowsFailsWithoutFetchingAndRecoversOnValidUrl() = runTest {
        val settings = MutableStateFlow(
            DetectionSettings.defaults().copy(backendUrl = "https://valid.example/"),
        )
        var fetches = 0
        val adapter = BackendPrivacySourceAdapter(
            settings = settings,
            fetch = {
                fetches += 1
                LivePrivacyDevicesDto(devices = listOf(device(fingerprint = "row-$fetches")))
            },
            clock = FakeClock(),
            scope = backgroundScope,
        )
        runCurrent()
        assertEquals(1, adapter.snapshot().findings.size)

        settings.value = settings.value.copy(backendUrl = "not a backend url")
        runCurrent()

        assertEquals(1, fetches)
        assertEquals(SourceHealthState.FAILED, adapter.snapshot().health.state)
        assertTrue(adapter.snapshot().findings.isEmpty())

        settings.value = settings.value.copy(backendUrl = "https://replacement.example/api")
        runCurrent()

        assertEquals(2, fetches)
        assertEquals(SourceHealthState.LIVE, adapter.snapshot().health.state)
        assertTrue(adapter.snapshot().findings.single().stableSourceId.orEmpty()
            .startsWith("https://replacement.example/|"))
    }

    private fun BackendPrivacySourceAdapter.snapshot(): PrivacySourceSnapshot = snapshots.value.single()

    private fun reduced(finding: PrivacyFinding) = PrivacyCurrentReducer().reduce(
        sources = listOf(
            PrivacySourceSnapshot(
                health = PrivacySourceHealth(
                    source = finding.source,
                    state = SourceHealthState.LIVE,
                    lastSuccessElapsedMs = finding.lastObservedElapsedMs,
                    lastSuccessWallMs = finding.lastSeenWallMs,
                    recoveryLabel = null,
                    message = null,
                ),
                findings = listOf(finding),
                emittedAtElapsedMs = finding.lastObservedElapsedMs,
            ),
        ),
        ignoredKeys = emptySet(),
        nowElapsedMs = finding.lastObservedElapsedMs,
    )

    private fun device(
        fingerprint: String?,
        lastSeen: Double? = 99.0,
        lastBssid: String? = null,
        bleJa3: String? = null,
        manufacturer: String? = "Test maker",
        deviceType: String? = "Privacy device",
        privacyKind: String? = "TRACKER_NEAR",
        displayLabel: String? = "Nearby device",
        bleCompanyId: Int? = null,
    ) = LivePrivacyDeviceDto(
        fingerprint = fingerprint,
        deviceType = deviceType,
        manufacturer = manufacturer,
        currentRssi = -55,
        firstSeen = 90.0,
        lastSeen = lastSeen,
        lastBssid = lastBssid,
        confidence = 0.8f,
        bleJa3 = bleJa3,
        bleCompanyId = bleCompanyId,
        privacyKind = privacyKind,
        riskLevel = "high",
        displayLabel = displayLabel,
    )

    private class FakeClock(
        var elapsed: Long = 10_000L,
        var wall: Long = 100_000L,
    ) : MonotonicClock {
        override fun nowElapsedMs(): Long = elapsed
        override fun nowWallClock(): Instant = Instant.ofEpochMilli(wall)
        override fun ticks(periodMs: Long): Flow<Long> = MutableStateFlow(elapsed)
    }
}
