package com.friendorfoe.presentation.badge

import com.friendorfoe.data.badge.BadgeBleControlStatus
import com.friendorfoe.data.badge.BadgeCapability
import com.friendorfoe.data.badge.BadgeCapabilitySupport
import com.friendorfoe.data.badge.BadgeCommand
import com.friendorfoe.data.badge.BadgeCommandOutcome
import com.friendorfoe.data.badge.BadgeConfigReadback
import com.friendorfoe.data.badge.BadgeConnectionEvidence
import com.friendorfoe.data.badge.BadgeConnectionPhase
import com.friendorfoe.data.badge.BadgeControlAcknowledgement
import com.friendorfoe.data.badge.BadgeControlPort
import com.friendorfoe.data.badge.BadgeControlStatus
import com.friendorfoe.data.badge.BadgeDebugBridgeEvidence
import com.friendorfoe.data.badge.BadgeDisplayAction
import com.friendorfoe.data.badge.BadgeDisplayPolicy
import com.friendorfoe.data.badge.BadgeDisplayState
import com.friendorfoe.data.badge.BadgeNetworkMode
import com.friendorfoe.data.badge.BadgeNetworkModeReadback
import com.friendorfoe.data.badge.BadgeReportingStatus
import com.friendorfoe.data.badge.BadgeRepositoryState
import com.friendorfoe.data.badge.BadgeRuntimeNetworkMode
import com.friendorfoe.data.badge.BadgeScannerStatus
import com.friendorfoe.data.badge.BadgeTheme
import com.friendorfoe.data.badge.BadgeThreatCounts
import com.friendorfoe.data.badge.BadgeTransport
import com.friendorfoe.data.badge.BadgeUsbDetection
import com.friendorfoe.data.badge.firmwareHash
import com.friendorfoe.data.time.MonotonicClock
import java.time.Instant
import java.util.ArrayDeque
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.async
import kotlinx.coroutines.awaitAll
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.emptyFlow
import kotlinx.coroutines.test.StandardTestDispatcher
import kotlinx.coroutines.test.advanceTimeBy
import kotlinx.coroutines.test.advanceUntilIdle
import kotlinx.coroutines.test.resetMain
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import kotlinx.coroutines.test.setMain
import kotlinx.coroutines.withContext
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test

@OptIn(ExperimentalCoroutinesApi::class)
class BadgeViewModelTest {
    private val dispatcher = StandardTestDispatcher()
    private val testClock = FakeMonotonicClock(1_000)

    @Before
    fun setUp() {
        Dispatchers.setMain(dispatcher)
    }

    @After
    fun tearDown() {
        Dispatchers.resetMain()
    }

    @Test
    fun fallbackObjectsNeverEnableApply() = runTest(dispatcher) {
        val viewModel = BadgeViewModel(FakeBadgeControlPort(statusWithUnknownConfig()), testClock)
        runCurrent()

        assertNull(viewModel.uiState.value.draftTheme)
        assertNull(viewModel.uiState.value.draftPolicy)
        assertNull(viewModel.uiState.value.draftNetworkMode)
        assertFalse(viewModel.uiState.value.canApply)
    }

    @Test
    fun validThemeRemainsIndependentlyEditableWhenOtherReadbacksAreUnknown() =
        runTest(dispatcher) {
            val theme = BadgeTheme.firmwareDefaults()
            val mixed = badgeStatus(
                receivedAt = 1_000,
                themeReadback = BadgeConfigReadback(theme, theme.firmwareHash(), null),
                policyReadback = BadgeConfigReadback(null, null, "unknown policy"),
                networkReadback = BadgeNetworkModeReadback(null, "unknown network"),
            )
            val viewModel = BadgeViewModel(FakeBadgeControlPort(mixed), testClock)
            runCurrent()

            assertEquals(theme, viewModel.uiState.value.draftTheme)
            assertNull(viewModel.uiState.value.draftPolicy)
            assertNull(viewModel.uiState.value.draftNetworkMode)
            viewModel.updateTheme { it.copy(intensity = 70) }
            assertTrue(viewModel.uiState.value.canApply)
        }

    @Test
    fun onlyLiveStatusWhoseReceiptMatchesConnectionCanInitializeDrafts() = runTest(dispatcher) {
        val staleConnection = certifiedUsbConnection(
            phase = BadgeConnectionPhase.STALE,
            receivedAt = 1_000,
        )
        val port = FakeBadgeControlPort(statusWithDefaultConfig(), staleConnection)
        val viewModel = BadgeViewModel(port, testClock)
        runCurrent()
        assertNull(viewModel.uiState.value.draftTheme)

        port.emit(
            statusWithDefaultConfig(receivedAt = 1_001),
            certifiedUsbConnection(receivedAt = 1_000),
        )
        runCurrent()
        assertNull(viewModel.uiState.value.draftTheme)

        port.emit(
            statusWithDefaultConfig(receivedAt = 1_002),
            certifiedUsbConnection(receivedAt = 1_002),
        )
        runCurrent()
        assertEquals(BadgeTheme.firmwareDefaults(), viewModel.uiState.value.draftTheme)
    }

    @Test
    fun sameIdentityInvalidConnectionEvidenceClearsPreviouslyKnownReadbacks() =
        runTest(dispatcher) {
            val port = FakeBadgeControlPort(statusWithDefaultConfig())
            val viewModel = BadgeViewModel(port, testClock)
            runCurrent()
            viewModel.updateTheme { it.copy(intensity = 70) }
            assertTrue(viewModel.uiState.value.canApply)

            port.emit(
                statusWithDefaultConfig(receivedAt = 2_000),
                certifiedUsbConnection(
                    receivedAt = 1_000,
                    phase = BadgeConnectionPhase.STALE,
                ),
            )
            runCurrent()

            assertNull(viewModel.uiState.value.controlStatus)
            assertNull(viewModel.uiState.value.appliedTheme)
            assertNull(viewModel.uiState.value.draftTheme)
            assertFalse(viewModel.uiState.value.canApply)
        }

    @Test
    fun freshReadbackDoesNotOverwriteDirtyDraftOnSameIdentity() = runTest(dispatcher) {
        val port = FakeBadgeControlPort(statusWithDefaultConfig())
        val viewModel = BadgeViewModel(port, testClock)
        runCurrent()
        viewModel.updateTheme { it.copy(intensity = 70) }

        port.emit(statusWithDefaultConfig(receivedAt = 2_000))
        runCurrent()

        assertEquals(70, viewModel.uiState.value.draftTheme!!.intensity)
    }

    @Test
    fun sameIdentityUnknownReadbackClearsUnsafeDraftAndDisablesApply() = runTest(dispatcher) {
        val port = FakeBadgeControlPort(statusWithDefaultConfig())
        val viewModel = BadgeViewModel(port, testClock)
        runCurrent()
        viewModel.updateTheme { it.copy(intensity = 70) }
        assertTrue(viewModel.uiState.value.canApply)

        port.emit(statusWithUnknownConfig(receivedAt = 2_000))
        runCurrent()

        assertNull(viewModel.uiState.value.appliedTheme)
        assertNull(viewModel.uiState.value.draftTheme)
        assertEquals(BadgeApplyPhase.CLEAN, viewModel.uiState.value.applyState.theme.phase)
        assertFalse(viewModel.uiState.value.canApply)
    }

