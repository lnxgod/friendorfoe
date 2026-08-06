package com.friendorfoe.data.badge

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertSame
import org.junit.Assert.assertTrue
import org.junit.Test
import java.util.concurrent.CountDownLatch
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicInteger

class BadgeUsbLivenessTest {

    @Test
    fun `liveness constants are frozen`() {
        assertEquals(3, USB_STATUS_MAX_CONSECUTIVE_MISSES)
        assertEquals(15_250L, USB_READER_SILENCE_TIMEOUT_MS)
        assertEquals(30, USB_RECONNECT_MAX_ATTEMPTS)
        assertEquals(500L, USB_RECONNECT_INTERVAL_MS)
    }

    @Test
    fun `stale poll job cleanup cannot erase atomically installed replacement`() {
        val slot = BadgeUsbAtomicSlot<Any>()
        val jobA = Any()
        val jobB = Any()

        assertNull(slot.replace(jobA))
        assertSame(jobA, slot.current())

        assertSame(jobA, slot.replace(jobB))
        assertFalse(slot.clear(jobA))
        assertSame(jobB, slot.current())

        assertSame(jobB, slot.take())
        assertNull(slot.current())
        assertFalse(slot.clear(jobB))
        assertNull(slot.take())
    }

    @Test
    fun `status poll requires a response accepted after the ticket began`() {
        val owner = owner()
        val gate = BadgeUsbStatusPollGate()
        gate.bind(owner, initialResponsesCompleted = 4L)

        assertTrue(gate.recordStatus(owner, responsesCompleted = 5L))
        val staleSnapshotTicket = gate.beginPoll(owner)
        assertNotNull(staleSnapshotTicket)
        assertEquals(5L, staleSnapshotTicket!!.baselineResponsesCompleted)
        assertEquals(BadgeUsbStatusPollDecision.MISS, gate.finishPoll(staleSnapshotTicket, owner))

        val freshTicket = gate.beginPoll(owner)!!
        assertTrue(gate.recordStatus(owner, responsesCompleted = 6L))
        assertEquals(BadgeUsbStatusPollDecision.FRESH, gate.finishPoll(freshTicket, owner))
    }

    @Test
    fun `first two status misses survive and third terminates while fresh resets misses`() {
        val owner = owner()
        val gate = BadgeUsbStatusPollGate()
        gate.bind(owner, initialResponsesCompleted = 10L)

        assertEquals(BadgeUsbStatusPollDecision.MISS, finishWithoutStatus(gate, owner))
        assertEquals(BadgeUsbStatusPollDecision.MISS, finishWithoutStatus(gate, owner))

        val fresh = gate.beginPoll(owner)!!
        assertTrue(gate.recordStatus(owner, responsesCompleted = 11L))
        assertEquals(BadgeUsbStatusPollDecision.FRESH, gate.finishPoll(fresh, owner))

        assertEquals(BadgeUsbStatusPollDecision.MISS, finishWithoutStatus(gate, owner))
        assertEquals(BadgeUsbStatusPollDecision.MISS, finishWithoutStatus(gate, owner))
        assertEquals(BadgeUsbStatusPollDecision.TERMINATE, finishWithoutStatus(gate, owner))
    }

    @Test
    fun `schema one status requires a strictly newer nonnull response counter`() {
        val owner = owner()
        val gate = BadgeUsbStatusPollGate()
        gate.bind(owner, initialResponsesCompleted = 8L)

        val same = gate.beginPoll(owner)!!
        assertTrue(gate.recordStatus(owner, responsesCompleted = 8L))
        assertEquals(BadgeUsbStatusPollDecision.MISS, gate.finishPoll(same, owner))

        val lower = gate.beginPoll(owner)!!
        assertTrue(gate.recordStatus(owner, responsesCompleted = 7L))
        assertEquals(BadgeUsbStatusPollDecision.MISS, gate.finishPoll(lower, owner))

        val missing = gate.beginPoll(owner)!!
        assertTrue(gate.recordStatus(owner, responsesCompleted = null))
        assertEquals(BadgeUsbStatusPollDecision.TERMINATE, gate.finishPoll(missing, owner))
    }

    @Test
    fun `schema one binding never downgrades or regresses its trusted counter baseline`() {
        val owner = owner()

        val missingCounterGate = BadgeUsbStatusPollGate()
        missingCounterGate.bind(owner, initialResponsesCompleted = 8L)
        repeat(2) {
            val ticket = missingCounterGate.beginPoll(owner)!!
            assertTrue(missingCounterGate.recordStatus(owner, responsesCompleted = null))
            assertEquals(
                BadgeUsbStatusPollDecision.MISS,
                missingCounterGate.finishPoll(ticket, owner),
            )
        }

        val regressedCounterGate = BadgeUsbStatusPollGate()
        regressedCounterGate.bind(owner, initialResponsesCompleted = 8L)
        val lower = regressedCounterGate.beginPoll(owner)!!
        assertTrue(regressedCounterGate.recordStatus(owner, responsesCompleted = 7L))
        assertEquals(BadgeUsbStatusPollDecision.MISS, regressedCounterGate.finishPoll(lower, owner))
        val originalValue = regressedCounterGate.beginPoll(owner)!!
        assertTrue(regressedCounterGate.recordStatus(owner, responsesCompleted = 8L))
        assertEquals(
            BadgeUsbStatusPollDecision.MISS,
            regressedCounterGate.finishPoll(originalValue, owner),
        )
    }

    @Test
    fun `legacy schema zero status uses only local accepted generation`() {
        val owner = owner()
        val gate = BadgeUsbStatusPollGate()
        gate.bind(owner, initialResponsesCompleted = null)

        assertTrue(gate.recordStatus(owner, responsesCompleted = null))
        assertEquals(BadgeUsbStatusPollDecision.MISS, finishWithoutStatus(gate, owner))

        val ticket = gate.beginPoll(owner)!!
        assertTrue(gate.recordStatus(owner, responsesCompleted = null))
        assertEquals(BadgeUsbStatusPollDecision.FRESH, gate.finishPoll(ticket, owner))
    }

    @Test
    fun `binding B makes late A status and ticket stale without charging B`() {
        val ownerA = owner(hardwareId = "A4:CF:12:34:56:78")
        val ownerB = owner(
            attachmentGeneration = 2L,
            deviceId = 202,
            devicePath = "/dev/b",
            lifecycleSession = 8L,
            hardwareId = "A4:CF:12:34:56:79",
        )
        val gate = BadgeUsbStatusPollGate()
        gate.bind(ownerA, initialResponsesCompleted = 2L)
        val ticketA = gate.beginPoll(ownerA)!!

        gate.bind(ownerB, initialResponsesCompleted = 10L)
        assertFalse(gate.recordStatus(ownerA, responsesCompleted = 3L))
        assertEquals(BadgeUsbStatusPollDecision.STALE_OWNER, gate.finishPoll(ticketA, ownerA))
        assertEquals(BadgeUsbStatusPollDecision.MISS, finishWithoutStatus(gate, ownerB))
        assertEquals(BadgeUsbStatusPollDecision.MISS, finishWithoutStatus(gate, ownerB))
        assertEquals(BadgeUsbStatusPollDecision.TERMINATE, finishWithoutStatus(gate, ownerB))
    }

    @Test
    fun `same OS path with changed owner components is stale and cannot mutate misses`() {
        val connection = Any()
        val endpoint = Any()
        val exact = owner(connection = connection, endpoint = endpoint)
        val changedOwners = listOf(
            exact.copy(
                attachmentToken = exact.attachmentToken.copy(generation = 2L),
            ),
            exact.copy(lifecycleSession = exact.lifecycleSession + 1L),
            exact.copy(connectionIdentity = Any()),
            exact.copy(endpointIdentity = Any()),
            exact.copy(hardwareId = "A4:CF:12:34:56:79"),
        )
        val gate = BadgeUsbStatusPollGate()
        gate.bind(exact, initialResponsesCompleted = 1L)
        val ticket = gate.beginPoll(exact)!!

        changedOwners.forEach { changed ->
            assertFalse(gate.recordStatus(changed, responsesCompleted = 2L))
            assertEquals(
                BadgeUsbStatusPollDecision.STALE_OWNER,
                gate.finishPoll(ticket, changed),
            )
        }
        assertEquals(BadgeUsbStatusPollDecision.MISS, gate.finishPoll(ticket, exact))
    }

    @Test
    fun `superseded poll ticket is stale and exact clear owns the binding`() {
        val exact = owner()
        val other = owner(hardwareId = "A4:CF:12:34:56:79")
        val gate = BadgeUsbStatusPollGate()
        gate.bind(exact, initialResponsesCompleted = 1L)
        val oldTicket = gate.beginPoll(exact)!!
        val currentTicket = gate.beginPoll(exact)!!

        assertNotEquals(oldTicket.pollSequence, currentTicket.pollSequence)
        assertEquals(
            BadgeUsbStatusPollDecision.STALE_OWNER,
            gate.finishPoll(oldTicket, exact),
        )
        assertEquals(BadgeUsbStatusPollDecision.MISS, gate.finishPoll(currentTicket, exact))
        assertFalse(gate.clear(other))
        assertTrue(gate.clear(exact))
        assertNull(gate.beginPoll(exact))
        assertFalse(gate.recordStatus(exact, responsesCompleted = 2L))
    }

    @Test
    fun `forged schema one ticket cannot claim preexisting status or consume real ticket`() {
        val owner = owner()
        val gate = BadgeUsbStatusPollGate()
        gate.bind(owner, initialResponsesCompleted = 8L)
        assertEquals(BadgeUsbStatusPollDecision.MISS, finishWithoutStatus(gate, owner))

        assertTrue(gate.recordStatus(owner, responsesCompleted = 9L))
        val realTicket = gate.beginPoll(owner)!!
        val forgedTicket = realTicket.copy(
            acceptedStatusGeneration = realTicket.acceptedStatusGeneration - 1L,
            baselineResponsesCompleted = realTicket.baselineResponsesCompleted!! - 1L,
        )

        assertEquals(
            BadgeUsbStatusPollDecision.STALE_OWNER,
            gate.finishPoll(forgedTicket, owner),
        )
        assertEquals(BadgeUsbStatusPollDecision.MISS, gate.finishPoll(realTicket, owner))
        assertEquals(BadgeUsbStatusPollDecision.TERMINATE, finishWithoutStatus(gate, owner))
    }

