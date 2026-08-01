package com.friendorfoe.detection

import com.friendorfoe.data.time.MonotonicClock
import java.time.Instant
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.take
import kotlinx.coroutines.flow.toList
import kotlinx.coroutines.test.StandardTestDispatcher
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

@OptIn(ExperimentalCoroutinesApi::class)
class UltrasonicDetectorTest {

    @Test
    fun negativeAudioReadEmitsFailureAndReleasesCapture() = runTest {
        val clock = FakeClock()
        val platform = FakeAudioPlatform(
            clock = clock,
            reads = ArrayDeque(listOf(ReadStep(-3, elapsedMs = 100L))),
        )
        val detector = UltrasonicDetector(
            audioPlatform = platform,
            clock = clock,
            dispatcher = StandardTestDispatcher(testScheduler),
        )

        val events = detector.monitoringEvents().take(2).toList()

        assertEquals(UltrasonicScanEvent.Ready, events.first())
        assertTrue(events.last() is UltrasonicScanEvent.Failure)
        assertTrue((events.last() as UltrasonicScanEvent.Failure).message.contains("-3"))
        assertTrue(platform.activeCapture.released)
    }

    @Test
    fun stalledReadNeverProducesAHeartbeatBeforeTheFollowingFailure() = runTest {
        val clock = FakeClock()
        val platform = FakeAudioPlatform(
            clock = clock,
            reads = ArrayDeque(
                listOf(
                    ReadStep(0, elapsedMs = 20_000L),
                    ReadStep(-7, elapsedMs = 20_001L),
                ),
            ),
        )
        val detector = UltrasonicDetector(
            audioPlatform = platform,
            clock = clock,
            dispatcher = StandardTestDispatcher(testScheduler),
        )

        val events = detector.monitoringEvents().take(2).toList()

        assertEquals(UltrasonicScanEvent.Ready, events.first())
        assertTrue(events.last() is UltrasonicScanEvent.Failure)
    }

    @Test
    fun securityExceptionDuringAudioReadEmitsPermissionBlocked() = runTest {
        val clock = FakeClock()
        val platform = FakeAudioPlatform(
            clock = clock,
            reads = ArrayDeque(),
            readFailure = SecurityException("permission revoked"),
        )
        val detector = UltrasonicDetector(
            audioPlatform = platform,
            clock = clock,
            dispatcher = StandardTestDispatcher(testScheduler),
        )

        val events = detector.monitoringEvents().take(2).toList()

        assertEquals(UltrasonicScanEvent.Ready, events.first())
        assertTrue(events.last() is UltrasonicScanEvent.PermissionBlocked)
    }

    @Test
    fun successfulAudioReadsPublishAHeartbeatBeforeSourceHealthCanAge() = runTest {
        val clock = FakeClock()
        val platform = FakeAudioPlatform(
            clock = clock,
            reads = ArrayDeque(
                listOf(
                    ReadStep(1, elapsedMs = 14_999L),
                    ReadStep(1, elapsedMs = 15_000L),
                ),
            ),
        )
        val detector = UltrasonicDetector(
            audioPlatform = platform,
            clock = clock,
            dispatcher = StandardTestDispatcher(testScheduler),
        )

        val events = detector.monitoringEvents().take(2).toList()

        assertEquals(
            listOf(UltrasonicScanEvent.Ready, UltrasonicScanEvent.Ready),
            events,
        )
    }

    private data class ReadStep(val result: Int, val elapsedMs: Long)

    private class FakeAudioPlatform(
        private val clock: FakeClock,
        reads: ArrayDeque<ReadStep>,
        readFailure: Throwable? = null,
    ) : UltrasonicAudioPlatform {
        private val probeCapture = FakeAudioCapture(ArrayDeque(), clock)
        val activeCapture = FakeAudioCapture(reads, clock, readFailure)
        private var createCount = 0

        override fun minimumBufferSize(
            sampleRate: Int,
            channelConfig: Int,
            audioFormat: Int,
        ): Int = 8_192

        override fun createCapture(
            audioSource: Int,
            sampleRate: Int,
            channelConfig: Int,
            audioFormat: Int,
            bufferSize: Int,
        ): UltrasonicAudioCapture = if (createCount++ == 0) probeCapture else activeCapture
    }

    private class FakeAudioCapture(
        private val reads: ArrayDeque<ReadStep>,
        private val clock: FakeClock,
        private val readFailure: Throwable? = null,
    ) : UltrasonicAudioCapture {
        override val initialized: Boolean = true
        private var started = false
        override val recording: Boolean get() = started
        var released: Boolean = false
            private set

        override fun start() {
            started = true
        }

        override fun read(buffer: ShortArray, offset: Int, length: Int): Int {
            readFailure?.let { throw it }
            val step = reads.removeFirst()
            clock.elapsed = step.elapsedMs
            return step.result
        }

        override fun stop() {
            started = false
        }

        override fun release() {
            released = true
        }
    }

    private class FakeClock(var elapsed: Long = 0L) : MonotonicClock {
        override fun nowElapsedMs(): Long = elapsed
        override fun nowWallClock(): Instant = Instant.ofEpochMilli(elapsed)
        override fun ticks(periodMs: Long): Flow<Long> = MutableStateFlow(elapsed)
    }
}
