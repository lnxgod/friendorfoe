package com.friendorfoe.detection

import android.net.wifi.ScanResult
import com.friendorfoe.data.time.MonotonicClock
import java.time.Instant
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.collect
import kotlinx.coroutines.launch
import kotlinx.coroutines.test.TestScope
import kotlinx.coroutines.test.UnconfinedTestDispatcher
import kotlinx.coroutines.test.advanceTimeBy
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

@OptIn(ExperimentalCoroutinesApi::class)
class WifiScanCoordinatorTest {

    @Test
    fun multipleConsumersShareOneReceiverAndOnePhysicalScanJob() = runTest {
        val platform = FakeWifiScanPlatform()
        val coordinator = coordinator(platform)
        val firstEvents = mutableListOf<WifiScanEvent>()
        val secondEvents = mutableListOf<WifiScanEvent>()

        val first = collectEvents(coordinator, firstEvents)
        runCurrent()
        val second = collectEvents(coordinator, secondEvents)
        runCurrent()

        assertEquals(1, platform.registerCount)
        assertEquals(1, platform.startScanCalls)

        platform.dispatchResultsAvailable()
        runCurrent()
        assertEquals(1, firstEvents.filterIsInstance<WifiScanEvent.Success>().size)
        assertEquals(1, secondEvents.filterIsInstance<WifiScanEvent.Success>().size)

        first.cancel()
        runCurrent()
        assertEquals(0, platform.unregisterCount)
        second.cancel()
        runCurrent()
        assertEquals(1, platform.unregisterCount)
    }

    @Test
    fun resultBroadcastPublishesEvenEmptyAndTypedEventIsNeverReplayed() = runTest {
        val platform = FakeWifiScanPlatform()
        val coordinator = coordinator(platform)
        val firstEvents = mutableListOf<WifiScanEvent>()
        val first = collectEvents(coordinator, firstEvents)
        runCurrent()

        platform.dispatchResultsAvailable()
        runCurrent()
        val success = firstEvents.single() as WifiScanEvent.Success
        assertTrue(success.batch.networks.isEmpty())
        assertEquals(success.batch, coordinator.currentBatch.value)

        val lateEvents = mutableListOf<WifiScanEvent>()
        val late = collectEvents(coordinator, lateEvents)
        runCurrent()
        assertTrue(lateEvents.isEmpty())

        first.cancel()
        late.cancel()
    }

    @Test
    fun failedResultReadRetainsLastSuccessfulBatch() = runTest {
        val platform = FakeWifiScanPlatform()
        val coordinator = coordinator(platform)
        val events = mutableListOf<WifiScanEvent>()
        val collector = collectEvents(coordinator, events)
        runCurrent()

        platform.dispatchResultsAvailable()
        runCurrent()
        val successfulBatch = coordinator.currentBatch.value

        platform.cachedFailure = SecurityException("results revoked")
        platform.dispatchResultsAvailable()
        runCurrent()

        assertEquals(successfulBatch, coordinator.currentBatch.value)
        assertEquals(1, events.filterIsInstance<WifiScanEvent.Success>().size)
        assertEquals(1, events.filterIsInstance<WifiScanEvent.Failure>().size)
        assertEquals(WifiScanReadiness.TRANSIENT_FAILURE, coordinator.readiness.value)
        collector.cancel()
    }

    @Test
    fun rejectedStartIsFailureAndMissingPermissionCanRecoverWithoutResubscribe() = runTest {
        val rejectedPlatform = FakeWifiScanPlatform(startAccepted = false)
        val rejected = coordinator(rejectedPlatform)
        val rejectedEvents = mutableListOf<WifiScanEvent>()
        val rejectedCollector = collectEvents(rejected, rejectedEvents)
        runCurrent()
        assertEquals(1, rejectedEvents.filterIsInstance<WifiScanEvent.Failure>().size)
        assertTrue(rejectedCollector.isActive)
        rejectedCollector.cancel()
        runCurrent()

        val blockedPlatform = FakeWifiScanPlatform(
            readinessState = WifiScanReadiness.MISSING_FINE_LOCATION,
        )
        val blocked = coordinator(blockedPlatform)
        val blockedEvents = mutableListOf<WifiScanEvent>()
        val blockedCollector = collectEvents(blocked, blockedEvents)
        runCurrent()

        assertEquals(1, blockedEvents.filterIsInstance<WifiScanEvent.Unsupported>().size)
        assertEquals(1, blockedPlatform.registerCount)
        assertEquals(0, blockedPlatform.startScanCalls)
        assertNull(blocked.currentBatch.value)
        assertTrue(blockedCollector.isActive)

        blockedPlatform.readinessState = WifiScanReadiness.READY
        blocked.notifyPlatformStateChanged()
        runCurrent()

        assertEquals(1, blockedPlatform.startScanCalls)
        assertEquals(1, blockedPlatform.cachedResultsCalls)
        assertTrue(blockedCollector.isActive)
        blockedCollector.cancel()
    }

