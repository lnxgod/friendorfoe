package com.friendorfoe.detection

import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.async
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

@OptIn(ExperimentalCoroutinesApi::class)
class BleInvestigationCoordinatorTest {

    @Test
    fun `coordinator rejects concurrent request as busy`() = runTest {
        val fake = FakeInspector().apply { block = CompletableDeferred() }
        val coordinator = BleInvestigationCoordinator(fake)
        val first = async { coordinator.investigatePhone(request("r1")) }
        runCurrent()
        val second = coordinator.investigatePhone(request("r2"))
        assertEquals(BleInvestigationState.FAILED, second.state)
        assertEquals("busy", second.error)
        fake.block!!.complete(Unit)
        first.await()
    }

    @Test
    fun `request timeout becomes a failed result`() = runTest {
        val fake = FakeInspector().apply { block = CompletableDeferred() }
        val coordinator = BleInvestigationCoordinator(fake)
        val result = coordinator.investigatePhone(request("r1").copy(timeoutMs = 1))
        assertEquals(BleInvestigationState.FAILED, result.state)
        assertEquals("timeout", result.error)
    }

    @Test
    fun `cancel closes active request`() = runTest {
        val fake = FakeInspector().apply { block = CompletableDeferred() }
        val coordinator = BleInvestigationCoordinator(fake)
        val running = async { coordinator.investigatePhone(request("r1")) }
        runCurrent()

        coordinator.cancel()

        assertEquals(BleInvestigationState.CANCELLED, running.await().state)
        assertEquals(1, fake.cancelCount)
        assertEquals(BleInvestigationState.CANCELLED, coordinator.state.value?.state)
        coordinator.cancel()
        assertEquals(1, fake.cancelCount)
    }

    @Test
    fun `losing teardown caller cannot complete or release guard before cleanup finishes`() = runTest {
        val cleanupEntered = CompletableDeferred<Unit>()
        val cleanupRelease = CompletableDeferred<Unit>()
        val fake = FakeInspector().apply {
            block = CompletableDeferred()
            firstCancelEntered = cleanupEntered
            firstCancelBlock = cleanupRelease
        }
        val coordinator = BleInvestigationCoordinator(fake)
        val running = async { coordinator.investigatePhone(request("r1")) }
        runCurrent()
        val winningClose = async { coordinator.cancel() }
        cleanupEntered.await()

        running.cancel()
        runCurrent()
        fake.block = null

        try {
            assertFalse(running.isCompleted)
            val contender = coordinator.investigatePhone(request("r2"))
            assertEquals(BleInvestigationState.FAILED, contender.state)
            assertEquals("busy", contender.error)
        } finally {
            cleanupRelease.complete(Unit)
        }

        winningClose.await()
        running.join()
        assertEquals(
            BleInvestigationState.COMPLETE,
            coordinator.investigatePhone(request("r3")).state,
        )
    }

    @Test
    fun `authentication required remains structured evidence`() = runTest {
        val fake = FakeInspector().apply {
            result = completedResult().copy(authenticationRequired = true)
        }
        val result = BleInvestigationCoordinator(fake).investigatePhone(request("r1"))
        assertTrue(result.authenticationRequired)
        assertNull(result.error)
    }

    @Test
    fun `cancel cannot overwrite an already completed result`() = runTest {
        val cleanup = CompletableDeferred<Unit>()
        val fake = FakeInspector().apply { firstCancelBlock = cleanup }
        val coordinator = BleInvestigationCoordinator(fake)
        val running = async { coordinator.investigatePhone(request("r1")) }
        runCurrent()

        assertEquals(BleInvestigationState.COMPLETE, coordinator.state.value?.state)
        coordinator.cancel()

        assertEquals(BleInvestigationState.COMPLETE, coordinator.state.value?.state)
        assertEquals(1, fake.cancelCount)
        cleanup.complete(Unit)
        assertEquals(BleInvestigationState.COMPLETE, running.await().state)
    }

