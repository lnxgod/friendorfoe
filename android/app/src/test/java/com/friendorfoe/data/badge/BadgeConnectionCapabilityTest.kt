package com.friendorfoe.data.badge

import com.friendorfoe.data.time.MonotonicClock
import java.time.Instant
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

@OptIn(ExperimentalCoroutinesApi::class)
class BadgeConnectionCapabilityTest {

    @Test
    fun usbApAndDebugUseTenSecondLiveWindowAndSixtySecondExpiry() {
        listOf(
            BadgeTransport.USB_SERIAL,
            BadgeTransport.LOCAL_AP_HTTP,
            BadgeTransport.DEBUG_BRIDGE,
        ).forEach { transport ->
            assertEquals(BadgeConnectionPhase.LIVE, badgeFreshness(transport, 0, 9_999))
            assertEquals(BadgeConnectionPhase.STALE, badgeFreshness(transport, 0, 10_000))
            assertEquals(BadgeConnectionPhase.EXPIRED, badgeFreshness(transport, 0, 60_000))
        }
    }

    @Test
    fun bleUsesTwentySecondLiveWindowAndSixtySecondExpiry() {
        assertEquals(
            BadgeConnectionPhase.LIVE,
            badgeFreshness(BadgeTransport.BLE_GATT, 1_000, 20_999),
        )
        assertEquals(
            BadgeConnectionPhase.STALE,
            badgeFreshness(BadgeTransport.BLE_GATT, 1_000, 21_000),
        )
        assertEquals(
            BadgeConnectionPhase.EXPIRED,
            badgeFreshness(BadgeTransport.BLE_GATT, 1_000, 61_000),
        )
    }

    @Test
    fun freshnessUsesSaturatingElapsedAgeForNegativeAndExtremeTimestamps() {
        assertEquals(
            BadgeConnectionPhase.EXPIRED,
            badgeFreshness(
                BadgeTransport.DEBUG_BRIDGE,
                receivedAtElapsedMs = Long.MIN_VALUE,
                nowElapsedMs = Long.MAX_VALUE,
            ),
        )
        assertEquals(
            BadgeConnectionPhase.LIVE,
            badgeFreshness(
                BadgeTransport.DEBUG_BRIDGE,
                receivedAtElapsedMs = 100,
                nowElapsedMs = 0,
            ),
        )
    }

    @Test
    fun verifiedDebugStatusThatArrivesTenSecondsOldStartsStaleImmediately() {
        assertEquals(
            BadgeConnectionPhase.STALE,
            verifiedBadgeConnectionPhase(
                transport = BadgeTransport.DEBUG_BRIDGE,
                effectiveReceivedAtElapsedMs = 1_000,
                nowElapsedMs = 11_000,
            ),
        )
    }

    @Test
    fun bleOnlySupportsStatusAndMtuQualifiedNavigation() {
        val evidence = liveEvidence(BadgeTransport.BLE_GATT, negotiatedBleMtu = 41).copy(
            releaseCertifiedMutations = setOf(BadgeCapability.DISPLAY_NAV),
        )

        assertEquals(BadgeCapabilitySupport.SUPPORTED, badgeCapability(evidence, BadgeCapability.READ_STATUS))
        assertEquals(
            BadgeCapabilitySupport.SUPPORTED,
            badgeCapability(evidence, BadgeCapability.DISPLAY_NAV, payloadBytes = 37),
        )
        assertEquals(
            BadgeCapabilitySupport.UNSUPPORTED,
            badgeCapability(evidence, BadgeCapability.DISPLAY_NAV, payloadBytes = 39),
        )
        assertEquals(BadgeCapabilitySupport.UNSUPPORTED, badgeCapability(evidence, BadgeCapability.NETWORK_MODE))
        assertEquals(BadgeCapabilitySupport.UNSUPPORTED, badgeCapability(evidence, BadgeCapability.THEME_V1))
        assertEquals(
            BadgeCapabilitySupport.UNSUPPORTED,
            badgeCapability(evidence, BadgeCapability.DISPLAY_POLICY_V1),
        )
        assertEquals(
            BadgeCapabilitySupport.UNKNOWN,
            badgeCapability(
                evidence.copy(releaseCertifiedMutations = emptySet()),
                BadgeCapability.DISPLAY_NAV,
                payloadBytes = 37,
            ),
        )
    }

