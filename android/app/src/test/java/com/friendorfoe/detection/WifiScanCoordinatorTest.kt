package com.friendorfoe.detection

import com.friendorfoe.data.time.MonotonicClock
import java.time.Instant
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.test.UnconfinedTestDispatcher
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
        val coordinator = WifiScanCoordinator(
            platform = platform,
            clock = FakeClock(),
            scope = backgroundScope,
            scanIntervalMs = 30_000L,
        )
        val firstEvents = mutableListOf<WifiScanEvent>()
        val secondEvents = mutableListOf<WifiScanEvent>()

        val first = collect(coordinator, firstEvents)
        runCurrent()
        val second = collect(coordinator, secondEvents)
        runCurrent()

        assertEquals(1, platform.registerCount)
        assertEquals(1, platform.startScanCount)

        platform.deliver(updated = true, networks = listOf(network("one")))
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
    fun successfulUpdatedBroadcastPublishesEvenEmptyAndIsNeverReplayed() = runTest {
        val platform = FakeWifiScanPlatform()
        val coordinator = WifiScanCoordinator(platform, FakeClock(), backgroundScope, 30_000L)
        val firstEvents = mutableListOf<WifiScanEvent>()
        val first = collect(coordinator, firstEvents)
        runCurrent()

        platform.deliver(updated = true, networks = emptyList())
        runCurrent()
        val success = firstEvents.single() as WifiScanEvent.Success
        assertTrue(success.batch.networks.isEmpty())
        assertEquals(success.batch, coordinator.currentBatch.value)

        val lateEvents = mutableListOf<WifiScanEvent>()
        val late = collect(coordinator, lateEvents)
        runCurrent()
        assertTrue(lateEvents.isEmpty())

        first.cancel()
        late.cancel()
    }

    @Test
    fun failedBroadcastRetainsLastSuccessfulBatchAndNeverPublishesCachedSuccess() = runTest {
        val platform = FakeWifiScanPlatform()
        val coordinator = WifiScanCoordinator(platform, FakeClock(), backgroundScope, 30_000L)
        val events = mutableListOf<WifiScanEvent>()
        val collector = collect(coordinator, events)
        runCurrent()
        platform.deliver(updated = true, networks = listOf(network("real")))
        runCurrent()
        val successfulBatch = coordinator.currentBatch.value

        platform.deliver(updated = false, networks = listOf(network("cached")))
        runCurrent()

        assertEquals(successfulBatch, coordinator.currentBatch.value)
        assertEquals(1, events.filterIsInstance<WifiScanEvent.Success>().size)
        assertEquals(1, events.filterIsInstance<WifiScanEvent.Failure>().size)
        collector.cancel()
    }

    @Test
    fun rejectedStartScanIsFailureAndMissingPermissionIsUnsupportedWithoutRegistration() = runTest {
        val rejectedPlatform = FakeWifiScanPlatform(startAccepted = false)
        val rejected = WifiScanCoordinator(rejectedPlatform, FakeClock(), backgroundScope, 30_000L)
        val rejectedEvents = mutableListOf<WifiScanEvent>()
        val rejectedCollector = collect(rejected, rejectedEvents)
        runCurrent()
        assertEquals(1, rejectedEvents.filterIsInstance<WifiScanEvent.Failure>().size)
        rejectedCollector.cancel()

        val blockedPlatform = FakeWifiScanPlatform(permitted = false)
        val blocked = WifiScanCoordinator(blockedPlatform, FakeClock(), backgroundScope, 30_000L)
        val blockedEvents = mutableListOf<WifiScanEvent>()
        val blockedCollector = collect(blocked, blockedEvents)
        runCurrent()

        assertEquals(1, blockedEvents.filterIsInstance<WifiScanEvent.Unsupported>().size)
        assertEquals(0, blockedPlatform.registerCount)
        assertEquals(0, blockedPlatform.startScanCount)
        assertNull(blocked.currentBatch.value)
        blockedCollector.cancel()
    }

    private fun kotlinx.coroutines.test.TestScope.collect(
        coordinator: WifiScanCoordinator,
        sink: MutableList<WifiScanEvent>,
    ): Job = backgroundScope.launch(UnconfinedTestDispatcher(testScheduler)) {
        coordinator.scanEvents().collect(sink::add)
    }

    private fun network(suffix: String) = WifiScanNetwork(
        ssid = "ssid-$suffix",
        bssid = "AA:BB:CC:DD:EE:$suffix",
        capabilities = "[ESS]",
        rssi = -50,
        frequencyMhz = 2_437,
    )

    private class FakeClock : MonotonicClock {
        var elapsed = 1_000L
        var wall = 10_000L
        override fun nowElapsedMs(): Long = elapsed++
        override fun nowWallClock(): Instant = Instant.ofEpochMilli(wall++)
        override fun ticks(periodMs: Long): Flow<Long> = MutableStateFlow(elapsed)
    }

    private class FakeWifiScanPlatform(
        private val permitted: Boolean = true,
        private val startAccepted: Boolean = true,
    ) : WifiScanPlatform {
        var registerCount = 0
        var unregisterCount = 0
        var startScanCount = 0
        private var callback: ((Boolean) -> Unit)? = null
        private var networks = emptyList<WifiScanNetwork>()

        override fun hasRequiredPermission(): Boolean = permitted

        override fun registerResultsListener(listener: (Boolean) -> Unit) {
            registerCount += 1
            callback = listener
        }

        override fun unregisterResultsListener() {
            unregisterCount += 1
            callback = null
        }

        override fun startScan(): Boolean {
            startScanCount += 1
            return startAccepted
        }

        override fun readLatest(): WifiScanReading = WifiScanReading(networks = networks)

        fun deliver(updated: Boolean, networks: List<WifiScanNetwork>) {
            this.networks = networks
            callback?.invoke(updated)
        }
    }
}