    @Test
    fun `forged legacy ticket cannot claim preexisting status or consume real ticket`() {
        val owner = owner()
        val gate = BadgeUsbStatusPollGate()
        gate.bind(owner, initialResponsesCompleted = null)
        assertEquals(BadgeUsbStatusPollDecision.MISS, finishWithoutStatus(gate, owner))

        assertTrue(gate.recordStatus(owner, responsesCompleted = null))
        val realTicket = gate.beginPoll(owner)!!
        val forgedTicket = realTicket.copy(
            acceptedStatusGeneration = realTicket.acceptedStatusGeneration - 1L,
        )

        assertEquals(
            BadgeUsbStatusPollDecision.STALE_OWNER,
            gate.finishPoll(forgedTicket, owner),
        )
        assertEquals(BadgeUsbStatusPollDecision.MISS, gate.finishPoll(realTicket, owner))
        assertEquals(BadgeUsbStatusPollDecision.TERMINATE, finishWithoutStatus(gate, owner))
    }

    @Test
    fun `reader silence expires exactly at timeout and nonpositive reads do not reset`() {
        val gate = BadgeUsbReaderSilenceGate()
        gate.start(nowElapsedMs = 1_000L)

        assertFalse(gate.recordRead(byteCount = 0, nowElapsedMs = 2_000L))
        assertFalse(gate.recordRead(byteCount = -1, nowElapsedMs = 3_000L))
        assertFalse(gate.isExpired(nowElapsedMs = 16_249L))
        assertTrue(gate.isExpired(nowElapsedMs = 16_250L))
    }

    @Test
    fun `positive read resets reader silence deadline`() {
        val gate = BadgeUsbReaderSilenceGate()
        gate.start(nowElapsedMs = 1_000L)

        assertTrue(gate.recordRead(byteCount = 1, nowElapsedMs = 2_000L))
        assertFalse(gate.isExpired(nowElapsedMs = 17_249L))
        assertTrue(gate.isExpired(nowElapsedMs = 17_250L))
    }

    @Test
    fun `positive read at or after reader deadline cannot reset expiration`() {
        val exactDeadline = BadgeUsbReaderSilenceGate()
        exactDeadline.start(nowElapsedMs = 1_000L)
        assertFalse(exactDeadline.recordRead(byteCount = 1, nowElapsedMs = 16_250L))
        assertTrue(exactDeadline.isExpired(nowElapsedMs = 16_250L))

        val afterDeadline = BadgeUsbReaderSilenceGate()
        afterDeadline.start(nowElapsedMs = 1_000L)
        assertFalse(afterDeadline.recordRead(byteCount = 1, nowElapsedMs = 16_251L))
        assertTrue(afterDeadline.isExpired(nowElapsedMs = 16_251L))
    }

    @Test
    fun `reader expiration is terminal until explicit restart`() {
        val gate = BadgeUsbReaderSilenceGate()
        gate.start(nowElapsedMs = 1_000L)
        assertTrue(gate.isExpired(nowElapsedMs = 16_250L))
        assertFalse(gate.recordRead(byteCount = 4, nowElapsedMs = 16_251L))
        assertTrue(gate.isExpired(nowElapsedMs = 16_251L))

        gate.start(nowElapsedMs = 20_000L)
        assertTrue(gate.recordRead(byteCount = 4, nowElapsedMs = 20_001L))
        assertFalse(gate.isExpired(nowElapsedMs = 35_250L))
        assertTrue(gate.isExpired(nowElapsedMs = 35_251L))
    }

    @Test
    fun `positive read just before reader deadline resets successfully`() {
        val gate = BadgeUsbReaderSilenceGate()
        gate.start(nowElapsedMs = 1_000L)
        assertTrue(gate.recordRead(byteCount = 1, nowElapsedMs = 16_249L))
        assertFalse(gate.isExpired(nowElapsedMs = 31_498L))
        assertTrue(gate.isExpired(nowElapsedMs = 31_499L))
    }

    @Test
    fun `reader silence rejects invalid timeout and fails closed on backward time`() {
        assertTrue(runCatching { BadgeUsbReaderSilenceGate(timeoutMs = 0L) }.isFailure)
        assertTrue(runCatching { BadgeUsbReaderSilenceGate(timeoutMs = -1L) }.isFailure)

        val gate = BadgeUsbReaderSilenceGate()
        gate.start(nowElapsedMs = 1_000L)
        assertFalse(gate.recordRead(byteCount = 4, nowElapsedMs = 999L))
        assertTrue(gate.isExpired(nowElapsedMs = 1_000L))
    }

    @Test
    fun `reconnect ticket permits exactly thirty owner bound attempts`() {
        val oldOwner = owner()
        val gate = BadgeUsbReconnectGate()
        val ticket = gate.bind(oldOwner)

        assertNotNull(ticket)
        ticket!!
        assertEquals(oldOwner.lifecycleSession, ticket.lifecycleSession)
        assertEquals(oldOwner.hardwareId, ticket.hardwareId)
        assertTrue(badgeUsbOwnerKeysMatch(oldOwner, ticket.oldOwner))
        repeat(USB_RECONNECT_MAX_ATTEMPTS) {
            assertEquals(
                "attempt ${it + 1}",
                BadgeUsbReconnectDecision.RETRY,
                gate.nextAttempt(ticket, lifecycleSession = oldOwner.lifecycleSession),
            )
        }
        assertEquals(
            BadgeUsbReconnectDecision.EXPIRED,
            gate.nextAttempt(ticket, lifecycleSession = oldOwner.lifecycleSession),
        )
    }

    @Test
    fun `Lite reconnect ticket preserves its exact trusted product family`() {
        val oldOwner = owner(
            hardwareId = BADGE_LITE_OWNER_ID,
        ).copy(productFamily = BadgeUsbProductFamily.BACKEND_LITE)
        val gate = BadgeUsbReconnectGate()
        val ticket = gate.bind(oldOwner)

        assertNotNull(ticket)
        ticket!!
        assertEquals(BADGE_LITE_OWNER_ID, ticket.hardwareId)
        assertEquals(BadgeUsbProductFamily.BACKEND_LITE, ticket.productFamily)
        assertEquals(
            BadgeUsbProductFamily.BACKEND_LITE,
            gate.expectedProductFamily(ticket, oldOwner.lifecycleSession),
        )
        assertFalse(
            gate.isCurrent(
                ticket.copy(productFamily = BadgeUsbProductFamily.FULL_SIZE),
                oldOwner.lifecycleSession,
            ),
        )
    }

    @Test
    fun `wrong reconnect lifecycle and forged owner ticket are stale without consuming attempt`() {
        val oldOwner = owner()
        val gate = BadgeUsbReconnectGate()
        val ticket = gate.bind(oldOwner)!!

        assertEquals(
            BadgeUsbReconnectDecision.STALE,
            gate.nextAttempt(ticket, lifecycleSession = oldOwner.lifecycleSession + 1L),
        )
        listOf(
            oldOwner.copy(
                attachmentToken = oldOwner.attachmentToken.copy(generation = 2L),
            ),
            oldOwner.copy(connectionIdentity = Any()),
            oldOwner.copy(endpointIdentity = Any()),
            oldOwner.copy(hardwareId = "A4:CF:12:34:56:79"),
        ).forEach { forgedOwner ->
            assertEquals(
                BadgeUsbReconnectDecision.STALE,
                gate.nextAttempt(
                    ticket.copy(oldOwner = forgedOwner),
                    lifecycleSession = oldOwner.lifecycleSession,
                ),
            )
        }
        assertEquals(
            BadgeUsbReconnectDecision.RETRY,
            gate.nextAttempt(ticket, lifecycleSession = oldOwner.lifecycleSession),
        )
    }

    @Test
    fun `supersede and clear keep old reconnect A from affecting B`() {
        val ownerA = owner(hardwareId = "A4:CF:12:34:56:78")
        val ownerB = owner(
            attachmentGeneration = 2L,
            deviceId = 202,
            devicePath = "/dev/b",
            lifecycleSession = 8L,
            hardwareId = "A4:CF:12:34:56:79",
        )
        val gate = BadgeUsbReconnectGate()
        val ticketA = gate.bind(ownerA)!!
        val ticketB = gate.bind(ownerB)!!

        assertNotEquals(ticketA.generation, ticketB.generation)
        assertEquals(
            BadgeUsbReconnectDecision.STALE,
            gate.nextAttempt(ticketA, lifecycleSession = ownerA.lifecycleSession),
        )
        assertFalse(gate.clear(ticketA))
        assertEquals(
            BadgeUsbReconnectDecision.RETRY,
            gate.nextAttempt(ticketB, lifecycleSession = ownerB.lifecycleSession),
        )
        assertTrue(gate.clear(ticketB))
        assertEquals(
            BadgeUsbReconnectDecision.STALE,
            gate.nextAttempt(ticketB, lifecycleSession = ownerB.lifecycleSession),
        )
    }

    @Test
    fun `reconnect bind rejects a noncanonical hardware identity`() {
        val gate = BadgeUsbReconnectGate()
        assertNull(gate.bind(owner(hardwareId = "a4:cf:12:34:56:78")))
        assertNull(gate.bind(owner(hardwareId = "00:00:00:00:00:00")))
    }

    @Test
    fun `reconnect currentness and expected ID checks are exact and non-consuming`() {
        val oldOwner = owner()
        val gate = BadgeUsbReconnectGate()
        val ticket = gate.bind(oldOwner)!!

        repeat(3) {
            assertTrue(gate.isCurrent(ticket, oldOwner.lifecycleSession))
            assertEquals(
                oldOwner.hardwareId,
                gate.expectedHardwareId(ticket, oldOwner.lifecycleSession),
            )
        }
        assertFalse(gate.isCurrent(ticket, oldOwner.lifecycleSession + 1L))
        assertNull(gate.expectedHardwareId(ticket, oldOwner.lifecycleSession + 1L))

        listOf(
            ticket.copy(generation = ticket.generation + 1L),
            ticket.copy(hardwareId = "A4:CF:12:34:56:79"),
            ticket.copy(oldOwner = oldOwner.copy(connectionIdentity = Any())),
        ).forEach { forged ->
            assertFalse(gate.isCurrent(forged, oldOwner.lifecycleSession))
            assertNull(gate.expectedHardwareId(forged, oldOwner.lifecycleSession))
        }

        repeat(USB_RECONNECT_MAX_ATTEMPTS) {
            assertEquals(
                BadgeUsbReconnectDecision.RETRY,
                gate.nextAttempt(ticket, oldOwner.lifecycleSession),
            )
        }
        assertEquals(
            BadgeUsbReconnectDecision.EXPIRED,
            gate.nextAttempt(ticket, oldOwner.lifecycleSession),
        )
    }