    @Test
    fun `late progress cannot overwrite a terminal result`() = runTest {
        val fake = FakeInspector()
        val coordinator = BleInvestigationCoordinator(fake)

        val result = coordinator.investigatePhone(request("r1"))
        fake.emitProgress(
            completedResult().copy(
                requestId = "r1",
                state = BleInvestigationState.READING,
                summary = "late progress",
            ),
        )

        assertEquals(BleInvestigationState.COMPLETE, result.state)
        assertEquals(BleInvestigationState.COMPLETE, coordinator.state.value?.state)
    }

    @Test
    fun `cleanup failure does not replace success or retain the guard`() = runTest {
        val fake = FakeInspector().apply {
            cancelFailure = IllegalStateException("cleanup failed")
        }
        val coordinator = BleInvestigationCoordinator(fake)

        val first = coordinator.investigatePhone(request("r1"))

        assertEquals(BleInvestigationState.COMPLETE, first.state)
        fake.cancelFailure = null
        val second = coordinator.investigatePhone(request("r2"))
        assertEquals(BleInvestigationState.COMPLETE, second.state)
        assertEquals(listOf("r1", "r2"), fake.requests.map { it.requestId })
    }

    @Test
    fun `cleanup failure does not replace timeout or retain the guard`() = runTest {
        val fake = FakeInspector().apply {
            block = CompletableDeferred()
            cancelFailure = IllegalStateException("cleanup failed")
        }
        val coordinator = BleInvestigationCoordinator(fake)

        val timedOut = coordinator.investigatePhone(request("r1").copy(timeoutMs = 1))

        assertEquals(BleInvestigationState.FAILED, timedOut.state)
        assertEquals("timeout", timedOut.error)
        fake.block = null
        fake.cancelFailure = null
        assertEquals(
            BleInvestigationState.COMPLETE,
            coordinator.investigatePhone(request("r2")).state,
        )
    }

    @Test
    fun `cleanup failure cannot escape cancel or replace cancellation`() = runTest {
        val fake = FakeInspector().apply {
            block = CompletableDeferred()
            cancelFailure = IllegalStateException("cleanup failed")
        }
        val coordinator = BleInvestigationCoordinator(fake)
        val running = async { coordinator.investigatePhone(request("r1")) }
        runCurrent()

        coordinator.cancel()

        assertEquals(BleInvestigationState.CANCELLED, running.await().state)
        assertEquals(BleInvestigationState.CANCELLED, coordinator.state.value?.state)
        fake.block = null
        fake.cancelFailure = null
        assertEquals(
            BleInvestigationState.COMPLETE,
            coordinator.investigatePhone(request("r2")).state,
        )
    }

    @Test
    fun `same request id cannot retain evidence from a prior generation`() = runTest {
        val fake = FakeInspector().apply {
            result = completedResult().copy(
                services = listOf("prior-service"),
                reads = mapOf("prior-read" to "01"),
            )
        }
        val coordinator = BleInvestigationCoordinator(fake)
        assertEquals(
            BleInvestigationState.COMPLETE,
            coordinator.investigatePhone(request("reused")).state,
        )

        fake.beforeProgressBlock = CompletableDeferred()
        val running = async { coordinator.investigatePhone(request("reused")) }
        runCurrent()
        coordinator.cancel()
        val cancelled = running.await()

        assertEquals(BleInvestigationState.CANCELLED, cancelled.state)
        assertTrue(cancelled.services.isEmpty())
        assertTrue(cancelled.reads.isEmpty())
    }

    @Test
    fun `request timeout is clamped to supported bounds`() {
        assertEquals(1L, bleInvestigationTimeoutMs(Long.MIN_VALUE))
        assertEquals(1L, bleInvestigationTimeoutMs(0))
        assertEquals(1L, bleInvestigationTimeoutMs(1))
        assertEquals(12_000L, bleInvestigationTimeoutMs(12_000))
        assertEquals(12_000L, bleInvestigationTimeoutMs(Long.MAX_VALUE))
    }