    @Test
    fun compactNavigationPayloadsPreservePerActionMtuRequirements() {
        assertEquals(37, BadgeCommand.NavigateDisplay(BadgeDisplayAction.NEXT).payloadSizeOrNull())
        assertEquals(39, BadgeCommand.NavigateDisplay(BadgeDisplayAction.DETAIL).payloadSizeOrNull())
        assertEquals(37, BadgeCommand.NavigateDisplay(BadgeDisplayAction.BACK).payloadSizeOrNull())
    }

    @Test
    fun bleNavigationRequiresBondedEncryptedGattEvidence() {
        val certified = liveEvidence(BadgeTransport.BLE_GATT, negotiatedBleMtu = 64).copy(
            releaseCertifiedMutations = setOf(BadgeCapability.DISPLAY_NAV),
        )

        assertEquals(
            BadgeCapabilitySupport.UNKNOWN,
            badgeCapability(
                certified.copy(bleBonded = false),
                BadgeCapability.DISPLAY_NAV,
                payloadBytes = 37,
            ),
        )
        assertEquals(
            BadgeCapabilitySupport.UNKNOWN,
            badgeCapability(
                certified.copy(bleEncrypted = false),
                BadgeCapability.DISPLAY_NAV,
                payloadBytes = 37,
            ),
        )
    }

    @Test
    fun debugMutationRequiresPresentBlankPhysicalLastError() {
        val certified = liveEvidence(BadgeTransport.DEBUG_BRIDGE).copy(
            releaseCertifiedMutations = setOf(BadgeCapability.THEME_V1),
        )

        assertEquals(
            BadgeCapabilitySupport.SUPPORTED,
            badgeCapability(certified.copy(debugBridgeLastError = ""), BadgeCapability.THEME_V1),
        )
        assertEquals(
            BadgeCapabilitySupport.UNKNOWN,
            badgeCapability(certified.copy(debugBridgeLastError = null), BadgeCapability.THEME_V1),
        )
        assertEquals(
            BadgeCapabilitySupport.UNKNOWN,
            badgeCapability(
                certified.copy(debugBridgeLastError = "serial timeout"),
                BadgeCapability.THEME_V1,
            ),
        )
    }

    @Test
    fun debugBridgeAndApNeverSupportRecovery() {
        listOf(BadgeTransport.DEBUG_BRIDGE, BadgeTransport.LOCAL_AP_HTTP).forEach { transport ->
            val certified = liveEvidence(transport).copy(
                releaseCertifiedMutations = setOf(
                    BadgeCapability.REBOOT,
                    BadgeCapability.BOOTLOADER,
                ),
            )
            assertEquals(
                BadgeCapabilitySupport.UNSUPPORTED,
                badgeCapability(certified, BadgeCapability.REBOOT),
            )
            assertEquals(
                BadgeCapabilitySupport.UNSUPPORTED,
                badgeCapability(certified, BadgeCapability.BOOTLOADER),
            )
        }
    }

    @Test
    fun usbRecoveryRequiresOneReadableEspressifTargetAndCertification() {
        val certified = liveEvidence(BadgeTransport.USB_SERIAL).copy(
            releaseCertifiedMutations = setOf(BadgeCapability.REBOOT),
        )
        assertEquals(
            BadgeCapabilitySupport.SUPPORTED,
            badgeCapability(certified, BadgeCapability.REBOOT),
        )
        assertEquals(
            BadgeCapabilitySupport.UNKNOWN,
            badgeCapability(certified.copy(targetId = null), BadgeCapability.REBOOT),
        )
        assertEquals(
            BadgeCapabilitySupport.UNKNOWN,
            badgeCapability(certified.copy(usbCandidateCount = 2), BadgeCapability.REBOOT),
        )
    }

