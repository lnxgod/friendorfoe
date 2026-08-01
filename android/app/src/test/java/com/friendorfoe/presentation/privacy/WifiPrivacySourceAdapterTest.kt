package com.friendorfoe.presentation.privacy

import com.friendorfoe.data.DetectionSettings
import com.friendorfoe.data.repository.LocalDetectionPermissions
import com.friendorfoe.data.time.MonotonicClock
import com.friendorfoe.detection.GlassesDetection
import com.friendorfoe.detection.PrivacyCategory
import com.friendorfoe.detection.WifiAnomalyDetector
import com.friendorfoe.detection.WifiScanBatch
import com.friendorfoe.detection.WifiScanEvent
import com.friendorfoe.detection.WifiScanNetwork
import java.time.Instant
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.awaitCancellation
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.flow
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

@OptIn(ExperimentalCoroutinesApi::class)
class WifiPrivacySourceAdapterTest {

    @Test
    fun enabledStartupDoesNotRestampItsLoadingDeadline() = runTest {
        val clock = FakeClock(elapsed = 500L)
        val adapter = adapter(
            settings = enabledSettings(signatures = true, anomalies = false),
            permissions = permitted(),
            wifiEvents = { flow { awaitCancellation() } },
            clock = clock,
        )
        clock.elapsed = 5_000L

        runCurrent()

        assertEquals(SourceHealthState.LOADING, adapter.snapshot().health.state)
        assertEquals(500L, adapter.snapshot().emittedAtElapsedMs)
    }

    @Test
    fun pausedAndPermissionBlockedGatesNeverAcquireThePhysicalScanStream() = runTest {
        val settings = MutableStateFlow(
            DetectionSettings.defaults().copy(
                phonePrivacyScanEnabled = false,
                wifiAnomalyEnabled = false,
            ),
        )
        val permissions = MutableStateFlow(LocalDetectionPermissions.None)
        var starts = 0
        val adapter = adapter(
            settings = settings,
            permissions = permissions,
            wifiEvents = { flow { starts += 1; awaitCancellation() } },
        )
        runCurrent()

        assertEquals(SourceHealthState.PAUSED, adapter.snapshot().health.state)
        assertEquals(0, starts)

        settings.value = settings.value.copy(wifiAnomalyEnabled = true)
        runCurrent()
        assertEquals(SourceHealthState.PERMISSION_BLOCKED, adapter.snapshot().health.state)
        assertEquals(0, starts)

        permissions.value = permissions.value.copy(wifiManagerScanResults = true)
        runCurrent()
        assertEquals(SourceHealthState.LOADING, adapter.snapshot().health.state)
        assertEquals(1, starts)
    }

    @Test
    fun successfulEmptyBatchIsLiveButDoesNotPretendCachedRowsWereObservedAgain() = runTest {
        val events = MutableSharedFlow<WifiScanEvent>(extraBufferCapacity = 4)
        val adapter = adapter(
            settings = enabledSettings(signatures = true, anomalies = false),
            permissions = permitted(),
            wifiEvents = { events },
            signatureMapper = { batch ->
                if (batch.networks.isEmpty()) emptyList() else listOf(signatureDetection())
            },
        )
        runCurrent()

        events.emit(WifiScanEvent.Success(batch(id = 1L, elapsed = 1_000L, wall = 10_000L)))
        runCurrent()
        val first = adapter.snapshot().findings.single()
        assertEquals(1_000L, first.lastObservedElapsedMs)

        events.emit(
            WifiScanEvent.Success(
                batch(id = 2L, elapsed = 8_000L, wall = 17_000L, networks = emptyList()),
            ),
        )
        runCurrent()

        assertEquals(SourceHealthState.LIVE, adapter.snapshot().health.state)
        assertEquals(8_000L, adapter.snapshot().health.lastSuccessElapsedMs)
        assertEquals(first, adapter.snapshot().findings.single())
    }

