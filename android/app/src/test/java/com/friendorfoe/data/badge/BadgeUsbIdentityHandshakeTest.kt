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
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class BadgeUsbIdentityHandshakeTest {

    @Test
    fun `badge hardware identity canonicalizes only exact usable colon MACs`() {
        assertEquals("A4:CF:12:34:56:78", canonicalBadgeHardwareId("A4:CF:12:34:56:78"))
        assertEquals("A4:CF:12:34:56:78", canonicalBadgeHardwareId("a4:cf:12:34:56:78"))

        listOf(
            "",
            " ",
            " A4:CF:12:34:56:78",
            "A4:CF:12:34:56:78 ",
            "A4-CF-12-34-56-78",
            "A4CF12345678",
            "A4:CF:12:34:56",
            "A4:CF:12:34:56:789",
            "A4:CF:12:34:56:GG",
            "00:00:00:00:00:00",
            "ff:ff:ff:ff:ff:ff",
        ).forEach { raw ->
            assertNull(raw, canonicalBadgeHardwareId(raw))
        }
    }

    @Test
    fun `USB uplink identity requires a usable hardware MAC`() {
        listOf(
            "",
            "not-a-mac",
            "00:00:00:00:00:00",
            "FF:FF:FF:FF:FF:FF",
        ).forEach { hardwareId ->
            val error = badgeUsbIdentityError(
                badgeStatus(version = "invalid-mac").copy(hardwareId = hardwareId),
            )
            assertTrue(error.orEmpty(), error.orEmpty().contains("hardware ID"))
        }

        assertEquals(
            BadgeUsbStatus.ERROR,
            badgeUsbHandshakeStatus(badgeStatus(version = "missing-mac").copy(hardwareId = "")),
        )
    }

    @Test
    fun `verified USB status enforces same canonical hardware identity`() {
        val sameBadge = badgeStatus(version = "later").copy(hardwareId = "a4:cf:12:34:56:78")
        assertNull(
            badgeUsbIdentityError(
                status = sameBadge,
                expectedHardwareId = "A4:CF:12:34:56:78",
            ),
        )
        assertNull(
            badgeUsbStatusFrameIdentityError(
                isStatusFrame = true,
                status = sameBadge,
                expectedHardwareId = "A4:CF:12:34:56:78",
            ),
        )

        val drift = badgeUsbStatusFrameIdentityError(
            isStatusFrame = true,
            status = sameBadge.copy(hardwareId = "A4:CF:12:34:56:79"),
            expectedHardwareId = "A4:CF:12:34:56:78",
        )
        assertEquals("USB hardware ID mismatch", drift)
        assertFalse(drift.orEmpty().contains("A4:CF:12:34:56:78"))
        assertFalse(drift.orEmpty().contains("A4:CF:12:34:56:79"))
    }

    @Test
    fun `handshake owner requires full badge identity and stores canonical MAC`() {
        val token = BadgeUsbAttachmentToken(
            generation = 11L,
            identity = BadgeUsbDeviceIdentity(101, "/dev/badge"),
        )
        val connection = Any()
        val endpoint = Any()
        val owner = badgeUsbOwnerKeyFromHandshake(
            status = badgeStatus(version = "verified").copy(
                hardwareId = "a4:cf:12:34:56:78",
            ),
            attachmentToken = token,
            lifecycleSession = 7L,
            connectionIdentity = connection,
            endpointIdentity = endpoint,
        )

        assertNotNull(owner)
        assertEquals("A4:CF:12:34:56:78", owner!!.hardwareId)
        assertEquals(token, owner.attachmentToken)
        assertTrue(owner.connectionIdentity === connection)
        assertTrue(owner.endpointIdentity === endpoint)
        assertNull(
            badgeUsbOwnerKeyFromHandshake(
                status = badgeStatus(version = "missing").copy(hardwareId = ""),
                attachmentToken = token,
                lifecycleSession = 7L,
                connectionIdentity = connection,
                endpointIdentity = endpoint,
            ),
        )
        assertNull(
            badgeUsbOwnerKeyFromHandshake(
                status = badgeStatus(version = "scanner").copy(
                    firmwareName = "scanner-s3-combo-fof_badge",
                ),
                attachmentToken = token,
                lifecycleSession = 7L,
                connectionIdentity = connection,
                endpointIdentity = endpoint,
            ),
        )
    }

    @Test
    fun `status liveness counter is trusted only for schema one`() {
        assertEquals(
            12L,
            badgeUsbStatusResponseCounter(
                BadgeControlStatus(
                    usbHealth = BadgeUsbHealthStatus(
                        schema = 1,
                        responsesCompleted = 12L,
                    ),
                ),
            ),
        )
        assertNull(
            badgeUsbStatusResponseCounter(
                BadgeControlStatus(
                    usbHealth = BadgeUsbHealthStatus(
                        schema = 0,
                        responsesCompleted = 12L,
                    ),
                ),
            ),
        )
        assertNull(
            badgeUsbStatusResponseCounter(
                BadgeControlStatus(
                    usbHealth = BadgeUsbHealthStatus(
                        schema = 2,
                        responsesCompleted = 12L,
                    ),
                ),
            ),
        )
        assertNull(badgeUsbStatusResponseCounter(null))
    }

    @Test
    fun `USB owner equality includes canonical badge hardware identity`() {
        val token = BadgeUsbAttachmentToken(
            generation = 11L,
            identity = BadgeUsbDeviceIdentity(101, "/dev/badge"),
        )
        val connection = Any()
        val endpoint = Any()
        val expected = BadgeUsbOwnerKey(
            attachmentToken = token,
            lifecycleSession = 7L,
            connectionIdentity = connection,
            endpointIdentity = endpoint,
            hardwareId = "A4:CF:12:34:56:78",
        )
        val same = expected.copy()
        val otherBadge = expected.copy(hardwareId = "A4:CF:12:34:56:79")

        assertTrue(badgeUsbOwnerKeysMatch(expected, same))
        assertFalse(badgeUsbOwnerKeysMatch(expected, otherBadge))
    }

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
        val token = BadgeUsbAttachmentToken(
            generation = 11L,
            identity = BadgeUsbDeviceIdentity(101, "/dev/bus/usb/001/011"),
        )
        assertFalse(
            badgeUsbConnectionCanBeReused(
                status = BadgeUsbStatus.CONNECTED,
                activeAttachmentToken = token,
                requestedAttachmentToken = token,
                activeLifecycleSession = 7L,
                requestedLifecycleSession = 8L,
                connectionOpen = true,
            ),
        )
        assertTrue(
            badgeUsbConnectionCanBeReused(
                status = BadgeUsbStatus.CONNECTING,
                activeAttachmentToken = token,
                requestedAttachmentToken = token,
                activeLifecycleSession = 8L,
                requestedLifecycleSession = 8L,
                connectionOpen = true,
            ),
        )
    }

    @Test
    fun `same human name B connects before queued A cleanup and remains active`() {
        val gate = BadgeUsbAttachmentGate()
        val sameHumanNameA = "Espressif USB JTAG"
        val sameHumanNameB = "Espressif USB JTAG"
        val identityA = BadgeUsbDeviceIdentity(101, "/dev/bus/usb/001/011")
        val identityB = BadgeUsbDeviceIdentity(202, "/dev/bus/usb/002/022")
        val connectionA = Any()
        val connectionB = Any()
        assertEquals(sameHumanNameA, sameHumanNameB)

        val tokenA = gate.select(identityA)
        assertTrue(gate.activate(tokenA))
        val invalidatedA = gate.invalidateMatching(identityA)
        assertEquals(tokenA, invalidatedA?.token)

        val tokenB = gate.select(identityB)
        gate.clearActive(tokenA)
        assertTrue(gate.activate(tokenB))

        assertFalse(
            badgeUsbCleanupOwnsActive(
                expectedAttachmentToken = tokenA,
                expectedConnection = connectionA,
                activeAttachmentToken = tokenB,
                activeConnection = connectionB,
            ),
        )
        gate.clearActive(tokenA)
        assertTrue(gate.isCurrentAndActive(tokenB))
    }

    @Test
    fun `matching A cleanup may finish before B is selected and connected`() {
        val gate = BadgeUsbAttachmentGate()
        val identityA = BadgeUsbDeviceIdentity(101, "/dev/bus/usb/001/011")
        val identityB = BadgeUsbDeviceIdentity(202, "/dev/bus/usb/002/022")
        val tokenA = gate.select(identityA)
        assertTrue(gate.activate(tokenA))

        val invalidatedA = gate.invalidateMatching(identityA)
        assertEquals(tokenA, invalidatedA?.token)
        gate.clearActive(tokenA)

        val tokenB = gate.select(identityB)
        assertTrue(gate.activate(tokenB))
        assertTrue(tokenB.generation > tokenA.generation)
        assertTrue(gate.isCurrentAndActive(tokenB))
    }

    @Test
    fun `stale A cleanup after B verification is an exact owner no-op`() {
        val gate = BadgeUsbAttachmentGate()
        val tokenA = gate.select(BadgeUsbDeviceIdentity(101, "/dev/a"))
        assertTrue(gate.activate(tokenA))
        val tokenB = gate.select(BadgeUsbDeviceIdentity(202, "/dev/b"))
        gate.clearActive(tokenA)
        assertTrue(gate.activate(tokenB))
        val connectionA = Any()
        val connectionB = Any()

        assertFalse(
            badgeUsbCleanupOwnsActive(tokenA, connectionA, tokenB, connectionB),
        )
        gate.clearActive(tokenA)
        assertTrue(gate.isCurrentAndActive(tokenB))
    }

    @Test
    fun `stale A permission result is ignored after B supersedes selection`() {
        val gate = BadgeUsbAttachmentGate()
        val identityA = BadgeUsbDeviceIdentity(101, "/dev/a")
        val identityB = BadgeUsbDeviceIdentity(202, "/dev/b")
        val tokenA = gate.select(identityA)
        val tokenB = gate.select(identityB)

        assertFalse(gate.acceptsPermission(tokenA, identityA))
        assertTrue(gate.acceptsPermission(tokenB, identityB))
        assertFalse(gate.acceptsPermission(tokenB, identityA))
    }

    @Test
    fun `expiry invalidates only its exact pending attachment generation`() {
        val gate = BadgeUsbAttachmentGate()
        val identity = BadgeUsbDeviceIdentity(101, "/dev/a")
        val tokenA = gate.select(identity)
        val tokenB = gate.select(identity, forceNewGeneration = true)

        assertNull(gate.invalidateExact(tokenA))
        assertTrue(gate.isCurrent(tokenB))
        assertEquals(tokenB, gate.invalidateExact(tokenB)?.token)
        assertFalse(gate.isCurrent(tokenB))
    }

    @Test
    fun `unrelated C detach does not invalidate active A`() {
        val gate = BadgeUsbAttachmentGate()
        val identityA = BadgeUsbDeviceIdentity(101, "/dev/a")
        val identityC = BadgeUsbDeviceIdentity(303, "/dev/c")
        val tokenA = gate.select(identityA)
        assertTrue(gate.activate(tokenA))

        assertNull(gate.invalidateMatching(identityC))
        assertTrue(gate.isCurrentAndActive(tokenA))
    }

    @Test
    fun `A detach while B permission is selected queues only A cleanup and preserves B`() {
        val gate = BadgeUsbAttachmentGate()
        val identityA = BadgeUsbDeviceIdentity(101, "/dev/a")
        val identityB = BadgeUsbDeviceIdentity(202, "/dev/b")
        val tokenA = gate.select(identityA)
        assertTrue(gate.activate(tokenA))
        val tokenB = gate.select(identityB)

        val invalidatedA = gate.invalidateMatching(identityA)

        assertEquals(tokenA, invalidatedA?.token)
        assertTrue(invalidatedA?.wasActive == true)
        assertTrue(gate.isCurrent(tokenB))
        assertFalse(gate.isCurrentAndActive(tokenB))
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
    fun `HTTP status cannot hide USB permission or actionable USB errors`() {
        listOf(
            BadgeUsbState(
                status = BadgeUsbStatus.PERMISSION_NEEDED,
                deviceName = "USB device",
                transportLabel = "USB-C",
                message = "USB access required",
            ),
            BadgeUsbState(
                status = BadgeUsbStatus.ERROR,
                deviceName = null,
                transportLabel = "USB-C",
                message = "Multiple Espressif USB devices found",
            ),
        ).forEach { usbPriorityState ->
            val reduced = reduceBadgeHttpStatus(
                current = usbPriorityState,
                response = badgeStatus(version = "debug-bridge"),
                connectedStatus = BadgeUsbStatus.DEBUG_BRIDGE_CONNECTED,
                deviceName = "FoF Debug Bridge",
                transportLabel = "Debug Bridge",
                connectedMessage = "Debug Bridge connected",
                usbConnectionOpen = false,
            )

            assertEquals(usbPriorityState, reduced)
            assertNull(reduced.controlStatus)
        }
    }

    @Test
    fun `HTTP status may populate a genuinely disconnected state`() {
        val disconnected = BadgeUsbState(
            status = BadgeUsbStatus.DISCONNECTED,
            message = "Attach a FoF badge over USB-C",
        )

        val reduced = reduceBadgeHttpStatus(
            current = disconnected,
            response = badgeStatus(version = "debug-bridge"),
            connectedStatus = BadgeUsbStatus.DEBUG_BRIDGE_CONNECTED,
            deviceName = "FoF Debug Bridge",
            transportLabel = "Debug Bridge",
            connectedMessage = "Debug Bridge connected",
            usbConnectionOpen = false,
        )

        assertEquals(BadgeUsbStatus.DEBUG_BRIDGE_CONNECTED, reduced.status)
        assertEquals("debug-bridge", reduced.controlStatus?.version)
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
    fun `reader failure retains USB error ownership and reports error`() {
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
        assertEquals("USB-C", failed.transportLabel)
        assertNull(failed.controlStatus)
        assertTrue(failed.message.contains("read exploded"))

        val lateHttp = reduceBadgeHttpStatus(
            current = failed,
            response = badgeStatus(version = "debug-bridge"),
            connectedStatus = BadgeUsbStatus.DEBUG_BRIDGE_CONNECTED,
            deviceName = "FoF Debug Bridge",
            transportLabel = "Debug Bridge",
            connectedMessage = "Debug Bridge connected",
            usbConnectionOpen = false,
        )
        assertEquals(failed, lateHttp)
    }

    @Test
    fun `terminal USB errors clear stale badge control status`() {
        val failed = reduceBadgeUsbTerminalError(
            current = BadgeUsbState(
                status = BadgeUsbStatus.CONNECTED,
                transportLabel = "USB-C",
                controlStatus = badgeStatus(version = "stale"),
            ),
            deviceName = "FoF badge",
            message = "Badge USB write failed",
        )

        assertEquals(BadgeUsbStatus.ERROR, failed.status)
        assertEquals("USB-C", failed.transportLabel)
        assertNull(failed.controlStatus)
    }

    @Test
    fun `every USB status frame is subject to identity validation`() {
        assertNull(
            badgeUsbStatusFrameIdentityError(
                isStatusFrame = false,
                status = BadgeControlStatus(),
            )
        )
        assertNull(
            badgeUsbStatusFrameIdentityError(
                isStatusFrame = true,
                status = badgeStatus(version = "verified"),
            )
        )
        assertTrue(
            badgeUsbStatusFrameIdentityError(
                isStatusFrame = true,
                status = badgeStatus(version = "drifted").copy(appProject = "scanner"),
            )?.contains("Unexpected USB app project") == true
        )
        assertEquals(
            "Malformed badge status",
            badgeUsbStatusFrameIdentityError(isStatusFrame = true, status = null),
        )
    }

    @Test
    fun `handshake timer retries before deadline and fails closed at deadline`() {
        assertEquals(
            BadgeUsbHandshakeTimerAction.RETRY,
            badgeUsbHandshakeTimerAction(
                ownsSession = true,
                nowElapsedMs = 14_499L,
                deadlineElapsedMs = 15_000L,
                retryWriteBudgetMs = 500L,
            ),
        )
        assertEquals(
            BadgeUsbHandshakeTimerAction.FAIL,
            badgeUsbHandshakeTimerAction(
                ownsSession = true,
                nowElapsedMs = 14_500L,
                deadlineElapsedMs = 15_000L,
                retryWriteBudgetMs = 500L,
            ),
        )
        assertEquals(
            BadgeUsbHandshakeTimerAction.STOP,
            badgeUsbHandshakeTimerAction(
                ownsSession = false,
                nowElapsedMs = 10_000L,
                deadlineElapsedMs = 15_000L,
                retryWriteBudgetMs = 500L,
            ),
        )
    }

    @Test
    fun `handshake retry delay is clamped before the bounded write budget`() {
        assertEquals(
            500L,
            badgeUsbHandshakeDelayMs(
                nowElapsedMs = 14_000L,
                deadlineElapsedMs = 15_000L,
                retryIntervalMs = 1_000L,
                retryWriteBudgetMs = 500L,
            ),
        )
        assertEquals(
            1L,
            badgeUsbHandshakeDelayMs(
                nowElapsedMs = 14_499L,
                deadlineElapsedMs = 15_000L,
                retryIntervalMs = 1_000L,
                retryWriteBudgetMs = 500L,
            ),
        )
        assertEquals(
            0L,
            badgeUsbHandshakeDelayMs(
                nowElapsedMs = 14_500L,
                deadlineElapsedMs = 15_000L,
                retryIntervalMs = 1_000L,
                retryWriteBudgetMs = 500L,
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
                "hardware_id":"A4:CF:12:34:56:78",
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
        hardwareId = "A4:CF:12:34:56:78",
    )
}