    @Test
    fun timeAdvancingWithoutAnotherStatusMakesRepositoryStateStaleThenExpired() = runTest {
        val clock = FakeMonotonicClock(0)
        val store = BadgeRepositoryStateStore(
            initialState = BadgeRepositoryState(
                connection = liveEvidence(BadgeTransport.USB_SERIAL),
                controlStatus = statusFixture(receivedAtElapsedMs = 0),
                detections = listOf(detectionFixture()),
            ),
            clock = clock,
            scope = backgroundScope,
        )

        assertEquals(BadgeConnectionPhase.LIVE, store.state.value.connection.phase)
        clock.advanceBy(10_000)
        runCurrent()
        assertEquals(BadgeConnectionPhase.STALE, store.state.value.connection.phase)
        assertTrue(store.state.value.controlStatus != null)
        clock.advanceBy(50_000)
        runCurrent()
        assertEquals(BadgeConnectionPhase.EXPIRED, store.state.value.connection.phase)
        assertNull(store.state.value.controlStatus)
        assertTrue(store.state.value.detections.isEmpty())
    }

    @Test
    fun oldTimestampNeverResurrectsDisconnectedOrErrorTransport() {
        listOf(BadgeConnectionPhase.DISCONNECTED, BadgeConnectionPhase.ERROR).forEach { phase ->
            val evidence = liveEvidence(BadgeTransport.USB_SERIAL).copy(phase = phase)
            assertEquals(phase, evidence.aged(nowElapsedMs = 2_000L).phase)
        }
    }

    @Test
    fun disconnectedAndErrorPhasesStayPutButCachedStatusExpiresAtSixtySeconds() = runTest {
        listOf(BadgeConnectionPhase.DISCONNECTED, BadgeConnectionPhase.ERROR).forEach { phase ->
            val clock = FakeMonotonicClock(0)
            val store = BadgeRepositoryStateStore(
                initialState = BadgeRepositoryState(
                    connection = liveEvidence(BadgeTransport.USB_SERIAL).copy(phase = phase),
                    controlStatus = statusFixture(receivedAtElapsedMs = 0),
                    detections = listOf(detectionFixture()),
                ),
                clock = clock,
                scope = backgroundScope,
            )

            clock.advanceBy(60_000)
            runCurrent()

            assertEquals(phase, store.state.value.connection.phase)
            assertNull(store.state.value.controlStatus)
            assertTrue(store.state.value.detections.isEmpty())
        }
    }

    @Test
    fun openingDifferentTargetClearsPriorBadgeConfigurationImmediately() = runTest {
        val store = BadgeRepositoryStateStore(
            initialState = BadgeRepositoryState(
                connection = liveEvidence(BadgeTransport.USB_SERIAL),
                controlStatus = statusFixture(receivedAtElapsedMs = 0),
                detections = listOf(detectionFixture()),
            ),
            clock = FakeMonotonicClock(0),
            scope = backgroundScope,
        )

        store.publishConnection(
            liveEvidence(BadgeTransport.BLE_GATT).copy(
                phase = BadgeConnectionPhase.TRANSPORT_OPEN,
                lastValidStatusAtElapsedMs = null,
                protocolVersion = null,
            ),
        )

        assertNull(store.state.value.controlStatus)
        assertTrue(store.state.value.detections.isEmpty())
    }

    @Test
    fun reopeningSameConcreteTargetCannotRetainUnverifiableCachedState() = runTest {
        val target = liveEvidence(BadgeTransport.USB_SERIAL)
        val store = BadgeRepositoryStateStore(
            initialState = BadgeRepositoryState(
                connection = target,
                controlStatus = statusFixture(receivedAtElapsedMs = 0),
                detections = listOf(detectionFixture()),
            ),
            clock = FakeMonotonicClock(0),
            scope = backgroundScope,
        )

        store.publishConnection(
            target.copy(
                phase = BadgeConnectionPhase.TRANSPORT_OPEN,
                lastValidStatusAtElapsedMs = null,
                protocolVersion = null,
            ),
        )

        assertEquals(BadgeConnectionPhase.TRANSPORT_OPEN, store.state.value.connection.phase)
        assertNull(store.state.value.controlStatus)
        assertTrue(store.state.value.detections.isEmpty())
    }