    @Test
    fun `reconnect A cleanup cannot erase CAS published operation B`() {
        data class FakeReconnectOperation(
            val ticket: BadgeUsbReconnectTicket,
            val job: Any,
        )

        val gate = BadgeUsbReconnectGate()
        val ticketA = gate.bind(owner())!!
        val operationA = FakeReconnectOperation(ticketA, Any())
        val slot = BadgeUsbAtomicSlot<FakeReconnectOperation>()
        assertNull(slot.replace(operationA))

        val ticketB = gate.bind(
            owner(
                attachmentGeneration = 2L,
                deviceId = 202,
                devicePath = "/dev/b",
                hardwareId = "A4:CF:12:34:56:79",
            ),
        )!!
        val operationB = FakeReconnectOperation(ticketB, Any())
        assertSame(operationA, slot.replace(operationB))

        assertFalse(slot.clear(operationA))
        assertSame(operationB, slot.current())
        assertSame(operationB, slot.take())
        assertNull(slot.current())
    }

    @Test
    fun `reconnect success and expiry have one exact atomic winner`() {
        val successFirst = BadgeUsbAtomicSlot<Any>()
        val operationA = Any()
        successFirst.replace(operationA)
        assertTrue("success owns A", successFirst.clear(operationA))
        assertFalse("expiry is stale after success", successFirst.clear(operationA))

        val expiryFirst = BadgeUsbAtomicSlot<Any>()
        val operationB = Any()
        expiryFirst.replace(operationB)
        assertTrue("expiry owns B", expiryFirst.clear(operationB))
        assertFalse("success is stale after expiry", expiryFirst.clear(operationB))
    }

    @Test
    fun `terminal barrier wins delayed publication race and rejects both publications`() {
        val gate = BadgeUsbReconnectOperationGate()
        val publicationReady = CountDownLatch(1)
        val releasePublication = CountDownLatch(1)
        val connectingPublished = AtomicBoolean(false)
        val verifiedPublished = AtomicBoolean(false)
        val executor = Executors.newSingleThreadExecutor()
        try {
            val delayedPublication = executor.submit<Boolean> {
                publicationReady.countDown()
                check(releasePublication.await(2, TimeUnit.SECONDS))
                gate.publishConnectingIfActive {
                    connectingPublished.set(true)
                }
            }

            assertTrue(publicationReady.await(2, TimeUnit.SECONDS))
            assertTrue("ambiguity or detach must synchronously win", gate.tryTerminalize())
            releasePublication.countDown()

            assertFalse(delayedPublication.get(2, TimeUnit.SECONDS))
            assertFalse(connectingPublished.get())
            assertFalse(
                gate.completeHandshakeAndClearIfActive(
                    operationIsCurrent = { true },
                    fullCommit = {
                        verifiedPublished.set(true)
                        true
                    },
                    completion = {},
                ),
            )
            assertFalse(verifiedPublished.get())
        } finally {
            releasePublication.countDown()
            executor.shutdownNow()
        }
    }

    @Test
    fun `verified publication wins exactly once before later terminal cleanup`() {
        val gate = BadgeUsbReconnectOperationGate()
        val verifiedPublications = AtomicInteger(0)

        assertTrue(
            gate.completeHandshakeAndClearIfActive(
                operationIsCurrent = { true },
                fullCommit = {
                    verifiedPublications.incrementAndGet()
                    true
                },
                completion = {},
            ),
        )
        assertFalse(gate.tryTerminalize())
        assertFalse(
            gate.completeHandshakeAndClearIfActive(
                operationIsCurrent = { true },
                fullCommit = {
                    verifiedPublications.incrementAndGet()
                    true
                },
                completion = {},
            ),
        )
        assertFalse(gate.publishConnectingIfActive { error("completed operation republished") })
        assertEquals(1, verifiedPublications.get())
    }

    @Test
    fun `completion keeps exact operation published until owner publication finishes`() {
        val gate = BadgeUsbReconnectOperationGate()
        val operationSlot = BadgeUsbAtomicSlot<Any>()
        val operation = Any()
        operationSlot.replace(operation)
        val publicationEntered = CountDownLatch(1)
        val genericRequestCheckedSlot = CountDownLatch(1)
        val genericAttachmentMutation = AtomicBoolean(false)
        val ownerPublished = AtomicBoolean(false)
        val executor = Executors.newSingleThreadExecutor()
        try {
            val genericRequest = executor.submit {
                check(publicationEntered.await(2, TimeUnit.SECONDS))
                if (operationSlot.current() == null) {
                    genericAttachmentMutation.set(true)
                }
                genericRequestCheckedSlot.countDown()
            }

            assertTrue(
                gate.completeHandshakeAndClearIfActive(
                    operationIsCurrent = { operationSlot.current() === operation },
                    fullCommit = {
                        publicationEntered.countDown()
                        check(genericRequestCheckedSlot.await(2, TimeUnit.SECONDS))
                        ownerPublished.set(true)
                        true
                    },
                    completion = {
                        assertTrue(operationSlot.clear(operation))
                    },
                ),
            )
            genericRequest.get(2, TimeUnit.SECONDS)
            assertFalse(genericAttachmentMutation.get())
            assertTrue(ownerPublished.get())
            assertNull(operationSlot.current())
        } finally {
            publicationEntered.countDown()
            genericRequestCheckedSlot.countDown()
            executor.shutdownNow()
        }
    }

    @Test
    fun `verified failure handoff and detach have one selection barrier winner`() {
        val barrier = BadgeUsbReconnectSelectionGate()
        val failureEntered = CountDownLatch(1)
        val releaseFailure = CountDownLatch(1)
        val detachStarted = CountDownLatch(1)
        val exactOwnerPresent = AtomicBoolean(true)
        val reconnectPublished = AtomicBoolean(false)
        val detachSawReconnect = AtomicBoolean(false)
        val executor = Executors.newFixedThreadPool(2)
        try {
            val failure = executor.submit {
                barrier.withBarrier {
                    check(exactOwnerPresent.get())
                    failureEntered.countDown()
                    check(releaseFailure.await(2, TimeUnit.SECONDS))
                    exactOwnerPresent.set(false)
                    reconnectPublished.set(true)
                }
            }
            assertTrue(failureEntered.await(2, TimeUnit.SECONDS))
            val detach = executor.submit {
                detachStarted.countDown()
                barrier.withBarrier {
                    detachSawReconnect.set(reconnectPublished.get())
                    exactOwnerPresent.set(false)
                }
            }
            assertTrue(detachStarted.await(2, TimeUnit.SECONDS))
            releaseFailure.countDown()
            failure.get(2, TimeUnit.SECONDS)
            detach.get(2, TimeUnit.SECONDS)

            assertTrue(reconnectPublished.get())
            assertTrue(detachSawReconnect.get())

            val detachFirstBarrier = BadgeUsbReconnectSelectionGate()
            val detachFirstOwner = AtomicBoolean(true)
            val detachFirstReconnect = AtomicBoolean(false)
            detachFirstBarrier.withBarrier { detachFirstOwner.set(false) }
            detachFirstBarrier.withBarrier {
                if (detachFirstOwner.get()) detachFirstReconnect.set(true)
            }
            assertFalse(detachFirstReconnect.get())
        } finally {
            releaseFailure.countDown()
            executor.shutdownNow()
        }
    }

    @Test
    fun `ambiguity snapshot A completion is stale not a generic error`() {
        val operationA = Any()

        assertEquals(
            BadgeUsbReconnectAmbiguityDecision.STALE_NO_OP,
            badgeUsbReconnectAmbiguityDecision(
                expectedOperationIdentity = operationA,
                expectedGeneration = 41L,
                expectedLifecycleSession = 7L,
                currentOperationIdentity = null,
                currentGeneration = null,
                currentLifecycleSession = null,
                lifecycleActive = true,
            ),
        )
    }

    @Test
    fun `ambiguity snapshot A cannot terminalize replacement B`() {
        val operationA = Any()
        val operationB = Any()

        assertEquals(
            BadgeUsbReconnectAmbiguityDecision.STALE_NO_OP,
            badgeUsbReconnectAmbiguityDecision(
                expectedOperationIdentity = operationA,
                expectedGeneration = 41L,
                expectedLifecycleSession = 7L,
                currentOperationIdentity = operationB,
                currentGeneration = 42L,
                currentLifecycleSession = 7L,
                lifecycleActive = true,
            ),
        )
        assertEquals(
            BadgeUsbReconnectAmbiguityDecision.TERMINALIZE_EXPECTED,
            badgeUsbReconnectAmbiguityDecision(
                expectedOperationIdentity = operationA,
                expectedGeneration = 41L,
                expectedLifecycleSession = 7L,
                currentOperationIdentity = operationA,
                currentGeneration = 41L,
                currentLifecycleSession = 7L,
                lifecycleActive = true,
            ),
        )
        assertEquals(
            BadgeUsbReconnectAmbiguityDecision.REPORT_GENERIC,
            badgeUsbReconnectAmbiguityDecision(
                expectedOperationIdentity = null,
                expectedGeneration = null,
                expectedLifecycleSession = 7L,
                currentOperationIdentity = null,
                currentGeneration = null,
                currentLifecycleSession = null,
                lifecycleActive = true,
            ),
        )
    }

    @Test
    fun `handshake completion keeps ambiguity barred through full state and poller commit`() {
        val gate = BadgeUsbReconnectOperationGate()
        val operationSlot = BadgeUsbAtomicSlot<Any>()
        val operation = Any()
        operationSlot.replace(operation)
        val commitEntered = CountDownLatch(1)
        val ambiguityCheckedSlot = CountDownLatch(1)
        val ownerPublished = AtomicBoolean(false)
        val connectedStatePublished = AtomicBoolean(false)
        val controlStatusPublished = AtomicBoolean(false)
        val connectedMessagePublished = AtomicBoolean(false)
        val pollerRegistered = AtomicBoolean(false)
        val ambiguityOverwroteState = AtomicBoolean(false)
        val executor = Executors.newSingleThreadExecutor()
        try {
            val ambiguity = executor.submit {
                check(commitEntered.await(2, TimeUnit.SECONDS))
                if (operationSlot.current() == null) {
                    ambiguityOverwroteState.set(true)
                    connectedStatePublished.set(false)
                }
                ambiguityCheckedSlot.countDown()
            }

            assertTrue(
                gate.completeHandshakeAndClearIfActive(
                    operationIsCurrent = { operationSlot.current() === operation },
                    fullCommit = {
                        commitEntered.countDown()
                        check(ambiguityCheckedSlot.await(2, TimeUnit.SECONDS))
                        ownerPublished.set(true)
                        connectedStatePublished.set(true)
                        controlStatusPublished.set(true)
                        connectedMessagePublished.set(true)
                        pollerRegistered.set(true)
                        true
                    },
                    completion = {
                        assertTrue(ownerPublished.get())
                        assertTrue(connectedStatePublished.get())
                        assertTrue(controlStatusPublished.get())
                        assertTrue(connectedMessagePublished.get())
                        assertTrue(pollerRegistered.get())
                        assertTrue(operationSlot.clear(operation))
                    },
                ),
            )
            ambiguity.get(2, TimeUnit.SECONDS)
            assertFalse(ambiguityOverwroteState.get())
            assertTrue(connectedStatePublished.get())
            assertNull(operationSlot.current())
        } finally {
            commitEntered.countDown()
            ambiguityCheckedSlot.countDown()
            executor.shutdownNow()
        }
    }

