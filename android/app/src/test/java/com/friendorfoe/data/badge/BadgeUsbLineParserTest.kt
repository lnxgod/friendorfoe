package com.friendorfoe.data.badge

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
class BadgeUsbLineParserTest {

    @Test
    fun recoveryLinesCompleteOnlyTheirMatchingPendingUsbCommand() {
        assertEquals(
            BadgeCommandOutcome.Acknowledged(
                BadgeControlAcknowledgement("Reboot acknowledged"),
            ),
            parseUsbCommandLine(BadgeCommand.Reboot, "FOF_REBOOT:OK"),
        )
        assertEquals(
            BadgeCommandOutcome.Acknowledged(
                BadgeControlAcknowledgement("Bootloader acknowledged"),
            ),
            parseUsbCommandLine(BadgeCommand.EnterBootloader, "FOF_BOOTLOADER:OK"),
        )
        assertNull(parseUsbCommandLine(BadgeCommand.Reboot, "FOF_BOOTLOADER:OK"))
        assertNull(
            parseUsbCommandLine(
                BadgeCommand.ApplyTheme(BadgeTheme.firmwareDefaults()),
                "FOF_REBOOT:OK",
            ),
        )
    }

    @Test
    fun recoveryAcknowledgementsRejectWhitespaceAtTheRawSerialBoundary() {
        listOf(" FOF_REBOOT:OK", "FOF_REBOOT:OK ", "\tFOF_BOOTLOADER:OK", "FOF_BOOTLOADER:OK\r")
            .forEach { line ->
                assertNull(parseUsbCommandLine(BadgeCommand.Reboot, line))
                assertNull(parseUsbCommandLine(BadgeCommand.EnterBootloader, line))
            }
    }

    @Test
    fun ordinaryControlLinesCannotCompleteRecoveryAndRecoveryCannotCompleteOrdinaryControl() {
        assertNull(
            parseUsbCommandLine(
                BadgeCommand.Reboot,
                "FOF_CTL_OK:{\"ok\":true}",
            ),
        )
        assertNull(
            parseUsbCommandLine(
                BadgeCommand.ApplyPolicy(BadgeDisplayPolicy.firmwareDefaults()),
                "FOF_BOOTLOADER:OK",
            ),
        )
    }

    @Test
    fun usbTimeoutPoisonsMutationGenerationSoLateAckCannotCompleteAnotherCommand() {
        val coordinator = BadgeUsbCommandCoordinator()
        val firstGeneration = coordinator.currentTransportGeneration()
        val first = CompletableDeferred<BadgeCommandOutcome>()
        val second = CompletableDeferred<BadgeCommandOutcome>()
        assertTrue(coordinator.begin(firstGeneration, BadgeCommand.Reboot, first))

        coordinator.timeout(firstGeneration, BadgeCommand.Reboot, first)
        assertFalse(coordinator.begin(firstGeneration, BadgeCommand.Reboot, second))
        assertFalse(coordinator.acceptSerialLine(firstGeneration, "FOF_REBOOT:OK"))

        val secondGeneration = coordinator.resetTransportGeneration()
        assertTrue(coordinator.begin(secondGeneration, BadgeCommand.Reboot, second))
        assertFalse(coordinator.acceptSerialLine(firstGeneration, "FOF_REBOOT:OK"))
        assertTrue(coordinator.acceptSerialLine(secondGeneration, "FOF_REBOOT:OK"))
        assertTrue(second.isCompleted)
    }