    @Test
    fun concreteIdentityChangeClearsDraftAppliedAndProofState() = runTest(dispatcher) {
        val port = FakeBadgeControlPort(statusWithDefaultConfig())
        val viewModel = BadgeViewModel(port, testClock)
        runCurrent()
        viewModel.updateTheme { it.copy(intensity = 70) }

        port.emit(
            statusWithUnknownConfig(receivedAt = 2_000),
            certifiedUsbConnection(target = "usb-B", generation = 2, receivedAt = 2_000),
        )
        runCurrent()

        assertNull(viewModel.uiState.value.appliedTheme)
        assertNull(viewModel.uiState.value.draftTheme)
        assertTrue(viewModel.uiState.value.applyState.activeResults.isEmpty())
    }

    @Test
    fun revertAndDefaultsAreLocalUntilApply() = runTest(dispatcher) {
        val port = FakeBadgeControlPort(statusWithDefaultConfig())
        val viewModel = BadgeViewModel(port, testClock)
        runCurrent()

        viewModel.useFirmwareDefaultsInDraft()
        viewModel.revertDraft()

        assertTrue(port.commands.isEmpty())
        assertEquals(0, port.startCalls)
        assertEquals(0, port.stopCalls)
    }

    @Test
    fun defaultsPreserveEveryAppliedPriorityAndPaletteAndNeverChangeNetwork() = runTest(dispatcher) {
        val priorities = BadgeDisplayPolicy.classOrder.withIndex()
            .associate { (index, key) -> key to (index + 3) }
        val appliedPolicy = BadgeDisplayPolicy.firmwareDefaults().let { defaults ->
            defaults.copy(
                classes = defaults.classes.mapValues { (key, rule) ->
                    rule.copy(priority = priorities.getValue(key))
                },
            )
        }
        val appliedTheme = BadgeTheme.firmwareDefaults().copy(palette = "night", intensity = 70)
        val port = FakeBadgeControlPort(
            statusWithDefaultConfig(
                theme = appliedTheme,
                policy = appliedPolicy,
                networkMode = BadgeNetworkMode.BACKEND,
            ),
        )
        val viewModel = BadgeViewModel(port, testClock)
        runCurrent()

        viewModel.useFirmwareDefaultsInDraft()

        assertEquals("night", viewModel.uiState.value.draftTheme!!.palette)
        BadgeDisplayPolicy.classOrder.forEach { key ->
            assertEquals(
                priorities.getValue(key),
                viewModel.uiState.value.draftPolicy!!.classes.getValue(key).priority,
            )
        }
        assertEquals(BadgeNetworkMode.BACKEND, viewModel.uiState.value.draftNetworkMode)
    }

    @Test
    fun hashMismatchCannotBecomeVerified() = runTest(dispatcher) {
        val port = FakeBadgeControlPort(statusWithDefaultConfig())
        port.enqueue(acknowledged(themeHash = 123))
        val viewModel = BadgeViewModel(port, testClock)
        runCurrent()
        viewModel.updateTheme { it.copy(intensity = 70) }

        viewModel.applyChanges()
        runCurrent()
        port.emit(statusWithDefaultConfig(theme = BadgeTheme.firmwareDefaults().copy(intensity = 70), receivedAt = 2_000))
        advanceUntilIdle()

        assertEquals(BadgeApplyPhase.NOT_VERIFIED, viewModel.uiState.value.applyState.theme.phase)
    }

    @Test
    fun missingThemeAcknowledgementHashCanUseFreshMatchingResult() = runTest(dispatcher) {
        val changed = BadgeTheme.firmwareDefaults().copy(intensity = 70)
        val port = FakeBadgeControlPort(statusWithDefaultConfig())
        port.enqueue(acknowledged(themeHash = null))
        val viewModel = BadgeViewModel(port, testClock)
        runCurrent()
        viewModel.updateTheme { changed }

        viewModel.applyChanges()
        runCurrent()
        port.emit(statusWithDefaultConfig(theme = changed, receivedAt = 2_000))
        advanceUntilIdle()

        assertEquals(BadgeApplyPhase.VERIFIED, viewModel.uiState.value.applyState.theme.phase)
        assertEquals(changed.firmwareHash(), viewModel.uiState.value.applyState.theme.readbackHash)
    }

    @Test
    fun correctThemeAcknowledgementCannotVerifyMismatchingCanonicalReadback() =
        runTest(dispatcher) {
            val changed = BadgeTheme.firmwareDefaults().copy(intensity = 70)
            val port = FakeBadgeControlPort(statusWithDefaultConfig())
            port.enqueue(acknowledged(themeHash = changed.firmwareHash()))
            val viewModel = BadgeViewModel(port, testClock)
            runCurrent()
            viewModel.updateTheme { changed }

            viewModel.applyChanges()
            runCurrent()
            port.emit(statusWithDefaultConfig(receivedAt = 2_000))
            advanceUntilIdle()

            assertEquals(
                BadgeApplyPhase.NOT_VERIFIED,
                viewModel.uiState.value.applyState.theme.phase,
            )
            assertEquals(
                BadgeTheme.firmwareDefaults().firmwareHash(),
                viewModel.uiState.value.applyState.theme.readbackHash,
            )
            assertEquals(BadgeTheme.firmwareDefaults(), viewModel.uiState.value.appliedTheme)
            assertEquals(changed, viewModel.uiState.value.draftTheme)
            assertTrue(viewModel.uiState.value.themeDirty)
            assertTrue(viewModel.uiState.value.canApply)
        }

    @Test
    fun themeProofWaitsPastNewerMismatchForMatchingReadbackInsideDeadline() =
        runTest(dispatcher) {
            val changed = BadgeTheme.firmwareDefaults().copy(intensity = 70)
            val port = FakeBadgeControlPort(statusWithDefaultConfig())
            port.enqueue(acknowledged(themeHash = changed.firmwareHash()))
            val viewModel = BadgeViewModel(port, testClock)
            runCurrent()
            viewModel.updateTheme { changed }
            viewModel.applyChanges()
            runCurrent()

            port.emit(statusWithDefaultConfig(receivedAt = 2_000))
            runCurrent()
            assertTrue(viewModel.uiState.value.applyInFlight)

            port.emit(statusWithDefaultConfig(theme = changed, receivedAt = 3_000))
            advanceUntilIdle()

            assertEquals(BadgeApplyPhase.VERIFIED, viewModel.uiState.value.applyState.theme.phase)
        }

    @Test
    fun proofRequiresAStatusReceiptStrictlyNewerThanTheCommandBaseline() = runTest(dispatcher) {
        val changed = BadgeTheme.firmwareDefaults().copy(intensity = 70)
        val port = FakeBadgeControlPort(statusWithDefaultConfig())
        port.enqueue(acknowledged(themeHash = changed.firmwareHash()))
        val viewModel = BadgeViewModel(port, testClock)
        runCurrent()
        viewModel.updateTheme { changed }

        viewModel.applyChanges()
        runCurrent()
        port.emit(statusWithDefaultConfig(theme = changed, receivedAt = 1_000))
        advanceTimeBy(5_001)
        runCurrent()

        assertEquals(BadgeApplyPhase.NOT_VERIFIED, viewModel.uiState.value.applyState.theme.phase)
    }

    @Test
    fun zeroConnectedScannersStopsAtAppliedOnBadge() = runTest(dispatcher) {
        val changed = BadgeDisplayPolicy.firmwareDefaults().withEnabled("beacon", false)
        val port = FakeBadgeControlPort(statusWithDefaultConfig(scanners = emptyList()))
        port.enqueue(acknowledged(policyHash = changed.firmwareHash()))
        val viewModel = BadgeViewModel(port, testClock)
        runCurrent()
        viewModel.updatePolicy { changed }

        viewModel.applyChanges()
        runCurrent()
        port.emit(statusWithDefaultConfig(policy = changed, scanners = emptyList(), receivedAt = 2_000))
        advanceUntilIdle()

        assertEquals(
            BadgeApplyPhase.APPLIED_ON_BADGE,
            viewModel.uiState.value.applyState.policy.phase,
        )
    }