    @Test
    fun `freshness accepts only recent monotonic observations`() {
        assertTrue(isBleInvestigationTargetFresh(observedAtElapsedMs = 1_000, nowElapsedMs = 1_001))
        assertTrue(
            isBleInvestigationTargetFresh(
                observedAtElapsedMs = 1_000,
                nowElapsedMs = 1_000 + BLE_INVESTIGATION_TARGET_MAX_AGE_MS,
            ),
        )
        assertFalse(
            isBleInvestigationTargetFresh(
                observedAtElapsedMs = 1_000,
                nowElapsedMs = 1_001 + BLE_INVESTIGATION_TARGET_MAX_AGE_MS,
            ),
        )
        assertFalse(isBleInvestigationTargetFresh(observedAtElapsedMs = 1_001, nowElapsedMs = 1_000))
    }

    @Test
    fun `encrypted serial characteristic requires authentication without a read`() {
        assertEquals(
            BleReadDecision.AUTHENTICATION_REQUIRED,
            bleReadDecision(
                serviceUuid = "0000ffe0-0000-1000-8000-00805f9b34fb",
                characteristicUuid = "0000ffe1-0000-1000-8000-00805f9b34fb",
                readable = true,
                requiresEncryption = true,
            ),
        )
    }

    @Test
    fun `bond transition prevents a successful terminal result`() {
        assertEquals(
            BleTerminalDecision.BOND_CHANGED,
            bleTerminalDecision(
                cancelled = false,
                closed = false,
                bondTransitioned = true,
                currentBondChanged = false,
            ),
        )
        assertEquals(
            BleTerminalDecision.BOND_CHANGED,
            bleTerminalDecision(
                cancelled = false,
                closed = false,
                bondTransitioned = false,
                currentBondChanged = true,
            ),
        )
    }

    @Test
    fun `disconnect between waits is latched for the next operation`() {
        val disconnect = BleUnexpectedDisconnectLatch()

        disconnect.recordUnexpectedDisconnect(closing = false)

        assertTrue(disconnect.isLatched())
        assertEquals(
            BleGattOperationStart.DISCONNECTED,
            bleGattOperationStartDecision(
                cancelled = false,
                closed = false,
                permissionRevoked = false,
                bondTransitioned = false,
                unexpectedDisconnect = disconnect.isLatched(),
                terminalized = false,
                hasGatt = true,
            ),
        )
    }

    @Test
    fun `disconnect between waits prevents successful finalization`() {
        val disconnect = BleUnexpectedDisconnectLatch()
        disconnect.recordUnexpectedDisconnect(closing = false)

        assertEquals(
            BleTerminalDecision.DISCONNECTED,
            bleTerminalDecision(
                cancelled = false,
                closed = false,
                bondTransitioned = false,
                currentBondChanged = false,
                unexpectedDisconnect = disconnect.isLatched(),
            ),
        )
    }

    @Test
    fun `intentional cleanup disconnect is not latched`() {
        val disconnect = BleUnexpectedDisconnectLatch()

        disconnect.recordUnexpectedDisconnect(closing = true)

        assertFalse(disconnect.isLatched())
    }

    @Test
    fun `read selection is allowlisted and requires read property`() {
        assertEquals(
            BleReadDecision.READ,
            bleReadDecision(
                serviceUuid = "00001800-0000-1000-8000-00805f9b34fb",
                characteristicUuid = "00002a00-0000-1000-8000-00805f9b34fb",
                readable = true,
                requiresEncryption = false,
            ),
        )
        assertEquals(
            BleReadDecision.SKIP,
            bleReadDecision(
                serviceUuid = "00001800-0000-1000-8000-00805f9b34fb",
                characteristicUuid = "00002a00-0000-1000-8000-00805f9b34fb",
                readable = false,
                requiresEncryption = false,
            ),
        )
        assertEquals(
            BleReadDecision.SKIP,
            bleReadDecision(
                serviceUuid = "00001234-0000-1000-8000-00805f9b34fb",
                characteristicUuid = "00005678-0000-1000-8000-00805f9b34fb",
                readable = true,
                requiresEncryption = false,
            ),
        )
    }

    @Test
    fun `session cleanup guards unregister disconnect and close independently`() {
        val calls = mutableListOf<String>()

        runBleSessionCleanup(
            unregisterReceiver = {
                calls += "unregister"
                throw SecurityException("permission revoked")
            },
            disconnectGatt = {
                calls += "disconnect"
                throw IllegalStateException("disconnect failed")
            },
            closeGatt = { calls += "close" },
        )

        assertEquals(listOf("unregister", "disconnect", "close"), calls)
    }