    @Test
    fun `captured reconnect request cannot mutate attachment after completion`() {
        val gate = BadgeUsbReconnectOperationGate()
        val enumerationReady = CountDownLatch(1)
        val releaseEnumeration = CountDownLatch(1)
        val attachmentMutations = AtomicInteger(0)
        val executor = Executors.newSingleThreadExecutor()
        try {
            val staleRequest = executor.submit<String?> {
                enumerationReady.countDown()
                check(releaseEnumeration.await(2, TimeUnit.SECONDS))
                gate.prepareIfActive {
                    attachmentMutations.incrementAndGet()
                    "attachment-B"
                }
            }

            assertTrue(enumerationReady.await(2, TimeUnit.SECONDS))
            assertTrue(
                gate.completeHandshakeAndClearIfActive(
                    operationIsCurrent = { true },
                    fullCommit = { true },
                    completion = {},
                ),
            )
            releaseEnumeration.countDown()

            assertNull(staleRequest.get(2, TimeUnit.SECONDS))
            assertEquals(0, attachmentMutations.get())
        } finally {
            releaseEnumeration.countDown()
            executor.shutdownNow()
        }
    }

    @Test
    fun `attachment invalidation wins delayed active publication race`() {
        val attachmentGate = BadgeUsbAttachmentGate()
        val token = attachmentGate.select(BadgeUsbDeviceIdentity(101, "/dev/a"))
        val publicationReady = CountDownLatch(1)
        val releasePublication = CountDownLatch(1)
        val activePublished = AtomicBoolean(false)
        val executor = Executors.newSingleThreadExecutor()
        try {
            val delayedPublication = executor.submit<Boolean> {
                publicationReady.countDown()
                check(releasePublication.await(2, TimeUnit.SECONDS))
                attachmentGate.activateAndPublishIfCurrent(token) {
                    activePublished.set(true)
                }
            }

            assertTrue(publicationReady.await(2, TimeUnit.SECONDS))
            assertNotNull(attachmentGate.invalidateExact(token))
            releasePublication.countDown()

            assertFalse(delayedPublication.get(2, TimeUnit.SECONDS))
            assertFalse(activePublished.get())
        } finally {
            releasePublication.countDown()
            executor.shutdownNow()
        }
    }

    @Test
    fun `bound reconnect attempt rejects a different candidate before token selection`() {
        val attemptIdentity = BadgeUsbDeviceIdentity(101, "/dev/a")
        val otherIdentity = BadgeUsbDeviceIdentity(202, "/dev/b")

        assertEquals(
            BadgeUsbReconnectCandidatePreparation.REJECT_BEFORE_SELECTION,
            badgeUsbReconnectCandidatePreparation(
                operationActive = true,
                attemptIdentity = attemptIdentity,
                attemptConnectionBound = true,
                candidateIdentity = otherIdentity,
            ),
        )
        assertEquals(
            BadgeUsbReconnectCandidatePreparation.REUSE_ATTEMPT,
            badgeUsbReconnectCandidatePreparation(
                operationActive = true,
                attemptIdentity = attemptIdentity,
                attemptConnectionBound = true,
                candidateIdentity = attemptIdentity,
            ),
        )
        assertEquals(
            BadgeUsbReconnectCandidatePreparation.REJECT_BEFORE_SELECTION,
            badgeUsbReconnectCandidatePreparation(
                operationActive = false,
                attemptIdentity = null,
                attemptConnectionBound = false,
                candidateIdentity = otherIdentity,
            ),
        )
    }

    @Test
    fun `reconnect detach matching and expiry ownership are exact`() {
        val oldOwner = owner()
        val ticket = BadgeUsbReconnectGate().bind(oldOwner)!!

        assertTrue(
            badgeUsbReconnectDetachMatches(ticket, oldOwner.attachmentToken.identity),
        )
        assertFalse(
            badgeUsbReconnectDetachMatches(
                ticket,
                BadgeUsbDeviceIdentity(202, "/dev/unrelated"),
            ),
        )
        assertTrue(
            badgeUsbReconnectExpiryOwnsConnecting(
                ticket = ticket,
                reconnectAttachmentToken = oldOwner.attachmentToken,
                reconnectConnectionIdentity = oldOwner.connectionIdentity,
                lifecycleActive = true,
                status = BadgeUsbStatus.CONNECTING,
                transportLabel = "USB-C",
                activeLifecycleSession = ticket.lifecycleSession,
                activeAttachmentToken = oldOwner.attachmentToken,
                activeConnection = oldOwner.connectionIdentity,
                activeVerifiedOwner = null,
            ),
        )
        assertFalse(
            badgeUsbReconnectExpiryOwnsConnecting(
                ticket = ticket,
                reconnectAttachmentToken = oldOwner.attachmentToken,
                reconnectConnectionIdentity = oldOwner.connectionIdentity,
                lifecycleActive = true,
                status = BadgeUsbStatus.CONNECTED,
                transportLabel = "USB-C",
                activeLifecycleSession = ticket.lifecycleSession,
                activeAttachmentToken = oldOwner.attachmentToken,
                activeConnection = oldOwner.connectionIdentity,
                activeVerifiedOwner = oldOwner,
            ),
        )
        assertFalse(
            badgeUsbReconnectExpiryOwnsConnecting(
                ticket = ticket,
                reconnectAttachmentToken = oldOwner.attachmentToken,
                reconnectConnectionIdentity = oldOwner.connectionIdentity,
                lifecycleActive = true,
                status = BadgeUsbStatus.CONNECTING,
                transportLabel = "USB-C",
                activeLifecycleSession = ticket.lifecycleSession + 1L,
                activeAttachmentToken = oldOwner.attachmentToken,
                activeConnection = oldOwner.connectionIdentity,
                activeVerifiedOwner = null,
            ),
        )
        assertFalse(
            "A newer connecting attachment in the same lifecycle must survive A expiry",
            badgeUsbReconnectExpiryOwnsConnecting(
                ticket = ticket,
                reconnectAttachmentToken = oldOwner.attachmentToken,
                reconnectConnectionIdentity = oldOwner.connectionIdentity,
                lifecycleActive = true,
                status = BadgeUsbStatus.CONNECTING,
                transportLabel = "USB-C",
                activeLifecycleSession = ticket.lifecycleSession,
                activeAttachmentToken = oldOwner.attachmentToken.copy(generation = 2L),
                activeConnection = Any(),
                activeVerifiedOwner = null,
            ),
        )
        assertFalse(
            "A pending permission attempt has no connection for expiry to close",
            badgeUsbReconnectExpiryOwnsConnecting(
                ticket = ticket,
                reconnectAttachmentToken = oldOwner.attachmentToken,
                reconnectConnectionIdentity = null,
                lifecycleActive = true,
                status = BadgeUsbStatus.CONNECTING,
                transportLabel = "USB-C",
                activeLifecycleSession = ticket.lifecycleSession,
                activeAttachmentToken = oldOwner.attachmentToken,
                activeConnection = oldOwner.connectionIdentity,
                activeVerifiedOwner = null,
            ),
        )
    }

    @Test
    fun `reconnect candidate policy preserves zero candidate recovery and rejects ambiguity`() {
        assertEquals(
            BadgeUsbReconnectCandidateAction.PRESERVE_RECOVERY,
            badgeUsbReconnectCandidateAction(candidateCount = 0, preserveRecovery = true),
        )
        assertEquals(
            BadgeUsbReconnectCandidateAction.NORMAL_REFRESH,
            badgeUsbReconnectCandidateAction(candidateCount = 0, preserveRecovery = false),
        )
        assertEquals(
            BadgeUsbReconnectCandidateAction.CONNECT_ONE,
            badgeUsbReconnectCandidateAction(candidateCount = 1, preserveRecovery = true),
        )
        assertEquals(
            BadgeUsbReconnectCandidateAction.FAIL_AMBIGUOUS,
            badgeUsbReconnectCandidateAction(candidateCount = 2, preserveRecovery = true),
        )
    }

    @Test
    fun `selection stamp rejects null operation ABA stale ambiguity`() {
        val selectionGate = BadgeUsbReconnectSelectionGate()
        val operationSlot = BadgeUsbAtomicSlot<Any>()
        val staleEnumerationStamp = selectionStamp(selectionGate)
        val connectedGeneration = AtomicInteger(1)
        val staleAmbiguityPublished = AtomicBoolean(false)

        val reconnect = Any()
        selectionGate.withBarrier {
            operationSlot.replace(reconnect)
            advanceSelectionStamp(selectionGate)
            connectedGeneration.set(2)
            assertTrue(operationSlot.clear(reconnect))
            advanceSelectionStamp(selectionGate)
        }

        assertNull(operationSlot.current())
        if (selectionStampIsCurrent(selectionGate, staleEnumerationStamp)) {
            staleAmbiguityPublished.set(true)
            connectedGeneration.set(-1)
        }

        assertFalse(staleAmbiguityPublished.get())
        assertEquals(2, connectedGeneration.get())
    }

    @Test
    fun `selection stamp rejects null operation ABA stale one candidate selection`() {
        val selectionGate = BadgeUsbReconnectSelectionGate()
        val operationSlot = BadgeUsbAtomicSlot<Any>()
        val staleEnumerationStamp = selectionStamp(selectionGate)
        val healthyAttachmentGeneration = AtomicInteger(1)
        val staleAttachmentSelected = AtomicBoolean(false)

        val reconnect = Any()
        selectionGate.withBarrier {
            operationSlot.replace(reconnect)
            advanceSelectionStamp(selectionGate)
            healthyAttachmentGeneration.set(2)
            assertTrue(operationSlot.clear(reconnect))
            advanceSelectionStamp(selectionGate)
        }

        assertNull(operationSlot.current())
        if (selectionStampIsCurrent(selectionGate, staleEnumerationStamp)) {
            staleAttachmentSelected.set(true)
            healthyAttachmentGeneration.set(-1)
        }

        assertFalse(staleAttachmentSelected.get())
        assertEquals(2, healthyAttachmentGeneration.get())
    }