    @Test
    fun policyNeedsEveryConnectedScannerAcknowledgementForScannerVerification() = runTest(dispatcher) {
        val changed = BadgeDisplayPolicy.firmwareDefaults().withEnabled("beacon", false)
        val hash = changed.firmwareHash()
        val port = FakeBadgeControlPort(statusWithDefaultConfig())
        port.enqueue(acknowledged(policyHash = hash))
        val viewModel = BadgeViewModel(port, testClock)
        runCurrent()
        viewModel.updatePolicy { changed }

        viewModel.applyChanges()
        runCurrent()
        port.emit(
            statusWithDefaultConfig(
                policy = changed,
                scanners = listOf(
                    BadgeScannerStatus(slot = 0, connected = true, displayPolicyAckHash = hash),
                    BadgeScannerStatus(slot = 1, connected = true, displayPolicyAckHash = hash),
                    BadgeScannerStatus(slot = 2, connected = false, displayPolicyAckHash = 0),
                ),
                receivedAt = 2_000,
            ),
        )
        advanceUntilIdle()

        assertEquals(
            BadgeApplyPhase.VERIFIED_ON_SCANNERS,
            viewModel.uiState.value.applyState.policy.phase,
        )
    }

    @Test
    fun missingPolicyAcknowledgementHashCanUseFreshMatchingResult() = runTest(dispatcher) {
        val changed = BadgeDisplayPolicy.firmwareDefaults().withEnabled("beacon", false)
        val hash = changed.firmwareHash()
        val port = FakeBadgeControlPort(statusWithDefaultConfig())
        port.enqueue(acknowledged(policyHash = null))
        val viewModel = BadgeViewModel(port, testClock)
        runCurrent()
        viewModel.updatePolicy { changed }

        viewModel.applyChanges()
        runCurrent()
        port.emit(
            statusWithDefaultConfig(
                policy = changed,
                scanners = listOf(
                    BadgeScannerStatus(slot = 0, connected = true, displayPolicyAckHash = hash),
                ),
                receivedAt = 2_000,
            ),
        )
        advanceUntilIdle()

        assertEquals(
            BadgeApplyPhase.VERIFIED_ON_SCANNERS,
            viewModel.uiState.value.applyState.policy.phase,
        )
    }

    @Test
    fun scannerAcknowledgementsCanPromoteAWithinDeadlineBadgeReadback() = runTest(dispatcher) {
        val changed = BadgeDisplayPolicy.firmwareDefaults().withEnabled("beacon", false)
        val hash = changed.firmwareHash()
        val port = FakeBadgeControlPort(statusWithDefaultConfig())
        port.enqueue(acknowledged(policyHash = hash))
        val viewModel = BadgeViewModel(port, testClock)
        runCurrent()
        viewModel.updatePolicy { changed }

        viewModel.applyChanges()
        runCurrent()
        port.emit(
            statusWithDefaultConfig(
                policy = changed,
                scanners = listOf(
                    BadgeScannerStatus(slot = 0, connected = true, displayPolicyAckHash = 0),
                ),
                receivedAt = 2_000,
            ),
        )
        runCurrent()
        assertEquals(
            BadgeApplyPhase.APPLIED_ON_BADGE,
            viewModel.uiState.value.applyState.policy.phase,
        )

        port.emit(
            statusWithDefaultConfig(
                policy = changed,
                scanners = listOf(
                    BadgeScannerStatus(slot = 0, connected = true, displayPolicyAckHash = hash),
                ),
                receivedAt = 3_000,
            ),
        )
        advanceUntilIdle()

        assertEquals(
            BadgeApplyPhase.VERIFIED_ON_SCANNERS,
            viewModel.uiState.value.applyState.policy.phase,
        )
    }

    @Test
    fun staleConnectionCannotResurrectCachedPolicyProofAfterScannerWait() =
        runTest(dispatcher) {
            val changed = BadgeDisplayPolicy.firmwareDefaults().withEnabled("beacon", false)
            val hash = changed.firmwareHash()
            val port = FakeBadgeControlPort(statusWithDefaultConfig())
            port.enqueue(acknowledged(policyHash = hash))
            val viewModel = BadgeViewModel(port, testClock)
            runCurrent()
            viewModel.updatePolicy { changed }
            viewModel.applyChanges()
            runCurrent()
            port.emit(
                statusWithDefaultConfig(
                    policy = changed,
                    scanners = listOf(
                        BadgeScannerStatus(slot = 0, connected = true, displayPolicyAckHash = 0),
                    ),
                    receivedAt = 2_000,
                ),
            )
            runCurrent()
            assertEquals(
                BadgeApplyPhase.APPLIED_ON_BADGE,
                viewModel.uiState.value.applyState.policy.phase,
            )

            port.emit(
                statusWithDefaultConfig(policy = changed, receivedAt = 2_000),
                certifiedUsbConnection(
                    receivedAt = 2_000,
                    phase = BadgeConnectionPhase.STALE,
                ),
            )
            runCurrent()
            assertNull(viewModel.uiState.value.appliedPolicy)
            advanceTimeBy(5_001)
            runCurrent()

            assertNull(viewModel.uiState.value.appliedPolicy)
            assertNull(viewModel.uiState.value.draftPolicy)
            assertFalse(viewModel.uiState.value.canApply)
            assertTrue(
                viewModel.uiState.value.applyState.policy.phase !in setOf(
                    BadgeApplyPhase.APPLIED_ON_BADGE,
                    BadgeApplyPhase.VERIFIED_ON_SCANNERS,
                ),
            )
        }

    @Test
    fun newerContradictoryLivePolicyReconcilesAppliedTruthAndRetainsDirtyDraft() =
        runTest(dispatcher) {
            val submitted = BadgeDisplayPolicy.firmwareDefaults().withEnabled("beacon", false)
            val newerOnBadge = BadgeDisplayPolicy.firmwareDefaults().withEnabled("auracast", false)
            val submittedHash = submitted.firmwareHash()
            val port = FakeBadgeControlPort(statusWithDefaultConfig())
            port.enqueue(acknowledged(policyHash = submittedHash))
            val viewModel = BadgeViewModel(port, testClock)
            runCurrent()
            viewModel.updatePolicy { submitted }
            viewModel.applyChanges()
            runCurrent()
            port.emit(
                statusWithDefaultConfig(
                    policy = submitted,
                    scanners = listOf(
                        BadgeScannerStatus(slot = 0, connected = true, displayPolicyAckHash = 0),
                    ),
                    receivedAt = 2_000,
                ),
            )
            runCurrent()
            assertEquals(submitted, viewModel.uiState.value.appliedPolicy)

            port.emit(
                statusWithDefaultConfig(policy = newerOnBadge, receivedAt = 3_000),
            )
            advanceTimeBy(5_001)
            runCurrent()

            assertEquals(newerOnBadge, viewModel.uiState.value.appliedPolicy)
            assertEquals(submitted, viewModel.uiState.value.draftPolicy)
            assertTrue(viewModel.uiState.value.policyDirty)
            assertTrue(viewModel.uiState.value.canApply)
            assertEquals(
                BadgeApplyPhase.NOT_VERIFIED,
                viewModel.uiState.value.applyState.policy.phase,
            )
            assertEquals(
                newerOnBadge.firmwareHash(),
                viewModel.uiState.value.applyState.policy.readbackHash,
            )
        }