    @Test
    fun sameTargetPermissionNeededCannotRetainPriorVerifiedConfiguration() = runTest {
        val target = liveEvidence(BadgeTransport.USB_SERIAL)
        val store = BadgeRepositoryStateStore(
            initialState = BadgeRepositoryState(
                connection = target,
                controlStatus = statusFixture(receivedAtElapsedMs = 0),
                detections = listOf(detectionFixture()),
            ),
            clock = FakeMonotonicClock(0),
            scope = backgroundScope,
        )

        store.publishConnection(
            target.copy(
                phase = BadgeConnectionPhase.PERMISSION_NEEDED,
                lastValidStatusAtElapsedMs = null,
                protocolVersion = null,
            ),
        )

        assertNull(store.state.value.controlStatus)
        assertTrue(store.state.value.detections.isEmpty())
    }

    @Test
    fun publishingOneBadgeDetectionCreatesExactlyOneCurrentFeedRow() = runTest {
        val store = BadgeRepositoryStateStore(
            initialState = BadgeRepositoryState(
                connection = liveEvidence(BadgeTransport.USB_SERIAL),
            ),
            clock = FakeMonotonicClock(0),
            scope = backgroundScope,
        )

        store.publishDetection(detectionFixture(), maxRows = 20)

        assertEquals(listOf("fixture"), store.state.value.detections.map { it.id })
    }

    @Test
    fun checkedInReleaseCertificationStartsEmpty() {
        assertTrue(CheckedInBadgeReleaseCertification.mutationsByTransport.isEmpty())
        BadgeTransport.entries.forEach { transport ->
            assertTrue(CheckedInBadgeReleaseCertification.forTransport(transport).isEmpty())
        }
    }

    @Test
    fun debugBridgeConfigurationIsBuildGatedAndRejectsMalformedUrls() {
        val defaultDebug = badgeDebugBridgeConfig(true, "http://10.0.2.2:8765/")
        assertTrue(defaultDebug.enabled)
        assertEquals("http://10.0.2.2:8765/", defaultDebug.baseUrl.toString())

        val overridden = badgeDebugBridgeConfig(true, "http://127.0.0.1:8765/")
        assertTrue(overridden.enabled)
        assertEquals("127.0.0.1", overridden.baseUrl?.host)

        val malformed = badgeDebugBridgeConfig(true, "not a URL")
        assertFalse(malformed.enabled)
        assertNull(malformed.baseUrl)

        val release = badgeDebugBridgeConfig(false, "http://10.0.2.2:8765/")
        assertFalse(release.enabled)
        assertNull(release.baseUrl)
    }

    @Test
    fun stopInvalidatesSessionBeforeLateTransportResultCanPublish() {
        val gate = BadgeTransportGenerationGate()
        val session = gate.startSession()
        val apRequest = gate.captureSession(session)

        gate.stopSession()

        assertNull(gate.claim(apRequest, BadgeTransport.LOCAL_AP_HTTP))
        assertFalse(gate.isSessionCurrent(session))
    }

    @Test
    fun winningDirectUsbInvalidatesLateHttpAndBlePublishers() {
        val gate = BadgeTransportGenerationGate()
        val session = gate.startSession()
        val bleConnection = gate.claim(
            gate.captureSession(session),
            BadgeTransport.BLE_GATT,
        )!!

        val usbConnection = gate.claim(
            gate.captureSession(session),
            BadgeTransport.USB_SERIAL,
        )!!

        assertTrue(gate.isCurrent(usbConnection))
        assertFalse(gate.isCurrent(bleConnection))
        assertNull(
            gate.claim(
                gate.captureSession(session),
                BadgeTransport.DEBUG_BRIDGE,
            ),
        )
        assertNull(
            gate.claim(
                gate.captureSession(session),
                BadgeTransport.LOCAL_AP_HTTP,
            ),
        )
    }

