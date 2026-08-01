package com.friendorfoe.presentation

import android.net.wifi.ScanResult
import com.friendorfoe.data.repository.RuntimePermissionChangeNotifier
import com.friendorfoe.data.time.MonotonicClock
import com.friendorfoe.detection.WifiScanCoordinator
import com.friendorfoe.detection.WifiScanPlatform
import com.friendorfoe.detection.WifiScanReadiness
import java.time.Instant
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.collect
import kotlinx.coroutines.launch
import kotlinx.coroutines.test.UnconfinedTestDispatcher
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Test

@OptIn(ExperimentalCoroutinesApi::class)
class MainActivityWifiRecoveryContractTest {

    @Test
    fun runtime_platform_change_notifies_detection_and_wakes_wifi_scanning() = runTest {
        val platform = ResumeWifiPlatform(WifiScanReadiness.MISSING_FINE_LOCATION)
        val coordinator = WifiScanCoordinator(
            platform = platform,
            clock = ResumeClock,
            scope = backgroundScope,
            readyIntervalMs = 30_000L,
            blockedRecheckMs = 30_000L,
        )
        val receivedResults = mutableListOf<List<ScanResult>>()
        val collector = backgroundScope.launch(UnconfinedTestDispatcher(testScheduler)) {
            coordinator.scanResults().collect(receivedResults::add)
        }
        runCurrent()
        assertEquals(0, platform.startScanCalls)

        val notifier = RecordingRuntimePermissionChangeNotifier()
        platform.readinessState = WifiScanReadiness.READY

        notifyRuntimePlatformStateChanged(notifier, coordinator)
        runCurrent()

        assertEquals(1, notifier.calls)
        assertEquals(1, platform.startScanCalls)
        assertEquals(1, platform.cachedResultsCalls)
        assertEquals(listOf(emptyList<ScanResult>()), receivedResults)
        collector.cancel()
    }
}

private class RecordingRuntimePermissionChangeNotifier : RuntimePermissionChangeNotifier {
    var calls: Int = 0

    override fun onRuntimePermissionsChanged() {
        calls += 1
    }
}

private class ResumeWifiPlatform(
    var readinessState: WifiScanReadiness,
) : WifiScanPlatform {
    var startScanCalls: Int = 0
    var cachedResultsCalls: Int = 0

    override fun readiness(): WifiScanReadiness = readinessState

    override fun registerResultsReceiver(onResultsAvailable: () -> Unit) = Unit

    override fun unregisterResultsReceiver() = Unit

    override fun startScan(): Boolean {
        startScanCalls += 1
        return true
    }

    override fun cachedResults(): List<ScanResult> {
        cachedResultsCalls += 1
        return emptyList()
    }
}

private data object ResumeClock : MonotonicClock {
    override fun nowElapsedMs(): Long = 1_000L
    override fun nowWallClock(): Instant = Instant.EPOCH
    override fun ticks(periodMs: Long): Flow<Long> = MutableStateFlow(nowElapsedMs())
}