    @Test
    fun correctPolicyAcknowledgementCannotVerifyMismatchingCanonicalReadback() =
        runTest(dispatcher) {
            val changed = BadgeDisplayPolicy.firmwareDefaults().withEnabled("beacon", false)
            val port = FakeBadgeControlPort(statusWithDefaultConfig())
            port.enqueue(acknowledged(policyHash = changed.firmwareHash()))
            val viewModel = BadgeViewModel(port, testClock)
            runCurrent()
            viewModel.updatePolicy { changed }

            viewModel.applyChanges()
            runCurrent()
            port.emit(statusWithDefaultConfig(receivedAt = 2_000))
            advanceUntilIdle()

            assertEquals(
                BadgeApplyPhase.NOT_VERIFIED,
                viewModel.uiState.value.applyState.policy.phase,
            )
            assertEquals(
                BadgeDisplayPolicy.firmwareDefaults().firmwareHash(),
                viewModel.uiState.value.applyState.policy.readbackHash,
            )
            assertEquals(
                BadgeDisplayPolicy.firmwareDefaults(),
                viewModel.uiState.value.appliedPolicy,
            )
            assertEquals(changed, viewModel.uiState.value.draftPolicy)
            assertTrue(viewModel.uiState.value.policyDirty)
            assertTrue(viewModel.uiState.value.canApply)
        }

    @Test
    fun themeSuccessCannotHidePolicyFailureAndOrderIsStable() = runTest(dispatcher) {
        val changedTheme = BadgeTheme.firmwareDefaults().copy(intensity = 70)
        val changedPolicy = BadgeDisplayPolicy.firmwareDefaults().withEnabled("beacon", false)
        val port = FakeBadgeControlPort(statusWithDefaultConfig())
        port.enqueue(acknowledged(themeHash = changedTheme.firmwareHash()))
        port.enqueue(BadgeCommandOutcome.Failed("policy rejected"))
        val viewModel = BadgeViewModel(port, testClock)
        runCurrent()
        viewModel.updateTheme { changedTheme }
        viewModel.updatePolicy { changedPolicy }

        viewModel.applyChanges()
        runCurrent()
        port.emit(statusWithDefaultConfig(theme = changedTheme, receivedAt = 2_000))
        advanceUntilIdle()

        assertEquals(BadgeApplyPhase.VERIFIED, viewModel.uiState.value.applyState.theme.phase)
        assertEquals(BadgeApplyPhase.FAILED, viewModel.uiState.value.applyState.policy.phase)
        assertEquals(
            listOf(BadgeCommand.ApplyTheme(changedTheme), BadgeCommand.ApplyPolicy(changedPolicy)),
            port.commands,
        )
    }

    @Test
    fun themeFailureStillRunsIndependentPolicyAndPreservesBothTypedFailures() =
        runTest(dispatcher) {
            val changedTheme = BadgeTheme.firmwareDefaults().copy(intensity = 70)
            val changedPolicy = BadgeDisplayPolicy.firmwareDefaults().withEnabled("beacon", false)
            val port = FakeBadgeControlPort(statusWithDefaultConfig())
            port.enqueue(BadgeCommandOutcome.Failed("theme rejected"))
            port.enqueue(BadgeCommandOutcome.Failed("policy rejected"))
            val viewModel = BadgeViewModel(port, testClock)
            runCurrent()
            viewModel.updateTheme { changedTheme }
            viewModel.updatePolicy { changedPolicy }

            viewModel.applyChanges()
            advanceUntilIdle()

            assertEquals(
                listOf(
                    BadgeCommand.ApplyTheme(changedTheme),
                    BadgeCommand.ApplyPolicy(changedPolicy),
                ),
                port.commands,
            )
            assertEquals(BadgeApplyPhase.FAILED, viewModel.uiState.value.applyState.theme.phase)
            assertEquals("theme rejected", viewModel.uiState.value.applyState.theme.message)
            assertEquals(BadgeApplyPhase.FAILED, viewModel.uiState.value.applyState.policy.phase)
            assertEquals("policy rejected", viewModel.uiState.value.applyState.policy.message)
        }

    @Test
    fun themeFailureStillAllowsIndependentPolicyProof() = runTest(dispatcher) {
        val changedTheme = BadgeTheme.firmwareDefaults().copy(intensity = 70)
        val changedPolicy = BadgeDisplayPolicy.firmwareDefaults().withEnabled("beacon", false)
        val hash = changedPolicy.firmwareHash()
        val port = FakeBadgeControlPort(statusWithDefaultConfig())
        port.enqueue(BadgeCommandOutcome.Failed("theme rejected"))
        port.enqueue(acknowledged(policyHash = hash))
        val viewModel = BadgeViewModel(port, testClock)
        runCurrent()
        viewModel.updateTheme { changedTheme }
        viewModel.updatePolicy { changedPolicy }

        viewModel.applyChanges()
        runCurrent()
        port.emit(
            statusWithDefaultConfig(
                policy = changedPolicy,
                scanners = listOf(
                    BadgeScannerStatus(slot = 0, connected = true, displayPolicyAckHash = hash),
                ),
                receivedAt = 2_000,
            ),
        )
        advanceUntilIdle()

        assertEquals(BadgeApplyPhase.FAILED, viewModel.uiState.value.applyState.theme.phase)
        assertEquals(
            BadgeApplyPhase.VERIFIED_ON_SCANNERS,
            viewModel.uiState.value.applyState.policy.phase,
        )
    }

    @Test
    fun networkModeNeedsAcknowledgementAndMatchingReadback() = runTest(dispatcher) {
        val port = FakeBadgeControlPort(
            statusWithDefaultConfig(networkMode = BadgeNetworkMode.USB_ONLY),
        )
        port.enqueue(
            acknowledged(
                networkApplied = true,
                runtimeNetworkMode = BadgeRuntimeNetworkMode.BACKEND,
            ),
        )
        val viewModel = BadgeViewModel(port, testClock)
        runCurrent()
        viewModel.updateNetworkMode(BadgeNetworkMode.BACKEND)

        viewModel.applyChanges()
        runCurrent()
        port.emit(statusWithDefaultConfig(networkMode = BadgeNetworkMode.BACKEND, receivedAt = 2_000))
        advanceUntilIdle()

        assertEquals(BadgeApplyPhase.VERIFIED, viewModel.uiState.value.applyState.network.phase)
        assertEquals(
            listOf(BadgeCommand.SetNetworkMode(BadgeNetworkMode.BACKEND)),
            port.commands,
        )
    }

    @Test
    fun persistedUsbOnlyVerifiesAgainstRuntimeOffAndPersistedModeReadback() = runTest(dispatcher) {
        val port = FakeBadgeControlPort(
            statusWithDefaultConfig(networkMode = BadgeNetworkMode.BACKEND),
        )
        port.enqueue(
            acknowledged(
                networkApplied = true,
                runtimeNetworkMode = BadgeRuntimeNetworkMode.OFF,
            ),
        )
        val viewModel = BadgeViewModel(port, testClock)
        runCurrent()
        viewModel.updateNetworkMode(BadgeNetworkMode.USB_ONLY)

        viewModel.applyChanges()
        runCurrent()
        port.emit(statusWithDefaultConfig(networkMode = BadgeNetworkMode.USB_ONLY, receivedAt = 2_000))
        advanceUntilIdle()

        assertEquals(BadgeApplyPhase.VERIFIED, viewModel.uiState.value.applyState.network.phase)
    }

    @Test
    fun networkRuntimeMismatchIsNotVerifiedEvenWhenPersistedReadbackMatches() = runTest(dispatcher) {
        val port = FakeBadgeControlPort(statusWithDefaultConfig())
        port.enqueue(
            acknowledged(
                networkApplied = true,
                runtimeNetworkMode = BadgeRuntimeNetworkMode.LOCAL_AP,
            ),
        )
        val viewModel = BadgeViewModel(port, testClock)
        runCurrent()
        viewModel.updateNetworkMode(BadgeNetworkMode.BACKEND)

        viewModel.applyChanges()
        runCurrent()
        port.emit(statusWithDefaultConfig(networkMode = BadgeNetworkMode.BACKEND, receivedAt = 2_000))
        advanceUntilIdle()

        assertEquals(BadgeApplyPhase.NOT_VERIFIED, viewModel.uiState.value.applyState.network.phase)
    }