    @Test
    fun `process receiver is claimed once across stop A start B and stale scheduler`() {
        val receiverGate = newReceiverLifetimeGate()
        val lifecycleGate = BadgeUsbLifecycleGate()
        val registrations = AtomicInteger(0)
        assertTrue(lifecycleGate.begin())
        val lifecycleA = lifecycleGate.activeSession()!!
        assertTrue(registerReceiverOnce(receiverGate) { registrations.incrementAndGet() })
        assertTrue(lifecycleGate.end(lifecycleA))

        val executor = Executors.newFixedThreadPool(2)
        try {
            val staleScheduler = executor.submit<Boolean> {
                registerReceiverOnce(receiverGate) { registrations.incrementAndGet() }
            }
            assertTrue(lifecycleGate.begin())
            val lifecycleB = lifecycleGate.activeSession()!!
            val startB = executor.submit<Boolean> {
                registerReceiverOnce(receiverGate) { registrations.incrementAndGet() }
            }

            assertFalse(staleScheduler.get(2, TimeUnit.SECONDS))
            assertFalse(startB.get(2, TimeUnit.SECONDS))
            assertTrue(lifecycleGate.isActive(lifecycleB))
            assertTrue(receiverIsRegistered(receiverGate))
            assertEquals(1, registrations.get())
        } finally {
            executor.shutdownNow()
        }
    }

    @Test
    fun `receiver callbacks are inert while stopped and stale A callback cannot mutate B`() {
        val lifecycleGate = BadgeUsbLifecycleGate()
        val mutations = AtomicInteger(0)
        assertTrue(lifecycleGate.begin())
        val lifecycleA = lifecycleGate.activeSession()!!
        assertTrue(lifecycleGate.end(lifecycleA))

        lifecycleGate.activeSession()?.let { callbackSession ->
            if (lifecycleGate.isActive(callbackSession)) mutations.incrementAndGet()
        }
        assertEquals(0, mutations.get())

        assertTrue(lifecycleGate.begin())
        val lifecycleB = lifecycleGate.activeSession()!!
        if (lifecycleGate.isActive(lifecycleA)) mutations.incrementAndGet()

        assertTrue(lifecycleGate.isActive(lifecycleB))
        assertEquals(0, mutations.get())
    }

    @Test
    fun `terminal families revalidate A inside connection and selection barrier before mutating B`() {
        listOf(
            "HANDSHAKE_TIMEOUT",
            "IDENTITY_REJECTION",
            "UNVERIFIED_READER_FAILURE",
            "ASYNC_STOP",
        ).forEach { family ->
            val connectionLock = Any()
            val selectionGate = BadgeUsbReconnectSelectionGate()
            val lifecycleGate = BadgeUsbLifecycleGate()
            val activeConnection = BadgeUsbAtomicSlot<Any>()
            val connectionA = Any()
            val connectionB = Any()
            assertTrue(lifecycleGate.begin())
            val lifecycleA = lifecycleGate.activeSession()!!
            activeConnection.replace(connectionA)

            val staleTerminalValidated = CountDownLatch(1)
            val releaseStaleTerminal = CountDownLatch(1)
            val visibleLifecycle = AtomicInteger(lifecycleA.toInt())
            val executor = Executors.newSingleThreadExecutor()
            try {
                val staleTerminal = executor.submit {
                    assertTrue(lifecycleGate.isActive(lifecycleA))
                    assertSame(connectionA, activeConnection.current())
                    staleTerminalValidated.countDown()
                    check(releaseStaleTerminal.await(2, TimeUnit.SECONDS))
                    synchronized(connectionLock) {
                        selectionGate.withBarrier {
                            if (lifecycleGate.isActive(lifecycleA) &&
                                activeConnection.current() === connectionA
                            ) {
                                visibleLifecycle.set(-1)
                            }
                        }
                    }
                }
                assertTrue(staleTerminalValidated.await(2, TimeUnit.SECONDS))
                assertTrue(lifecycleGate.end(lifecycleA))
                assertTrue(lifecycleGate.begin())
                val lifecycleB = lifecycleGate.activeSession()!!
                activeConnection.replace(connectionB)
                visibleLifecycle.set(lifecycleB.toInt())
                releaseStaleTerminal.countDown()

                staleTerminal.get(2, TimeUnit.SECONDS)
                assertEquals(family, lifecycleB.toInt(), visibleLifecycle.get())
                assertSame(family, connectionB, activeConnection.current())
            } finally {
                releaseStaleTerminal.countDown()
                executor.shutdownNow()
            }
        }
    }

    @Test
    fun `stale permission denial cannot overwrite replacement lifecycle`() {
        val selectionGate = BadgeUsbReconnectSelectionGate()
        val operationSlot = BadgeUsbAtomicSlot<Any>()
        val oldValidationComplete = CountDownLatch(1)
        val releaseOldCommit = CountDownLatch(1)
        val oldLifecycleActive = AtomicBoolean(true)
        val oldAttachmentCurrent = AtomicBoolean(true)
        val verifiedOwnerPresent = AtomicBoolean(false)
        val visibleLifecycle = AtomicInteger(1)
        val executor = Executors.newSingleThreadExecutor()
        try {
            val oldDenial = executor.submit {
                oldValidationComplete.countDown()
                check(releaseOldCommit.await(2, TimeUnit.SECONDS))
                selectionGate.withBarrier {
                    if (oldLifecycleActive.get() && oldAttachmentCurrent.get() &&
                        operationSlot.current() == null && !verifiedOwnerPresent.get()
                    ) {
                        visibleLifecycle.set(-1)
                    }
                }
            }
            assertTrue(oldValidationComplete.await(2, TimeUnit.SECONDS))
            selectionGate.withBarrier {
                oldLifecycleActive.set(false)
                oldAttachmentCurrent.set(false)
                verifiedOwnerPresent.set(true)
                visibleLifecycle.set(2)
            }
            releaseOldCommit.countDown()

            oldDenial.get(2, TimeUnit.SECONDS)
            assertEquals(2, visibleLifecycle.get())
        } finally {
            releaseOldCommit.countDown()
            executor.shutdownNow()
        }
    }

    @Test
    fun `stale connect and endpoint error commits cannot overwrite replacement lifecycle`() {
        listOf("CONNECTING", "ENDPOINT_ERROR").forEach { staleCommit ->
            val selectionGate = BadgeUsbReconnectSelectionGate()
            val oldValidationComplete = CountDownLatch(1)
            val releaseOldCommit = CountDownLatch(1)
            val oldLifecycleActive = AtomicBoolean(true)
            val oldAttachmentCurrent = AtomicBoolean(true)
            val visibleLifecycle = AtomicInteger(1)
            val executor = Executors.newSingleThreadExecutor()
            try {
                val oldConnect = executor.submit {
                    oldValidationComplete.countDown()
                    check(releaseOldCommit.await(2, TimeUnit.SECONDS))
                    selectionGate.withBarrier {
                        if (oldLifecycleActive.get() && oldAttachmentCurrent.get()) {
                            visibleLifecycle.set(if (staleCommit == "CONNECTING") -1 else -2)
                        }
                    }
                }
                assertTrue(oldValidationComplete.await(2, TimeUnit.SECONDS))
                selectionGate.withBarrier {
                    oldLifecycleActive.set(false)
                    oldAttachmentCurrent.set(false)
                    visibleLifecycle.set(2)
                }
                releaseOldCommit.countDown()

                oldConnect.get(2, TimeUnit.SECONDS)
                assertEquals(staleCommit, 2, visibleLifecycle.get())
            } finally {
                releaseOldCommit.countDown()
                executor.shutdownNow()
            }
        }
    }

    @Test
    fun `ambiguity and expiry terminal commits cannot overwrite replacement lifecycle`() {
        listOf("AMBIGUITY", "EXPIRY").forEach { staleTerminal ->
            val selectionGate = BadgeUsbReconnectSelectionGate()
            val operationSlot = BadgeUsbAtomicSlot<Any>()
            val operationA = Any()
            val operationB = Any()
            operationSlot.replace(operationA)
            val oldValidationComplete = CountDownLatch(1)
            val releaseOldCommit = CountDownLatch(1)
            val oldLifecycleActive = AtomicBoolean(true)
            val visibleLifecycle = AtomicInteger(1)
            val executor = Executors.newSingleThreadExecutor()
            try {
                val oldTerminal = executor.submit {
                    oldValidationComplete.countDown()
                    check(releaseOldCommit.await(2, TimeUnit.SECONDS))
                    selectionGate.withBarrier {
                        if (oldLifecycleActive.get() && operationSlot.current() === operationA) {
                            visibleLifecycle.set(if (staleTerminal == "AMBIGUITY") -1 else -2)
                            operationSlot.clear(operationA)
                        }
                    }
                }
                assertTrue(oldValidationComplete.await(2, TimeUnit.SECONDS))
                selectionGate.withBarrier {
                    oldLifecycleActive.set(false)
                    operationSlot.replace(operationB)
                    visibleLifecycle.set(2)
                }
                releaseOldCommit.countDown()

                oldTerminal.get(2, TimeUnit.SECONDS)
                assertEquals(staleTerminal, 2, visibleLifecycle.get())
                assertSame(operationB, operationSlot.current())
            } finally {
                releaseOldCommit.countDown()
                executor.shutdownNow()
            }
        }
    }

    @Test
    fun `enumeration epoch rejects detach ABA and newer no-op decisions without expiring permission`() {
        val selectionGate = BadgeUsbReconnectSelectionGate()
        val enumerationGate = newEnumerationEpochGate()
        val permissionStamp = selectionStamp(selectionGate)
        val staleEpoch = enumerationEpoch(enumerationGate)
        val enumerationCaptured = CountDownLatch(1)
        val releaseStaleEnumeration = CountDownLatch(1)
        val selectedDetachedDevice = AtomicBoolean(false)
        val executor = Executors.newSingleThreadExecutor()
        try {
            val staleEnumeration = executor.submit {
                enumerationCaptured.countDown()
                check(releaseStaleEnumeration.await(2, TimeUnit.SECONDS))
                selectionGate.withBarrier {
                    if (enumerationEpochIsCurrent(enumerationGate, staleEpoch)) {
                        selectedDetachedDevice.set(true)
                    }
                }
            }
            assertTrue(enumerationCaptured.await(2, TimeUnit.SECONDS))

            // A relevant detach observation must invalidate enumeration A even when
            // there was no selected attachment or reconnect operation to mutate.
            advanceEnumerationEpoch(enumerationGate)
            assertTrue(selectionStampIsCurrent(selectionGate, permissionStamp))
            releaseStaleEnumeration.countDown()
            staleEnumeration.get(2, TimeUnit.SECONDS)
            assertFalse(selectedDetachedDevice.get())

            // An accepted same-token/no-op observation must also defeat an older
            // empty/ambiguous decision without expiring the permission identity.
            val olderDecisionEpoch = enumerationEpoch(enumerationGate)
            advanceEnumerationEpoch(enumerationGate)
            assertFalse(enumerationEpochIsCurrent(enumerationGate, olderDecisionEpoch))
            assertTrue(selectionStampIsCurrent(selectionGate, permissionStamp))
        } finally {
            releaseStaleEnumeration.countDown()
            executor.shutdownNow()
        }
    }