    @Test
    fun replacingConcreteUsbOrBleInstanceAlwaysAdvancesTransportGeneration() {
        listOf(BadgeTransport.USB_SERIAL, BadgeTransport.BLE_GATT).forEach { transport ->
            val gate = BadgeTransportGenerationGate()
            val session = gate.startSession()
            val sessionToken = gate.captureSession(session)
            val first = gate.claimNewConnection(sessionToken, transport)!!
            val replacement = gate.claimNewConnection(sessionToken, transport)!!

            assertFalse(gate.isCurrent(first))
            assertTrue(gate.isCurrent(replacement))
            assertTrue(replacement.transportGeneration > first.transportGeneration)
        }
    }

    @Test
    fun lateCommandCompletionCannotPublishAfterStopOrDirectUsbWinner() {
        val stoppedGate = BadgeTransportGenerationGate()
        val stoppedSession = stoppedGate.startSession()
        val stoppedAp = stoppedGate.claim(
            stoppedGate.captureSession(stoppedSession),
            BadgeTransport.LOCAL_AP_HTTP,
        )!!
        var stoppedPublished = false

        stoppedGate.stopSession()

        assertFalse(stoppedGate.runIfCurrent(stoppedAp) { stoppedPublished = true })
        assertFalse(stoppedPublished)

        val switchedGate = BadgeTransportGenerationGate()
        val switchedSession = switchedGate.startSession()
        val switchedAp = switchedGate.claim(
            switchedGate.captureSession(switchedSession),
            BadgeTransport.LOCAL_AP_HTTP,
        )!!
        switchedGate.claimNewConnection(
            switchedGate.captureSession(switchedSession),
            BadgeTransport.USB_SERIAL,
        )!!
        var switchedPublished = false

        assertFalse(switchedGate.runIfCurrent(switchedAp) { switchedPublished = true })
        assertFalse(switchedPublished)
    }

    @Test
    fun usbDetachMustMatchExactActivePhysicalTarget() {
        assertTrue(
            matchesActiveUsbTarget(
                detachedTargetId = "usb:303a:7:/dev/bus/usb/001/007",
                activeTargetId = "usb:303a:7:/dev/bus/usb/001/007",
            ),
        )
        assertFalse(
            matchesActiveUsbTarget(
                detachedTargetId = "usb:303a:8:/dev/bus/usb/001/008",
                activeTargetId = "usb:303a:7:/dev/bus/usb/001/007",
            ),
        )
        assertFalse(matchesActiveUsbTarget("usb:303a:7:x", null))
    }

    @Test
    fun usbPermissionResultMustMatchIssuingSessionTargetAndNonce() {
        val requests = BadgeUsbPermissionRequestGate()
        val old = requests.issue(sessionGeneration = 11L, targetId = "usb:a")

        requests.clear()
        val current = requests.issue(sessionGeneration = 22L, targetId = "usb:a")

        assertFalse(requests.consume(old, currentSessionGeneration = 22L, resultTargetId = "usb:a"))
        assertFalse(
            requests.consume(
                current,
                currentSessionGeneration = 22L,
                resultTargetId = "usb:b",
            ),
        )
        assertTrue(
            requests.consume(
                current,
                currentSessionGeneration = 22L,
                resultTargetId = "usb:a",
            ),
        )
        assertFalse(
            requests.consume(
                current,
                currentSessionGeneration = 22L,
                resultTargetId = "usb:a",
            ),
        )
    }

    @Test
    fun expiredOrUnverifiedHttpLeaseCannotBlockFallbackOrPublishLateResult() {
        val gate = BadgeTransportGenerationGate()
        val session = gate.startSession()
        val sessionToken = gate.captureSession(session)
        val ap = gate.claim(sessionToken, BadgeTransport.LOCAL_AP_HTTP)!!
        val expired = liveEvidence(BadgeTransport.LOCAL_AP_HTTP).copy(
            phase = BadgeConnectionPhase.EXPIRED,
        )

        assertFalse(
            shouldRetainHttpLease(
                evidenceComplete = true,
                phase = BadgeConnectionPhase.EXPIRED,
            ),
        )
        assertFalse(
            shouldRetainHttpLease(
                evidenceComplete = false,
                phase = BadgeConnectionPhase.TRANSPORT_OPEN,
            ),
        )
        assertTrue(shouldReleaseExpiredHttpLease(expired, ap))
        assertTrue(gate.releaseAndRunIfCurrent(ap) {})

        val ble = gate.claimNewConnection(sessionToken, BadgeTransport.BLE_GATT)!!
        assertTrue(gate.isCurrent(ble))
        assertFalse(gate.runIfCurrent(ap) { error("late AP publication must be ignored") })
    }

