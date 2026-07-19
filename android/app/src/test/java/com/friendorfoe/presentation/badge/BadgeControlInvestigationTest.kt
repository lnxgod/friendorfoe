package com.friendorfoe.presentation.badge

import com.friendorfoe.data.badge.BadgeControlStatus
import com.friendorfoe.data.badge.BadgeScannerStatus
import com.friendorfoe.data.badge.BadgeThreatEntity
import com.friendorfoe.data.badge.BadgeUsbState
import com.friendorfoe.data.badge.BadgeUsbStatus
import com.friendorfoe.detection.BleInvestigationMode
import com.friendorfoe.detection.BleInvestigationResult
import com.friendorfoe.detection.BleInvestigationRoute
import com.friendorfoe.detection.BleInvestigationState
import com.friendorfoe.detection.PrivacyDetectionOrigin
import java.io.File
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class BadgeControlInvestigationTest {

    @Test
    fun `fresh BLE entity maps to exact badge GATT request`() {
        val request = bleEntity(
            bssid = "C0:98:E5:00:00:01",
            snapshotAtElapsedMs = 100_000L,
            lastSeenSeconds = 2,
        ).badgeInvestigationRequest(
            nowElapsedMs = 101_000L,
            requestId = "badge-request-1",
        )

        assertNotNull(request)
        request!!
        assertEquals("badge-request-1", request.requestId)
        assertEquals(BleInvestigationRoute.BADGE, request.route)
        assertEquals(BleInvestigationMode.GATT, request.target.mode)
        assertEquals("C0:98:E5:00:00:01", request.target.mac)
        assertEquals(PrivacyDetectionOrigin.BADGE, request.target.origin)
        assertEquals(98_000L, request.target.observedAtElapsedMs)
    }

    @Test
    fun `GATT mapping rejects invalid MAC stale snapshot and stale entity flag`() {
        assertNull(bleEntity(bssid = "not-a-mac").badgeInvestigationTarget(10_000L))
        assertNull(
            bleEntity(
                snapshotAtElapsedMs = 100_000L,
                lastSeenSeconds = 31,
            ).badgeInvestigationTarget(100_000L),
        )
        assertNull(bleEntity(stale = true).badgeInvestigationTarget(10_000L))
    }

    @Test
    fun `pairing and BLE spam map to fresh badge passive capture`() {
        listOf("PAIRING_SPAM", "BLE-SPAM").forEach { code ->
            val target = bleEntity(
                bssid = "",
                code = code,
                snapshotAtElapsedMs = 20_000L,
                lastSeenSeconds = 1,
            ).badgeInvestigationTarget(21_000L)

            assertNotNull(code, target)
            assertEquals(BleInvestigationMode.PASSIVE_CAPTURE, target?.mode)
            assertNull(target?.mac)
            assertEquals(PrivacyDetectionOrigin.BADGE, target?.origin)
        }
    }

    @Test
    fun `availability requires verified USB and connected scanner slot zero`() {
        val connected = badgeState(BadgeUsbStatus.CONNECTED, slot = 0, scannerConnected = true)
        assertTrue(connected.badgeInvestigationAvailable())

        listOf(
            BadgeUsbStatus.AP_CONNECTED,
            BadgeUsbStatus.BLE_CONNECTED,
            BadgeUsbStatus.DEBUG_BRIDGE_CONNECTED,
            BadgeUsbStatus.CONNECTING,
            BadgeUsbStatus.DISCONNECTED,
        ).forEach { status ->
            assertFalse(status.name, badgeState(status, 0, true).badgeInvestigationAvailable())
        }
        assertFalse(badgeState(BadgeUsbStatus.CONNECTED, 0, false).badgeInvestigationAvailable())
        assertFalse(badgeState(BadgeUsbStatus.CONNECTED, 1, true).badgeInvestigationAvailable())
    }

    @Test
    fun `request IDs are unique printable and bounded for repository contract`() {
        val tracker = BadgeInvestigationSessionTracker()
        val first = tracker.nextRequestId(nowElapsedMs = 123_456L)
        val second = tracker.nextRequestId(nowElapsedMs = 123_456L)

        assertNotEquals(first, second)
        listOf(first, second).forEach { requestId ->
            assertTrue(requestId.length in 1..32)
            assertTrue(requestId.all { it.code in 0x21..0x7E })
        }
    }

    @Test
    fun `repository result is globally selected and stale previous result is hidden`() {
        val tracker = BadgeInvestigationSessionTracker()
        val oldId = tracker.nextRequestId(1_000L)
        val oldResult = result(oldId, BleInvestigationState.COMPLETE)

        assertNull(tracker.visibleResult(oldResult))
        tracker.recordRepositoryResult(oldId, repositoryResult = oldResult)
        assertEquals(oldResult, tracker.visibleResult(oldResult))

        val newId = tracker.nextRequestId(1_001L)
        tracker.recordRepositoryResult(
            newId,
            repositoryResult = result(newId, BleInvestigationState.QUEUED),
        )
        assertNull(tracker.visibleResult(oldResult))
        assertEquals(newId, tracker.visibleResult(result(newId, BleInvestigationState.QUEUED))?.requestId)
    }

    @Test
    fun `rapid duplicate starts reject active repository result and cancel derives its ID`() {
        val state = badgeState(BadgeUsbStatus.CONNECTED, 0, true)
        val tracker = BadgeInvestigationSessionTracker()
        val requestId = tracker.nextRequestId(1_000L)
        val active = result(requestId, BleInvestigationState.READING)
        tracker.recordRepositoryResult(requestId, repositoryResult = active)

        assertFalse(badgeInvestigationStartAllowed(state, active))
        assertEquals(requestId, tracker.activeCancelRequestId(active))
        assertTrue(badgeInvestigationStartAllowed(state, result(requestId, BleInvestigationState.COMPLETE)))
        assertNull(tracker.activeCancelRequestId(result(requestId, BleInvestigationState.COMPLETE)))
    }

    @Test
    fun `failed disconnect result stays visible while stale attempt cannot replace newer owner`() {
        val tracker = BadgeInvestigationSessionTracker()
        val second = tracker.nextRequestId(2_000L)
        val disconnected = result(
            requestId = second,
            state = BleInvestigationState.FAILED,
            error = "transport_disconnected",
        )
        assertTrue(
            tracker.recordRepositoryResult(
                requestId = second,
                repositoryResult = disconnected,
            ),
        )
        assertEquals("transport_disconnected", tracker.visibleResult(disconnected)?.error)
    }

    @Test
    fun `N way rejected starts cannot replace accepted repository owner out of order`() {
        val tracker = BadgeInvestigationSessionTracker()
        val acceptedId = tracker.nextRequestId(3_000L)
        val rejectedB = tracker.nextRequestId(3_000L)
        val rejectedC = tracker.nextRequestId(3_000L)
        val acceptedResult = result(acceptedId, BleInvestigationState.QUEUED)

        assertFalse(tracker.recordRepositoryResult(rejectedB, acceptedResult))
        assertFalse(tracker.recordRepositoryResult(rejectedC, acceptedResult))
        assertTrue(tracker.recordRepositoryResult(acceptedId, acceptedResult))
        assertEquals(acceptedId, tracker.currentRequestId.value)

        assertFalse(tracker.recordRepositoryResult(rejectedC, acceptedResult))
        assertFalse(tracker.recordRepositoryResult(rejectedB, acceptedResult))
        assertEquals(acceptedId, tracker.currentRequestId.value)
        assertEquals(acceptedId, tracker.activeCancelRequestId(acceptedResult))
    }

    @Test
    fun `descheduled accepted caller cannot replace a newer accepted repository owner`() {
        val tracker = BadgeInvestigationSessionTracker()
        val staleAcceptedId = tracker.nextRequestId(4_000L)
        val newerAcceptedId = tracker.nextRequestId(4_001L)
        val newerResult = result(newerAcceptedId, BleInvestigationState.QUEUED)

        assertTrue(tracker.recordRepositoryResult(newerAcceptedId, newerResult))
        assertFalse(tracker.recordRepositoryResult(staleAcceptedId, newerResult))
        assertEquals(newerAcceptedId, tracker.currentRequestId.value)
        assertEquals(newerAcceptedId, tracker.activeCancelRequestId(newerResult))
    }

    @Test
    fun `dedicated view model owns badge route without navigation cancellation`() {
        val viewModel = source("presentation/badge/BadgeControlViewModel.kt")
        val screen = source("presentation/badge/BadgeControlScreen.kt")

        assertTrue(viewModel.contains("repository.investigation"))
        assertTrue(viewModel.contains("repository.investigateBle"))
        assertTrue(viewModel.contains("repository.cancelBleInvestigation"))
        assertTrue(viewModel.contains("BleInvestigationRoute.BADGE"))
        assertFalse(viewModel.contains("override fun onCleared"))
        assertTrue(screen.contains("badgeInvestigationAvailable()"))
        assertTrue(screen.contains("transport_disconnected") || screen.contains("result.error"))
        assertFalse(screen.contains("initialFocusKey") && screen.contains("controlStatus?.entities.firstOrNull"))
    }

    private fun bleEntity(
        bssid: String = "AA:BB:CC:DD:EE:FF",
        code: String = "SERIAL",
        snapshotAtElapsedMs: Long = 10_000L,
        lastSeenSeconds: Int = 0,
        stale: Boolean = false,
    ) = BadgeThreatEntity(
        label = "Serial device",
        threatClass = "ble",
        category = "BLE",
        code = code,
        bssid = bssid,
        score = 90,
        ageSeconds = lastSeenSeconds,
        lastSeenSeconds = lastSeenSeconds,
        snapshotAtElapsedMs = snapshotAtElapsedMs,
        rssi = -45,
        events = 1,
        stale = stale,
    )

    private fun badgeState(
        status: BadgeUsbStatus,
        slot: Int,
        scannerConnected: Boolean,
    ) = BadgeUsbState(
        status = status,
        controlStatus = BadgeControlStatus(
            scanners = listOf(BadgeScannerStatus(slot = slot, connected = scannerConnected)),
        ),
    )

    private fun result(
        requestId: String,
        state: BleInvestigationState,
        error: String? = null,
    ) = BleInvestigationResult(
        requestId = requestId,
        transport = "badge-usb",
        mode = BleInvestigationMode.GATT,
        targetMac = "AA:BB:CC:DD:EE:FF",
        state = state,
        connectable = null,
        services = emptyList(),
        characteristics = emptyList(),
        reads = emptyMap(),
        bonded = false,
        encrypted = false,
        authenticationRequired = false,
        summary = state.name,
        error = error,
        truncated = false,
    )

    private fun source(relativePath: String): String {
        val candidates = listOf(
            File("src/main/java/com/friendorfoe/$relativePath"),
            File("app/src/main/java/com/friendorfoe/$relativePath"),
            File("android/app/src/main/java/com/friendorfoe/$relativePath"),
        )
        return candidates.firstOrNull(File::isFile)?.readText()
            ?: error("Unable to locate source: $relativePath")
    }
}