    @Test
    fun collectorStaysAliveUntilFirstPermissionGrant() = runTest {
        val platform = FakeWifiScanPlatform(WifiScanReadiness.MISSING_FINE_LOCATION)
        val coordinator = coordinator(platform)
        val job = collectRawResults(coordinator)
        runCurrent()

        assertTrue(job.isActive)
        assertEquals(0, platform.startScanCalls)
        assertEquals(0, platform.cachedResultsCalls)

        platform.readinessState = WifiScanReadiness.READY
        coordinator.notifyPlatformStateChanged()
        runCurrent()

        assertEquals(1, platform.startScanCalls)
        assertEquals(1, platform.cachedResultsCalls)
        assertTrue(job.isActive)
    }

    @Test
    fun blockedPlatformStatesNeverTouchScanApis() = runTest {
        val platform = FakeWifiScanPlatform(WifiScanReadiness.LOCATION_SERVICES_DISABLED)
        val coordinator = coordinator(platform)
        val job = collectRawResults(coordinator)
        runCurrent()
        advanceTimeBy(1_001L)
        runCurrent()

        assertEquals(0, platform.startScanCalls)
        assertEquals(0, platform.cachedResultsCalls)
        assertEquals(WifiScanReadiness.LOCATION_SERVICES_DISABLED, coordinator.readiness.value)

        platform.readinessState = WifiScanReadiness.WIFI_DISABLED
        coordinator.notifyPlatformStateChanged()
        runCurrent()

        assertEquals(0, platform.startScanCalls)
        assertEquals(0, platform.cachedResultsCalls)
        assertEquals(WifiScanReadiness.WIFI_DISABLED, coordinator.readiness.value)
        assertTrue(job.isActive)
    }

    @Test
    fun securityExceptionDoesNotCloseStreamAndLaterScanRecovers() = runTest {
        val platform = FakeWifiScanPlatform().apply {
            startFailure = SecurityException("permission revoked")
        }
        val coordinator = coordinator(platform)
        val job = collectRawResults(coordinator)
        runCurrent()

        assertEquals(WifiScanReadiness.TRANSIENT_FAILURE, coordinator.readiness.value)
        assertTrue(job.isActive)

        platform.startFailure = null
        coordinator.notifyPlatformStateChanged()
        runCurrent()

        assertEquals(WifiScanReadiness.READY, coordinator.readiness.value)
        assertEquals(2, platform.startScanCalls)
        assertEquals(1, platform.cachedResultsCalls)
        assertTrue(job.isActive)
    }

    @Test
    fun receiverSecurityExceptionIsRecoverable() = runTest {
        val platform = FakeWifiScanPlatform()
        val coordinator = coordinator(platform)
        val job = collectRawResults(coordinator)
        runCurrent()

        platform.cachedFailure = SecurityException("results revoked")
        platform.dispatchResultsAvailable()
        runCurrent()

        assertEquals(WifiScanReadiness.TRANSIENT_FAILURE, coordinator.readiness.value)
        assertTrue(job.isActive)

        platform.cachedFailure = null
        platform.dispatchResultsAvailable()
        runCurrent()

        assertEquals(WifiScanReadiness.READY, coordinator.readiness.value)
        assertTrue(job.isActive)
    }

    private fun TestScope.coordinator(
        platform: FakeWifiScanPlatform,
    ): WifiScanCoordinator = WifiScanCoordinator(
        platform = platform,
        clock = FakeClock(),
        scope = backgroundScope,
        readyIntervalMs = 30_000L,
        blockedRecheckMs = 1_000L,
    )

    private fun TestScope.collectEvents(
        coordinator: WifiScanCoordinator,
        sink: MutableList<WifiScanEvent>,
    ): Job = backgroundScope.launch(UnconfinedTestDispatcher(testScheduler)) {
        coordinator.scanEvents().collect(sink::add)
    }

    private fun TestScope.collectRawResults(coordinator: WifiScanCoordinator): Job =
        backgroundScope.launch(UnconfinedTestDispatcher(testScheduler)) {
            coordinator.scanResults().collect()
        }

    private class FakeClock : MonotonicClock {
        var elapsed = 1_000L
        var wall = 10_000L

        override fun nowElapsedMs(): Long = elapsed++
        override fun nowWallClock(): Instant = Instant.ofEpochMilli(wall++)
        override fun ticks(periodMs: Long): Flow<Long> = MutableStateFlow(elapsed)
    }
}

private class FakeWifiScanPlatform(
    var readinessState: WifiScanReadiness = WifiScanReadiness.READY,
    var startAccepted: Boolean = true,
) : WifiScanPlatform {
    var registerCount = 0
    var unregisterCount = 0
    var startScanCalls = 0
    var cachedResultsCalls = 0
    var startFailure: SecurityException? = null
    var cachedFailure: SecurityException? = null
    var results: List<ScanResult> = emptyList()
    private var resultsCallback: (() -> Unit)? = null

    override fun readiness(): WifiScanReadiness = readinessState

    override fun registerResultsReceiver(onResultsAvailable: () -> Unit) {
        registerCount += 1
        resultsCallback = onResultsAvailable
    }

    override fun unregisterResultsReceiver() {
        unregisterCount += 1
        resultsCallback = null
    }

    override fun startScan(): Boolean {
        startScanCalls += 1
        startFailure?.let { throw it }
        return startAccepted
    }

    override fun cachedResults(): List<ScanResult> {
        cachedResultsCalls += 1
        cachedFailure?.let { throw it }
        return results
    }

    fun dispatchResultsAvailable() {
        resultsCallback?.invoke()
    }
}