    @Test
    fun usbDetachAtomicallyInvalidatesPendingCommandBeforeLateLine() {
        val gate = BadgeTransportGenerationGate()
        val session = gate.startSession()
        val usb = gate.claimNewConnection(
            gate.captureSession(session),
            BadgeTransport.USB_SERIAL,
        )!!
        val commands = BadgeUsbCommandCoordinator()
        val generation = commands.currentTransportGeneration()
        val pending = kotlinx.coroutines.CompletableDeferred<BadgeCommandOutcome>()
        assertTrue(commands.begin(generation, BadgeCommand.Reboot, pending))

        assertTrue(
            gate.releaseAndRunIfCurrent(usb) {
                commands.invalidateTransport("Badge detached")
            },
        )

        assertFalse(commands.acceptSerialLine(generation, "FOF_REBOOT:OK"))
        assertFalse(gate.runIfCurrent(usb) { error("detached USB callback must be ignored") })
    }

    @Test
    fun debugPhysicalTargetSwitchRotatesGenerationAndRejectsOldCommandCompletion() {
        val gate = BadgeTransportGenerationGate()
        val session = gate.startSession()
        val sessionToken = gate.captureSession(session)
        val physicalTargetA = gate.claim(sessionToken, BadgeTransport.DEBUG_BRIDGE)!!

        val physicalTargetB = gate.claimNewConnection(
            sessionToken,
            BadgeTransport.DEBUG_BRIDGE,
        )!!

        assertFalse(gate.isCurrent(physicalTargetA))
        assertTrue(gate.isCurrent(physicalTargetB))
        assertFalse(
            gate.runIfCurrent(physicalTargetA) {
                error("target A completion must not publish after target B appears")
            },
        )
    }

    @Test
    fun liveSnapshotCannotAuthorizeMutationAgainstReplacementConcreteToken() {
        val gate = BadgeTransportGenerationGate()
        val session = gate.startSession()
        val sessionToken = gate.captureSession(session)
        val targetA = gate.claimNewConnection(sessionToken, BadgeTransport.USB_SERIAL)!!
        val snapshotA = liveEvidence(BadgeTransport.USB_SERIAL).copy(
            transportGeneration = targetA.transportGeneration,
        )

        val targetB = gate.claimNewConnection(sessionToken, BadgeTransport.USB_SERIAL)!!

        assertTrue(snapshotA.matchesActiveToken(targetA))
        assertFalse(gate.isCurrent(targetA))
        assertFalse(snapshotA.matchesActiveToken(targetB))
        assertTrue(gate.isCurrent(targetB))
    }

    @Test
    fun exactRecoveryAckMaySurviveOnlyExpectedSameTargetDisconnectWithNoWinner() {
        val gate = BadgeTransportGenerationGate()
        val session = gate.startSession()
        val sessionToken = gate.captureSession(session)
        val usb = gate.claimNewConnection(sessionToken, BadgeTransport.USB_SERIAL)!!
        val attempted = liveEvidence(BadgeTransport.USB_SERIAL).copy(
            transportGeneration = usb.transportGeneration,
        )
        val disconnected = attempted.copy(phase = BadgeConnectionPhase.DISCONNECTED)
        val acknowledged = BadgeCommandOutcome.Acknowledged(
            BadgeControlAcknowledgement("Reboot acknowledged"),
        )

        assertTrue(gate.releaseAndRunIfCurrent(usb) {})
        assertTrue(
            canPublishExpectedRecoveryDisconnect(
                BadgeCommand.Reboot,
                acknowledged,
                attempted,
                disconnected,
            ),
        )
        var published = false
        assertTrue(
            gate.runIfSessionHasNoActiveTransport(session) {
                published = true
            },
        )
        assertTrue(published)

        assertFalse(
            canPublishExpectedRecoveryDisconnect(
                BadgeCommand.Reboot,
                acknowledged,
                attempted,
                disconnected.copy(targetId = "usb:303a:other"),
            ),
        )
        gate.claim(sessionToken, BadgeTransport.LOCAL_AP_HTTP)!!
        assertFalse(gate.runIfSessionHasNoActiveTransport(session) {})
    }