    @Test
    fun contradictoryFreshNetworkReadbackBecomesAppliedTruthAndRetainsSubmittedDraft() =
        runTest(dispatcher) {
            val port = FakeBadgeControlPort(statusWithDefaultConfig())
            port.enqueue(
                acknowledged(
                    networkApplied = true,
                    runtimeNetworkMode = BadgeRuntimeNetworkMode.BACKEND,
                ),
            )
            val viewModel = BadgeViewModel(port, testClock)
            runCurrent()
            viewModel.updateNetworkMode(BadgeNetworkMode.BACKEND)

            viewModel.applyChanges()
            runCurrent()
            port.emit(
                statusWithDefaultConfig(
                    networkMode = BadgeNetworkMode.LOCAL_AP,
                    receivedAt = 2_000,
                ),
            )
            advanceUntilIdle()

            assertEquals(BadgeApplyPhase.NOT_VERIFIED, viewModel.uiState.value.applyState.network.phase)
            assertEquals(BadgeNetworkMode.LOCAL_AP, viewModel.uiState.value.appliedNetworkMode)
            assertEquals(BadgeNetworkMode.BACKEND, viewModel.uiState.value.draftNetworkMode)
            assertTrue(viewModel.uiState.value.networkDirty)
            assertTrue(viewModel.uiState.value.canApply)
        }

    @Test
    fun networkProofWaitsPastNewerMismatchForMatchingReadbackInsideDeadline() =
        runTest(dispatcher) {
            val port = FakeBadgeControlPort(statusWithDefaultConfig())
            port.enqueue(
                acknowledged(
                    networkApplied = true,
                    runtimeNetworkMode = BadgeRuntimeNetworkMode.BACKEND,
                ),
            )
            val viewModel = BadgeViewModel(port, testClock)
            runCurrent()
            viewModel.updateNetworkMode(BadgeNetworkMode.BACKEND)
            viewModel.applyChanges()
            runCurrent()

            port.emit(
                statusWithDefaultConfig(
                    networkMode = BadgeNetworkMode.LOCAL_AP,
                    receivedAt = 2_000,
                ),
            )
            runCurrent()
            assertTrue(viewModel.uiState.value.applyInFlight)

            port.emit(
                statusWithDefaultConfig(
                    networkMode = BadgeNetworkMode.BACKEND,
                    receivedAt = 3_000,
                ),
            )
            advanceUntilIdle()

            assertEquals(BadgeApplyPhase.VERIFIED, viewModel.uiState.value.applyState.network.phase)
        }

    @Test
    fun displayNavigationSupportIsDerivedPerSerializedAction() = runTest(dispatcher) {
        val connection = certifiedBleConnection(mtu = 41)
        val viewModel = BadgeViewModel(
            FakeBadgeControlPort(statusWithDefaultConfig(), connection),
            testClock,
        )
        runCurrent()

        assertEquals(
            BadgeCapabilitySupport.SUPPORTED,
            viewModel.uiState.value.displayNavigationSupport.getValue(BadgeDisplayAction.NEXT),
        )
        assertEquals(
            BadgeCapabilitySupport.UNSUPPORTED,
            viewModel.uiState.value.displayNavigationSupport.getValue(BadgeDisplayAction.DETAIL),
        )
        assertEquals(
            BadgeCapabilitySupport.SUPPORTED,
            viewModel.uiState.value.displayNavigationSupport.getValue(BadgeDisplayAction.BACK),
        )
    }

    @Test
    fun unsupportedDisplayNavigationDoesNotCallPort() = runTest(dispatcher) {
        val connection = certifiedBleConnection(mtu = 41)
        val port = FakeBadgeControlPort(statusWithDefaultConfig(), connection)
        val viewModel = BadgeViewModel(port, testClock)
        runCurrent()

        viewModel.navigateDisplay(BadgeDisplayAction.DETAIL)
        runCurrent()

        assertTrue(port.commands.isEmpty())
    }

    @Test
    fun staleNavigationCompletionCannotOverwriteNewBadgeIdentity() = runTest(dispatcher) {
        val port = FakeBadgeControlPort(statusWithDefaultConfig())
        port.blockNextCommand()
        val viewModel = BadgeViewModel(port, testClock)
        runCurrent()

        viewModel.navigateDisplay(BadgeDisplayAction.NEXT)
        runCurrent()
        port.emit(
            statusWithDefaultConfig(receivedAt = 2_000),
            certifiedUsbConnection(target = "usb-B", generation = 2, receivedAt = 2_000),
        )
        port.releaseBlockedCommand(BadgeCommandOutcome.Accepted("old badge"))
        runCurrent()

        assertNull(viewModel.uiState.value.displayNavigationResult)
    }

    @Test
    fun rapidApplyCallsSendOneCommandAndDisableApplyWhilePending() = runTest(dispatcher) {
        val port = FakeBadgeControlPort(statusWithDefaultConfig())
        port.blockNextCommand()
        val viewModel = BadgeViewModel(port, testClock)
        runCurrent()
        viewModel.updateTheme { it.copy(intensity = 70) }

        viewModel.applyChanges()
        assertTrue(viewModel.uiState.value.applyInFlight)
        assertFalse(viewModel.uiState.value.canApply)
        runCurrent()
        viewModel.applyChanges()

        assertEquals(1, port.commands.size)
        port.releaseBlockedCommand(
            acknowledged(themeHash = BadgeTheme.firmwareDefaults().copy(intensity = 70).firmwareHash()),
        )
        runCurrent()
        port.emit(
            statusWithDefaultConfig(
                theme = BadgeTheme.firmwareDefaults().copy(intensity = 70),
                receivedAt = 2_000,
            ),
        )
        advanceUntilIdle()
    }

    @Test
    fun simultaneousApplyCallsAtomicallyClaimOneDraftSnapshot() = runTest(dispatcher) {
        val port = FakeBadgeControlPort(statusWithDefaultConfig())
        port.blockNextCommand()
        val viewModel = BadgeViewModel(port, testClock)
        runCurrent()
        val changed = BadgeTheme.firmwareDefaults().copy(intensity = 70)
        viewModel.updateTheme { changed }
        val start = CompletableDeferred<Unit>()

        val calls = List(64) {
            async(Dispatchers.Default) {
                start.await()
                viewModel.applyChanges()
            }
        }
        start.complete(Unit)
        calls.awaitAll()
        runCurrent()

        assertEquals(listOf(BadgeCommand.ApplyTheme(changed)), port.commands)
        port.releaseBlockedCommand(acknowledged(themeHash = changed.firmwareHash()))
        runCurrent()
        port.emit(statusWithDefaultConfig(theme = changed, receivedAt = 2_000))
        advanceUntilIdle()
    }

    @Test
    fun identitySwitchAbortsRemainingMutationsAndDropsOldCompletion() = runTest(dispatcher) {
        val port = FakeBadgeControlPort(statusWithDefaultConfig())
        port.blockNextCommand()
        val viewModel = BadgeViewModel(port, testClock)
        runCurrent()
        viewModel.updateTheme { it.copy(intensity = 70) }
        viewModel.updatePolicy { it.withEnabled("beacon", false) }

        viewModel.applyChanges()
        runCurrent()
        port.emit(
            statusWithUnknownConfig(receivedAt = 2_000),
            certifiedUsbConnection(target = "usb-B", generation = 2, receivedAt = 2_000),
        )
        port.releaseBlockedCommand(BadgeCommandOutcome.Failed("old badge"))
        runCurrent()

        assertEquals(1, port.commands.size)
        assertTrue(viewModel.uiState.value.applyState.activeResults.isEmpty())
        assertFalse(viewModel.uiState.value.applyInFlight)
    }