    @Test
    fun bleTimeoutPoisonsMutationGenerationSoLateCallbackCannotCompleteAnotherCommand() {
        val coordinator = BadgeBleCommandCoordinator()
        val firstGeneration = coordinator.currentTransportGeneration()
        val first = CompletableDeferred<BadgeCommandOutcome>()
        val second = CompletableDeferred<BadgeCommandOutcome>()
        assertTrue(
            coordinator.begin(
                firstGeneration,
                BadgeCommand.NavigateDisplay(BadgeDisplayAction.NEXT),
                first,
            ),
        )

        coordinator.timeout(firstGeneration, first)
        assertFalse(
            coordinator.begin(
                firstGeneration,
                BadgeCommand.NavigateDisplay(BadgeDisplayAction.BACK),
                second,
            ),
        )
        assertFalse(coordinator.acceptWriteCallback(firstGeneration, success = true))
        assertFalse(second.isCompleted)

        val secondGeneration = coordinator.resetTransportGeneration()
        assertTrue(
            coordinator.begin(
                secondGeneration,
                BadgeCommand.NavigateDisplay(BadgeDisplayAction.BACK),
                second,
            ),
        )
        assertFalse(coordinator.acceptWriteCallback(firstGeneration, success = true))
        assertTrue(coordinator.acceptWriteCallback(secondGeneration, success = true))
        assertEquals(
            BadgeCommandOutcome.Accepted("Badge BLE command accepted; checking readback"),
            second.getCompleted(),
        )
    }

    @Test
    fun usbCancellationAfterWritePoisonsGenerationAndIgnoresLateAck() {
        val coordinator = BadgeUsbCommandCoordinator()
        val generation = coordinator.currentTransportGeneration()
        val cancelled = CompletableDeferred<BadgeCommandOutcome>()
        val next = CompletableDeferred<BadgeCommandOutcome>()
        assertTrue(coordinator.begin(generation, BadgeCommand.Reboot, cancelled))

        coordinator.cancelAfterAttempt(generation, BadgeCommand.Reboot, cancelled)

        assertFalse(coordinator.begin(generation, BadgeCommand.Reboot, next))
        assertFalse(coordinator.acceptSerialLine(generation, "FOF_REBOOT:OK"))
    }

    @Test
    fun bleCancellationAfterWritePoisonsGenerationAndIgnoresLateCallback() {
        val coordinator = BadgeBleCommandCoordinator()
        val generation = coordinator.currentTransportGeneration()
        val command = BadgeCommand.NavigateDisplay(BadgeDisplayAction.NEXT)
        val cancelled = CompletableDeferred<BadgeCommandOutcome>()
        val next = CompletableDeferred<BadgeCommandOutcome>()
        assertTrue(coordinator.begin(generation, command, cancelled))

        coordinator.cancelAfterAttempt(generation, cancelled)

        assertFalse(coordinator.begin(generation, command, next))
        assertFalse(coordinator.acceptWriteCallback(generation, success = true))
    }

    @Test
    fun disconnectCompletesPendingAndInvalidatesItsTransportGeneration() {
        val coordinator = BadgeUsbCommandCoordinator()
        val generation = coordinator.currentTransportGeneration()
        val pending = CompletableDeferred<BadgeCommandOutcome>()
        assertTrue(coordinator.begin(generation, BadgeCommand.EnterBootloader, pending))

        val nextGeneration = coordinator.invalidateTransport("Badge disconnected")

        assertEquals(
            BadgeCommandOutcome.Failed("Badge disconnected"),
            pending.getCompleted(),
        )
        val next = CompletableDeferred<BadgeCommandOutcome>()
        assertFalse(coordinator.begin(generation, BadgeCommand.EnterBootloader, next))
        assertTrue(coordinator.begin(nextGeneration, BadgeCommand.EnterBootloader, next))
    }

    @Test
    fun usbTimeoutStillPoisonsWhenAckWonTheCompletionRace() {
        val coordinator = BadgeUsbCommandCoordinator()
        val generation = coordinator.currentTransportGeneration()
        val abandoned = CompletableDeferred<BadgeCommandOutcome>()
        assertTrue(coordinator.begin(generation, BadgeCommand.Reboot, abandoned))
        assertTrue(coordinator.acceptSerialLine(generation, "FOF_REBOOT:OK"))

        coordinator.timeout(generation, BadgeCommand.Reboot, abandoned)

        assertFalse(
            coordinator.begin(
                generation,
                BadgeCommand.Reboot,
                CompletableDeferred(),
            ),
        )
    }