    @Test
    fun `enumeration epoch fails closed instead of wrapping`() {
        val gate = newEnumerationEpochGate()
        val epochField = gate.javaClass.getDeclaredField("epoch")
        epochField.isAccessible = true
        epochField.setLong(gate, Long.MAX_VALUE)

        val failure = runCatching { advanceEnumerationEpoch(gate) }.exceptionOrNull()
        assertNotNull(failure)
        assertTrue(failure?.cause is IllegalStateException)
        assertEquals(Long.MAX_VALUE, enumerationEpoch(gate))
        assertFalse(
            "An exhausted enumeration gate must reject even the formerly-current MAX snapshot",
            enumerationEpochIsCurrent(gate, Long.MAX_VALUE),
        )
    }

    @Test
    fun `USB IO arbiter linearizes transfer-first and stop-first winners`() {
        val arbiter = newUsbIoArbiter()
        val sessionA = Any()
        assertTrue(activateUsbIoSession(arbiter, sessionA))
        val leaseA = acquireUsbIoLease(arbiter, sessionA)
        assertNotNull(leaseA)

        val stopLinearized = CountDownLatch(1)
        val closeFinished = CountDownLatch(1)
        val executor = Executors.newSingleThreadExecutor()
        try {
            val stopAfterTransfer = executor.submit {
                val drain = revokeUsbIoSession(arbiter, sessionA)
                check(drain != null)
                stopLinearized.countDown()
                check(awaitUsbIoDrain(arbiter, drain, 2_000L))
                check(completeUsbIoDrain(arbiter, drain))
                closeFinished.countDown()
            }
            assertTrue(stopLinearized.await(2, TimeUnit.SECONDS))
            assertFalse(
                "Close must wait while the admitted platform call owns its lease",
                closeFinished.await(100, TimeUnit.MILLISECONDS),
            )
            releaseUsbIoLease(leaseA!!)
            assertTrue(closeFinished.await(2, TimeUnit.SECONDS))
            stopAfterTransfer.get(2, TimeUnit.SECONDS)

            val sessionB = Any()
            assertTrue(activateUsbIoSession(arbiter, sessionB))
            val drainB = revokeUsbIoSession(arbiter, sessionB)
            assertNotNull(drainB)
            val platformCalls = AtomicInteger(0)
            val deniedLease = acquireUsbIoLease(arbiter, sessionB)
            if (deniedLease != null) {
                platformCalls.incrementAndGet()
                releaseUsbIoLease(deniedLease)
            }
            assertEquals("Stop-first must deny the queued USB action", 0, platformCalls.get())
            assertTrue(awaitUsbIoDrain(arbiter, drainB!!, 100L))
            assertTrue(completeUsbIoDrain(arbiter, drainB))
        } finally {
            releaseUsbIoLease(leaseA)
            executor.shutdownNow()
        }
    }

    @Test
    fun `stale frame commit after transfer cannot publish into replacement USB session`() {
        val selectionGate = BadgeUsbReconnectSelectionGate()
        val lifecycleA = AtomicBoolean(true)
        val activeSession = java.util.concurrent.atomic.AtomicReference<Any>()
        val sessionA = Any()
        val sessionB = Any()
        activeSession.set(sessionA)
        val frameReady = CountDownLatch(1)
        val releaseFrameCommit = CountDownLatch(1)
        val visibleSession = AtomicInteger(1)
        val connectionLock = Any()
        val executor = Executors.newSingleThreadExecutor()
        try {
            val staleFrame = executor.submit {
                // The transfer lease has already been released here. Frame commit
                // must reacquire connection ownership and selection exactness.
                frameReady.countDown()
                check(releaseFrameCommit.await(2, TimeUnit.SECONDS))
                synchronized(connectionLock) {
                    selectionGate.withBarrier {
                        if (lifecycleA.get() && activeSession.get() === sessionA) {
                            visibleSession.set(-1)
                        }
                    }
                }
            }
            assertTrue(frameReady.await(2, TimeUnit.SECONDS))
            selectionGate.withBarrier {
                lifecycleA.set(false)
                activeSession.set(sessionB)
                visibleSession.set(2)
            }
            releaseFrameCommit.countDown()
            staleFrame.get(2, TimeUnit.SECONDS)
            assertEquals(2, visibleSession.get())
            assertSame(sessionB, activeSession.get())
        } finally {
            releaseFrameCommit.countDown()
            executor.shutdownNow()
        }
    }

    @Test
    fun `stale reader terminal after blocked transfer cannot revoke replacement session`() {
        val selectionGate = BadgeUsbReconnectSelectionGate()
        val sessionA = Any()
        val sessionB = Any()
        val activeSession = java.util.concurrent.atomic.AtomicReference(sessionA)
        val visibleSession = AtomicInteger(1)
        val transferEntered = CountDownLatch(1)
        val releaseTransfer = CountDownLatch(1)
        val executor = Executors.newSingleThreadExecutor()
        try {
            val oldReader = executor.submit {
                transferEntered.countDown()
                check(releaseTransfer.await(2, TimeUnit.SECONDS))
                selectionGate.withBarrier {
                    if (activeSession.get() === sessionA) {
                        activeSession.set(null)
                        visibleSession.set(-1)
                    }
                }
            }
            assertTrue(transferEntered.await(2, TimeUnit.SECONDS))
            selectionGate.withBarrier {
                activeSession.set(sessionB)
                visibleSession.set(2)
            }
            releaseTransfer.countDown()
            oldReader.get(2, TimeUnit.SECONDS)
            assertSame(sessionB, activeSession.get())
            assertEquals(2, visibleSession.get())
        } finally {
            releaseTransfer.countDown()
            executor.shutdownNow()
        }
    }

    @Test
    fun `stale USB ACK and investigation frame cannot complete after stop or replacement`() {
        val selectionGate = BadgeUsbReconnectSelectionGate()
        val sessionA = Any()
        val sessionB = Any()
        val activeSession = java.util.concurrent.atomic.AtomicReference(sessionA)
        val logicalAck = AtomicBoolean(false)
        val externalCompletion = AtomicBoolean(false)
        val frameParsed = CountDownLatch(1)
        val releaseCommit = CountDownLatch(1)
        val executor = Executors.newSingleThreadExecutor()
        try {
            val oldFrame = executor.submit {
                frameParsed.countDown()
                check(releaseCommit.await(2, TimeUnit.SECONDS))
                val detachedCompletion = selectionGate.withBarrier {
                    if (activeSession.get() !== sessionA) {
                        false
                    } else {
                        logicalAck.set(true)
                        true
                    }
                }
                if (detachedCompletion) externalCompletion.set(true)
            }
            assertTrue(frameParsed.await(2, TimeUnit.SECONDS))
            selectionGate.withBarrier { activeSession.set(sessionB) }
            releaseCommit.countDown()
            oldFrame.get(2, TimeUnit.SECONDS)
            assertFalse(logicalAck.get())
            assertFalse(externalCompletion.get())
            assertSame(sessionB, activeSession.get())
        } finally {
            releaseCommit.countDown()
            executor.shutdownNow()
        }
    }

    @Test
    fun `permission operation is one shot reusable and cannot overwrite synchronous outcome`() {
        data class PermissionOperation(val generation: Long, val selectionStamp: Long)

        val selectionGate = BadgeUsbReconnectSelectionGate()
        val enumerationGate = newEnumerationEpochGate()
        val active = java.util.concurrent.atomic.AtomicReference<PermissionOperation?>()
        val visibleState = java.util.concurrent.atomic.AtomicReference("idle")
        val dispatchEntered = CountDownLatch(1)
        val releaseCallerContinuation = CountDownLatch(1)
        val executor = Executors.newSingleThreadExecutor()
        try {
            val p1 = PermissionOperation(1L, selectionStamp(selectionGate))
            selectionGate.withBarrier {
                active.set(p1)
                visibleState.set("waiting")
            }
            val caller = executor.submit {
                dispatchEntered.countDown()
                check(releaseCallerContinuation.await(2, TimeUnit.SECONDS))
                // requestPermission returned. There is deliberately no waiting write here.
            }
            assertTrue(dispatchEntered.await(2, TimeUnit.SECONDS))

            val denied = selectionGate.withBarrier {
                if (!active.compareAndSet(p1, null)) return@withBarrier false
                selectionGate.advanceStamp()
                visibleState.set("denied")
                true
            }
            assertTrue(denied)
            releaseCallerContinuation.countDown()
            caller.get(2, TimeUnit.SECONDS)
            assertEquals("denied", visibleState.get())
            assertFalse(active.compareAndSet(p1, null))

            val p2 = PermissionOperation(2L, selectionStamp(selectionGate))
            selectionGate.withBarrier {
                active.set(p2)
                visibleState.set("waiting")
            }
            advanceEnumerationEpoch(enumerationGate)
            advanceEnumerationEpoch(enumerationGate)
            assertSame("Polling must reuse the exact outstanding operation", p2, active.get())
            assertFalse("Delayed P1 cannot consume P2", active.compareAndSet(p1, null))

            val beforeGrantStamp = selectionStamp(selectionGate)
            val freshGrantStamp = selectionGate.withBarrier {
                if (!active.compareAndSet(p2, null)) return@withBarrier -1L
                selectionGate.advanceStamp()
            }
            assertTrue(freshGrantStamp > beforeGrantStamp)
            assertTrue(selectionStampIsCurrent(selectionGate, freshGrantStamp))
            assertNull(active.get())
        } finally {
            releaseCallerContinuation.countDown()
            executor.shutdownNow()
        }
    }

    @Test
    fun `timed out USB drain retains A until late release closes once before B activates`() {
        val arbiter = newUsbIoArbiter()
        val sessionA = Any()
        val sessionB = Any()
        assertTrue(activateUsbIoSession(arbiter, sessionA))
        val lease = acquireUsbIoLease(arbiter, sessionA)
        assertNotNull(lease)
        val drain = revokeUsbIoSession(arbiter, sessionA)
        assertNotNull(drain)

        val closeCount = AtomicInteger(0)
        assertFalse(awaitUsbIoDrain(arbiter, drain!!, 1L))
        assertFalse("B must remain blocked while A is retained", activateUsbIoSession(arbiter, sessionB))
        assertEquals(0, closeCount.get())

        releaseUsbIoLease(lease)
        assertTrue(awaitUsbIoDrain(arbiter, drain, 1_000L))
        closeCount.incrementAndGet()
        assertTrue(completeUsbIoDrain(arbiter, drain))
        assertEquals("A must close exactly once", 1, closeCount.get())
        assertTrue("B may activate only after A closes and completes", activateUsbIoSession(arbiter, sessionB))
    }