    @Test
    fun `bond receiver converts permission revocation to a terminal decision`() {
        assertEquals(
            BleBondReceiverDecision.PERMISSION_REVOKED,
            bleBondReceiverDecision(
                targetAddress = "AA:BB:CC:DD:EE:FF",
                initialBondState = 10,
            ) {
                throw SecurityException("permission revoked")
            },
        )
    }

    @Test
    fun `bond receiver accepts only a matching target transition`() {
        assertEquals(
            BleBondReceiverDecision.BOND_CHANGED,
            bleBondReceiverDecision(
                targetAddress = "AA:BB:CC:DD:EE:FF",
                initialBondState = 10,
            ) {
                BleBondEvent(address = "aa:bb:cc:dd:ee:ff", state = 12)
            },
        )
        assertEquals(
            BleBondReceiverDecision.IGNORE,
            bleBondReceiverDecision(
                targetAddress = "AA:BB:CC:DD:EE:FF",
                initialBondState = 10,
            ) {
                BleBondEvent(address = "11:22:33:44:55:66", state = 12)
            },
        )
    }

    @Test
    fun `characteristic traversal stops at the bound while iterating`() {
        val transformed = mutableListOf<Int>()

        val traversal = boundedBleTraversal(
            groups = listOf((0 until 40).toList()),
            limit = 32,
            itemsForGroup = { it },
        ) { _, item ->
            transformed += item
            item
        }

        assertEquals((0 until 32).toList(), traversal.items)
        assertEquals((0 until 32).toList(), transformed)
        assertTrue(traversal.truncated)
    }
}

private fun completedResult(requestId: String = "r1") = BleInvestigationResult(
    requestId = requestId,
    transport = "phone",
    mode = BleInvestigationMode.GATT,
    targetMac = "AA:BB:CC:DD:EE:FF",
    state = BleInvestigationState.COMPLETE,
    connectable = true,
    services = emptyList(),
    characteristics = emptyList(),
    reads = emptyMap(),
    bonded = false,
    encrypted = false,
    authenticationRequired = false,
    summary = "complete",
    error = null,
    truncated = false,
)

private fun request(id: String) = BleInvestigationRequest(
    requestId = id,
    target = BleInvestigationTarget(
        mode = BleInvestigationMode.GATT,
        mac = "AA:BB:CC:DD:EE:FF",
        entityKey = "mac:AA:BB:CC:DD:EE:FF",
        observedAtElapsedMs = 1_000,
        origin = PrivacyDetectionOrigin.ANDROID,
    ),
    route = BleInvestigationRoute.PHONE,
)

private class FakeInspector : BleInvestigator {
    val requests = mutableListOf<BleInvestigationRequest>()
    var result: BleInvestigationResult = completedResult()
    var block: CompletableDeferred<Unit>? = null
    var beforeProgressBlock: CompletableDeferred<Unit>? = null
    var firstCancelEntered: CompletableDeferred<Unit>? = null
    var firstCancelBlock: CompletableDeferred<Unit>? = null
    var cancelFailure: Exception? = null
    var cancelCount = 0
    private var progressCallback: (suspend (BleInvestigationResult) -> Unit)? = null

    override suspend fun investigate(
        request: BleInvestigationRequest,
        progress: suspend (BleInvestigationResult) -> Unit,
    ): BleInvestigationResult {
        requests += request
        progressCallback = progress
        beforeProgressBlock?.await()
        progress(result.copy(requestId = request.requestId, state = BleInvestigationState.CONNECTING))
        block?.await()
        return result.copy(requestId = request.requestId)
    }

    suspend fun emitProgress(result: BleInvestigationResult) {
        progressCallback?.invoke(result)
    }

    override suspend fun cancel() {
        cancelCount++
        if (cancelCount == 1) {
            firstCancelEntered?.complete(Unit)
            firstCancelBlock?.await()
        }
        block?.cancel()
        cancelFailure?.let { throw it }
    }
}