    @Test
    fun failureAndUnsupportedRetainRowsAndExactSourceTimestamps() = runTest {
        val events = MutableSharedFlow<WifiScanEvent>(extraBufferCapacity = 4)
        val adapter = adapter(
            settings = enabledSettings(signatures = true, anomalies = false),
            permissions = permitted(),
            wifiEvents = { events },
            signatureMapper = { listOf(signatureDetection()) },
        )
        runCurrent()
        events.emit(WifiScanEvent.Success(batch(id = 1L, elapsed = 3_000L, wall = 30_000L)))
        runCurrent()
        val row = adapter.snapshot().findings.single()

        events.emit(WifiScanEvent.Failure("startScan rejected", 9_000L, 90_000L))
        runCurrent()
        assertEquals(SourceHealthState.FAILED, adapter.snapshot().health.state)
        assertEquals(row, adapter.snapshot().findings.single())
        assertEquals(3_000L, adapter.snapshot().health.lastSuccessElapsedMs)

        events.emit(WifiScanEvent.Unsupported("Wi-Fi scanning unavailable", 10_000L, 100_000L))
        runCurrent()
        assertEquals(SourceHealthState.UNSUPPORTED, adapter.snapshot().health.state)
        assertEquals(row, adapter.snapshot().findings.single())
    }

    @Test
    fun signatureAndAnomalyAnalysisConsumeTheSameSuccessfulBatchAndEmitOnlyWifiRows() = runTest {
        val events = MutableSharedFlow<WifiScanEvent>(extraBufferCapacity = 4)
        val signatureBatchIds = mutableListOf<Long>()
        val anomalyBatchIds = mutableListOf<Long>()
        val adapter = adapter(
            settings = enabledSettings(signatures = true, anomalies = true),
            permissions = permitted(),
            wifiEvents = { events },
            signatureMapper = {
                signatureBatchIds += it.batchId
                listOf(signatureDetection())
            },
            anomalyMapper = {
                anomalyBatchIds += it.batchId
                listOf(anomaly())
            },
        )
        runCurrent()

        events.emit(WifiScanEvent.Success(batch(id = 77L, elapsed = 4_000L, wall = 40_000L)))
        runCurrent()

        assertEquals(listOf(77L), signatureBatchIds)
        assertEquals(listOf(77L), anomalyBatchIds)
        assertEquals(2, adapter.snapshot().findings.size)
        assertTrue(adapter.snapshot().findings.all { it.source == PrivacySourceKind.WIFI_ANALYSIS })
        assertTrue(adapter.snapshot().findings.any { it.observationKey.sourceRecordId.startsWith("signature:") })
        assertTrue(adapter.snapshot().findings.any { it.observationKey.sourceRecordId.startsWith("anomaly:") })
    }

    @Test
    fun partialFeatureDisableDropsOnlyItsRowsWhileBroadPauseRetainsTheRest() = runTest {
        val settings = enabledSettings(signatures = true, anomalies = true)
        val events = MutableSharedFlow<WifiScanEvent>(extraBufferCapacity = 4)
        val adapter = adapter(
            settings = settings,
            permissions = permitted(),
            wifiEvents = { events },
            signatureMapper = { listOf(signatureDetection()) },
            anomalyMapper = { listOf(anomaly()) },
        )
        runCurrent()
        events.emit(WifiScanEvent.Success(batch(id = 1L, elapsed = 1_000L, wall = 10_000L)))
        runCurrent()
        assertEquals(2, adapter.snapshot().findings.size)

        settings.value = settings.value.copy(phonePrivacyScanEnabled = false)
        runCurrent()
        assertEquals(1, adapter.snapshot().findings.size)
        assertTrue(adapter.snapshot().findings.single().observationKey.sourceRecordId.startsWith("anomaly:"))

        settings.value = settings.value.copy(backendOnlyMode = true)
        runCurrent()
        assertEquals(SourceHealthState.PAUSED, adapter.snapshot().health.state)
        assertEquals(1, adapter.snapshot().findings.size)
    }