    @Test
    fun lossOfConcreteIdentityCancelsApplyAndClearsPendingState() = runTest(dispatcher) {
        val port = FakeBadgeControlPort(statusWithDefaultConfig())
        port.blockNextCommand()
        val viewModel = BadgeViewModel(port, testClock)
        runCurrent()
        viewModel.updateTheme { it.copy(intensity = 70) }
        viewModel.applyChanges()
        runCurrent()

        port.emit(statusWithDefaultConfig(), BadgeConnectionEvidence())
        runCurrent()

        assertFalse(viewModel.uiState.value.applyInFlight)
        assertTrue(viewModel.uiState.value.applyState.activeResults.isEmpty())
        assertEquals(1, port.cancelledCommands.size)
    }

    @Test
    fun oldTransactionFinallyCannotPreventNextIdentityTransactionCancellation() =
        runTest(dispatcher) {
            val port = FakeBadgeControlPort(statusWithDefaultConfig())
            port.blockNextCommand(ignoringCancellation = true)
            val viewModel = BadgeViewModel(port, testClock)
            runCurrent()
            viewModel.updateTheme { it.copy(intensity = 70) }
            viewModel.applyChanges()
            runCurrent()

            port.emit(
                statusWithDefaultConfig(receivedAt = 2_000),
                certifiedUsbConnection(target = "usb-B", generation = 2, receivedAt = 2_000),
            )
            runCurrent()
            port.blockNextCommand()
            viewModel.updateTheme { it.copy(intensity = 80) }
            viewModel.applyChanges()
            runCurrent()

            port.releaseBlockedCommandAt(0, BadgeCommandOutcome.Failed("old transaction"))
            runCurrent()
            port.emit(
                statusWithDefaultConfig(receivedAt = 3_000),
                certifiedUsbConnection(target = "usb-C", generation = 3, receivedAt = 3_000),
            )
            runCurrent()

            assertEquals(2, port.commands.size)
            assertTrue(
                port.cancelledCommands.contains(
                    BadgeCommand.ApplyTheme(BadgeTheme.firmwareDefaults().copy(intensity = 80)),
                ),
            )
        }

    @Test
    fun applyRefreshesAfterAcceptableOutcomeButNeverRetries() = runTest(dispatcher) {
        val port = FakeBadgeControlPort(statusWithDefaultConfig())
        port.enqueue(BadgeCommandOutcome.Accepted("transport accepted"))
        val viewModel = BadgeViewModel(port, testClock)
        runCurrent()
        viewModel.updateTheme { it.copy(intensity = 70) }

        viewModel.applyChanges()
        advanceUntilIdle()

        assertEquals(1, port.commands.size)
        assertEquals(1, port.refreshCalls)
        assertEquals(BadgeApplyPhase.NOT_VERIFIED, viewModel.uiState.value.applyState.theme.phase)
    }

    @Test
    fun recoveryRequiresExactVerifiedDirectUsbAndCancelSendsNothing() = runTest(dispatcher) {
        val apConnection = certifiedApConnection()
        val apPort = FakeBadgeControlPort(statusWithDefaultConfig(), apConnection)
        val apViewModel = BadgeViewModel(apPort, testClock)
        runCurrent()
        apViewModel.requestRecovery(BadgeRecoveryAction.REBOOT)
        apViewModel.confirmRecovery()
        runCurrent()
        assertTrue(apPort.commands.isEmpty())

        val usbPort = FakeBadgeControlPort(statusWithDefaultConfig())
        val usbViewModel = BadgeViewModel(usbPort, testClock)
        runCurrent()
        usbViewModel.requestRecovery(BadgeRecoveryAction.REBOOT)
        assertEquals(BadgeRecoveryPhase.CONFIRMING, usbViewModel.uiState.value.recovery.phase)
        usbViewModel.cancelRecovery()
        assertEquals(BadgeRecoveryPhase.IDLE, usbViewModel.uiState.value.recovery.phase)
        assertTrue(usbPort.commands.isEmpty())
    }

    @Test
    fun bleAndDebugConnectionsNeverExecuteRecoveryCommands() = runTest(dispatcher) {
        listOf(
            certifiedBleConnection(mtu = 64),
            certifiedDebugConnection(),
        ).forEach { connection ->
            val port = FakeBadgeControlPort(statusWithDefaultConfig(), connection)
            val viewModel = BadgeViewModel(port, testClock)
            runCurrent()

            BadgeRecoveryAction.entries.forEach { action ->
                assertEquals(
                    "Verified direct USB is required",
                    viewModel.uiState.value.recoveryAvailability.getValue(action).reason,
                )
                viewModel.requestRecovery(action)
                viewModel.confirmRecovery()
                runCurrent()
            }

            assertTrue(port.commands.isEmpty())
        }
    }

    @Test
    fun recoveryCapabilityRevokedBeforeConfirmationNeverExecutes() = runTest(dispatcher) {
        val port = FakeBadgeControlPort(statusWithDefaultConfig())
        val viewModel = BadgeViewModel(port, testClock)
        runCurrent()
        viewModel.requestRecovery(BadgeRecoveryAction.REBOOT)
        assertEquals(BadgeRecoveryPhase.CONFIRMING, viewModel.uiState.value.recovery.phase)

        port.emit(
            statusWithDefaultConfig(receivedAt = 2_000),
            certifiedUsbConnection(receivedAt = 2_000).copy(
                releaseCertifiedMutations = BadgeCapability.entries.toSet() -
                    BadgeCapability.REBOOT,
            ),
        )
        runCurrent()
        viewModel.confirmRecovery()
        runCurrent()

        assertTrue(port.commands.isEmpty())
        assertFalse(
            viewModel.uiState.value.recoveryAvailability
                .getValue(BadgeRecoveryAction.REBOOT).enabled,
        )
    }

    @Test
    fun simultaneousRecoveryConfirmationsAtomicallyClaimOneCommand() = runTest(dispatcher) {
        val port = FakeBadgeControlPort(statusWithDefaultConfig())
        port.blockNextCommand()
        val viewModel = BadgeViewModel(port, testClock)
        runCurrent()
        viewModel.requestRecovery(BadgeRecoveryAction.REBOOT)
        val start = CompletableDeferred<Unit>()

        val calls = List(64) {
            async(Dispatchers.Default) {
                start.await()
                viewModel.confirmRecovery()
            }
        }
        start.complete(Unit)
        calls.awaitAll()
        runCurrent()

        assertEquals(listOf(BadgeCommand.Reboot), port.commands)
        port.releaseBlockedCommand(acknowledged())
        advanceUntilIdle()
    }