    @Test
    fun `rejected USB publication returns the exact activated drain`() {
        val arbiter = BadgeUsbIoArbiter()
        val sessionA = Any()
        val sessionB = Any()

        val rejected = publishBadgeUsbIoSession(arbiter, sessionA) { false }
        assertFalse(rejected.published)
        assertNotNull("Failed publication must return ownership of A's drain", rejected.drain)
        assertFalse("B remains blocked until A's exact drain completes", arbiter.activate(sessionB))
        assertTrue(arbiter.awaitDrained(rejected.drain!!, 1_000L))
        assertTrue(arbiter.completeDrain(rejected.drain))
        assertTrue(arbiter.activate(sessionB))
    }

    @Test
    fun `accepted USB publication keeps the active session and returns no drain`() {
        val arbiter = BadgeUsbIoArbiter()
        val sessionA = Any()

        val accepted = publishBadgeUsbIoSession(arbiter, sessionA) { true }
        assertTrue(accepted.published)
        assertNull(accepted.drain)
        val lease = arbiter.tryAcquire(sessionA)
        assertNotNull(lease)
        lease!!.close()
    }

    @Test
    fun `USB cleanup retries failed close and completion without closing A twice`() {
        val cleanup = newUsbIoCleanupPhaseGate()
        var closeAttempts = 0
        var completionAttempts = 0

        assertEquals("DRAINING", usbIoCleanupPhase(cleanup))
        assertFalse(usbIoCleanupShouldClose(cleanup))
        assertFalse(usbIoCleanupShouldCompleteDrain(cleanup))
        assertTrue(markUsbIoCleanupDrained(cleanup))

        assertTrue(usbIoCleanupShouldClose(cleanup))
        closeAttempts += 1 // First platform close throws: do not advance the phase.
        assertEquals("DRAINED", usbIoCleanupPhase(cleanup))
        assertTrue(usbIoCleanupShouldClose(cleanup))
        assertFalse(usbIoCleanupShouldCompleteDrain(cleanup))

        closeAttempts += 1
        assertTrue(markUsbIoCleanupClosed(cleanup))
        assertEquals("CLOSED", usbIoCleanupPhase(cleanup))
        assertFalse("Confirmed close must never be repeated", usbIoCleanupShouldClose(cleanup))
        assertTrue(usbIoCleanupShouldCompleteDrain(cleanup))

        completionAttempts += 1 // First completeDrain returns false: retain CLOSED.
        assertEquals("CLOSED", usbIoCleanupPhase(cleanup))
        assertTrue(usbIoCleanupShouldCompleteDrain(cleanup))
        completionAttempts += 1
        assertTrue(markUsbIoCleanupCompleted(cleanup))

        assertEquals("COMPLETED", usbIoCleanupPhase(cleanup))
        assertFalse(usbIoCleanupShouldClose(cleanup))
        assertFalse(usbIoCleanupShouldCompleteDrain(cleanup))
        assertEquals(2, closeAttempts)
        assertEquals(2, completionAttempts)
    }

    @Test
    fun `cancelled USB cleanup worker retains A for one exact successor`() {
        val slot = newUsbRetainedCleanupSlot()
        val cleanupA = Any()
        val cleanupB = Any()
        val workerA1 = Any()
        val workerA2 = Any()
        val workerB = Any()

        assertTrue(installUsbCleanupWorker(slot, cleanupA, workerA1))
        assertTrue(usbCleanupSlotOwns(slot, cleanupA))
        assertFalse(installUsbCleanupWorker(slot, cleanupA, workerA2))
        assertFalse(installUsbCleanupWorker(slot, cleanupB, workerB))

        assertTrue(finishUsbCleanupWorker(slot, cleanupA, workerA1, completed = false))
        assertTrue("Cancellation must retain exact A ownership", usbCleanupSlotOwns(slot, cleanupA))
        assertTrue(installUsbCleanupWorker(slot, cleanupA, workerA2))
        assertFalse("B stays blocked while successor owns A", installUsbCleanupWorker(slot, cleanupB, workerB))

        assertTrue(finishUsbCleanupWorker(slot, cleanupA, workerA2, completed = true))
        assertFalse(usbCleanupSlotOwns(slot, cleanupA))
        assertTrue(installUsbCleanupWorker(slot, cleanupB, workerB))
    }

    @Test
    fun `permission operation publication invalidates every pre-operation selection snapshot`() {
        val selectionGate = BadgeUsbReconnectSelectionGate()
        val staleSelectionStamp = selectionStamp(selectionGate)
        val waitingPublished = AtomicBoolean(false)
        val operationPublished = AtomicBoolean(false)

        val permissionStamp = selectionGate.withBarrier {
            val stamp = selectionGate.advanceStamp()
            operationPublished.set(true)
            waitingPublished.set(true)
            stamp
        }

        assertTrue(operationPublished.get())
        assertTrue(waitingPublished.get())
        assertFalse(selectionStampIsCurrent(selectionGate, staleSelectionStamp))
        assertTrue(selectionStampIsCurrent(selectionGate, permissionStamp))
    }

    @Test
    fun `generic permission cleanup requires exact lifecycle token and no reconnect owner`() {
        val tokenA = BadgeUsbAttachmentToken(
            generation = 11L,
            identity = BadgeUsbDeviceIdentity(101, "/dev/a"),
        )
        val tokenB = BadgeUsbAttachmentToken(
            generation = 12L,
            identity = BadgeUsbDeviceIdentity(102, "/dev/b"),
        )
        val reconnectOwner = Any()

        assertTrue(
            badgeUsbGenericPermissionCleanupMatches(
                permissionLifecycleSession = 7L,
                permissionAttachmentToken = tokenA,
                permissionReconnectOperation = null,
                expectedLifecycleSession = 7L,
                expectedAttachmentToken = tokenA,
            ),
        )
        assertFalse(
            "A missing expected token must never become a wildcard",
            badgeUsbGenericPermissionCleanupMatches(
                permissionLifecycleSession = 7L,
                permissionAttachmentToken = tokenA,
                permissionReconnectOperation = null,
                expectedLifecycleSession = 7L,
                expectedAttachmentToken = null,
            ),
        )
        assertFalse(
            badgeUsbGenericPermissionCleanupMatches(
                permissionLifecycleSession = 7L,
                permissionAttachmentToken = tokenA,
                permissionReconnectOperation = null,
                expectedLifecycleSession = 7L,
                expectedAttachmentToken = tokenB,
            ),
        )
        assertFalse(
            badgeUsbGenericPermissionCleanupMatches(
                permissionLifecycleSession = 7L,
                permissionAttachmentToken = tokenA,
                permissionReconnectOperation = null,
                expectedLifecycleSession = 8L,
                expectedAttachmentToken = tokenA,
            ),
        )
        assertFalse(
            "Generic cleanup must not consume a reconnect-owned request",
            badgeUsbGenericPermissionCleanupMatches(
                permissionLifecycleSession = 7L,
                permissionAttachmentToken = tokenA,
                permissionReconnectOperation = reconnectOwner,
                expectedLifecycleSession = 7L,
                expectedAttachmentToken = tokenA,
            ),
        )
    }

    @Test
    fun `reconnect permission cleanup follows exact operation across attempt replacement`() {
        val reconnectA = Any()
        val reconnectB = Any()
        val permissionTokenA = BadgeUsbAttachmentToken(
            generation = 21L,
            identity = BadgeUsbDeviceIdentity(201, "/dev/reconnect-a"),
        )
        val replacementAttemptTokenB = BadgeUsbAttachmentToken(
            generation = 22L,
            identity = BadgeUsbDeviceIdentity(202, "/dev/reconnect-b"),
        )

        assertNotEquals(permissionTokenA, replacementAttemptTokenB)
        assertTrue(
            "Attempt B must not prevent exact reconnect operation A from retiring P_A",
            badgeUsbReconnectPermissionCleanupMatches(
                permissionLifecycleSession = 9L,
                permissionReconnectOperation = reconnectA,
                expectedLifecycleSession = 9L,
                expectedReconnectOperation = reconnectA,
            ),
        )
        assertFalse(
            badgeUsbReconnectPermissionCleanupMatches(
                permissionLifecycleSession = 9L,
                permissionReconnectOperation = reconnectA,
                expectedLifecycleSession = 9L,
                expectedReconnectOperation = reconnectB,
            ),
        )
        assertFalse(
            badgeUsbReconnectPermissionCleanupMatches(
                permissionLifecycleSession = 9L,
                permissionReconnectOperation = null,
                expectedLifecycleSession = 9L,
                expectedReconnectOperation = reconnectA,
            ),
        )
        assertFalse(
            badgeUsbReconnectPermissionCleanupMatches(
                permissionLifecycleSession = 9L,
                permissionReconnectOperation = reconnectA,
                expectedLifecycleSession = 10L,
                expectedReconnectOperation = reconnectA,
            ),
        )
    }

    @Test
    fun `permission side effect requires the exact still current operation`() {
        val operationA = Any()
        val operationB = Any()

        assertTrue(
            badgeUsbPermissionMayDispatch(
                activeOperation = operationA,
                expectedOperation = operationA,
                selectionStampCurrent = true,
                lifecycleActive = true,
                attachmentAccepted = true,
                reconnectOwned = true,
            ),
        )
        assertFalse(
            "A superseding operation must stop stale A before the platform side effect",
            badgeUsbPermissionMayDispatch(
                activeOperation = operationB,
                expectedOperation = operationA,
                selectionStampCurrent = true,
                lifecycleActive = true,
                attachmentAccepted = true,
                reconnectOwned = true,
            ),
        )
        assertFalse(
            badgeUsbPermissionMayDispatch(
                activeOperation = operationA,
                expectedOperation = operationA,
                selectionStampCurrent = false,
                lifecycleActive = true,
                attachmentAccepted = true,
                reconnectOwned = true,
            ),
        )
        assertFalse(
            badgeUsbPermissionMayDispatch(
                activeOperation = operationA,
                expectedOperation = operationA,
                selectionStampCurrent = true,
                lifecycleActive = false,
                attachmentAccepted = true,
                reconnectOwned = true,
            ),
        )
        assertFalse(
            badgeUsbPermissionMayDispatch(
                activeOperation = operationA,
                expectedOperation = operationA,
                selectionStampCurrent = true,
                lifecycleActive = true,
                attachmentAccepted = false,
                reconnectOwned = true,
            ),
        )
        assertFalse(
            badgeUsbPermissionMayDispatch(
                activeOperation = operationA,
                expectedOperation = operationA,
                selectionStampCurrent = true,
                lifecycleActive = true,
                attachmentAccepted = true,
                reconnectOwned = false,
            ),
        )
    }

