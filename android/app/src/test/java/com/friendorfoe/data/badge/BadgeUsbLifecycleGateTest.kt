package com.friendorfoe.data.badge

import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicReference
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class BadgeUsbLifecycleGateTest {

    @Test
    fun `begin and end are idempotent`() {
        val gate = BadgeUsbLifecycleGate()

        assertTrue(gate.begin())
        assertFalse(gate.begin())
        assertTrue(gate.end())
        assertFalse(gate.end())
        assertTrue(gate.begin())
    }

    @Test
    fun `stop invalidates a queued connect from its session`() {
        val gate = BadgeUsbLifecycleGate()

        assertTrue(gate.begin())
        val queuedConnectSession = requireNotNull(gate.activeSession())

        assertTrue(gate.end(queuedConnectSession))
        assertFalse(gate.isActive(queuedConnectSession))
    }

    @Test
    fun `stop then start invalidates stale cleanup`() {
        val gate = BadgeUsbLifecycleGate()

        assertTrue(gate.begin())
        val staleCleanupSession = requireNotNull(gate.activeSession())
        assertTrue(gate.end(staleCleanupSession))
        assertTrue(gate.canClean(staleCleanupSession))

        assertTrue(gate.begin())
        val currentSession = requireNotNull(gate.activeSession())
        assertFalse(gate.canClean(staleCleanupSession))
        assertTrue(gate.isActive(currentSession))
    }

    @Test
    fun `reader errors require the active session and exact connection`() {
        assertTrue(badgeUsbReaderOwnsSession(lifecycleActive = true, activeConnectionMatches = true))
        assertFalse(badgeUsbReaderOwnsSession(lifecycleActive = false, activeConnectionMatches = true))
        assertFalse(badgeUsbReaderOwnsSession(lifecycleActive = true, activeConnectionMatches = false))
    }

    @Test
    @OptIn(ExperimentalCoroutinesApi::class)
    fun `stale reader action queued behind USB mutex is skipped`() = runTest {
        val mutex = Mutex()
        val ownsReader = AtomicBoolean(true)
        val actionRan = AtomicBoolean(false)

        mutex.lock()
        val queuedAction = launch {
            mutex.withBadgeUsbReaderOwner(ownsReader::get) {
                actionRan.set(true)
            }
        }
        runCurrent()
        ownsReader.set(false)
        mutex.unlock()
        queuedAction.join()

        assertFalse(actionRan.get())
    }

    @Test
    fun `current reader owner executes action under USB mutex`() = runTest {
        val actionRan = AtomicBoolean(false)

        val executed = Mutex().withBadgeUsbReaderOwner({ true }) {
            actionRan.set(true)
        }

        assertTrue(executed)
        assertTrue(actionRan.get())
    }

    @Test
    @OptIn(ExperimentalCoroutinesApi::class)
    fun `stale write failure queued behind USB mutex cannot terminate replacement owner`() = runTest {
        val mutex = Mutex()
        val ownerA = owner(
            attachmentGeneration = 1L,
            deviceId = 101,
            lifecycleSession = 7L,
            hardwareId = "A4:CF:12:34:56:78",
        )
        val ownerB = owner(
            attachmentGeneration = 2L,
            deviceId = 202,
            lifecycleSession = 8L,
            hardwareId = "A4:CF:12:34:56:79",
        )
        val activeOwner = AtomicReference<BadgeUsbOwnerKey?>(ownerA)
        val terminalActionRan = AtomicBoolean(false)

        mutex.lock()
        val queuedFailure = launch {
            mutex.withLock {
                val current = activeOwner.get()
                if (badgeUsbTerminalFailureOwnsExactSession(
                        expectedOwner = ownerA,
                        activeOwner = current,
                        lifecycleActive = true,
                        status = BadgeUsbStatus.CONNECTED,
                        transportLabel = "USB-C",
                        activeLifecycleSession = current?.lifecycleSession,
                        activeAttachmentToken = current?.attachmentToken,
                        attachmentCurrentAndActive = true,
                        activeConnection = current?.connectionIdentity,
                        activeEndpoint = current?.endpointIdentity,
                    )
                ) {
                    terminalActionRan.set(true)
                }
            }
        }
        runCurrent()

        activeOwner.set(ownerB)
        mutex.unlock()
        queuedFailure.join()

        assertFalse(terminalActionRan.get())
    }

    @Test
    fun `current exact write failure owner may terminate verified session`() {
        val owner = owner()

        assertTrue(
            badgeUsbTerminalFailureOwnsExactSession(
                expectedOwner = owner,
                activeOwner = owner,
                lifecycleActive = true,
                status = BadgeUsbStatus.CONNECTED,
                transportLabel = "USB-C",
                activeLifecycleSession = owner.lifecycleSession,
                activeAttachmentToken = owner.attachmentToken,
                attachmentCurrentAndActive = true,
                activeConnection = owner.connectionIdentity,
                activeEndpoint = owner.endpointIdentity,
            ),
        )
        assertFalse(
            badgeUsbTerminalFailureOwnsExactSession(
                expectedOwner = owner,
                activeOwner = owner.copy(endpointIdentity = Any()),
                lifecycleActive = true,
                status = BadgeUsbStatus.CONNECTED,
                transportLabel = "USB-C",
                activeLifecycleSession = owner.lifecycleSession,
                activeAttachmentToken = owner.attachmentToken,
                attachmentCurrentAndActive = true,
                activeConnection = owner.connectionIdentity,
                activeEndpoint = owner.endpointIdentity,
            ),
        )
    }

    @Test
    @OptIn(ExperimentalCoroutinesApi::class)
    fun `stale reader terminal queued behind USB mutex cannot close replacement B`() = runTest {
        val mutex = Mutex()
        val ownerA = owner(
            attachmentGeneration = 1L,
            deviceId = 101,
            lifecycleSession = 7L,
            hardwareId = "A4:CF:12:34:56:78",
        )
        val ownerB = owner(
            attachmentGeneration = 2L,
            deviceId = 202,
            lifecycleSession = 8L,
            hardwareId = "A4:CF:12:34:56:79",
        )
        val activeOwner = AtomicReference<BadgeUsbOwnerKey?>(ownerA)
        val terminalActionRan = AtomicBoolean(false)

        mutex.lock()
        val queuedFailure = launch {
            mutex.withLock {
                val current = activeOwner.get()
                if (badgeUsbReaderTerminalOwnsExactSession(
                        expectedLifecycleSession = ownerA.lifecycleSession,
                        expectedAttachmentToken = ownerA.attachmentToken,
                        expectedConnection = ownerA.connectionIdentity,
                        lifecycleActive = true,
                        status = BadgeUsbStatus.CONNECTED,
                        transportLabel = "USB-C",
                        activeLifecycleSession = current?.lifecycleSession,
                        activeAttachmentToken = current?.attachmentToken,
                        attachmentCurrentAndActive = true,
                        activeConnection = current?.connectionIdentity,
                        activeEndpoint = current?.endpointIdentity,
                        expectedVerifiedOwner = ownerA,
                        activeVerifiedOwner = current,
                    )
                ) {
                    terminalActionRan.set(true)
                }
            }
        }
        runCurrent()

        activeOwner.set(ownerB)
        mutex.unlock()
        queuedFailure.join()

        assertFalse(terminalActionRan.get())
    }

    @Test
    fun `exact current connecting and verified readers may terminalize`() {
        val connectingToken = BadgeUsbAttachmentToken(
            generation = 3L,
            identity = BadgeUsbDeviceIdentity(303, "/dev/connecting"),
        )
        val connectingConnection = Any()
        assertTrue(
            badgeUsbReaderTerminalOwnsExactSession(
                expectedLifecycleSession = 9L,
                expectedAttachmentToken = connectingToken,
                expectedConnection = connectingConnection,
                lifecycleActive = true,
                status = BadgeUsbStatus.CONNECTING,
                transportLabel = "USB-C",
                activeLifecycleSession = 9L,
                activeAttachmentToken = connectingToken,
                attachmentCurrentAndActive = true,
                activeConnection = connectingConnection,
                activeEndpoint = Any(),
                expectedVerifiedOwner = null,
                activeVerifiedOwner = null,
            ),
        )

        val verified = owner()
        assertTrue(
            badgeUsbReaderTerminalOwnsExactSession(
                expectedLifecycleSession = verified.lifecycleSession,
                expectedAttachmentToken = verified.attachmentToken,
                expectedConnection = verified.connectionIdentity,
                lifecycleActive = true,
                status = BadgeUsbStatus.CONNECTED,
                transportLabel = "USB-C",
                activeLifecycleSession = verified.lifecycleSession,
                activeAttachmentToken = verified.attachmentToken,
                attachmentCurrentAndActive = true,
                activeConnection = verified.connectionIdentity,
                activeEndpoint = verified.endpointIdentity,
                expectedVerifiedOwner = verified,
                activeVerifiedOwner = verified,
            ),
        )
        assertFalse(
            badgeUsbReaderTerminalOwnsExactSession(
                expectedLifecycleSession = verified.lifecycleSession,
                expectedAttachmentToken = verified.attachmentToken,
                expectedConnection = verified.connectionIdentity,
                lifecycleActive = true,
                status = BadgeUsbStatus.CONNECTED,
                transportLabel = "USB-C",
                activeLifecycleSession = verified.lifecycleSession,
                activeAttachmentToken = verified.attachmentToken,
                attachmentCurrentAndActive = true,
                activeConnection = verified.connectionIdentity,
                activeEndpoint = Any(),
                expectedVerifiedOwner = verified,
                activeVerifiedOwner = verified,
            ),
        )
    }

    private fun owner(
        attachmentGeneration: Long = 1L,
        deviceId: Int = 101,
        lifecycleSession: Long = 7L,
        hardwareId: String = "A4:CF:12:34:56:78",
    ): BadgeUsbOwnerKey = BadgeUsbOwnerKey(
        attachmentToken = BadgeUsbAttachmentToken(
            generation = attachmentGeneration,
            identity = BadgeUsbDeviceIdentity(deviceId, "/dev/$deviceId"),
        ),
        lifecycleSession = lifecycleSession,
        connectionIdentity = Any(),
        endpointIdentity = Any(),
        hardwareId = hardwareId,
    )
}