    @Test
    fun unrelatedSettingsDoNotRestartWifiAndRecoveryIsSourceExact() = runTest {
        val settings = enabledSettings(signatures = true, anomalies = false)
        var starts = 0
        val adapter = adapter(
            settings = settings,
            permissions = permitted(),
            wifiEvents = { flow { starts += 1; awaitCancellation() } },
        )
        runCurrent()
        assertEquals(1, starts)

        settings.value = settings.value.copy(adsbEnabled = !settings.value.adsbEnabled)
        runCurrent()
        assertEquals(1, starts)

        settings.value = settings.value.copy(wifiEnabled = !settings.value.wifiEnabled)
        runCurrent()
        assertEquals(SourceHealthState.LOADING, adapter.snapshot().health.state)
        assertEquals(1, starts)

        assertTrue(adapter.recover(PrivacySourceKind.BACKEND) is PrivacyRecoveryResult.SourceUnavailable)
        runCurrent()
        assertEquals(1, starts)

        assertEquals(
            PrivacyRecoveryResult.Recovered(PrivacySourceKind.WIFI_ANALYSIS),
            adapter.recover(PrivacySourceKind.WIFI_ANALYSIS),
        )
        runCurrent()
        assertEquals(2, starts)
    }

    private fun kotlinx.coroutines.test.TestScope.adapter(
        settings: MutableStateFlow<DetectionSettings>,
        permissions: MutableStateFlow<LocalDetectionPermissions>,
        wifiEvents: () -> Flow<WifiScanEvent>,
        signatureMapper: (WifiScanBatch) -> List<GlassesDetection> = { emptyList() },
        anomalyMapper: (WifiScanBatch) -> List<WifiAnomalyDetector.WifiAnomaly> = { emptyList() },
        clock: FakeClock = FakeClock(),
    ) = WifiPrivacySourceAdapter(
        settings = settings,
        permissions = permissions,
        wifiEvents = wifiEvents,
        signatureMapper = signatureMapper,
        anomalyMapper = anomalyMapper,
        clock = clock,
        scope = backgroundScope,
    )

    private fun enabledSettings(
        signatures: Boolean,
        anomalies: Boolean,
    ) = MutableStateFlow(
        DetectionSettings.defaults().copy(
            wifiEnabled = true,
            phonePrivacyScanEnabled = signatures,
            wifiAnomalyEnabled = anomalies,
            backendOnlyMode = false,
        ),
    )

    private fun permitted() = MutableStateFlow(
        LocalDetectionPermissions.None.copy(wifiManagerScanResults = true),
    )

    private fun batch(
        id: Long,
        elapsed: Long,
        wall: Long,
        networks: List<WifiScanNetwork> = listOf(
            WifiScanNetwork("LookCam_AB12", "AA:BB:CC:00:00:01", "[ESS]", -48, 2_437),
        ),
    ) = WifiScanBatch(
        batchId = id,
        networks = networks,
        observedElapsedMs = elapsed,
        observedWallMs = wall,
    )

    private fun signatureDetection() = GlassesDetection(
        mac = "AA:BB:CC:00:00:01",
        deviceName = "LookCam_AB12",
        deviceType = "Hidden Camera",
        manufacturer = "LookCam",
        hasCamera = true,
        rssi = -48,
        confidence = 0.85f,
        matchReason = "Wi-Fi SSID signature",
        firstSeen = Instant.ofEpochMilli(10_000L),
        lastSeen = Instant.ofEpochMilli(10_000L),
        category = PrivacyCategory.HIDDEN_CAMERA,
        fingerprintKey = "mac:AA:BB:CC:00:00:01",
    )

    private fun anomaly() = WifiAnomalyDetector.WifiAnomaly(
        type = "evil_twin",
        ssid = "CafeWiFi",
        details = "Mixed OPEN and WPA2 security",
        threatLevel = 3,
        bssids = listOf("00:11:22:33:44:55", "66:77:88:99:AA:BB"),
        timestamp = Instant.ofEpochMilli(40_000L),
    )

    private fun WifiPrivacySourceAdapter.snapshot(): PrivacySourceSnapshot = snapshots.value.single()

    private class FakeClock(
        var elapsed: Long = 500L,
    ) : MonotonicClock {
        override fun nowElapsedMs(): Long = elapsed
        override fun nowWallClock(): Instant = Instant.ofEpochMilli(5_000L)
        override fun ticks(periodMs: Long): Flow<Long> = MutableStateFlow(nowElapsedMs())
    }
}
