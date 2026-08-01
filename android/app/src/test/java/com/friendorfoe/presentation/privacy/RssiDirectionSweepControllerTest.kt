package com.friendorfoe.presentation.privacy

import com.friendorfoe.detection.PrivacyCategory
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertSame
import org.junit.Assert.assertTrue
import org.junit.Test

@OptIn(ExperimentalCoroutinesApi::class)
class RssiDirectionSweepControllerTest {
    @Test
    fun cancelStopsSamplingAndNeverReportsLocated() = runTest {
        val samples = MutableSharedFlow<RssiSample>(extraBufferCapacity = 16)
        val finding = phoneFinding(canTrack = true)
        val controller = RssiDirectionSweepController(
            sampleSource = RssiSampleSource { samples },
            scope = backgroundScope,
        )

        controller.start(finding)
        runCurrent()
        samples.emit(sample(finding.observationKey, -55, 45f))
        controller.cancel()
        samples.emit(sample(finding.observationKey, -20, 90f))
        runCurrent()

        assertEquals(DirectionSweepState.Idle, controller.state.value)
        assertFalse(controller.resultText.value.contains("located", ignoreCase = true))
    }

    @Test
    fun finishNeedsEnoughTargetBoundSamples() = runTest {
        val samples = MutableSharedFlow<RssiSample>(extraBufferCapacity = 16)
        val finding = phoneFinding(canTrack = true)
        val controller = RssiDirectionSweepController(
            RssiSampleSource { samples },
            backgroundScope,
        )
        controller.start(finding)
        runCurrent()
        repeat(5) { samples.emit(sample(finding.observationKey, -60 + it, it * 20f)) }
        samples.emit(sample(PrivacyFindingKey(PrivacySourceKind.PHONE_BLE, "other"), -10, 90f))
        runCurrent()

        assertEquals(
            DirectionSweepState.Sampling(finding.observationKey, sampleCount = 5, currentDbm = -56),
            controller.state.value,
        )

        controller.finish()

        assertEquals(DirectionSweepState.InsufficientSamples(5), controller.state.value)
        assertTrue(controller.resultText.value.contains("Not enough samples"))
    }

    @Test
    fun completeReportsOnlyTheStrongestSectorDuringThisSweep() = runTest {
        val samples = MutableSharedFlow<RssiSample>(extraBufferCapacity = 16)
        val finding = phoneFinding(canTrack = true)
        val controller = RssiDirectionSweepController(
            RssiSampleSource { samples },
            backgroundScope,
        )
        controller.start(finding)
        runCurrent()
        listOf(
            sample(finding.observationKey, -80, 5f),
            sample(finding.observationKey, -78, 20f),
            sample(finding.observationKey, -45, 92f),
            sample(finding.observationKey, -43, 100f),
            sample(finding.observationKey, -44, 120f),
            sample(finding.observationKey, -75, 190f),
        ).forEach { samples.emit(it) }
        runCurrent()

        controller.finish()

        assertEquals(DirectionSweepState.Complete(finding.observationKey), controller.state.value)
        assertTrue(controller.resultText.value.startsWith("Strongest signal was toward 90°–134°"))
        assertTrue(controller.resultText.value.contains("does not locate the device"))
    }

    @Test
    fun unavailableFindingCannotStartASweep() = runTest {
        val controller = RssiDirectionSweepController(
            RssiSampleSource { MutableSharedFlow() },
            backgroundScope,
        )

        controller.start(phoneFinding(canTrack = false))

        assertEquals(DirectionSweepState.Idle, controller.state.value)
    }

    @Test
    fun replacingASweepCannotLetTheCancelledCollectorCompleteTheNewTarget() = runTest {
        val firstSamples = MutableSharedFlow<RssiSample>(extraBufferCapacity = 16)
        val secondSamples = MutableSharedFlow<RssiSample>(extraBufferCapacity = 16)
        val first = phoneFinding(canTrack = true, id = "first")
        val second = phoneFinding(canTrack = true, id = "second")
        val controller = RssiDirectionSweepController(
            RssiSampleSource { key ->
                if (key == first.observationKey) firstSamples else secondSamples
            },
            backgroundScope,
        )
        controller.start(first)
        runCurrent()
        repeat(6) { firstSamples.emit(sample(first.observationKey, -50, 90f)) }

        controller.start(second)
        runCurrent()

        assertEquals(DirectionSweepState.Sampling(second.observationKey), controller.state.value)
        assertEquals("", controller.resultText.value)
    }

    @Test
    fun clickTimeResolutionRejectsAnExpiredOrPausedRenderedRow() {
        val rendered = phoneFinding(canTrack = true)
        val staleCurrent = rendered.copy(
            freshness = FindingFreshness.STALE,
            capabilities = PrivacyCapabilities(),
        )
        val state = PrivacyCurrentState(
            sources = emptyList(),
            findings = listOf(staleCurrent),
            threatCount = 0,
            alertEligible = emptyList(),
        )

        assertNull(resolveDirectionSweepTarget(state, rendered.observationKey))
    }

    @Test
    fun clickTimeResolutionReturnsOnlyTheExactCurrentLiveRow() {
        val current = phoneFinding(canTrack = true)
        val state = PrivacyCurrentState(
            sources = emptyList(),
            findings = listOf(current, phoneFinding(canTrack = true, id = "other")),
            threatCount = 0,
            alertEligible = emptyList(),
        )

        assertSame(current, resolveDirectionSweepTarget(state, current.observationKey))
    }

    private fun sample(
        key: PrivacyFindingKey,
        dbm: Int,
        azimuth: Float,
    ) = RssiSample(key, dbm, azimuth, observedAtElapsedMs = 1_000L)

    private fun phoneFinding(canTrack: Boolean, id: String = "phone") = PrivacyFinding(
        displayId = id,
        observationKey = PrivacyFindingKey(PrivacySourceKind.PHONE_BLE, "observation:$id"),
        source = PrivacySourceKind.PHONE_BLE,
        stableSourceId = "fp:$id",
        routableKey = PrivacyFindingKey(PrivacySourceKind.PHONE_BLE, "mac:$id"),
        title = "BLE tracker",
        evidence = null,
        limitation = null,
        category = PrivacyCategory.BLE_TRACKER,
        severity = FindingSeverity.NEARBY,
        ownership = Ownership.UNKNOWN,
        signalDbm = -60,
        firstSeenWallMs = null,
        lastSeenWallMs = null,
        lastObservedElapsedMs = 1_000L,
        protocolTtlMs = null,
        hasLiveLocalSamples = true,
        capabilities = PrivacyCapabilities(
            canIgnore = true,
            canTrack = canTrack,
            canOpenDirectionSweep = canTrack,
        ),
    )
}