    @Test
    fun `permission dispatch gate cancels before side effect and dispatches at most once`() {
        val cancelled = BadgeUsbPermissionDispatchGate()
        var cancelledSideEffects = 0
        cancelled.cancel()
        assertFalse(cancelled.dispatchIfActive { cancelledSideEffects += 1 })
        assertEquals(0, cancelledSideEffects)

        val oneShot = BadgeUsbPermissionDispatchGate()
        var sideEffects = 0
        assertTrue(oneShot.dispatchIfActive { sideEffects += 1 })
        assertFalse(oneShot.dispatchIfActive { sideEffects += 1 })
        assertEquals(1, sideEffects)
    }

    @Test
    fun `permission retirement cannot finish ahead of an in flight platform dispatch`() {
        val gate = BadgeUsbPermissionDispatchGate()
        val sideEffectStarted = CountDownLatch(1)
        val allowSideEffectToReturn = CountDownLatch(1)
        val cancellationFinished = CountDownLatch(1)
        val executor = Executors.newFixedThreadPool(2)

        try {
            val dispatchFuture = executor.submit<Boolean> {
                gate.dispatchIfActive {
                    sideEffectStarted.countDown()
                    assertTrue(allowSideEffectToReturn.await(1, TimeUnit.SECONDS))
                }
            }
            assertTrue(sideEffectStarted.await(1, TimeUnit.SECONDS))
            val cancellationFuture = executor.submit {
                gate.cancel()
                cancellationFinished.countDown()
            }
            assertFalse(
                "Stop/detach/replacement must not return while the platform call is in flight",
                cancellationFinished.await(50, TimeUnit.MILLISECONDS),
            )

            allowSideEffectToReturn.countDown()
            assertTrue(cancellationFinished.await(1, TimeUnit.SECONDS))
            assertTrue(dispatchFuture.get(1, TimeUnit.SECONDS))
            cancellationFuture.get(1, TimeUnit.SECONDS)
        } finally {
            allowSideEffectToReturn.countDown()
            executor.shutdownNow()
        }
    }

    @Test
    fun `permission dispatch failure clears exact operation but cannot overwrite superseding state`() {
        data class PermissionOperation(val generation: Long, val selectionStamp: Long)

        val selectionGate = BadgeUsbReconnectSelectionGate()
        val active = java.util.concurrent.atomic.AtomicReference<PermissionOperation?>()
        val state = java.util.concurrent.atomic.AtomicReference("idle")

        fun publishOperation(generation: Long): PermissionOperation =
            selectionGate.withBarrier {
                val operation = PermissionOperation(generation, selectionGate.advanceStamp())
                active.set(operation)
                state.set("waiting")
                operation
            }

        fun failDispatch(operation: PermissionOperation) = selectionGate.withBarrier {
            if (!active.compareAndSet(operation, null)) return@withBarrier
            val stillOwnsState = selectionStampIsCurrent(
                selectionGate,
                operation.selectionStamp,
            )
            selectionGate.advanceStamp()
            if (stillOwnsState) state.set("error")
        }

        val setupFailure = publishOperation(1L)
        failDispatch(setupFailure)
        assertNull(active.get())
        assertEquals("error", state.get())

        val blockedBinderFailure = publishOperation(2L)
        selectionGate.withBarrier {
            selectionGate.advanceStamp()
            state.set("ambiguous")
        }
        failDispatch(blockedBinderFailure)
        assertNull("Stale exact operation must still be cleared", active.get())
        assertEquals("ambiguous", state.get())

        val replacement = publishOperation(3L)
        failDispatch(blockedBinderFailure)
        assertSame("P2/P3 replacement must survive P1/P2 failure", replacement, active.get())
        assertEquals("waiting", state.get())
    }

    private fun finishWithoutStatus(
        gate: BadgeUsbStatusPollGate,
        owner: BadgeUsbOwnerKey,
    ): BadgeUsbStatusPollDecision {
        val ticket = gate.beginPoll(owner)!!
        return gate.finishPoll(ticket, owner)
    }

    private fun selectionStamp(gate: BadgeUsbReconnectSelectionGate): Long =
        invokeRequired(gate, "currentStamp") as Long

    private fun advanceSelectionStamp(gate: BadgeUsbReconnectSelectionGate): Long =
        invokeRequired(gate, "advanceStamp") as Long

    private fun selectionStampIsCurrent(
        gate: BadgeUsbReconnectSelectionGate,
        stamp: Long,
    ): Boolean = invokeRequired(gate, "isStampCurrent", stamp) as Boolean

    private fun newReceiverLifetimeGate(): Any {
        val className = "com.friendorfoe.data.badge.BadgeUsbReceiverLifetimeGate"
        val receiverGateClass = runCatching { Class.forName(className) }.getOrNull()
        assertNotNull("Missing process-lifetime receiver gate: $className", receiverGateClass)
        return receiverGateClass!!.getDeclaredConstructor().newInstance()
    }

    private fun registerReceiverOnce(gate: Any, registration: () -> Unit): Boolean =
        invokeRequired(gate, "registerOnce", registration) as Boolean

    private fun receiverIsRegistered(gate: Any): Boolean =
        invokeRequired(gate, "isRegistered") as Boolean

    private fun newEnumerationEpochGate(): Any {
        val className = "com.friendorfoe.data.badge.BadgeUsbEnumerationEpochGate"
        val gateClass = runCatching { Class.forName(className) }.getOrNull()
        assertNotNull("Missing enumeration epoch gate: $className", gateClass)
        return gateClass!!.getDeclaredConstructor().newInstance()
    }

    private fun enumerationEpoch(gate: Any): Long =
        invokeRequired(gate, "currentEpoch") as Long

    private fun advanceEnumerationEpoch(gate: Any): Long =
        invokeRequired(gate, "advanceEpoch") as Long

    private fun enumerationEpochIsCurrent(gate: Any, epoch: Long): Boolean =
        invokeRequired(gate, "isEpochCurrent", epoch) as Boolean

    private fun newUsbIoArbiter(): Any {
        val className = "com.friendorfoe.data.badge.BadgeUsbIoArbiter"
        val arbiterClass = runCatching { Class.forName(className) }.getOrNull()
        assertNotNull("Missing exact-session USB I/O arbiter: $className", arbiterClass)
        return arbiterClass!!.getDeclaredConstructor().newInstance()
    }

    private fun activateUsbIoSession(arbiter: Any, session: Any): Boolean =
        invokeRequired(arbiter, "activate", session) as Boolean

    private fun acquireUsbIoLease(arbiter: Any, session: Any): Any? =
        invokeOptional(arbiter, "tryAcquire", session)

    private fun revokeUsbIoSession(arbiter: Any, session: Any): Any? =
        invokeOptional(arbiter, "revoke", session)

    private fun awaitUsbIoDrain(arbiter: Any, drain: Any, timeoutMs: Long): Boolean =
        invokeRequired(arbiter, "awaitDrained", drain, timeoutMs) as Boolean

    private fun completeUsbIoDrain(arbiter: Any, drain: Any): Boolean =
        invokeRequired(arbiter, "completeDrain", drain) as Boolean

    private fun releaseUsbIoLease(lease: Any?) {
        if (lease != null) invokeOptional(lease, "close")
    }

    private fun newUsbIoCleanupPhaseGate(): Any {
        val className = "com.friendorfoe.data.badge.BadgeUsbIoCleanupPhaseGate"
        val gateClass = runCatching { Class.forName(className) }.getOrNull()
        assertNotNull("Missing USB cleanup phase gate: $className", gateClass)
        return gateClass!!.getDeclaredConstructor().newInstance()
    }

    private fun usbIoCleanupPhase(gate: Any): String =
        invokeRequired(gate, "phaseName") as String

    private fun usbIoCleanupShouldClose(gate: Any): Boolean =
        invokeRequired(gate, "shouldAttemptClose") as Boolean

    private fun usbIoCleanupShouldCompleteDrain(gate: Any): Boolean =
        invokeRequired(gate, "shouldAttemptDrainCompletion") as Boolean

    private fun markUsbIoCleanupDrained(gate: Any): Boolean =
        invokeRequired(gate, "markDrained") as Boolean

    private fun markUsbIoCleanupClosed(gate: Any): Boolean =
        invokeRequired(gate, "markClosed") as Boolean

    private fun markUsbIoCleanupCompleted(gate: Any): Boolean =
        invokeRequired(gate, "markCompleted") as Boolean

    private fun newUsbRetainedCleanupSlot(): Any {
        val className = "com.friendorfoe.data.badge.BadgeUsbRetainedCleanupSlot"
        val slotClass = runCatching { Class.forName(className) }.getOrNull()
        assertNotNull("Missing retained USB cleanup slot: $className", slotClass)
        return slotClass!!.getDeclaredConstructor().newInstance()
    }

    private fun installUsbCleanupWorker(slot: Any, cleanup: Any, worker: Any): Boolean =
        invokeRequired(slot, "tryInstall", cleanup, worker) as Boolean

    private fun finishUsbCleanupWorker(
        slot: Any,
        cleanup: Any,
        worker: Any,
        completed: Boolean,
    ): Boolean = invokeRequired(slot, "finishWorker", cleanup, worker, completed) as Boolean

    private fun usbCleanupSlotOwns(slot: Any, cleanup: Any): Boolean =
        invokeRequired(slot, "ownsCleanup", cleanup) as Boolean

    private fun invokeOptional(target: Any, name: String, vararg arguments: Any): Any? {
        val method = target.javaClass.declaredMethods.singleOrNull {
            it.name == name && it.parameterCount == arguments.size
        }
        assertNotNull("Missing ${target.javaClass.simpleName}.$name", method)
        method!!.isAccessible = true
        return method.invoke(target, *arguments)
    }

    private fun invokeRequired(target: Any, name: String, vararg arguments: Any): Any {
        val method = target.javaClass.declaredMethods.singleOrNull {
            it.name == name && it.parameterCount == arguments.size
        }
        assertNotNull("Missing ${target.javaClass.simpleName}.$name", method)
        method!!.isAccessible = true
        return requireNotNull(method.invoke(target, *arguments)) {
            "${target.javaClass.simpleName}.$name returned null"
        }
    }

    private fun owner(
        attachmentGeneration: Long = 1L,
        deviceId: Int = 101,
        devicePath: String = "/dev/a",
        lifecycleSession: Long = 7L,
        connection: Any = Any(),
        endpoint: Any = Any(),
        hardwareId: String = "A4:CF:12:34:56:78",
    ): BadgeUsbOwnerKey = BadgeUsbOwnerKey(
        attachmentToken = BadgeUsbAttachmentToken(
            generation = attachmentGeneration,
            identity = BadgeUsbDeviceIdentity(deviceId, devicePath),
        ),
        lifecycleSession = lifecycleSession,
        connectionIdentity = connection,
        endpointIdentity = endpoint,
        hardwareId = hardwareId,
    )
}
