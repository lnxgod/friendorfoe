package com.friendorfoe.detection

import android.net.wifi.ScanResult
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.flow.collect
import kotlinx.coroutines.launch
import kotlinx.coroutines.test.StandardTestDispatcher
import kotlinx.coroutines.test.UnconfinedTestDispatcher
import kotlinx.coroutines.test.advanceTimeBy
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

@OptIn(ExperimentalCoroutinesApi::class)
class WifiScanCoordinatorTest {

    @Test
    fun collector_stays_alive_until_first_permission_grant() = runTest {
        val platform = FakeWifiScanPlatform(WifiScanReadiness.MISSING_FINE_LOCATION)
        val coordinator = WifiScanCoordinator(
            platform = platform,
            dispatcher = StandardTestDispatcher(testScheduler),
            readyIntervalMs = 30_000L,
            blockedRecheckMs = 1_000L,
            clockMillis = { testScheduler.currentTime },
        )
        val job = backgroundScope.launch(UnconfinedTestDispatcher(testScheduler)) {
            coordinator.scanResults().collect()
        }
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
    fun blocked_platform_states_never_touch_scan_apis() = runTest {
        val platform = FakeWifiScanPlatform(WifiScanReadiness.LOCATION_SERVICES_DISABLED)
        val coordinator = WifiScanCoordinator(
            platform = platform,
            dispatcher = StandardTestDispatcher(testScheduler),
            readyIntervalMs = 30_000L,
            blockedRecheckMs = 1_000L,
            clockMillis = { testScheduler.currentTime },
        )
        val job = backgroundScope.launch(UnconfinedTestDispatcher(testScheduler)) {
            coordinator.scanResults().collect()
        }
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
    fun security_exception_does_not_close_stream_and_later_scan_recovers() = runTest {
        val platform = FakeWifiScanPlatform(WifiScanReadiness.READY).apply {
            startFailure = SecurityException("permission revoked")
        }
        val coordinator = WifiScanCoordinator(
            platform = platform,
            dispatcher = StandardTestDispatcher(testScheduler),
            readyIntervalMs = 30_000L,
            blockedRecheckMs = 1_000L,
            clockMillis = { testScheduler.currentTime },
        )
        val job = backgroundScope.launch(UnconfinedTestDispatcher(testScheduler)) {
            coordinator.scanResults().collect()
        }
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
    fun receiver_security_exception_is_recoverable() = runTest {
        val platform = FakeWifiScanPlatform(WifiScanReadiness.READY)
        val coordinator = WifiScanCoordinator(
            platform = platform,
            dispatcher = StandardTestDispatcher(testScheduler),
            readyIntervalMs = 30_000L,
            blockedRecheckMs = 1_000L,
            clockMillis = { testScheduler.currentTime },
        )
        val job = backgroundScope.launch(UnconfinedTestDispatcher(testScheduler)) {
            coordinator.scanResults().collect()
        }
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
}

private class FakeWifiScanPlatform(
    var readinessState: WifiScanReadiness,
) : WifiScanPlatform {
    var startScanCalls = 0
    var cachedResultsCalls = 0
    var startFailure: SecurityException? = null
    var cachedFailure: SecurityException? = null
    private var resultsCallback: (() -> Unit)? = null

    override fun readiness(): WifiScanReadiness = readinessState

    override fun registerResultsReceiver(onResultsAvailable: () -> Unit) {
        resultsCallback = onResultsAvailable
    }

    override fun unregisterResultsReceiver() {
        resultsCallback = null
    }

    override fun startScan(): Boolean {
        startScanCalls += 1
        startFailure?.let { throw it }
        return true
    }

    override fun cachedResults(): List<ScanResult> {
        cachedResultsCalls += 1
        cachedFailure?.let { throw it }
        return emptyList()
    }

    fun dispatchResultsAvailable() {
        resultsCallback?.invoke()
    }
}