    @Test
    fun bleCancellationStillPoisonsWhenCallbackWonTheCompletionRace() {
        val coordinator = BadgeBleCommandCoordinator()
        val generation = coordinator.currentTransportGeneration()
        val command = BadgeCommand.NavigateDisplay(BadgeDisplayAction.NEXT)
        val abandoned = CompletableDeferred<BadgeCommandOutcome>()
        assertTrue(coordinator.begin(generation, command, abandoned))
        assertTrue(coordinator.acceptWriteCallback(generation, success = true))

        coordinator.cancelAfterAttempt(generation, abandoned)

        assertFalse(
            coordinator.begin(
                generation,
                command,
                CompletableDeferred(),
            ),
        )
    }

    @Test
    fun resetRejectsEveryAttemptUsingThePreviousTransportGeneration() {
        val coordinator = BadgeUsbCommandCoordinator()
        val oldGeneration = coordinator.currentTransportGeneration()
        val newGeneration = coordinator.resetTransportGeneration()

        assertFalse(
            coordinator.begin(
                oldGeneration,
                BadgeCommand.Reboot,
                CompletableDeferred(),
            ),
        )
        assertTrue(
            coordinator.begin(
                newGeneration,
                BadgeCommand.Reboot,
                CompletableDeferred(),
            ),
        )
    }

    @Test
    fun bleControlWaitsForInFlightStatusReadBeforeClaimingGattOperation() = runTest {
        val operations = BadgeBleGattOperationCoordinator()
        val epoch = operations.currentEpoch()
        assertTrue(operations.tryBegin(BadgeBleGattOperation.STATUS_READ))

        val control = async {
            operations.awaitAndBegin(
                operation = BadgeBleGattOperation.CONTROL_WRITE,
                expectedEpoch = epoch,
                timeoutMs = 1_000L,
            )
        }
        runCurrent()
        assertFalse(control.isCompleted)

        assertTrue(operations.complete(BadgeBleGattOperation.STATUS_READ))
        runCurrent()

        assertTrue(control.await())
        assertEquals(BadgeBleGattOperation.CONTROL_WRITE, operations.current())
    }

    @Test
    fun bleGattSetupOperationsAdvanceOnlyAfterExactPredecessorCompletes() {
        val operations = BadgeBleGattOperationCoordinator()
        assertTrue(operations.tryBegin(BadgeBleGattOperation.MTU_REQUEST))
        assertFalse(operations.tryBegin(BadgeBleGattOperation.SERVICE_DISCOVERY))
        assertFalse(operations.complete(BadgeBleGattOperation.SERVICE_DISCOVERY))
        assertTrue(operations.complete(BadgeBleGattOperation.MTU_REQUEST))
        assertTrue(operations.tryBegin(BadgeBleGattOperation.SERVICE_DISCOVERY))
        assertTrue(operations.complete(BadgeBleGattOperation.SERVICE_DISCOVERY))
        assertTrue(operations.tryBegin(BadgeBleGattOperation.DESCRIPTOR_WRITE))
        assertFalse(operations.tryBegin(BadgeBleGattOperation.STATUS_READ))
        assertTrue(operations.complete(BadgeBleGattOperation.DESCRIPTOR_WRITE))
        assertTrue(operations.tryBegin(BadgeBleGattOperation.STATUS_READ))
    }

    @Test
    fun bleResetRejectsWaiterFromPreviousGattEpochBeforeNewMtuSetup() = runTest {
        val operations = BadgeBleGattOperationCoordinator()
        val oldEpoch = operations.currentEpoch()
        assertTrue(operations.tryBegin(BadgeBleGattOperation.STATUS_READ))
        val staleControl = async {
            operations.awaitAndBegin(
                operation = BadgeBleGattOperation.CONTROL_WRITE,
                expectedEpoch = oldEpoch,
                timeoutMs = 1_000L,
            )
        }
        runCurrent()
        assertFalse(staleControl.isCompleted)

        operations.reset()
        runCurrent()

        assertFalse(staleControl.await())
        assertEquals(null, operations.current())
        assertTrue(operations.tryBegin(BadgeBleGattOperation.MTU_REQUEST))
    }
}