    private fun liveEvidence(
        transport: BadgeTransport,
        negotiatedBleMtu: Int? = 64,
    ) = BadgeConnectionEvidence(
        transport = transport,
        phase = BadgeConnectionPhase.LIVE,
        lastValidStatusAtElapsedMs = 0,
        protocolVersion = "0.64.65",
        targetId = when (transport) {
            BadgeTransport.USB_SERIAL -> "usb:303a:1"
            BadgeTransport.LOCAL_AP_HTTP -> "http://192.168.4.1"
            BadgeTransport.BLE_GATT -> "AA:BB:CC:DD:EE:FF"
            BadgeTransport.DEBUG_BRIDGE -> "/dev/cu.usbmodem-test"
        },
        usbCandidateCount = if (transport == BadgeTransport.USB_SERIAL) 1 else null,
        exactEspressifVendorMatch = transport == BadgeTransport.USB_SERIAL,
        serialInterfaceReadable = transport == BadgeTransport.USB_SERIAL,
        badgeApEndpoint = if (transport == BadgeTransport.LOCAL_AP_HTTP) {
            "http://192.168.4.1"
        } else {
            null
        },
        negotiatedBleMtu = if (transport == BadgeTransport.BLE_GATT) negotiatedBleMtu else null,
        fofBleServicePresent = transport == BadgeTransport.BLE_GATT,
        bleStatusCharacteristicPresent = transport == BadgeTransport.BLE_GATT,
        bleControlCharacteristicPresent = transport == BadgeTransport.BLE_GATT,
        bleBonded = transport == BadgeTransport.BLE_GATT,
        bleEncrypted = transport == BadgeTransport.BLE_GATT,
        debugBridgeSerialPort = if (transport == BadgeTransport.DEBUG_BRIDGE) {
            "/dev/cu.usbmodem-test"
        } else {
            null
        },
        debugPhysicalStatusAtElapsedMs = if (transport == BadgeTransport.DEBUG_BRIDGE) 0 else null,
        debugBridgeLastError = if (transport == BadgeTransport.DEBUG_BRIDGE) "" else null,
        releaseCertifiedMutations = emptySet(),
    )

    private fun statusFixture(receivedAtElapsedMs: Long) = BadgeControlStatus(
        version = "0.64.65",
        receivedAtElapsedMs = receivedAtElapsedMs,
        receivedAtWallClock = Instant.EPOCH.plusMillis(receivedAtElapsedMs),
        themeReadback = BadgeConfigReadback(null, null, "not included in fixture"),
        policyReadback = BadgeConfigReadback(null, null, "not included in fixture"),
        networkModeReadback = BadgeNetworkModeReadback(null, "not included in fixture"),
        entities = emptyList(),
        scanners = emptyList(),
        displayState = null,
        debugBridge = null,
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

    private fun detectionFixture() = BadgeUsbDetection(
        id = "fixture",
        manufacturer = "fixture",
        source = 1,
        confidence = 0.9f,
        rssi = -50,
    )
}

private class FakeMonotonicClock(initialElapsedMs: Long) : MonotonicClock {
    private val now = MutableStateFlow(initialElapsedMs)

    override fun nowElapsedMs(): Long = now.value

    override fun nowWallClock(): Instant = Instant.EPOCH.plusMillis(now.value)

    override fun ticks(periodMs: Long): Flow<Long> = now

    fun advanceBy(deltaMs: Long) {
        now.value += deltaMs
    }
}