    @Test
    fun recoveryNonSuccessOutcomesRemainTypedAndIncludeReconnectGuidance() =
        runTest(dispatcher) {
            val port = FakeBadgeControlPort(statusWithDefaultConfig())
            port.enqueue(BadgeCommandOutcome.TimedOut)
            port.enqueue(BadgeCommandOutcome.Accepted("transport only"))
            port.enqueue(BadgeCommandOutcome.Failed("reboot rejected"))
            val viewModel = BadgeViewModel(port, testClock)
            runCurrent()

            viewModel.requestRecovery(BadgeRecoveryAction.REBOOT)
            viewModel.confirmRecovery()
            advanceUntilIdle()
            assertEquals(BadgeRecoveryPhase.NOT_VERIFIED, viewModel.uiState.value.recovery.phase)
            assertEquals(
                "Reconnect and refresh badge status",
                viewModel.uiState.value.recovery.reconnectGuidance,
            )

            viewModel.requestRecovery(BadgeRecoveryAction.REBOOT)
            viewModel.confirmRecovery()
            advanceUntilIdle()
            assertEquals(BadgeRecoveryPhase.NOT_VERIFIED, viewModel.uiState.value.recovery.phase)

            viewModel.requestRecovery(BadgeRecoveryAction.REBOOT)
            viewModel.confirmRecovery()
            advanceUntilIdle()
            assertEquals(BadgeRecoveryPhase.FAILED, viewModel.uiState.value.recovery.phase)
            assertEquals("reboot rejected", viewModel.uiState.value.recovery.message)
            assertEquals(
                "Reconnect and refresh badge status",
                viewModel.uiState.value.recovery.reconnectGuidance,
            )
            assertEquals(3, port.commands.size)
        }

    @Test
    fun recoveryConfirmationBindsExactGenerationAndTarget() = runTest(dispatcher) {
        val port = FakeBadgeControlPort(statusWithDefaultConfig())
        val viewModel = BadgeViewModel(port, testClock)
        runCurrent()
        viewModel.requestRecovery(BadgeRecoveryAction.ENTER_BOOTLOADER)

        port.emit(
            statusWithDefaultConfig(receivedAt = 2_000),
            certifiedUsbConnection(target = "usb-A", generation = 2, receivedAt = 2_000),
        )
        runCurrent()
        viewModel.confirmRecovery()
        runCurrent()

        assertTrue(port.commands.isEmpty())
        assertEquals(BadgeRecoveryPhase.IDLE, viewModel.uiState.value.recovery.phase)
    }

    @Test
    fun recoveryAcknowledgementIsTypedAndOldCompletionCannotOverwriteNewIdentity() = runTest(dispatcher) {
        val port = FakeBadgeControlPort(statusWithDefaultConfig())
        port.blockNextCommand()
        val viewModel = BadgeViewModel(port, testClock)
        runCurrent()
        viewModel.requestRecovery(BadgeRecoveryAction.REBOOT)
        viewModel.confirmRecovery()
        assertEquals(BadgeRecoveryPhase.PENDING, viewModel.uiState.value.recovery.phase)
        runCurrent()

        port.emit(
            statusWithDefaultConfig(receivedAt = 2_000),
            certifiedUsbConnection(target = "usb-B", generation = 2, receivedAt = 2_000),
        )
        port.releaseBlockedCommand(acknowledged())
        runCurrent()

        assertEquals(BadgeRecoveryPhase.IDLE, viewModel.uiState.value.recovery.phase)
    }

    @Test
    fun bothRecoveryControlsAreDisabledWhileRecoveryIsPending() = runTest(dispatcher) {
        val port = FakeBadgeControlPort(statusWithDefaultConfig())
        port.blockNextCommand()
        val viewModel = BadgeViewModel(port, testClock)
        runCurrent()
        viewModel.requestRecovery(BadgeRecoveryAction.REBOOT)

        viewModel.confirmRecovery()
        runCurrent()

        BadgeRecoveryAction.entries.forEach { action ->
            assertFalse(viewModel.uiState.value.recoveryAvailability.getValue(action).enabled)
        }
        port.releaseBlockedCommand(acknowledged())
        advanceUntilIdle()
    }

    @Test
    fun lossOfConcreteIdentityCancelsRecoveryAndClearsPendingState() = runTest(dispatcher) {
        val port = FakeBadgeControlPort(statusWithDefaultConfig())
        port.blockNextCommand()
        val viewModel = BadgeViewModel(port, testClock)
        runCurrent()
        viewModel.requestRecovery(BadgeRecoveryAction.REBOOT)
        viewModel.confirmRecovery()
        runCurrent()

        port.emit(statusWithDefaultConfig(), BadgeConnectionEvidence())
        runCurrent()

        assertEquals(BadgeRecoveryPhase.IDLE, viewModel.uiState.value.recovery.phase)
        assertEquals(1, port.cancelledCommands.size)
    }

    @Test
    fun oldRecoveryFinallyCannotPreventNextIdentityRecoveryCancellation() =
        runTest(dispatcher) {
            val port = FakeBadgeControlPort(statusWithDefaultConfig())
            port.blockNextCommand(ignoringCancellation = true)
            val viewModel = BadgeViewModel(port, testClock)
            runCurrent()
            viewModel.requestRecovery(BadgeRecoveryAction.REBOOT)
            viewModel.confirmRecovery()
            runCurrent()

            port.emit(
                statusWithDefaultConfig(receivedAt = 2_000),
                certifiedUsbConnection(target = "usb-B", generation = 2, receivedAt = 2_000),
            )
            runCurrent()
            port.blockNextCommand()
            viewModel.requestRecovery(BadgeRecoveryAction.REBOOT)
            viewModel.confirmRecovery()
            runCurrent()

            port.releaseBlockedCommandAt(0, BadgeCommandOutcome.Failed("old recovery"))
            runCurrent()
            port.emit(
                statusWithDefaultConfig(receivedAt = 3_000),
                certifiedUsbConnection(target = "usb-C", generation = 3, receivedAt = 3_000),
            )
            runCurrent()

            assertEquals(2, port.commands.size)
            assertEquals(listOf(BadgeCommand.Reboot), port.cancelledCommands)
        }

    @Test
    fun successfulRecoveryIncludesReconnectGuidance() = runTest(dispatcher) {
        val port = FakeBadgeControlPort(statusWithDefaultConfig())
        port.enqueue(acknowledged())
        val viewModel = BadgeViewModel(port, testClock)
        runCurrent()
        viewModel.requestRecovery(BadgeRecoveryAction.REBOOT)
        viewModel.confirmRecovery()
        advanceUntilIdle()

        assertEquals(BadgeRecoveryPhase.ACKNOWLEDGED, viewModel.uiState.value.recovery.phase)
        assertEquals(
            "Reconnect and refresh badge status",
            viewModel.uiState.value.recovery.reconnectGuidance,
        )
    }

    @Test
    fun refreshAndReconnectDelegateWithoutOwningPortLifecycle() = runTest(dispatcher) {
        val port = FakeBadgeControlPort(statusWithDefaultConfig())
        val viewModel = BadgeViewModel(port, testClock)
        runCurrent()

        viewModel.refresh()
        viewModel.reconnect()

        assertEquals(1, port.refreshCalls)
        assertEquals(1, port.connectionRequests)
        assertEquals(0, port.startCalls)
        assertEquals(0, port.stopCalls)
    }
}

