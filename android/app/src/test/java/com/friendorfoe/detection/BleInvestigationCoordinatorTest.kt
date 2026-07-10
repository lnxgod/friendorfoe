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
    var firstCancelBlock: CompletableDeferred<Unit>? = null
    var cancelCount = 0
    private var progressCallback: (suspend (BleInvestigationResult) -> Unit)? = null

    override suspend fun investigate(
        request: BleInvestigationRequest,
        progress: suspend (BleInvestigationResult) -> Unit,
    ): BleInvestigationResult {
        requests += request
        progressCallback = progress
        progress(result.copy(requestId = request.requestId, state = BleInvestigationState.CONNECTING))
        block?.await()
        return result.copy(requestId = request.requestId)
    }

    suspend fun emitProgress(result: BleInvestigationResult) {
        progressCallback?.invoke(result)
    }

    override suspend fun cancel() {
        cancelCount++
        if (cancelCount == 1) firstCancelBlock?.await()
        block?.cancel()
    }
}
