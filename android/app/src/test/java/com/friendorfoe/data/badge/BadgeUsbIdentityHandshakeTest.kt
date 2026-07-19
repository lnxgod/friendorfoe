package com.friendorfoe.data.badge

import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicReference
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class BadgeUsbIdentityHandshakeTest {

    @Test
    @OptIn(ExperimentalCoroutinesApi::class)
    fun `queued command cannot cross from verified connection A to unverified B`() = runTest {
        val connectionMutex = Mutex()
        val verifiedConnectionA = Any()
        val verifiedEndpointA = Any()
        val replacementConnectionB = Any()
        val replacementEndpointB = Any()
        val activeConnection = AtomicReference<Any>(verifiedConnectionA)
        val activeEndpoint = AtomicReference<Any>(verifiedEndpointA)
        val activeSession = AtomicReference(7L)
        val status = AtomicReference(BadgeUsbStatus.CONNECTED)
        val actionRan = AtomicBoolean(false)

        connectionMutex.lock()
        val queuedAction = launch {
            connectionMutex.withLock {
                if (badgeUsbVerifiedWriteAllowed(
                        lifecycleActive = true,
                        status = status.get(),
                        transportLabel = "USB-C",
                        activeLifecycleSession = activeSession.get(),
                        verifiedLifecycleSession = 7L,
                        activeConnection = activeConnection.get(),
                        activeEndpoint = activeEndpoint.get(),
                        verifiedConnection = verifiedConnectionA,
                        verifiedEndpoint = verifiedEndpointA,
                    )
                ) {
                    actionRan.set(true)
                }
            }
        }
        runCurrent()

        activeConnection.set(replacementConnectionB)
        activeEndpoint.set(replacementEndpointB)
        activeSession.set(8L)
        status.set(BadgeUsbStatus.CONNECTING)
        connectionMutex.unlock()
        queuedAction.join()

        assertFalse(actionRan.get())
    }

    @Test
    fun `current exact verified owner may execute command`() {
        val connection = Any()
        val endpoint = Any()

        assertTrue(
            badgeUsbVerifiedWriteAllowed(
                lifecycleActive = true,
                status = BadgeUsbStatus.CONNECTED,
                transportLabel = "USB-C",
                activeLifecycleSession = 7L,
                verifiedLifecycleSession = 7L,
                activeConnection = connection,
                activeEndpoint = endpoint,
                verifiedConnection = connection,
                verifiedEndpoint = endpoint,
            ),
        )
    }

    @Test
    fun `stale reader rejection cannot close replacement connection`() {
        val staleConnectionA = Any()
        val replacementConnectionB = Any()

        assertFalse(
            badgeUsbHandshakeOwnsSession(
                lifecycleActive = true,
                status = BadgeUsbStatus.CONNECTING,
                transportLabel = "USB-C",
                expectedLifecycleSession = 7L,
                activeLifecycleSession = 8L,
                expectedConnection = staleConnectionA,
                activeConnection = replacementConnectionB,
            ),
        )
        assertTrue(
            badgeUsbHandshakeOwnsSession(
                lifecycleActive = true,
                status = BadgeUsbStatus.CONNECTING,
                transportLabel = "USB-C",
                expectedLifecycleSession = 8L,
                activeLifecycleSession = 8L,
                expectedConnection = replacementConnectionB,
                activeConnection = replacementConnectionB,
            ),
        )
    }

    @Test
    fun `reader drops remaining lines after synchronous rejection clears its session`() {
        val rejectedConnection = Any()

        assertFalse(
            badgeUsbReaderOwnsExactSession(
                lifecycleActive = true,
                expectedLifecycleSession = 7L,
                activeLifecycleSession = null,
                expectedConnection = rejectedConnection,
                activeConnection = null,
            ),
        )
    }

    @Test
    fun `same USB name from a new lifecycle is reopened instead of reusing stale session`() {
        assertFalse(
            badgeUsbConnectionCanBeReused(
                status = BadgeUsbStatus.CONNECTED,
                existingDeviceName = "Espressif USB JTAG",
                requestedDeviceName = "Espressif USB JTAG",
                activeLifecycleSession = 7L,
                requestedLifecycleSession = 8L,
                connectionOpen = true,
            ),
        )
        assertTrue(
            badgeUsbConnectionCanBeReused(
                status = BadgeUsbStatus.CONNECTING,
                existingDeviceName = "Espressif USB JTAG",
                requestedDeviceName = "Espressif USB JTAG",
                activeLifecycleSession = 8L,
                requestedLifecycleSession = 8L,
                connectionOpen = true,
            ),
        )
    }

    @Test
    fun `late HTTP response is ignored after USB session takes ownership`() {
        val usbStatus = badgeStatus(version = "usb-badge")
        val lateHttpStatus = badgeStatus(version = "other-http-badge")
        val usbState = BadgeUsbState(
            status = BadgeUsbStatus.CONNECTED,
            deviceName = "FoF badge",
            transportLabel = "USB-C",
            message = "Badge USB connected",
            controlStatus = usbStatus,
        )

        val reduced = reduceBadgeHttpStatus(
            current = usbState,
            response = lateHttpStatus,
            connectedStatus = BadgeUsbStatus.AP_CONNECTED,
            deviceName = "FoF Badge AP",
            transportLabel = "Badge AP",
            connectedMessage = "Badge AP connected",
            usbConnectionOpen = true,
        )

        assertEquals(usbState, reduced)
        assertEquals("usb-badge", reduced.controlStatus?.version)
    }

    @Test
    fun `late HTTP response is ignored while USB identity is still connecting`() {
        val connecting = BadgeUsbState(
            status = BadgeUsbStatus.CONNECTING,
            deviceName = "USB device",
            transportLabel = "USB-C",
            message = "Verifying badge USB identity",
            controlStatus = null,
        )

        val reduced = reduceBadgeHttpStatus(
            current = connecting,
            response = badgeStatus(version = "other-http-badge"),
            connectedStatus = BadgeUsbStatus.DEBUG_BRIDGE_CONNECTED,
            deviceName = "FoF Debug Bridge",
            transportLabel = "Debug Bridge",
            connectedMessage = "Debug Bridge connected",
            usbConnectionOpen = true,
        )

        assertEquals(connecting, reduced)
        assertNull(reduced.controlStatus)
    }

    @Test
    fun `normal USB disconnect clears transport and badge status`() {
        val connected = BadgeUsbState(
            status = BadgeUsbStatus.CONNECTED,
            deviceName = "FoF badge",
            transportLabel = "USB-C",
            controlStatus = badgeStatus(version = "usb-badge"),
        )

        val disconnected = reduceBadgeUsbDisconnected(connected, "Badge disconnected")

        assertEquals(BadgeUsbStatus.DISCONNECTED, disconnected.status)
        assertEquals("", disconnected.transportLabel)
        assertNull(disconnected.controlStatus)
    }

    @Test
    fun `reader failure clears transport status and reports error`() {
        val connected = BadgeUsbState(
            status = BadgeUsbStatus.CONNECTED,
            transportLabel = "USB-C",
            controlStatus = badgeStatus(version = "usb-badge"),
        )

        val failed = reduceBadgeUsbReaderFailure(
            connected,
            deviceName = "FoF badge",
            detail = "read exploded",
        )

        assertEquals(BadgeUsbStatus.ERROR, failed.status)
        assertEquals("", failed.transportLabel)
        assertNull(failed.controlStatus)
        assertTrue(failed.message.contains("read exploded"))
    }

    @Test
    fun `handshake timer retries before deadline and fails closed at deadline`() {
        assertEquals(
            BadgeUsbHandshakeTimerAction.RETRY,
            badgeUsbHandshakeTimerAction(
                ownsSession = true,
                nowElapsedMs = 14_999L,
                deadlineElapsedMs = 15_000L,
            ),
        )
        assertEquals(
            BadgeUsbHandshakeTimerAction.FAIL,
            badgeUsbHandshakeTimerAction(
                ownsSession = true,
                nowElapsedMs = 15_000L,
                deadlineElapsedMs = 15_000L,
            ),
        )
        assertEquals(
            BadgeUsbHandshakeTimerAction.STOP,
            badgeUsbHandshakeTimerAction(
                ownsSession = false,
                nowElapsedMs = 10_000L,
                deadlineElapsedMs = 15_000L,
            ),
        )
    }

    @Test
    fun `expected self asserted badge uplink identity enables USB command routing`() {
        val status = parseBadgeControlStatus(
            """{
                "firmware_name":"uplink-s3-fof_badge",
                "app_project":"fof_badge_uplink",
                "hardware_type":"seeed_xiao_esp32s3",
                "version":"0.64.76-badge-defcon34"
            }""".trimIndent(),
        )

        assertNull(badgeUsbIdentityError(status))
        assertEquals(BadgeUsbStatus.CONNECTED, badgeUsbHandshakeStatus(status))
        assertTrue(BadgeControlTransportPolicy.allowsCommandSurface(
            badgeUsbHandshakeStatus(status),
        ))
    }

    @Test
    fun `direct scanner identity is rejected`() {
        val scanner = parseBadgeControlStatus(
            """{
                "firmware_name":"scanner-s3-combo-fof_badge",
                "app_project":"fof_badge_scanner",
                "hardware_type":"seeed_xiao_esp32s3"
            }""".trimIndent(),
        )

        assertTrue(badgeUsbIdentityError(scanner).orEmpty().contains("firmware"))
        assertEquals(BadgeUsbStatus.ERROR, badgeUsbHandshakeStatus(scanner))
    }

    @Test
    fun `generic Espressif and malformed status are rejected`() {
        val generic = parseBadgeControlStatus(
            """{
                "firmware_name":"diagnostic-console",
                "app_project":"esp_probe",
                "hardware_type":"esp32s3"
            }""".trimIndent(),
        )

        assertEquals(BadgeUsbStatus.ERROR, badgeUsbHandshakeStatus(generic))
        assertEquals(BadgeUsbStatus.ERROR, badgeUsbHandshakeStatus(null))
        assertFalse(BadgeControlTransportPolicy.allowsCommandSurface(
            badgeUsbHandshakeStatus(null),
        ))
    }

    @Test
    fun `optional target must agree with badge uplink identity`() {
        val wrongTarget = parseBadgeControlStatus(
            """{
                "target":"scanner-s3-combo-fof_badge",
                "firmware_name":"uplink-s3-fof_badge",
                "app_project":"fof_badge_uplink",
                "hardware_type":"seeed_xiao_esp32s3"
            }""".trimIndent(),
        )

        assertTrue(badgeUsbIdentityError(wrongTarget).orEmpty().contains("target"))
        assertEquals(BadgeUsbStatus.ERROR, badgeUsbHandshakeStatus(wrongTarget))
    }

    @Test
    fun `handshake timeout fails only an unverified connecting session`() {
        assertEquals(
            BadgeUsbStatus.ERROR,
            badgeUsbHandshakeTimeoutStatus(BadgeUsbStatus.CONNECTING),
        )
        assertEquals(
            BadgeUsbStatus.CONNECTED,
            badgeUsbHandshakeTimeoutStatus(BadgeUsbStatus.CONNECTED),
        )
        assertFalse(BadgeControlTransportPolicy.allowsCommandSurface(
            BadgeUsbStatus.CONNECTING,
        ))
    }

    @Test
    fun `open USB handshake owns state against alternate transport pollers`() {
        assertTrue(badgeUsbSessionOwnsTransport(
            BadgeUsbStatus.CONNECTING,
            "USB-C",
            connectionOpen = true,
        ))
        assertTrue(badgeUsbSessionOwnsTransport(
            BadgeUsbStatus.CONNECTED,
            "USB-C",
            connectionOpen = true,
        ))
        assertFalse(badgeUsbSessionOwnsTransport(
            BadgeUsbStatus.CONNECTING,
            "USB-C",
            connectionOpen = false,
        ))
        assertFalse(badgeUsbSessionOwnsTransport(
            BadgeUsbStatus.AP_CONNECTED,
            "Badge AP",
            connectionOpen = true,
        ))
    }

    private fun badgeStatus(version: String): BadgeControlStatus = BadgeControlStatus(
        version = version,
        firmwareName = "uplink-s3-fof_badge",
        appProject = "fof_badge_uplink",
        hardwareType = "seeed_xiao_esp32s3",
    )
}