private class FakeBadgeControlPort(
    status: BadgeControlStatus,
    connection: BadgeConnectionEvidence = certifiedUsbConnection(
        receivedAt = status.receivedAtElapsedMs,
    ),
) : BadgeControlPort {
    private val mutableState = MutableStateFlow(
        BadgeRepositoryState(connection = connection, controlStatus = status),
    )
    override val state: StateFlow<BadgeRepositoryState> = mutableState
    val commands = mutableListOf<BadgeCommand>()
    val queuedOutcomes = ArrayDeque<BadgeCommandOutcome>()
    val cancelledCommands = mutableListOf<BadgeCommand>()
    var startCalls = 0
    var stopCalls = 0
    var connectionRequests = 0
    var refreshCalls = 0
    private val blockModes = ArrayDeque<Boolean>()
    private val blockedOutcomes = mutableListOf<CompletableDeferred<BadgeCommandOutcome>>()

    override fun start() {
        startCalls += 1
    }

    override fun stop() {
        stopCalls += 1
    }

    override fun requestConnection() {
        connectionRequests += 1
    }

    override fun refreshStatus() {
        refreshCalls += 1
    }

    override suspend fun execute(command: BadgeCommand): BadgeCommandOutcome {
        commands += command
        val ignoringCancellation = blockModes.pollFirst()
        if (ignoringCancellation != null) {
            val outcome = CompletableDeferred<BadgeCommandOutcome>().also(blockedOutcomes::add)
            return try {
                if (ignoringCancellation) {
                    withContext(NonCancellable) { outcome.await() }
                } else {
                    outcome.await()
                }
            } catch (cancelled: CancellationException) {
                cancelledCommands += command
                throw cancelled
            }
        }
        return queuedOutcomes.pollFirst()
            ?: BadgeCommandOutcome.Failed("No fake command outcome queued")
    }

    fun enqueue(outcome: BadgeCommandOutcome) {
        queuedOutcomes += outcome
    }

    fun blockNextCommand(ignoringCancellation: Boolean = false) {
        blockModes += ignoringCancellation
    }

    fun releaseBlockedCommand(outcome: BadgeCommandOutcome) {
        checkNotNull(blockedOutcomes.firstOrNull { !it.isCompleted }).complete(outcome)
    }

    fun releaseBlockedCommandAt(index: Int, outcome: BadgeCommandOutcome) {
        blockedOutcomes[index].complete(outcome)
    }

    fun emit(
        status: BadgeControlStatus,
        connection: BadgeConnectionEvidence = mutableState.value.connection.copy(
            phase = BadgeConnectionPhase.LIVE,
            lastValidStatusAtElapsedMs = status.receivedAtElapsedMs,
        ),
    ) {
        mutableState.value = mutableState.value.copy(
            connection = connection,
            controlStatus = status,
        )
    }
}

private class FakeMonotonicClock(initialElapsedMs: Long) : MonotonicClock {
    var elapsedMs = initialElapsedMs

    override fun nowElapsedMs(): Long = elapsedMs

    override fun nowWallClock(): Instant = Instant.ofEpochMilli(elapsedMs)

    override fun ticks(periodMs: Long): Flow<Long> = emptyFlow()
}

private fun certifiedUsbConnection(
    target: String = "usb-A",
    generation: Long = 1,
    receivedAt: Long = 1_000,
    phase: BadgeConnectionPhase = BadgeConnectionPhase.LIVE,
) = BadgeConnectionEvidence(
    transport = BadgeTransport.USB_SERIAL,
    transportGeneration = generation,
    phase = phase,
    lastValidStatusAtElapsedMs = receivedAt,
    protocolVersion = "1",
    targetId = target,
    usbCandidateCount = 1,
    exactEspressifVendorMatch = true,
    serialInterfaceReadable = true,
    releaseCertifiedMutations = BadgeCapability.entries.toSet(),
)

private fun certifiedBleConnection(
    target: String = "ble-A",
    generation: Long = 1,
    receivedAt: Long = 1_000,
    mtu: Int,
) = BadgeConnectionEvidence(
    transport = BadgeTransport.BLE_GATT,
    transportGeneration = generation,
    phase = BadgeConnectionPhase.LIVE,
    lastValidStatusAtElapsedMs = receivedAt,
    protocolVersion = "1",
    targetId = target,
    negotiatedBleMtu = mtu,
    fofBleServicePresent = true,
    bleStatusCharacteristicPresent = true,
    bleControlCharacteristicPresent = true,
    bleBonded = true,
    bleEncrypted = true,
    releaseCertifiedMutations = setOf(BadgeCapability.DISPLAY_NAV),
)

private fun certifiedApConnection(
    target: String = "ap-A",
    generation: Long = 1,
    receivedAt: Long = 1_000,
) = BadgeConnectionEvidence(
    transport = BadgeTransport.LOCAL_AP_HTTP,
    transportGeneration = generation,
    phase = BadgeConnectionPhase.LIVE,
    lastValidStatusAtElapsedMs = receivedAt,
    protocolVersion = "1",
    targetId = target,
    badgeApEndpoint = "http://192.168.4.1",
    releaseCertifiedMutations = BadgeCapability.entries.toSet(),
)

private fun certifiedDebugConnection(
    target: String = "debug-A",
    generation: Long = 1,
    receivedAt: Long = 1_000,
) = BadgeConnectionEvidence(
    transport = BadgeTransport.DEBUG_BRIDGE,
    transportGeneration = generation,
    phase = BadgeConnectionPhase.LIVE,
    lastValidStatusAtElapsedMs = receivedAt,
    protocolVersion = "1",
    targetId = target,
    debugBridgeSerialPort = "/dev/cu.usbmodem1",
    debugPhysicalStatusAtElapsedMs = receivedAt,
    debugBridgeLastError = "",
    releaseCertifiedMutations = BadgeCapability.entries.toSet(),
)

private fun statusWithDefaultConfig(
    receivedAt: Long = 1_000,
    theme: BadgeTheme = BadgeTheme.firmwareDefaults(),
    policy: BadgeDisplayPolicy = BadgeDisplayPolicy.firmwareDefaults(),
    networkMode: BadgeNetworkMode = BadgeNetworkMode.USB_ONLY,
    scanners: List<BadgeScannerStatus> = emptyList(),
) = badgeStatus(
    receivedAt = receivedAt,
    themeReadback = BadgeConfigReadback(theme, theme.firmwareHash(), null),
    policyReadback = BadgeConfigReadback(policy, policy.firmwareHash(), null),
    networkReadback = BadgeNetworkModeReadback(networkMode, null),
    scanners = scanners,
)

private fun statusWithUnknownConfig(receivedAt: Long = 1_000) = badgeStatus(
    receivedAt = receivedAt,
    themeReadback = BadgeConfigReadback(null, null, "unknown theme"),
    policyReadback = BadgeConfigReadback(null, null, "unknown policy"),
    networkReadback = BadgeNetworkModeReadback(null, "unknown network"),
)

private fun badgeStatus(
    receivedAt: Long,
    themeReadback: BadgeConfigReadback<BadgeTheme>,
    policyReadback: BadgeConfigReadback<BadgeDisplayPolicy>,
    networkReadback: BadgeNetworkModeReadback,
    scanners: List<BadgeScannerStatus> = emptyList(),
) = BadgeControlStatus(
    version = "1",
    receivedAtElapsedMs = receivedAt,
    receivedAtWallClock = Instant.ofEpochMilli(receivedAt),
    themeReadback = themeReadback,
    policyReadback = policyReadback,
    networkModeReadback = networkReadback,
    entities = emptyList(),
    scanners = scanners,
    displayState = BadgeDisplayState(),
    debugBridge = BadgeDebugBridgeEvidence(null, null, null),
    reporting = BadgeReportingStatus(),
    counts = BadgeThreatCounts(),
    bleControl = BadgeBleControlStatus(),
    safeMode = false,
    safeReason = "",
    resetReason = "",
    crashCount = 0,
    recoveryMode = "",
    stackFreeBytes = emptyMap(),
    heapInternalFreeBytes = 0,
    heapInternalMinimumFreeBytes = 0,
    psramFreeBytes = 0,
)

private fun acknowledged(
    themeHash: Long? = null,
    policyHash: Long? = null,
    networkApplied: Boolean? = null,
    runtimeNetworkMode: BadgeRuntimeNetworkMode? = null,
) = BadgeCommandOutcome.Acknowledged(
    BadgeControlAcknowledgement(
        message = "acknowledged",
        themeHash = themeHash,
        policyHash = policyHash,
        networkApplied = networkApplied,
        runtimeNetworkMode = runtimeNetworkMode,
    ),
)
