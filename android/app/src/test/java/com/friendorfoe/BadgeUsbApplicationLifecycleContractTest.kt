package com.friendorfoe

import java.io.File
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class BadgeUsbApplicationLifecycleContractTest {

    @Test
    fun badge_usb_lifecycle_is_owned_by_the_application() {
        val application = source("FriendOrFoeApplication.kt")

        assertTrue(application.contains("import com.friendorfoe.data.badge.BadgeUsbRepository"))
        assertTrue(application.contains("lateinit var badgeUsbRepository: BadgeUsbRepository"))
        assertTrue(application.contains("override fun onStart(owner: LifecycleOwner)"))
        assertTrue(application.contains("badgeUsbRepository.start()"))
        assertTrue(application.contains("override fun onStop(owner: LifecycleOwner)"))
        assertTrue(application.contains("badgeUsbRepository.stop()"))

        for (screen in listOf("presentation/list/ListViewScreen.kt", "presentation/privacy/PrivacyScreen.kt")) {
            val contents = source(screen)
            assertFalse(screen, contents.contains("startBadgeUsb()"))
            assertFalse(screen, contents.contains("stopBadgeUsb()"))
        }

        val listScreen = source("presentation/list/ListViewScreen.kt")
        assertFalse(listScreen.contains("onConnect = viewModel::connectBadgeUsb"))
        assertFalse(listScreen.contains("Button(onClick = onConnect)"))

        val badgeScreen = source("presentation/badge/BadgeControlScreen.kt")
        assertTrue(badgeScreen.contains("state.status == BadgeUsbStatus.PERMISSION_NEEDED"))
        assertTrue(badgeScreen.contains("Grant USB access"))

        val privacyScreen = source("presentation/privacy/PrivacyScreen.kt")
        assertFalse(privacyScreen.contains("BadgeUsbStatus.PERMISSION_NEEDED"))
        assertFalse(privacyScreen.contains("Grant USB access"))

        for (viewModel in listOf(
            "presentation/list/ListViewModel.kt",
            "presentation/privacy/PrivacyViewModel.kt",
        )) {
            val contents = source(viewModel)
            assertFalse(viewModel, contents.contains("fun startBadgeUsb()"))
            assertFalse(viewModel, contents.contains("fun stopBadgeUsb()"))
            assertFalse(viewModel, contents.contains("badgeUsbRepository.start()"))
            assertFalse(viewModel, contents.contains("badgeUsbRepository.stop()"))
        }

        val repository = source("data/badge/BadgeUsbRepository.kt")
        assertTrue(repository.contains("UsbManager.ACTION_USB_DEVICE_ATTACHED -> requestConnection()"))
        assertTrue(repository.contains("\"Attach a FoF badge over USB-C\""))
        assertFalse(repository.contains("\"Connect a FoF badge over USB-C\""))
        assertTrue(repository.contains("Attach only the badge over USB-C"))
        assertTrue(repository.contains("attach the badge over USB-C"))
        assertFalse(repository.contains("Connect only the badge over USB-C"))
        assertFalse(repository.contains("connect via USB-C"))
        assertTrue(repository.contains("Intent(ACTION_USB_PERMISSION)"))
        assertTrue(repository.contains("PendingIntent.FLAG_MUTABLE"))
        assertFalse(repository.contains("PendingIntent.FLAG_IMMUTABLE"))
        assertTrue(repository.contains("putExtra(EXTRA_USB_PERMISSION_SESSION, lifecycleSession)"))
        assertTrue(repository.contains("EXTRA_USB_PERMISSION_ATTACHMENT_GENERATION"))
        assertTrue(repository.contains("attachmentToken.generation"))
        assertTrue(repository.contains("EXTRA_USB_PERMISSION_DEVICE_ID"))
        assertTrue(repository.contains("attachmentToken.identity.deviceId"))
        assertTrue(repository.contains("EXTRA_USB_PERMISSION_DEVICE_PATH"))
        assertTrue(repository.contains("attachmentToken.identity.devicePath"))
        assertTrue(repository.contains("intent.getLongExtra("))
        assertTrue(repository.contains("EXTRA_USB_PERMISSION_SESSION,"))
        assertTrue(repository.contains("attachmentGate.acceptsPermission"))
        assertTrue(repository.contains("expectedAttachmentToken"))
        assertTrue(repository.contains("expectedConnection"))
        assertTrue(repository.contains("devicePath = deviceName"))
        assertTrue(repository.contains("detachUsbInvestigationLocked(disconnectedOwner)"))
        assertTrue(repository.contains("expectedOwner = owner"))
        assertTrue(repository.contains("prepareUsbInvestigationFrameLocked("))
        assertTrue(repository.contains("completePreparedUsbInvestigationFrame("))
        assertTrue(repository.contains("catch (cancelled: CancellationException)"))
        assertTrue(repository.contains("throw cancelled"))
        assertTrue(repository.contains("attachmentToken = attachmentToken"))
        assertTrue(repository.contains("connectionMutex.withBadgeUsbReaderOwner"))
        assertTrue(repository.contains("private fun rejectUsbIdentityLocked("))
        assertTrue(repository.contains("rejectUsbIdentityLocked("))
        val identityRejector = repository
            .substringAfter("private fun rejectUsbIdentityLocked(")
            .substringBefore("private fun prepareUsbReaderLocked(")
        assertFalse(
            "Identity rejection must stay synchronous inside the reader's connection mutex",
            identityRejector.contains("scope.launch"),
        )

        val detachBlock = repository
            .substringAfter("UsbManager.ACTION_USB_DEVICE_DETACHED -> {")
            .substringBefore("fun start()")
        assertTrue(
            "Every Espressif detach must rescan so an ambiguous pair can recover automatically",
            detachBlock.contains("cleanupUsbDetachAndRescan(invalidation, lifecycleSession)") &&
                detachBlock.contains("cancelUsbReconnectForDetachedOwner("),
        )
        assertFalse(
            "A detach with no selected attachment token must not return before rescanning",
            detachBlock.contains("invalidation ?: return"),
        )

        val logicalRevoke = functionBody(repository, "private fun revokeUsbSessionLocked(")
        val revokeOwnerAt = logicalRevoke.indexOf("verifiedUsbOwnerKey = null")
        val detachAt = logicalRevoke.indexOf(
            "detachUsbInvestigationLocked(disconnectedOwner)",
        )
        assertTrue(revokeOwnerAt >= 0)
        assertTrue(detachAt >= 0)
        assertTrue("USB owner must be revoked before investigation detachment", revokeOwnerAt < detachAt)
        assertFalse(logicalRevoke.contains("completeDetachedUsbInvestigation("))
        assertFalse(logicalRevoke.contains(".cancel()"))
        assertFalse(logicalRevoke.contains("releaseInterface("))
        assertFalse(logicalRevoke.contains(".close()"))

        val physicalClose = functionBody(repository, "private fun closeDetachedUsbResources(")
        val completeAt = physicalClose.indexOf("completeDetachedUsbInvestigation(")
        val cleanupAdvanceAt = physicalClose.indexOf("advanceUsbIoCleanup(cleanup)")
        assertTrue(completeAt >= 0)
        assertTrue(cleanupAdvanceAt > completeAt)
        val cleanupAdvance = functionBody(repository, "private fun advanceUsbIoCleanup(")
        assertTrue(cleanupAdvance.contains("physicallyCloseDetachedUsbResources("))
        val physicalHelper = functionBody(
            repository,
            "private fun physicallyCloseDetachedUsbResources(",
        )
        val releaseAt = physicalHelper.indexOf("releaseInterface(")
        val closeAt = physicalHelper.indexOf(".close()")
        assertTrue(releaseAt >= 0)
        assertTrue(closeAt > releaseAt)

        val handshake = functionBody(repository, "private fun startUsbIdentityHandshake(")
        val stopAction = handshake
            .substringAfter("BadgeUsbHandshakeTimerAction.STOP -> {")
            .substringBefore("BadgeUsbHandshakeTimerAction.RETRY")
        val ownJobCheckAt = stopAction.indexOf("usbHandshakeJob === ownJob")
        val clearAt = stopAction.indexOf("usbHandshakeJob = null")
        assertTrue("Handshake STOP must prove exact job ownership", ownJobCheckAt >= 0)
        assertTrue("Only the owning STOP path may clear its handshake slot", clearAt > ownJobCheckAt)
    }

    @Test
    fun badge_usb_status_liveness_is_owner_bound_and_not_UI_state_bound() {
        val repository = source("data/badge/BadgeUsbRepository.kt")

        assertTrue(repository.contains("private val usbStatusPollGate = BadgeUsbStatusPollGate()"))
        assertFalse(repository.contains("state.value.controlStatus == null"))

        val pollerDeclaration = repository
            .substringAfter("private fun prepareUsbStatusPollerLocked(")
            .substringBefore("private fun startUsbStatusPoller(")
        val poller = functionBody(repository, "private fun prepareUsbStatusPollerLocked(")
        assertTrue(pollerDeclaration.contains("owner: BadgeUsbOwnerKey"))
        assertTrue(pollerDeclaration.contains("deviceName: String"))
        val beginAt = poller.indexOf("usbStatusPollGate.beginPoll(owner)")
        val writeAt = poller.indexOf("writeVerifiedUsbLine(")
        val delayAt = poller.indexOf("delay(USB_STATUS_POLL_INTERVAL_MS)")
        val finishAt = poller.indexOf("usbStatusPollGate.finishPoll(ticket, owner)")
        assertTrue(beginAt >= 0)
        assertTrue(writeAt > beginAt)
        assertTrue(delayAt > writeAt)
        assertTrue(finishAt > delayAt)
        assertTrue(poller.contains("BadgeUsbStatusPollDecision.FRESH"))
        assertTrue(poller.contains("BadgeUsbStatusPollDecision.MISS"))
        assertTrue(poller.contains("BadgeUsbStatusPollDecision.TERMINATE"))
        assertTrue(poller.contains("BadgeUsbStatusPollDecision.STALE_OWNER"))
        assertTrue(poller.contains("val ownJob = coroutineContext[Job]"))
        assertTrue(poller.contains("usbStatusPollJobSlot.clear(ownJob)"))
        assertFalse(poller.contains("controlStatus"))

        val lineHandler = repository
            .substringAfter("private fun handleLine(")
            .substringBefore("private fun handleUsbInvestigationLine(")
        val identityValidationAt = lineHandler.indexOf("badgeUsbStatusFrameIdentityError(")
        val bindAt = lineHandler.indexOf("usbStatusPollGate.bind(")
        val assignAt = lineHandler.indexOf("verifiedUsbOwnerKey = acceptedOwner")
        val recordAt = lineHandler.indexOf("usbStatusPollGate.recordStatus(")
        val handshakeStateMutationAt = lineHandler.indexOf("setState(frameStateUpdate(false))")
        val ordinaryStateMutationAt = lineHandler.lastIndexOf("setState(frameStateUpdate)")
        val prepareAt = lineHandler.indexOf("prepareUsbStatusPollerLocked(")
        val completionAt = lineHandler.indexOf("completeHandshakeAndClearIfActive(")
        val startAt = lineHandler.indexOf("preparedStatusPoller?.let(::startUsbStatusPoller)")
        assertTrue(identityValidationAt >= 0)
        assertTrue(bindAt > identityValidationAt)
        assertTrue(assignAt > bindAt)
        assertTrue(handshakeStateMutationAt > assignAt)
        assertTrue(prepareAt > handshakeStateMutationAt)
        assertTrue(completionAt > prepareAt)
        assertTrue(startAt > completionAt)
        assertTrue(recordAt > identityValidationAt)
        assertTrue(ordinaryStateMutationAt > recordAt)

        val logicalRevoke = functionBody(repository, "private fun revokeUsbSessionLocked(")
        assertTrue(logicalRevoke.contains("usbStatusPollGate.clear(disconnectedOwner)"))
        assertTrue(logicalRevoke.contains("usbStatusPollJobSlot.take()"))
        assertFalse(logicalRevoke.contains(".cancel()"))
        val physicalClose = functionBody(repository, "private fun closeDetachedUsbResources(")
        assertTrue(physicalClose.contains("detached.statusPollJob?.cancel()"))

        assertTrue(repository.contains(
            "private val usbStatusPollJobSlot = BadgeUsbAtomicSlot<Job>()",
        ))
        assertFalse(
            "USB status jobs must not use a raw check-then-write field",
            Regex("\\busbStatusPollJob\\b").containsMatchIn(repository),
        )
        val replaceJobAt = poller.indexOf("usbStatusPollJobSlot.replace(pollJob)")
        val starter = functionBody(repository, "private fun startUsbStatusPoller(")
        val cancelPreviousAt = starter.indexOf("prepared.previousJob?.cancel()")
        val startReplacementAt = starter.indexOf("prepared.job.start()")
        assertTrue(replaceJobAt >= 0)
        assertTrue("Replacement must be installed before the post-gate starter exists", cancelPreviousAt >= 0)
        assertTrue("Replacement starts only after atomic install", startReplacementAt > cancelPreviousAt)
    }

    @Test
    fun verified_USB_write_failure_terminalizes_only_after_an_attempt() {
        val repository = source("data/badge/BadgeUsbRepository.kt")
        val writer = repository
            .substringAfter("private suspend fun writeVerifiedUsbLine(")
            .substringBefore("private fun hasUsbCommandPath(")

        val eligibilityAt = writer.indexOf("badgeUsbVerifiedWriteAllowed(")
        val attemptedWriteAt = writer.indexOf(
            "val writeSucceeded = writeConnectionBoundLineLocked(",
        )
        val failedWriteAt = writer.indexOf("if (!writeSucceeded)")
        val terminalAt = writer.indexOf("terminateVerifiedUsbSessionLocked(")
        assertTrue(eligibilityAt >= 0)
        assertTrue(attemptedWriteAt > eligibilityAt)
        assertTrue(failedWriteAt > attemptedWriteAt)
        assertTrue(terminalAt > failedWriteAt)

        val preAttempt = writer.substring(0, attemptedWriteAt)
        assertFalse(preAttempt.contains("terminateVerifiedUsbSessionLocked("))
    }

    @Test
    fun asynchronous_USB_commands_capture_the_expected_owner_at_invocation_time() {
        val repository = source("data/badge/BadgeUsbRepository.kt")

        val requestStatus = repository
            .substringAfter("fun requestStatus()")
            .substringBefore("fun investigateBle(")
        assertInvocationTimeOwnerCapture(requestStatus)
        assertTrue(requestStatus.contains("hasUsbCommandPath(expectedOwner)"))
        assertTrue(requestStatus.contains("expectedOwner = expectedOwner"))

        val sendControl = repository
            .substringAfter("private fun sendControl(payload: JsonObject)")
            .substringBefore("private fun startApPoller()")
        assertInvocationTimeOwnerCapture(sendControl)
        assertTrue(sendControl.contains("hasUsbCommandPath(expectedOwner)"))
        assertTrue(sendControl.contains("usbInvestigationOwnsControlReply(expectedOwner)"))
        assertTrue(sendControl.contains("expectedOwner = expectedOwner ?: return@launch"))
    }

    @Test
    fun badge_USB_reader_has_a_bounded_exact_owner_silence_watchdog() {
        val repository = source("data/badge/BadgeUsbRepository.kt")
        val reader = repository
            .substringAfter("private fun prepareUsbReaderLocked(")
            .substringBefore("private suspend fun writeConnectionBoundLineLocked(")

        val gateCreateAt = reader.indexOf("val readerSilenceGate = BadgeUsbReaderSilenceGate()")
        val gateStartAt = reader.indexOf("readerSilenceGate.start(elapsedRealtimeMs())")
        val loopAt = reader.indexOf("while (isActive)")
        assertTrue(gateCreateAt >= 0)
        assertTrue(gateStartAt > gateCreateAt)
        assertTrue(loopAt > gateStartAt)

        val positiveRead = reader
            .substringAfter("if (read > 0) {")
            .substringBefore("} else {")
        val recordAt = positiveRead.indexOf(
            "readerSilenceGate.recordRead(read, readObservedAtElapsedMs)",
        )
        val terminalAt = positiveRead.indexOf("terminateUsbReaderSessionLocked(")
        val breakAt = positiveRead.indexOf("break")
        val frameAt = positiveRead.indexOf("lineFramer.accept(buffer, read)")
        assertTrue(recordAt >= 0)
        assertTrue("Late positive read must terminalize before framing", terminalAt > recordAt)
        assertTrue(
            positiveRead.substring(recordAt, terminalAt).contains("connectionMutex.withLock"),
        )
        assertTrue("Late positive read must break before framing", breakAt > terminalAt)
        assertTrue("Fresh read records liveness before framing", frameAt > breakAt)

        val nonPositiveRead = reader.substringAfter("} else {")
        val expiredAt = nonPositiveRead.indexOf(
            "readerSilenceGate.isExpired(readObservedAtElapsedMs)",
        )
        val nonPositiveTerminalAt = nonPositiveRead.indexOf("terminateUsbReaderSessionLocked(")
        val nonPositiveBreakAt = nonPositiveRead.indexOf("break")
        val retryDelayAt = nonPositiveRead.indexOf("delay(25)")
        assertTrue(expiredAt >= 0)
        assertTrue(nonPositiveTerminalAt > expiredAt)
        assertTrue(
            nonPositiveRead.substring(expiredAt, nonPositiveTerminalAt)
                .contains("connectionMutex.withLock"),
        )
        assertTrue(nonPositiveBreakAt > nonPositiveTerminalAt)
        assertTrue("Nonpositive reads retry only before the silence deadline", retryDelayAt > nonPositiveBreakAt)

        assertTrue(reader.contains("catch (cancelled: CancellationException)"))
        assertTrue(reader.contains("throw cancelled"))
        val exceptionHandler = reader
            .substringAfter("catch (e: Exception)")
            .substringBefore("private fun terminateUsbReaderSessionLocked(")
        assertTrue(exceptionHandler.contains("connectionMutex.withLock"))
        assertTrue(exceptionHandler.contains("terminateUsbReaderSessionLocked("))
        assertFalse(exceptionHandler.contains("disconnectLocked()"))
        assertFalse(exceptionHandler.contains("reduceBadgeUsbReaderFailure("))

        val terminalHelper = repository
            .substringAfter("private fun terminateUsbReaderSessionLocked(")
            .substringBefore("private suspend fun writeConnectionBoundLineLocked(")
        assertFalse(
            "Reader terminalization must remain synchronous under connectionMutex",
            terminalHelper.contains("scope.launch"),
        )
        assertTrue(terminalHelper.contains("badgeUsbReaderTerminalOwnsExactSession("))
        assertTrue(terminalHelper.contains("terminateVerifiedUsbSessionLocked("))
        val barrierAt = terminalHelper.indexOf("usbReconnectSelectionGate.withBarrier")
        val exactAt = terminalHelper.indexOf("badgeUsbReaderTerminalOwnsExactSession(")
        val revokeAt = terminalHelper.indexOf("revokeUsbSessionLocked(")
        val stateAt = terminalHelper.indexOf("reduceBadgeUsbTerminalError(")
        val closeAt = terminalHelper.indexOf("closeDetachedUsbResources(")
        assertTrue(barrierAt >= 0)
        assertTrue(exactAt > barrierAt)
        assertTrue(revokeAt > exactAt)
        assertTrue(stateAt > revokeAt)
        assertTrue(closeAt > stateAt)
        assertTrue(terminalHelper.contains("reduceBadgeUsbTerminalError("))
    }

    @Test
    fun verified_USB_failures_schedule_one_exact_reconnect_operation() {
        val repository = source("data/badge/BadgeUsbRepository.kt")
        assertTrue(repository.contains("private val usbReconnectGate = BadgeUsbReconnectGate()"))
        assertTrue(repository.contains(
            "private val usbReconnectOperationSlot = " +
                "BadgeUsbAtomicSlot<ActiveBadgeUsbReconnectOperation>()",
        ))

        val terminal = repository
            .substringAfter("private fun terminateVerifiedUsbSessionLocked(")
            .substringBefore("private fun hasUsbCommandPath(")
        val validationAt = terminal.indexOf("badgeUsbTerminalFailureOwnsExactSession(")
        val barrierAt = terminal.indexOf("usbReconnectSelectionGate.withBarrier")
        val revokeAt = terminal.indexOf("revokeUsbSessionLocked(")
        val terminalStateAt = terminal.indexOf("reduceBadgeUsbTerminalError(")
        val publishAt = terminal.indexOf("prepareVerifiedUsbReconnectLocked(expectedOwner)")
        val closeAt = terminal.indexOf("closeDetachedUsbResources(")
        val scheduleAt = terminal.indexOf("preparedReconnect?.let(::startVerifiedUsbReconnectLocked)")
        assertTrue(barrierAt >= 0)
        assertTrue(validationAt > barrierAt)
        assertTrue(revokeAt > validationAt)
        assertTrue(terminalStateAt > revokeAt)
        assertTrue(publishAt > terminalStateAt)
        assertTrue(closeAt > publishAt)
        assertTrue(scheduleAt > closeAt)

        val readerTerminal = repository
            .substringAfter("private fun terminateUsbReaderSessionLocked(")
            .substringBefore("private suspend fun writeConnectionBoundLineLocked(")
        assertFalse(
            "Unverified CONNECTING reader failures must remain manual",
            readerTerminal.substringAfter("if (expectedVerifiedOwner != null)")
                .contains("startVerifiedUsbReconnectLocked("),
        )

        val scheduler = repository
            .substringAfter("private suspend fun runUsbReconnectScheduler(")
            .substringBefore("private fun registerReceiverIfNeeded()")
        val attemptAt = scheduler.indexOf("usbReconnectGate.nextAttempt(")
        val requestAt = scheduler.indexOf("requestConnection(")
        val delayAt = scheduler.indexOf("delay(USB_RECONNECT_INTERVAL_MS)")
        assertTrue(attemptAt >= 0)
        assertTrue(requestAt > attemptAt)
        assertTrue(delayAt > requestAt)
        assertTrue(scheduler.contains("preserveRecoveryOnNoCandidates = true"))
        assertTrue(scheduler.contains("reconnectTicket = operation.ticket"))
        assertFalse(
            "Reconnect operation ownership must never be held across connection work",
            scheduler.substring(0, requestAt.coerceAtLeast(0))
                .contains("synchronized(usbReconnectLock)"),
        )
    }

    @Test
    fun reconnect_recovery_preserves_error_and_requires_the_same_MAC_before_publication() {
        val repository = source("data/badge/BadgeUsbRepository.kt")
        val request = repository
            .substringAfter("private fun requestConnection(")
            .substringBefore("fun sendPing()")
        assertTrue(request.contains("preserveRecoveryOnNoCandidates: Boolean"))
        assertTrue(request.contains("reconnectTicket: BadgeUsbReconnectTicket?"))
        val noCandidates = request
            .substringAfter("BadgeUsbReconnectCandidateAction.PRESERVE_RECOVERY ->")
            .substringBefore("BadgeUsbReconnectCandidateAction.NORMAL_REFRESH ->")
        assertTrue(noCandidates.contains("return"))
        assertFalse(noCandidates.contains("refresh("))
        assertFalse(noCandidates.contains("BadgeUsbStatus.DISCONNECTED"))
        val ambiguity = request
            .substringAfter("BadgeUsbReconnectCandidateAction.FAIL_AMBIGUOUS ->")
            .substringBefore("BadgeUsbReconnectCandidateAction.CONNECT_ONE ->")
        val ambiguityTerminalAt = ambiguity.indexOf("effectiveOperation.tryTerminalize()")
        val ambiguityStampAt = ambiguity.indexOf("usbReconnectSelectionGate.advanceStamp()")
        val ambiguityCleanupAt = request.indexOf("failAmbiguousUsbReconnect(it, candidates)")
        assertTrue(ambiguityTerminalAt >= 0)
        assertTrue(ambiguityStampAt > ambiguityTerminalAt)
        assertTrue(
            "Ambiguous reconnect cleanup must run only after the stamped selection commit",
            ambiguityCleanupAt > ambiguityStampAt,
        )

        val lineHandler = repository
            .substringAfter("private fun handleLine(")
            .substringBefore("private fun handleUsbInvestigationLine(")
        val expectedIdAt = lineHandler.indexOf("expectedHardwareId = reconnectExpectedHardwareId")
        val identityCheckAt = lineHandler.indexOf("badgeUsbStatusFrameIdentityError(")
        val expectedIdentityArgumentAt = lineHandler.indexOf(
            "expectedHardwareId = frameOwner?.hardwareId ?: expectedHardwareId",
        )
        val completeAt = lineHandler.indexOf(
            "reconnectOperation.completeHandshakeAndClearIfActive(",
        )
        val ownerPublishAt = lineHandler.indexOf("verifiedUsbOwnerKey = acceptedOwner")
        val pollerPrepareAt = lineHandler.indexOf("prepareUsbStatusPollerLocked(")
        val pollerStartAt = lineHandler.indexOf("preparedStatusPoller?.let(::startUsbStatusPoller)")
        assertTrue(expectedIdAt >= 0)
        assertTrue(identityCheckAt > expectedIdAt)
        assertTrue(expectedIdentityArgumentAt > identityCheckAt)
        assertTrue(completeAt > expectedIdentityArgumentAt)
        assertTrue(ownerPublishAt > expectedIdentityArgumentAt)
        assertTrue(pollerPrepareAt > ownerPublishAt)
        assertTrue(completeAt > pollerPrepareAt)
        assertTrue(pollerStartAt > completeAt)
        val acceptance = lineHandler.substringAfter("if (acceptedOwner != null) {")
        val ownershipRecheckAt = acceptance.indexOf("badgeUsbReaderOwnsExactSession(")
        val acceptanceCompleteAt = acceptance.indexOf(
            "reconnectOperation.completeHandshakeAndClearIfActive(",
        )
        assertTrue(ownershipRecheckAt >= 0)
        assertTrue(acceptanceCompleteAt > ownershipRecheckAt)
        assertTrue(acceptance.contains("attachmentGate.publishIfCurrentAndActive("))
        assertTrue(acceptance.contains("usbReconnectSelectionGate.withBarrier"))
        assertTrue(acceptance.contains("fullCommit = publishVerifiedOwner"))
    }

    @Test
    fun reconnect_handshake_commits_full_state_and_poller_before_releasing_operation() {
        val repository = source("data/badge/BadgeUsbRepository.kt")
        val lineHandler = repository
            .substringAfter("private fun handleLine(")
            .substringBefore("private fun handleUsbInvestigationLine(")
        val acceptance = lineHandler.substringAfter("if (acceptedOwner != null) {")
        val frameStateUpdate = lineHandler
            .substringAfter("val frameStateUpdate:")
            .substringBefore("if (acceptedOwner != null) {")
        val handshakeCommit = acceptance
            .substringAfter("val publishVerifiedOwner = {")
            .substringBefore("val verifiedOwnerPublished")

        assertTrue(frameStateUpdate.contains("controlStatus = status"))
        assertTrue(frameStateUpdate.contains("usbHandshakeAccepted -> \"Badge USB connected\""))
        assertTrue(handshakeCommit.contains("verifiedUsbOwnerKey = acceptedOwner"))
        assertTrue(handshakeCommit.contains("setState(frameStateUpdate(false))"))
        assertTrue(handshakeCommit.contains("prepareUsbStatusPollerLocked("))
        val ownerAt = handshakeCommit.indexOf("verifiedUsbOwnerKey = acceptedOwner")
        val stateAt = handshakeCommit.indexOf("setState(frameStateUpdate(false))")
        val pollerAt = handshakeCommit.indexOf("prepareUsbStatusPollerLocked(")
        val revokeHandshakeAt = handshakeCommit.indexOf("usbHandshakeJob = null")
        val stampAt = handshakeCommit.indexOf("usbReconnectSelectionGate.advanceStamp()")
        assertTrue(stateAt > ownerAt)
        assertTrue(pollerAt > stateAt)
        assertTrue(revokeHandshakeAt > pollerAt)
        assertTrue(stampAt > revokeHandshakeAt)
        assertTrue(acceptance.contains("completeHandshakeAndClearIfActive"))
        assertTrue(acceptance.contains("fullCommit = publishVerifiedOwner"))
        val barrierAt = acceptance.indexOf("usbReconnectSelectionGate.withBarrier")
        val cancelReconnectAt = acceptance.indexOf("completedReconnectJob?.cancel()")
        val cancelHandshakeAt = acceptance.indexOf("completedHandshakeJob?.cancel()")
        val startPollerAt = acceptance.indexOf("preparedStatusPoller?.let(::startUsbStatusPoller)")
        assertTrue(barrierAt >= 0)
        assertTrue(cancelReconnectAt > barrierAt)
        assertTrue(cancelHandshakeAt > cancelReconnectAt)
        assertTrue(startPollerAt > cancelHandshakeAt)
    }

    @Test
    fun reconnect_stop_detach_rejection_and_expiry_are_exact_owner_bounded() {
        val repository = source("data/badge/BadgeUsbRepository.kt")
        val stop = repository.substringAfter("fun stop()").substringBefore("fun refresh()")
        assertTrue(stop.contains("cancelUsbReconnectForLifecycle(lifecycleSession)"))
        val stopBarrierAt = stop.indexOf("?.tryTerminalize()")
        val lifecycleEndAt = stop.indexOf("lifecycleGate.end(lifecycleSession)")
        val stopInvalidateAt = stop.indexOf("attachmentGate.invalidateCurrent()")
        val stopCancelAt = stop.indexOf("cancelUsbReconnectForLifecycle(lifecycleSession)")
        assertTrue(stopBarrierAt >= 0)
        assertTrue(lifecycleEndAt > stopBarrierAt)
        assertTrue(stopInvalidateAt > lifecycleEndAt)
        assertTrue(
            "Stop must make lifecycle and attachment ineligible before clearing the MAC gate",
            stopCancelAt > stopInvalidateAt,
        )

        val detach = repository
            .substringAfter("UsbManager.ACTION_USB_DEVICE_DETACHED -> {")
            .substringBefore("fun start()")
        assertTrue(detach.contains("badgeUsbReconnectDetachMatches("))
        assertTrue(detach.contains("cancelUsbReconnectForDetachedOwner("))
        assertTrue(detach.contains("cleanupUsbDetachAndRescan(invalidation, lifecycleSession)"))
        val detachBarrierAt = detach.indexOf("?.tryTerminalize()")
        val detachInvalidateAt = detach.indexOf("attachmentGate.invalidateMatching(")
        val detachCancelAt = detach.indexOf("cancelUsbReconnectForDetachedOwner(")
        assertTrue(detachBarrierAt >= 0)
        assertTrue(detachInvalidateAt > detachBarrierAt)
        assertTrue(detachInvalidateAt >= 0)
        assertTrue(
            "Detach must revoke attachment ownership before clearing the MAC gate",
            detachCancelAt > detachInvalidateAt,
        )

        val detachCancellation = repository
            .substringAfter("private fun cancelUsbReconnectForDetachedOwner(")
            .substringBefore("private fun failAmbiguousUsbReconnect(")
        val detachMutexAt = detachCancellation.indexOf("connectionMutex.withLock")
        val detachExactAt = detachCancellation.indexOf(
            "usbReconnectOperationSlot.current() === operation",
        )
        val detachAttemptInvalidateAt = detachCancellation.indexOf(
            "attachmentGate.invalidateExact(",
        )
        val detachRevokeAt = detachCancellation.indexOf("revokeUsbSessionLocked(")
        val detachGateClearAt = detachCancellation.indexOf("clearUsbReconnectOperationLocked(operation)")
        val detachJobCancelAt = detachCancellation.indexOf("operation.job.cancel()")
        val detachCloseAt = detachCancellation.indexOf("closeDetachedUsbResources(")
        val detachRescanAt = detachCancellation.lastIndexOf("requestConnection()")
        assertTrue(detachMutexAt >= 0)
        assertTrue(detachExactAt > detachMutexAt)
        assertTrue(detachAttemptInvalidateAt > detachExactAt)
        assertTrue(detachRevokeAt > detachAttemptInvalidateAt)
        assertTrue(detachGateClearAt > detachRevokeAt)
        assertTrue(detachJobCancelAt > detachGateClearAt)
        assertTrue(detachCloseAt > detachJobCancelAt)
        assertTrue(detachRescanAt > detachCloseAt)

        val disconnect = functionBody(repository, "private fun disconnect(")
        assertTrue(disconnect.contains("activeUsbLifecycleSession == lifecycleSession"))
        assertTrue(disconnect.contains("activeAttachmentToken == expectedAttachmentToken"))
        assertTrue(disconnect.contains("expectedSnapshot"))

        val rejectionDeclaration = repository
            .substringAfter("private fun rejectUsbIdentityLocked(")
            .substringBefore("private fun prepareUsbReaderLocked(")
        val rejection = functionBody(repository, "private fun rejectUsbIdentityLocked(")
        assertTrue(rejectionDeclaration.contains(
            "reconnectOperation: ActiveBadgeUsbReconnectOperation?",
        ))
        val rejectionRevokeAt = rejection.indexOf("revokeUsbSessionLocked(")
        val rejectionStateAt = rejection.indexOf("reduceBadgeUsbTerminalError(")
        val rejectionClearAt = rejection.indexOf("clearUsbReconnectOperationLocked(reconnectOperation)")
        val rejectionCancelAt = rejection.indexOf("reconnectOperation?.job?.cancel()")
        val rejectionCloseAt = rejection.indexOf("closeDetachedUsbResources(")
        assertTrue(rejectionRevokeAt >= 0)
        assertTrue(rejectionStateAt > rejectionRevokeAt)
        assertTrue(rejectionClearAt > rejectionStateAt)
        assertTrue(rejectionCancelAt > rejectionClearAt)
        assertTrue(rejectionCloseAt > rejectionCancelAt)

        val expiry = repository
            .substringAfter("private suspend fun expireUsbReconnect(")
            .substringBefore("private fun registerReceiverIfNeeded()")
        val exactAt = expiry.indexOf("usbReconnectGate.isCurrent(")
        val invalidateAt = expiry.indexOf("attachmentGate.invalidateExact(")
        val closeGuardAt = expiry.indexOf("badgeUsbReconnectExpiryOwnsConnecting(")
        val revokeAt = expiry.indexOf("revokeUsbSessionLocked(")
        val errorAt = expiry.indexOf("reduceBadgeUsbTerminalError(")
        val clearAt = expiry.indexOf("clearUsbReconnectOperationLocked(operation)", errorAt)
        val closeAt = expiry.indexOf("closeDetachedUsbResources(")
        assertTrue(exactAt >= 0)
        assertTrue(invalidateAt > exactAt)
        assertTrue(closeGuardAt > exactAt)
        assertTrue(revokeAt > closeGuardAt)
        assertTrue(errorAt > revokeAt)
        assertTrue(clearAt > errorAt)
        assertTrue(closeAt > clearAt)
    }

    @Test
    fun verified_failure_handoff_revalidates_and_publishes_reconnect_inside_selection_barrier() {
        val repository = source("data/badge/BadgeUsbRepository.kt")
        val terminal = repository
            .substringAfter("private fun terminateVerifiedUsbSessionLocked(")
            .substringBefore("private fun hasUsbCommandPath(")
        val barrierAt = terminal.indexOf("usbReconnectSelectionGate.withBarrier")
        val revalidateAt = terminal.indexOf("badgeUsbTerminalFailureOwnsExactSession(")
        val revokeAt = terminal.indexOf("revokeUsbSessionLocked(")
        val stateAt = terminal.indexOf("reduceBadgeUsbTerminalError(")
        val publishAt = terminal.indexOf("prepareVerifiedUsbReconnectLocked(expectedOwner)")
        val closeAt = terminal.indexOf("closeDetachedUsbResources(")
        val reconnectAt = terminal.indexOf("preparedReconnect?.let(::startVerifiedUsbReconnectLocked)")

        assertTrue(barrierAt >= 0)
        assertTrue(revalidateAt > barrierAt)
        assertTrue(revokeAt > revalidateAt)
        assertTrue(stateAt > revokeAt)
        assertTrue(publishAt > stateAt)
        assertTrue(closeAt > publishAt)
        assertTrue(reconnectAt > closeAt)
    }

    @Test
    fun generic_permission_state_commit_is_lifecycle_slot_and_attachment_bounded() {
        val repository = source("data/badge/BadgeUsbRepository.kt")
        val request = repository
            .substringAfter("private fun requestConnection(")
            .substringBefore("fun sendPing()")
        val finalPermissionState = request
            .substringAfter("if (!permissionRequested) return")
            .substringBefore("fun sendPing()")

        assertTrue(finalPermissionState.contains("usbReconnectSelectionGate.withBarrier"))
        assertTrue(finalPermissionState.contains("genericUsbSelectionMayMutate(lifecycleSession)"))
        assertTrue(finalPermissionState.contains("attachmentGate.isCurrent(attachmentToken)"))
    }

    @Test
    fun reconnect_ambiguity_tears_down_the_exact_attempt_before_clearing_its_MAC_gate() {
        val repository = source("data/badge/BadgeUsbRepository.kt")
        val ambiguity = repository
            .substringAfter("private fun failAmbiguousUsbReconnect(")
            .substringBefore("private suspend fun runUsbReconnectScheduler(")
        val mutexAt = ambiguity.indexOf("connectionMutex.withLock")
        val exactAt = ambiguity.indexOf("usbReconnectOperationSlot.current() !== operation")
        val invalidateAt = ambiguity.indexOf("attachmentGate.invalidateExact(")
        val closeGuardAt = ambiguity.indexOf("badgeUsbReconnectExpiryOwnsConnecting(")
        val revokeAt = ambiguity.indexOf("revokeUsbSessionLocked(")
        val reportAt = ambiguity.indexOf("reportAmbiguousBadgeDevices(candidates)")
        val clearAt = ambiguity.indexOf("clearUsbReconnectOperationLocked(operation)")
        val cancelAt = ambiguity.indexOf("operation.job.cancel()")
        val closeAt = ambiguity.indexOf("closeDetachedUsbResources(")
        assertTrue(mutexAt >= 0)
        assertTrue(exactAt > mutexAt)
        assertTrue(invalidateAt > exactAt)
        assertTrue(closeGuardAt > exactAt)
        assertTrue(revokeAt > closeGuardAt)
        assertTrue(reportAt > revokeAt)
        assertTrue(clearAt > reportAt)
        assertTrue(cancelAt > clearAt)
        assertTrue(closeAt > cancelAt)
    }

    @Test
    fun reconnect_permission_open_and_handshake_share_one_exact_attempt_context() {
        val repository = source("data/badge/BadgeUsbRepository.kt")
        val operation = repository
            .substringAfter("private class ActiveBadgeUsbReconnectOperation(")
            .substringBefore("@Singleton")
        val boundAttemptGuardAt = operation.indexOf("attemptConnectionBound =")
        val selectionAt = operation.indexOf("attachmentToken = selectAttachment()")
        val replacementAt = operation.indexOf("ActiveBadgeUsbReconnectAttempt(")
        assertTrue(boundAttemptGuardAt >= 0)
        assertTrue(
            "A live connection-bound attempt must not be orphaned by a new attachment",
            selectionAt > boundAttemptGuardAt && replacementAt > boundAttemptGuardAt,
        )

        val permissionReceiver = repository
            .substringAfter("ACTION_USB_PERMISSION -> {")
            .substringBefore("UsbManager.ACTION_USB_DEVICE_ATTACHED")
        val generationReadAt = permissionReceiver.indexOf(
            "EXTRA_USB_RECONNECT_GENERATION",
        )
        val operationResolveAt = permissionReceiver.indexOf(
            "val operation = activeUsbPermissionOperation",
        )
        val attemptResolveAt = permissionReceiver.indexOf(
            "val reconnectAttempt = operation.reconnectAttempt",
        )
        val attemptExactAt = permissionReceiver.indexOf(
            "isUsbReconnectAttemptCurrent(",
        )
        val connectAt = permissionReceiver.indexOf("connectToDevice(")
        assertTrue(generationReadAt >= 0)
        assertTrue(operationResolveAt > generationReadAt)
        assertTrue(attemptResolveAt > operationResolveAt)
        assertTrue(attemptExactAt > attemptResolveAt)
        assertTrue(connectAt > attemptExactAt)
        assertTrue(permissionReceiver.contains(
            "reconnectOperation = consumed.operation.reconnectOperation",
        ))
        assertTrue(permissionReceiver.contains(
            "reconnectAttempt = consumed.operation.reconnectAttempt",
        ))

        val request = repository
            .substringAfter("private fun requestConnection(")
            .substringBefore("fun sendPing()")
        assertTrue(request.contains("EXTRA_USB_RECONNECT_GENERATION"))
        assertTrue(request.contains("effectiveOperation.preparePendingAttempt("))
        assertTrue(request.contains("usbReconnectSelectionGate.withBarrier"))
        assertTrue(request.contains("reconnectOperation = effectiveReconnectOperation"))
        assertTrue(request.contains("reconnectAttempt = reconnectAttempt"))

        val connector = repository
            .substringAfter("private suspend fun connectToDevice(")
            .substringBefore("private fun startUsbIdentityHandshake(")
        assertTrue(connector.contains("reconnectOperation: ActiveBadgeUsbReconnectOperation?"))
        assertTrue(connector.contains("reconnectAttempt: ActiveBadgeUsbReconnectAttempt?"))
        assertTrue(
            "Reconnect context must be revalidated at entry, after open/claim, and before publication",
            Regex("connectContextIsCurrent\\(").findAll(connector).count() >= 4,
        )
        val openAt = connector.indexOf("usbManager.openDevice(device)")
        val claimAt = connector.indexOf("connection.claimInterface(")
        val publishAt = connector.indexOf("activeConnection = connection")
        val validations = Regex("connectContextIsCurrent\\(")
            .findAll(connector)
            .map { it.range.first }
            .toList()
        assertTrue(validations.first() < openAt)
        assertTrue(validations.any { it > claimAt && it < publishAt })
        assertTrue(connector.contains("bindConnection("))
        assertTrue(connector.contains("publishConnectingIfActive"))
        assertTrue(connector.contains("activateAndPublishIfCurrent"))
        assertTrue(connector.contains("currentUsbReconnectOperation(lifecycleSession) == null"))

        val logicalRevoke = functionBody(repository, "private fun revokeUsbSessionLocked(")
        assertTrue(logicalRevoke.contains("clearAttempt("))
        assertFalse(
            "Routine connection cleanup must retain the live reconnect ticket",
            logicalRevoke.contains("cancelUsbReconnect("),
        )
    }

    @Test
    fun refresh_candidate_selection_rechecks_lifecycle_and_reconnect_under_selection_barrier() {
        val repository = source("data/badge/BadgeUsbRepository.kt")
        val refresh = repository
            .substringAfter("private fun refresh(lifecycleSession: Long)")
            .substringBefore("fun requestConnection()")
        val snapshotAt = refresh.indexOf("captureUsbEnumerationSnapshot(lifecycleSession)")
        val enumerationAt = refresh.indexOf("findBadgeCandidates()")
        val selectionBarrierAt = refresh.indexOf("usbReconnectSelectionGate.withBarrier")
        val guardedSelection = refresh.substring(selectionBarrierAt.coerceAtLeast(0))
        val snapshotRecheckAt = guardedSelection.indexOf(
            "usbEnumerationSnapshotIsCurrent(enumerationSnapshot)",
        )
        val reconnectRecheckAt = guardedSelection.indexOf("usbReconnectOperationSlot.current()")
        val selectAt = guardedSelection.indexOf("selectAttachment(device, lifecycleSession)")

        assertTrue(snapshotAt >= 0)
        assertTrue(enumerationAt > snapshotAt)
        assertTrue(selectionBarrierAt > enumerationAt)
        assertTrue(snapshotRecheckAt >= 0)
        assertTrue(reconnectRecheckAt > snapshotRecheckAt)
        assertTrue(selectAt > reconnectRecheckAt)
        assertTrue(
            "Refresh selection and ambiguity publication must remain inside the stamped commit",
            guardedSelection.contains("reportAmbiguousBadgeDevices(candidates)") &&
                guardedSelection.contains("PendingBadgeUsbSelection("),
        )
    }

    @Test
    fun receiver_registration_is_process_lifetime_and_stopped_callbacks_are_inert() {
        val repository = source("data/badge/BadgeUsbRepository.kt")
        val start = functionBody(repository, "fun start()")
        val stop = functionBody(repository, "fun stop()")
        val request = functionBody(repository, "private fun requestConnection(")
        val registration = functionBody(repository, "private fun registerReceiverIfNeeded()")
        val detach = repository
            .substringAfter("UsbManager.ACTION_USB_DEVICE_DETACHED -> {")
            .substringBefore("fun start()")

        assertTrue(
            "Receiver ownership must use one synchronized process-lifetime gate",
            repository.contains("BadgeUsbReceiverLifetimeGate"),
        )
        assertTrue(start.contains("registerReceiverIfNeeded()"))
        assertFalse(
            "Retries and stale schedulers must never register the process receiver",
            request.contains("registerReceiverIfNeeded()"),
        )
        assertFalse(
            "Stopping a lifecycle must not unregister the process receiver",
            stop.contains("unregisterReceiver(") || stop.contains("receiverRegistered = false"),
        )
        assertTrue(registration.contains("receiverLifetimeGate.registerOnce"))
        val epochAt = detach.indexOf("usbEnumerationEpochGate.advanceEpoch()")
        val lifecycleCaptureAt = detach.indexOf("lifecycleGate.activeSession()")
        val firstMutationAt = detach.indexOf("usbReconnectSelectionGate.withBarrier")
        assertTrue(
            "Detach may only mutate the internal epoch before resolving an active lifecycle",
            firstMutationAt >= 0 && epochAt > firstMutationAt &&
                lifecycleCaptureAt > epochAt,
        )
        val beforeLifecycle = detach.substring(firstMutationAt, lifecycleCaptureAt)
        assertFalse(beforeLifecycle.contains("setState"))
        assertFalse(beforeLifecycle.contains("attachmentGate."))
    }

    @Test
    fun candidate_enumeration_uses_a_stamped_snapshot_and_revalidates_before_commit() {
        val repository = source("data/badge/BadgeUsbRepository.kt")
        val liveness = source("data/badge/BadgeUsbLiveness.kt")
        val selectionGate = liveness
            .substringAfter("internal class BadgeUsbReconnectSelectionGate")
            .substringBefore("internal class BadgeUsbReconnectOperationGate")

        assertTrue(selectionGate.contains("fun currentStamp(): Long"))
        assertTrue(selectionGate.contains("fun advanceStamp(): Long"))
        assertTrue(selectionGate.contains("fun isStampCurrent(expectedStamp: Long): Boolean"))
        val advanceStamp = functionBody(selectionGate, "fun advanceStamp(): Long")
        assertTrue(
            "Selection stamps must fail closed instead of wrapping",
            advanceStamp.contains("Long.MAX_VALUE"),
        )
        assertTrue(repository.contains("private data class BadgeUsbSelectionSnapshot("))
        for (field in listOf(
            "stamp: Long",
            "lifecycleSession: Long",
            "lifecycleActive: Boolean",
            "operation: ActiveBadgeUsbReconnectOperation?",
            "operationGeneration: Long?",
            "operationActive: Boolean",
            "verifiedOwner: BadgeUsbOwnerKey?",
            "selectedAttachmentToken: BadgeUsbAttachmentToken?",
            "activeAttachmentToken: BadgeUsbAttachmentToken?",
            "activeConnection: Any?",
            "activeUsbLifecycleSession: Long?",
        )) {
            assertTrue("Selection snapshot missing exact component: $field", repository.contains(field))
        }
        assertTrue(repository.contains("private fun captureUsbEnumerationSnapshot("))
        assertTrue(repository.contains("private fun usbEnumerationSnapshotIsCurrent("))

        val refresh = repository
            .substringAfter("private fun refresh(lifecycleSession: Long)")
            .substringBefore("fun requestConnection()")
        val refreshCaptureAt = refresh.indexOf("captureUsbEnumerationSnapshot(")
        val refreshEnumerationAt = refresh.indexOf("findBadgeCandidates()")
        val refreshRevalidationAt = refresh.indexOf("usbEnumerationSnapshotIsCurrent(")
        assertTrue(refreshCaptureAt >= 0)
        assertTrue(refreshEnumerationAt > refreshCaptureAt)
        assertTrue(refreshRevalidationAt > refreshEnumerationAt)

        val request = repository
            .substringAfter("private fun requestConnection(")
            .substringBefore("fun sendPing()")
        val requestCaptureAt = request.indexOf("captureUsbEnumerationSnapshot(")
        val requestEnumerationAt = request.indexOf("findBadgeCandidates()")
        val requestRevalidationAt = request.indexOf("usbEnumerationSnapshotIsCurrent(")
        assertTrue(requestCaptureAt >= 0)
        assertTrue(requestEnumerationAt > requestCaptureAt)
        assertTrue(requestRevalidationAt > requestEnumerationAt)
    }

    @Test
    fun candidate_enumeration_uses_a_separate_epoch_for_detach_and_every_accepted_decision() {
        val repository = source("data/badge/BadgeUsbRepository.kt")
        val liveness = source("data/badge/BadgeUsbLiveness.kt")
        val epochGate = liveness
            .substringAfter("internal class BadgeUsbEnumerationEpochGate")
            .substringBefore("internal class BadgeUsbReceiverLifetimeGate")
        assertTrue(epochGate.contains("fun currentEpoch(): Long"))
        assertTrue(epochGate.contains("fun advanceEpoch(): Long"))
        assertTrue(epochGate.contains("fun isEpochCurrent(expectedEpoch: Long): Boolean"))
        assertTrue(epochGate.contains("Long.MAX_VALUE"))
        assertTrue(repository.contains(
            "private val usbEnumerationEpochGate = BadgeUsbEnumerationEpochGate()",
        ))
        assertTrue(repository.contains("private data class BadgeUsbEnumerationSnapshot("))
        assertTrue(repository.contains("selectionSnapshot: BadgeUsbSelectionSnapshot"))
        assertTrue(repository.contains("enumerationEpoch: Long"))

        val detach = repository
            .substringAfter("UsbManager.ACTION_USB_DEVICE_DETACHED -> {")
            .substringBefore("fun start()")
        val detachEpochAt = detach.indexOf("usbEnumerationEpochGate.advanceEpoch()")
        val detachActiveAt = detach.indexOf("lifecycleGate.activeSession()")
        val detachInvalidationAt = detach.indexOf("attachmentGate.invalidateMatching(")
        assertTrue(detachEpochAt >= 0)
        assertTrue(
            "Every relevant detach must advance the epoch before lifecycle routing",
            detachActiveAt > detachEpochAt,
        )
        assertTrue(detachInvalidationAt > detachActiveAt)

        val refresh = functionBody(repository, "private fun refresh(lifecycleSession: Long)")
        val refreshCaptureAt = refresh.indexOf("captureUsbEnumerationSnapshot(")
        val refreshEnumerateAt = refresh.indexOf("findBadgeCandidates()")
        val refreshRevalidateAt = refresh.indexOf("usbEnumerationSnapshotIsCurrent(")
        val refreshDecisionAt = refresh.indexOf("usbEnumerationEpochGate.advanceEpoch()")
        val refreshEmptyAt = refresh.indexOf("if (candidates.isEmpty())")
        val refreshAmbiguousAt = refresh.indexOf("if (candidates.size > 1)")
        val refreshSelectAt = refresh.indexOf("selectAttachment(device, lifecycleSession)")
        assertTrue(refreshCaptureAt >= 0)
        assertTrue(refreshEnumerateAt > refreshCaptureAt)
        assertTrue(refreshRevalidateAt > refreshEnumerateAt)
        assertTrue(refreshDecisionAt > refreshRevalidateAt)
        assertTrue(refreshEmptyAt > refreshDecisionAt)
        assertTrue(refreshAmbiguousAt > refreshDecisionAt)
        assertTrue(refreshSelectAt > refreshDecisionAt)

        val request = functionBody(repository, "private fun requestConnection(")
        val requestCaptureAt = request.indexOf("captureUsbEnumerationSnapshot(")
        val requestEnumerateAt = request.indexOf("findBadgeCandidates()")
        val requestRevalidateAt = request.indexOf("usbEnumerationSnapshotIsCurrent(")
        val requestDecisionAt = request.indexOf("usbEnumerationEpochGate.advanceEpoch()")
        val candidateDecisionAt = request.indexOf("badgeUsbReconnectCandidateAction(")
        assertTrue(requestCaptureAt >= 0)
        assertTrue(requestEnumerateAt > requestCaptureAt)
        assertTrue(requestRevalidateAt > requestEnumerateAt)
        assertTrue(requestDecisionAt > requestRevalidateAt)
        assertTrue(candidateDecisionAt > requestDecisionAt)

        val permissionReceiver = repository
            .substringAfter("ACTION_USB_PERMISSION -> {")
            .substringBefore("UsbManager.ACTION_USB_DEVICE_ATTACHED")
        assertFalse(
            "Enumeration churn must not invalidate an outstanding permission response",
            permissionReceiver.contains("usbEnumerationEpochGate"),
        )
    }

    @Test
    fun inactive_lifecycle_cannot_claim_or_invalidate_the_enumeration_epoch() {
        val repository = source("data/badge/BadgeUsbRepository.kt")
        val capture = functionBody(repository, "private fun captureUsbEnumerationSnapshot(")
        val selectionCaptureAt = capture.indexOf("captureUsbSelectionSnapshotLocked(lifecycleSession)")
        val activeCheckAt = capture.indexOf("selectionSnapshot.lifecycleActive")
        val inactiveReturnAt = capture.indexOf("return@withBarrier null")
        val advanceAt = capture.indexOf("usbEnumerationEpochGate.advanceEpoch()")

        assertTrue(
            "Enumeration must reject an inactive lifecycle before claiming the shared epoch",
            selectionCaptureAt >= 0 && activeCheckAt > selectionCaptureAt &&
                inactiveReturnAt > activeCheckAt && advanceAt > inactiveReturnAt,
        )
    }

    @Test
    fun selection_transactions_hold_no_external_USB_IO_except_exact_permission_dispatch() {
        val repository = source("data/badge/BadgeUsbRepository.kt")
        val selectionBlocks = balancedBlocksForCall(
            repository,
            "usbReconnectSelectionGate.withBarrier",
        )
        assertTrue("Expected selection transactions", selectionBlocks.isNotEmpty())
        val forbidden = listOf(
            "findBadgeCandidates(",
            "usbManager.hasPermission(",
            "usbManager.openDevice(",
            ".claimInterface(",
            ".bulkTransfer(",
            ".releaseInterface(",
            ".close()",
            "registerReceiver(",
            "unregisterReceiver(",
            "job.cancel()",
            "job.start()",
            "startUsbStatusPoller(",
            "terminateUsbInvestigationLocked(",
            "completeDetachedUsbInvestigation(",
            ".terminal.complete(",
            ".controlAck.complete(",
        )
        selectionBlocks.forEachIndexed { index, block ->
            forbidden.forEach { call ->
                assertFalse(
                    "Selection transaction #$index performs external I/O: $call",
                    block.contains(call),
                )
            }
        }
        val permissionDispatchBlocks = selectionBlocks.filter {
            it.contains("usbManager.requestPermission(")
        }
        assertTrue(
            "Only one exact permission transaction may hold the selection barrier",
            permissionDispatchBlocks.size == 1,
        )
        assertTrue(permissionDispatchBlocks.single().contains("dispatchGate.dispatchIfActive"))
    }

    @Test
    fun USB_reader_is_lazily_published_with_an_exact_stamped_start_gate() {
        val repository = source("data/badge/BadgeUsbRepository.kt")
        val connector = functionBody(repository, "private suspend fun connectToDevice(")
        val prepareReaderAt = connector.indexOf("prepareUsbReaderLocked(")
        val startReaderAt = connector.indexOf("startUsbReader(")
        val firstWriteAt = connector.indexOf("writeConnectionBoundLineLocked(")
        assertTrue("Reader must be prepared after exact active connection publication", prepareReaderAt >= 0)
        assertTrue("Reader must start only after its lazy slot is published", startReaderAt > prepareReaderAt)
        assertTrue("No handshake traffic may run before reader start succeeds", firstWriteAt > startReaderAt)
        assertTrue(connector.contains("if (preparedReader == null) return"))
        assertTrue(connector.contains("if (!startUsbReader(preparedReader)) return"))

        val preparation = functionBody(repository, "private fun prepareUsbReaderLocked(")
        val lazyAt = preparation.indexOf("CoroutineStart.LAZY")
        val barrierAt = preparation.indexOf("usbReconnectSelectionGate.withBarrier")
        val exactAt = preparation.indexOf("badgeUsbReaderOwnsExactSession(")
        val slotAt = preparation.indexOf("readJob = readerJob")
        val stampAt = preparation.indexOf("usbReconnectSelectionGate.advanceStamp()")
        assertTrue(lazyAt >= 0)
        assertTrue(barrierAt > lazyAt)
        assertTrue(exactAt > barrierAt)
        assertTrue(slotAt > exactAt)
        assertTrue(stampAt > slotAt)
        assertFalse(preparation.substring(barrierAt, stampAt).contains(".start()"))
        assertFalse(preparation.substring(barrierAt, stampAt).contains(".cancel()"))
        assertTrue(
            "A stop between publication and dispatch must make the reader inert at coroutine entry",
            preparation.contains("readJob === ownJob") &&
                Regex("badgeUsbReaderOwnsExactSession\\(").findAll(preparation).count() >= 2,
        )

        val starter = functionBody(repository, "private fun startUsbReader(")
        val cancelPreviousAt = starter.indexOf("prepared.previousJob?.cancel()")
        val startBarrierAt = starter.indexOf("usbReconnectSelectionGate.withBarrier")
        val stampCheckAt = starter.indexOf(
            "usbReconnectSelectionGate.isStampCurrent(prepared.selectionStamp)",
        )
        val slotCheckAt = starter.indexOf("readJob === prepared.job")
        val startAt = starter.indexOf("prepared.job.start()")
        assertTrue(cancelPreviousAt >= 0)
        assertTrue(startBarrierAt > cancelPreviousAt)
        assertTrue(stampCheckAt > startBarrierAt)
        assertTrue(slotCheckAt > stampCheckAt)
        assertTrue(startAt > slotCheckAt)
    }

    @Test
    fun every_USB_transfer_is_exact_session_leased_and_teardown_drains_before_close() {
        val repository = source("data/badge/BadgeUsbRepository.kt")
        val liveness = source("data/badge/BadgeUsbLiveness.kt")
        assertTrue(liveness.contains("internal class BadgeUsbIoArbiter"))
        assertTrue(repository.contains("private data class BadgeUsbIoSession("))
        assertTrue(repository.contains("private val usbIoArbiter = BadgeUsbIoArbiter()"))
        assertTrue(repository.contains("private var activeUsbIoSession: BadgeUsbIoSession? = null"))
        assertTrue(
            "Only the reader and writer should call the USB bulk-transfer API",
            Regex("\\.bulkTransfer\\(").findAll(repository).count() == 2,
        )

        val reader = functionBody(repository, "private fun prepareUsbReaderLocked(")
        val readerLeaseAt = reader.indexOf("usbIoArbiter.tryAcquire(ioSession)")
        val readerBarrierAt = reader.indexOf(
            "usbReconnectSelectionGate.withBarrier",
            readerLeaseAt.coerceAtLeast(0),
        )
        val readerExactAt = reader.indexOf("activeUsbIoSession === ioSession", readerBarrierAt)
        val readerTransferAt = reader.indexOf("connection.bulkTransfer(", readerExactAt)
        val readerReleaseAt = reader.indexOf("lease.close()", readerTransferAt)
        val readerTerminalAt = reader.indexOf("terminateUsbReaderSessionLocked(", readerReleaseAt)
        assertTrue(readerLeaseAt >= 0)
        assertTrue(readerBarrierAt > readerLeaseAt)
        assertTrue(readerExactAt > readerBarrierAt)
        assertTrue(readerTransferAt > readerExactAt)
        assertTrue(readerReleaseAt > readerTransferAt)
        assertTrue("Reader terminalization must happen after lease release", readerTerminalAt > readerReleaseAt)
        assertFalse(
            "A stale reader must never snapshot whichever verified owner is globally current",
            reader.contains("val expectedVerifiedOwner = verifiedUsbOwnerKey"),
        )

        val readerTerminal = functionBody(repository, "private fun terminateUsbReaderSessionLocked(")
        assertTrue(readerTerminal.contains("activeUsbIoSession !== ioSession"))
        assertTrue(readerTerminal.contains("verifiedUsbOwnerKey?.takeIf"))
        assertTrue(readerTerminal.contains("owner.lifecycleSession == lifecycleSession"))
        assertTrue(readerTerminal.contains("owner.attachmentToken == attachmentToken"))
        assertTrue(readerTerminal.contains("owner.connectionIdentity === connection"))

        val writer = functionBody(repository, "private suspend fun writeConnectionBoundLineLocked(")
        val writerLeaseAt = writer.indexOf("usbIoArbiter.tryAcquire(ioSession)")
        val writerBarrierAt = writer.indexOf("usbReconnectSelectionGate.withBarrier")
        val writerExactAt = writer.indexOf("activeUsbIoSession === ioSession")
        val writerTransferAt = writer.indexOf("connection.bulkTransfer(")
        val writerReleaseAt = writer.indexOf("lease.close()")
        assertTrue(writerLeaseAt >= 0)
        assertTrue(writerBarrierAt > writerLeaseAt)
        assertTrue(writerExactAt > writerBarrierAt)
        assertTrue(writerTransferAt > writerExactAt)
        assertTrue(writerReleaseAt > writerTransferAt)

        val stop = functionBody(repository, "fun stop()")
        val stopBarrierAt = stop.indexOf("usbReconnectSelectionGate.withBarrier")
        val stopRevokeAt = stop.indexOf("usbIoArbiter.revoke(")
        val stopEndAt = stop.indexOf("lifecycleGate.end(lifecycleSession)")
        assertTrue(stopBarrierAt >= 0)
        assertTrue(stopRevokeAt > stopBarrierAt)
        assertTrue(stopEndAt > stopRevokeAt)

        val revoke = functionBody(repository, "private fun revokeUsbSessionLocked(")
        assertTrue(revoke.contains("usbIoArbiter.revoke(ioSession)"))
        assertTrue(revoke.contains("activeUsbIoSession = null"))
        assertTrue(revoke.contains("ioDrain = ioDrain"))

        val close = functionBody(repository, "private fun closeDetachedUsbResources(")
        val cancelAt = close.indexOf("detached.readJob?.cancel()")
        val drainAt = close.indexOf("usbIoArbiter.awaitDrained(")
        assertTrue(cancelAt >= 0)
        assertTrue(drainAt > cancelAt)
        assertTrue(close.contains("advanceUsbIoCleanup(cleanup)"))
        val physicalClose = functionBody(
            repository,
            "private fun physicallyCloseDetachedUsbResources(",
        )
        val releaseAt = physicalClose.indexOf("releaseInterface(")
        val closeAt = physicalClose.indexOf("connection?.close()")
        assertTrue(releaseAt >= 0)
        assertTrue(closeAt > releaseAt)
        val cleanupAdvance = functionBody(repository, "private fun advanceUsbIoCleanup(")
        val physicalAt = cleanupAdvance.indexOf("physicallyCloseDetachedUsbResources(")
        val completeDrainAt = cleanupAdvance.indexOf("usbIoArbiter.completeDrain(")
        assertTrue(physicalAt >= 0)
        assertTrue(completeDrainAt > physicalAt)
        assertFalse(repository.contains("cancelAndJoin"))
        assertFalse(Regex("\\.join\\(").containsMatchIn(repository))

        val connector = functionBody(repository, "private suspend fun connectToDevice(")
        val ioActivateAt = connector.indexOf(
            "publishBadgeUsbIoSession(usbIoArbiter, ioSession)",
        )
        val activePublishAt = connector.indexOf("activeUsbIoSession = ioSession")
        assertTrue(ioActivateAt >= 0)
        assertTrue(activePublishAt > ioActivateAt)

        val lineHandler = functionBody(repository, "private fun handleLine(")
        val exactFrameBlocks = balancedBlocksForCall(
            lineHandler,
            "usbReconnectSelectionGate.withBarrier",
        ).filter { it.contains("activeUsbIoSession !== ioSession") }
        assertTrue("Frame publication needs an exact selection commit", exactFrameBlocks.isNotEmpty())
        assertTrue(exactFrameBlocks.any { it.contains("setState(frameStateUpdate)") })
        assertTrue(exactFrameBlocks.any { it.contains("prepareUsbInvestigationFrameLocked(") })
        assertTrue(lineHandler.contains("completePreparedUsbInvestigationFrame("))
    }

    @Test
    fun failed_publish_and_timed_out_drain_never_drop_USB_close_ownership() {
        val repository = source("data/badge/BadgeUsbRepository.kt")
        val connector = functionBody(repository, "private suspend fun connectToDevice(")
        val publish = connector.substringAfter("val publishActiveConnection = {")
            .substringBefore("val publicationResult")
        assertFalse(
            "A failed attachment publication must not complete the drain inside selection",
            publish.contains("usbIoArbiter.completeDrain("),
        )
        assertTrue(publish.contains("publishBadgeUsbIoSession(usbIoArbiter, ioSession)"))
        val failedPublish = connector.substringAfter("if (!publicationResult.published)")
            .substringBefore("val preparedReader")
        assertTrue(failedPublish.contains("ioDrain = publicationResult.drain"))

        val close = functionBody(repository, "private fun closeDetachedUsbResources(")
        assertTrue(close.contains("scheduleLateUsbDrain(cleanup)"))
        assertFalse(
            "A timeout must retain resources instead of dropping them",
            close.contains("leaving handle closed logically") && close.contains("return"),
        )
        val lateDrain = functionBody(repository, "private fun scheduleLateUsbDrain(")
        assertTrue(lateDrain.contains("lateUsbCleanupSlot"))
        assertTrue(lateDrain.contains("usbIoArbiter.awaitDrained("))
        assertTrue(lateDrain.contains("connectionMutex.withLock"))
        assertTrue(lateDrain.contains("advanceUsbIoCleanup(cleanup)"))
        assertTrue(lateDrain.contains("cleanup.phaseGate.isCompleted()"))
        assertTrue(lateDrain.contains("lifecycleGate.activeSession()"))
    }

    @Test
    fun abnormal_USB_cleanup_retains_exact_owner_until_close_and_completion_succeed() {
        val repository = source("data/badge/BadgeUsbRepository.kt")
        assertTrue(repository.contains("private data class RetainedBadgeUsbDrainCleanup("))
        assertTrue(repository.contains("BadgeUsbIoCleanupPhaseGate()"))
        assertTrue(repository.contains("BadgeUsbRetainedCleanupSlot()"))

        val close = functionBody(repository, "private fun closeDetachedUsbResources(")
        val advance = functionBody(repository, "private fun advanceUsbIoCleanup(")
        val noDrain = close.substringAfter("if (ioDrain == null)")
            .substringBefore("if (!usbIoArbiter.awaitDrained")
        assertTrue(close.contains("markDrained()"))
        assertTrue(advance.contains("shouldAttemptClose()"))
        assertTrue(advance.contains("shouldAttemptDrainCompletion()"))
        assertTrue(
            "Every unsuccessful cleanup phase must hand exact A to the retained reaper",
            close.contains("scheduleLateUsbDrain(cleanup)"),
        )
        assertTrue(
            "A close-only cleanup must retain ownership when platform close fails",
            close.indexOf("val cleanup = RetainedBadgeUsbDrainCleanup(") in
                0 until close.indexOf("if (ioDrain == null)") &&
                noDrain.contains("markDrained()") &&
                noDrain.contains("advanceUsbIoCleanup(cleanup)") &&
                noDrain.contains("scheduleLateUsbDrain(cleanup)"),
        )

        val physicalClose = functionBody(
            repository,
            "private fun physicallyCloseDetachedUsbResources(",
        )
        assertTrue(
            "Physical close must report success",
            physicalClose.contains("return runCatching"),
        )
        assertTrue(physicalClose.contains("connection?.close()"))
        assertTrue(physicalClose.contains(".isSuccess"))

        val lateDrain = functionBody(repository, "private fun scheduleLateUsbDrain(")
        assertTrue(lateDrain.contains("cleanup.phaseGate"))
        assertTrue(lateDrain.contains("lateUsbCleanupSlot.tryInstall("))
        assertTrue(lateDrain.contains("lateUsbCleanupSlot.finishWorker("))
        assertTrue(lateDrain.contains("completed = cleanup.phaseGate.isCompleted()"))
        assertTrue(
            "An incomplete/cancelled worker must hand retained A to a successor",
            lateDrain.contains("if (completed)") &&
                lateDrain.contains("scheduleLateUsbDrain(cleanup)"),
        )
        assertTrue(lateDrain.contains("advanceUsbIoCleanup(cleanup)"))
        val closeAt = advance.indexOf("physicallyCloseDetachedUsbResources(")
        val markClosedAt = advance.indexOf("markClosed()")
        val completeAt = advance.indexOf("usbIoArbiter.completeDrain(")
        val markCompletedAt = advance.indexOf("markCompleted()")
        assertTrue(closeAt >= 0 && markClosedAt > closeAt)
        assertTrue(completeAt > markClosedAt && markCompletedAt > completeAt)
    }

    @Test
    fun completed_late_cleanup_releases_A_before_connection_mutex_handoff_to_B() {
        val repository = source("data/badge/BadgeUsbRepository.kt")
        val lateDrain = functionBody(repository, "private fun scheduleLateUsbDrain(")
        val connectionBlocks = balancedBlocksForCall(
            lateDrain,
            "connectionMutex.withLock",
        )
        val completionHandoff = connectionBlocks.firstOrNull {
            it.contains("advanceUsbIoCleanup(cleanup)")
        }.orEmpty()
        val advanceAt = completionHandoff.indexOf("advanceUsbIoCleanup(cleanup)")
        val completedAt = completionHandoff.indexOf("cleanup.phaseGate.isCompleted()")
        val releaseAt = completionHandoff.indexOf("lateUsbCleanupSlot.finishWorker(")
        assertTrue(
            "A must release its retained slot inside the same connection mutex that gates B",
            advanceAt >= 0 && completedAt > advanceAt && releaseAt > completedAt,
        )
        assertTrue(
            "Outer cleanup must not finish a worker already released at the atomic handoff",
            lateDrain.contains("if (!workerReleased)") ||
                lateDrain.contains("if (!releasedUnderConnectionMutex)"),
        )

        val connector = functionBody(repository, "private suspend fun connectToDevice(")
        val connectorLock = balancedBlocksForCall(
            connector,
            "connectionMutex.withLock",
        ).firstOrNull().orEmpty()
        assertTrue(
            "B activation must remain behind the same connection mutex",
            connectorLock.contains("publishBadgeUsbIoSession(usbIoArbiter, ioSession)"),
        )
    }

    @Test
    fun empty_and_ambiguous_selection_clear_only_matching_permission_operation() {
        val repository = source("data/badge/BadgeUsbRepository.kt")
        val refresh = functionBody(repository, "private fun refresh(lifecycleSession: Long)")
        val request = functionBody(repository, "private fun requestConnection(")
        val genericHelper = functionBody(
            repository,
            "private fun clearGenericUsbPermissionOperationLocked(",
        )
        val reconnectHelper = functionBody(
            repository,
            "private fun clearReconnectUsbPermissionOperationLocked(",
        )

        assertTrue(genericHelper.contains("badgeUsbGenericPermissionCleanupMatches("))
        assertTrue(genericHelper.contains("clearExactUsbPermissionOperationLocked(permission)"))
        assertTrue(reconnectHelper.contains("badgeUsbReconnectPermissionCleanupMatches("))
        assertTrue(reconnectHelper.contains("clearExactUsbPermissionOperationLocked(permission)"))

        val refreshEmpty = refresh.substringAfter("if (candidates.isEmpty())")
            .substringBefore("if (candidates.size > 1)")
        val refreshAmbiguous = refresh.substringAfter("if (candidates.size > 1)")
            .substringBefore("val device = candidates.first()")
        assertTrue(refreshEmpty.contains("clearGenericUsbPermissionOperationLocked("))
        assertTrue(refreshEmpty.contains("permissionCleared"))
        assertTrue(refreshAmbiguous.contains("clearGenericUsbPermissionOperationLocked("))
        assertTrue(refreshAmbiguous.contains("permissionCleared"))

        val normalRefresh = request.substringAfter(
            "BadgeUsbReconnectCandidateAction.NORMAL_REFRESH ->",
        ).substringBefore("BadgeUsbReconnectCandidateAction.FAIL_AMBIGUOUS ->")
        val failAmbiguous = request.substringAfter(
            "BadgeUsbReconnectCandidateAction.FAIL_AMBIGUOUS ->",
        ).substringBefore("BadgeUsbReconnectCandidateAction.CONNECT_ONE ->")
        assertTrue(normalRefresh.contains("clearGenericUsbPermissionOperationLocked("))
        assertTrue(normalRefresh.contains("permissionCleared"))
        assertTrue(failAmbiguous.contains("clearReconnectUsbPermissionOperationLocked("))
        assertFalse(
            "Reconnect cleanup follows operation identity, not its mutable current attempt",
            failAmbiguous.contains("currentAttempt()?.attachmentToken"),
        )
    }

    @Test
    fun permission_platform_dispatch_revalidates_exact_operation_under_selection_barrier() {
        val repository = source("data/badge/BadgeUsbRepository.kt")
        val request = functionBody(repository, "private fun requestConnection(")
        val dispatchBlocks = balancedBlocksForCall(
            request,
            "usbReconnectSelectionGate.withBarrier",
        ).filter { it.contains("usbManager.requestPermission(") }

        assertTrue(
            "Final validation and the physical permission side effect must be one transaction",
            dispatchBlocks.size == 1,
        )
        val dispatch = dispatchBlocks.single()
        val validateAt = dispatch.indexOf("usbPermissionOperationMayDispatchLocked(")
        val gateAt = dispatch.indexOf("permissionOperation.dispatchGate.dispatchIfActive")
        val sideEffectAt = dispatch.indexOf("usbManager.requestPermission(")
        assertTrue(validateAt >= 0 && gateAt > validateAt && sideEffectAt > gateAt)

        val validator = functionBody(
            repository,
            "private fun usbPermissionOperationMayDispatchLocked(",
        )
        assertTrue(validator.contains("badgeUsbPermissionMayDispatch("))
        assertTrue(validator.contains("activeUsbPermissionOperation"))
        assertTrue(validator.contains("usbReconnectSelectionGate.isStampCurrent("))
        assertTrue(validator.contains("lifecycleGate.isActive("))
        assertTrue(validator.contains("attachmentGate.acceptsPermission("))

        val exactClear = functionBody(
            repository,
            "private fun clearExactUsbPermissionOperationLocked(",
        )
        val cancelAt = exactClear.indexOf("expected.dispatchGate.cancel()")
        val clearAt = exactClear.indexOf("activeUsbPermissionOperation = null")
        assertTrue(
            "Every winning stop/detach/replacement must cancel or await dispatch before clearing",
            cancelAt >= 0 && clearAt > cancelAt,
        )
        assertTrue(
            "Permission nulling must be centralized behind the dispatch gate",
            Regex("activeUsbPermissionOperation\\s*=\\s*null")
                .findAll(repository)
                .count() == 1,
        )

        val refresh = functionBody(repository, "private fun refresh(lifecycleSession: Long)")
        val refreshReplacement = refresh.substringAfter(
            "if (attachmentToken != previousToken)",
        ).substringBefore("PendingBadgeUsbSelection(")
        val requestReplacement = request.substringAfter(
            "if (preparedAttempt.first != previousToken",
        ).substringBefore("PendingBadgeUsbSelection(")
        for ((label, replacement) in listOf(
            "refresh attachment replacement" to refreshReplacement,
            "request attempt replacement" to requestReplacement,
        )) {
            val cancelReplacementAt = replacement.indexOf(
                "clearExactUsbPermissionOperationLocked",
            )
            val advanceReplacementAt = replacement.indexOf(
                "usbReconnectSelectionGate.advanceStamp()",
            )
            assertTrue(
                "$label must cancel P_A in the same selection transaction before stamp B",
                cancelReplacementAt >= 0 && advanceReplacementAt > cancelReplacementAt,
            )
        }
    }

    @Test
    fun reconnect_terminal_cleanup_retires_its_exact_permission_before_attempt_removal() {
        val repository = source("data/badge/BadgeUsbRepository.kt")
        val clearReconnect = functionBody(
            repository,
            "private fun clearUsbReconnectOperationLocked(",
        )
        val permissionAt = clearReconnect.indexOf(
            "clearReconnectUsbPermissionOperationLocked(",
        )
        val clearAttemptAt = clearReconnect.indexOf("operation.clearAttempt()")

        assertTrue(
            "Expiry and every other reconnect terminal path must consume exact permission P_A",
            permissionAt >= 0 && clearAttemptAt > permissionAt,
        )
        assertTrue(clearReconnect.contains("operation.ticket.lifecycleSession"))
    }

    @Test
    fun permission_request_carries_and_revalidates_the_exact_selection_stamp() {
        val repository = source("data/badge/BadgeUsbRepository.kt")
        val permissionReceiver = repository
            .substringAfter("ACTION_USB_PERMISSION -> {")
            .substringBefore("UsbManager.ACTION_USB_DEVICE_ATTACHED")
        val request = repository
            .substringAfter("private fun requestConnection(")
            .substringBefore("fun sendPing()")

        assertTrue(repository.contains("private const val EXTRA_USB_SELECTION_STAMP"))
        assertTrue(request.contains(
            "putExtra(EXTRA_USB_SELECTION_STAMP, permissionOperation.selectionStamp)",
        ))
        assertTrue(request.contains("selectionStamp = permissionOperation.selectionStamp"))
        assertTrue(
            "PendingIntent identity must include the stamp because Android ignores extras",
            request.contains(".setData(Uri.parse(") &&
                request.contains(
                    "permission/\${permissionOperation.generation}/\" +",
                ) &&
                request.contains("\${permissionOperation.selectionStamp}/"),
        )
        assertTrue(permissionReceiver.contains("EXTRA_USB_SELECTION_STAMP"))
        assertTrue(permissionReceiver.contains("usbReconnectSelectionGate.isStampCurrent("))
        assertTrue(permissionReceiver.contains("operation.selectionStamp != selectionStamp"))
        assertTrue(permissionReceiver.contains("operation.generation != permissionGeneration"))
        assertTrue(
            "The permission request code must distinguish reused lifecycle/attachment identities",
            functionBody(repository, "private fun usbPermissionRequestCode(")
                .contains("selectionStamp"),
        )
    }

    @Test
    fun permission_outcome_is_one_shot_and_waiting_precedes_dispatch() {
        val repository = source("data/badge/BadgeUsbRepository.kt")
        val request = functionBody(repository, "private fun requestConnection(")
        val permissionReceiver = repository
            .substringAfter("ACTION_USB_PERMISSION -> {")
            .substringBefore("UsbManager.ACTION_USB_DEVICE_ATTACHED")

        assertTrue(repository.contains("private data class ActiveBadgeUsbPermissionOperation("))
        assertTrue(repository.contains("private var activeUsbPermissionOperation:"))
        assertTrue(repository.contains("private const val EXTRA_USB_PERMISSION_GENERATION"))
        assertTrue(request.contains("permissionGeneration = permissionOperation.generation"))
        assertTrue(request.contains("permission/\${permissionOperation.generation}/"))

        val waitingAt = request.indexOf("message = \"Waiting for USB permission\"")
        val dispatchAt = request.indexOf("usbManager.requestPermission(")
        assertTrue("Waiting must publish before permission dispatch", waitingAt >= 0 && dispatchAt > waitingAt)
        assertFalse(
            "No post-dispatch waiting write may overwrite a synchronous denial",
            request.substring(dispatchAt.coerceAtLeast(0)).contains(
                "message = \"Waiting for USB permission\"",
            ),
        )
        assertTrue(repository.contains("clearExactUsbPermissionOperationLocked(permissionOperation)"))
        assertTrue(request.contains("catch (failure: Exception)"))

        val prepare = balancedBlocksForCall(
            request,
            "usbReconnectSelectionGate.withBarrier",
        ).first { it.contains("ActiveBadgeUsbPermissionOperation(") }
        val publishStampAt = prepare.indexOf("usbReconnectSelectionGate.advanceStamp()")
        val operationAt = prepare.indexOf("ActiveBadgeUsbPermissionOperation(")
        val activeAt = prepare.indexOf("replaceUsbPermissionOperationLocked(permissionOperation)")
        val atomicWaitingAt = prepare.indexOf("message = \"Waiting for USB permission\"")
        assertTrue(
            "Permission operation publication must invalidate every stale selection snapshot",
            publishStampAt >= 0 && operationAt > publishStampAt && activeAt > operationAt &&
                atomicWaitingAt > activeAt,
        )
        assertTrue(prepare.contains("selectionStamp = permissionSelectionStamp"))

        val consumeAt = permissionReceiver.indexOf(
            "clearExactUsbPermissionOperationLocked(operation)",
        )
        val stampAt = permissionReceiver.indexOf("usbReconnectSelectionGate.advanceStamp()")
        val denialAt = permissionReceiver.indexOf("message = \"USB permission denied\"")
        val snapshotAt = permissionReceiver.indexOf("captureUsbSelectionSnapshotLocked(")
        assertTrue(permissionReceiver.contains("EXTRA_USB_PERMISSION_GENERATION"))
        assertTrue(permissionReceiver.contains("operation.generation != permissionGeneration"))
        assertTrue("Permission callback must consume once", consumeAt >= 0)
        assertTrue("Consumption must advance stable selection identity", stampAt > consumeAt)
        assertTrue("Denial must publish in the same atomic callback commit", denialAt > stampAt)
        assertTrue("Grant must capture a fresh post-consumption snapshot", snapshotAt > stampAt)
    }

    @Test
    fun permission_setup_and_dispatch_failure_clear_exact_without_overwriting_newer_state() {
        val repository = source("data/badge/BadgeUsbRepository.kt")
        val request = functionBody(repository, "private fun requestConnection(")
        val tryAt = request.indexOf("try {")
        val pendingIntentAt = request.indexOf("PendingIntent.getBroadcast(")
        val dispatchAt = request.indexOf("usbManager.requestPermission(")
        val catchAt = request.indexOf("catch (failure: Exception)")
        assertTrue(tryAt >= 0 && pendingIntentAt > tryAt && dispatchAt > pendingIntentAt)
        assertTrue(catchAt > dispatchAt)
        assertTrue(request.contains("failUsbPermissionDispatch(permissionOperation, device, failure)"))

        val failure = functionBody(repository, "private fun failUsbPermissionDispatch(")
        val clearAt = failure.indexOf(
            "clearExactUsbPermissionOperationLocked(permissionOperation)",
        )
        val fullOwnershipAt = failure.indexOf(
            "usbReconnectSelectionGate.isStampCurrent(permissionOperation.selectionStamp)",
        )
        val advanceAt = failure.indexOf("usbReconnectSelectionGate.advanceStamp()")
        val stateAt = failure.indexOf("message = \"USB permission request failed\"")
        assertTrue(clearAt >= 0)
        assertTrue(fullOwnershipAt > clearAt)
        assertTrue(advanceAt > fullOwnershipAt)
        assertTrue("Only a still-current operation may publish failure", stateAt > advanceAt)
        val exactClear = functionBody(
            repository,
            "private fun clearExactUsbPermissionOperationLocked(",
        )
        assertTrue(exactClear.contains("activeUsbPermissionOperation !== expected"))
    }

    @Test
    fun detach_advances_enumeration_before_resolving_the_active_lifecycle() {
        val repository = source("data/badge/BadgeUsbRepository.kt")
        val detach = repository
            .substringAfter("UsbManager.ACTION_USB_DEVICE_DETACHED -> {")
            .substringBefore("fun start()")
        val barrierAt = detach.indexOf("usbReconnectSelectionGate.withBarrier")
        val epochAt = detach.indexOf("usbEnumerationEpochGate.advanceEpoch()")
        val lifecycleAt = detach.indexOf("lifecycleGate.activeSession()")
        assertTrue(barrierAt >= 0)
        assertTrue("Detach epoch must advance before lifecycle routing", epochAt > barrierAt)
        assertTrue(lifecycleAt > epochAt)
        assertFalse(
            "Detach must not capture a lifecycle outside the epoch transaction",
            detach.substring(0, barrierAt).contains("lifecycleGate.activeSession()"),
        )
    }

    @Test
    fun USB_teardown_revokes_logical_ownership_before_physical_close() {
        val repository = source("data/badge/BadgeUsbRepository.kt")
        assertTrue(repository.contains("private data class DetachedBadgeUsbResources("))
        assertFalse(
            "The mixed logical and physical disconnect helper must be removed",
            repository.contains("private fun disconnectLocked("),
        )

        val revoke = functionBody(repository, "private fun revokeUsbSessionLocked(")
        assertTrue(revoke.contains("verifiedUsbOwnerKey = null"))
        assertTrue(revoke.contains("activeConnection = null"))
        assertTrue(revoke.contains("activeAttachmentToken = null"))
        assertFalse(revoke.contains("releaseInterface("))
        assertFalse(revoke.contains(".close()"))

        val close = functionBody(repository, "private fun closeDetachedUsbResources(")
        assertTrue(close.contains("advanceUsbIoCleanup(cleanup)"))
        val physicalClose = functionBody(
            repository,
            "private fun physicallyCloseDetachedUsbResources(",
        )
        assertTrue(physicalClose.contains("releaseInterface("))
        assertTrue(physicalClose.contains(".close()"))

        val convertedFamilies = listOf(
            "private fun cancelUsbReconnectForDetachedOwner(",
            "private fun cleanupUsbDetachAndRescan(",
            "private fun failAmbiguousUsbReconnect(",
            "private suspend fun expireUsbReconnect(",
            "private suspend fun connectToDevice(",
            "private fun startUsbIdentityHandshake(",
            "private fun rejectUsbIdentityLocked(",
            "private fun terminateUsbReaderSessionLocked(",
            "private fun terminateVerifiedUsbSessionLocked(",
            "private fun disconnect(",
        )
        convertedFamilies.forEach { marker ->
            val body = functionBody(repository, marker)
            assertTrue("$marker does not logically revoke exact USB resources", body.contains("revokeUsbSessionLocked("))
            val revokeAt = body.indexOf("revokeUsbSessionLocked(")
            val closeAt = body.indexOf("closeDetachedUsbResources(")
            assertTrue("$marker closes before logical revoke", closeAt > revokeAt)
        }
    }

    @Test
    fun terminal_families_commit_exact_state_before_physical_close() {
        val repository = source("data/badge/BadgeUsbRepository.kt")
        val handshake = functionBody(repository, "private fun startUsbIdentityHandshake(")
        assertTerminalOrdering(
            label = "handshake timeout",
            body = handshake,
            exactMarker = "badgeUsbHandshakeOwnsSession(",
            requireLocalMutex = true,
        )

        val identityRejection = functionBody(repository, "private fun rejectUsbIdentityLocked(")
        assertFalse(identityRejection.contains("scope.launch"))
        assertTrue(
            "Identity rejection must be invoked by the reader while it owns connectionMutex",
            repository.contains("connectionMutex.withBadgeUsbReaderOwner") &&
                repository.contains("rejectUsbIdentityLocked("),
        )
        assertTerminalOrdering(
            label = "identity rejection",
            body = identityRejection,
            exactMarker = "val ownsExactHandshake",
            requireLocalMutex = false,
        )

        val readerTerminal = functionBody(repository, "private fun terminateUsbReaderSessionLocked(")
        assertFalse(readerTerminal.contains("scope.launch"))
        val reader = functionBody(repository, "private fun prepareUsbReaderLocked(")
        assertTrue(
            "Reader terminal helpers must be called while connectionMutex is held",
            reader.contains("connectionMutex.withLock") &&
                reader.contains("terminateUsbReaderSessionLocked("),
        )
        assertTerminalOrdering(
            label = "unverified reader failure",
            body = readerTerminal,
            exactMarker = "badgeUsbReaderTerminalOwnsExactSession(",
            requireLocalMutex = false,
        )

        val asyncStop = functionBody(repository, "private fun disconnect(")
        assertTerminalOrdering(
            label = "async stop",
            body = asyncStop,
            exactMarker = "val ownsExpectedConnection",
            requireLocalMutex = true,
        )

        val retryAt = handshake.indexOf("BadgeUsbHandshakeTimerAction.RETRY")
        val retryBlock = handshake.substring(retryAt.coerceAtLeast(0))
            .substringBefore("BadgeUsbHandshakeTimerAction.FAIL")
        assertFalse(
            "Handshake retry writes must remain outside selection",
            retryBlock.contains("usbReconnectSelectionGate.withBarrier"),
        )

        val verifiedFailure = functionBody(
            repository,
            "private fun terminateVerifiedUsbSessionLocked(",
        )
        val verifiedRevokeAt = verifiedFailure.indexOf("revokeUsbSessionLocked(")
        val verifiedCloseAt = verifiedFailure.indexOf("closeDetachedUsbResources(")
        val reconnectAt = verifiedFailure.indexOf(
            "preparedReconnect?.let(::startVerifiedUsbReconnectLocked)",
        )
        assertTrue(verifiedRevokeAt >= 0)
        assertTrue(verifiedCloseAt > verifiedRevokeAt)
        assertTrue(
            "Reconnect scheduler must start only after the old USB resources close",
            reconnectAt > verifiedCloseAt,
        )
    }

    @Test
    fun permission_denial_publication_is_exact_and_selection_barrier_bounded() {
        val repository = source("data/badge/BadgeUsbRepository.kt")
        val permissionReceiver = repository
            .substringAfter("ACTION_USB_PERMISSION -> {")
            .substringBefore("UsbManager.ACTION_USB_DEVICE_ATTACHED")
        val barrierAt = permissionReceiver.indexOf("usbReconnectSelectionGate.withBarrier")
        val consumeAt = permissionReceiver.indexOf(
            "clearExactUsbPermissionOperationLocked(operation)",
        )
        val stateAt = permissionReceiver.indexOf("message = \"USB permission denied\"")

        assertTrue(barrierAt >= 0)
        assertTrue(consumeAt > barrierAt)
        assertTrue(stateAt > barrierAt)
        val atomicCommit = permissionReceiver.substring(barrierAt, stateAt)
        assertTrue(atomicCommit.contains("operation.generation != permissionGeneration"))
        assertTrue(atomicCommit.contains("operation.selectionStamp != selectionStamp"))
        assertTrue(atomicCommit.contains("usbReconnectSelectionGate.isStampCurrent(selectionStamp)"))
        assertTrue(atomicCommit.contains("lifecycleGate.isActive(lifecycleSession)"))
        assertTrue(atomicCommit.contains("attachmentGate.acceptsPermission("))
        assertTrue(atomicCommit.contains("attachmentToken"))
        assertTrue(atomicCommit.contains("identity"))
        assertTrue(atomicCommit.contains("verifiedUsbOwnerKey == null"))
        assertTrue(atomicCommit.contains("activeConnection == null"))
    }

    @Test
    fun connect_state_commits_revalidate_inside_the_selection_barrier() {
        val repository = source("data/badge/BadgeUsbRepository.kt")
        val connector = repository
            .substringAfter("private suspend fun connectToDevice(")
            .substringBefore("private fun startUsbIdentityHandshake(")
        val commitHelper = connector
            .substringAfter("fun commitConnectStateIfCurrent(")
            .substringBefore("val entryConnectionIdentity")

        assertTrue(connector.contains("fun commitConnectStateIfCurrent("))
        assertTrue(commitHelper.contains("usbReconnectSelectionGate.withBarrier"))
        assertTrue(commitHelper.contains("connectContextIsCurrent(expectedConnectionIdentity)"))
        assertTrue(commitHelper.contains("setState(update)"))
        assertTrue(
            "Endpoint/open/claim failures must all use the exact commit helper",
            Regex("commitConnectStateIfCurrent\\(").findAll(connector).count() >= 4,
        )
        val initialCommit = connector
            .substringAfter("val mayOpen = usbReconnectSelectionGate.withBarrier {")
            .substringBefore("closeDetachedUsbResources(detachedPrevious)")
        val initialExactAt = initialCommit.indexOf("connectContextIsCurrent(entryConnectionIdentity)")
        val initialRevokeAt = initialCommit.indexOf("revokeUsbSessionLocked()")
        val initialStateAt = initialCommit.indexOf("message = \"Opening badge USB serial\"")
        assertTrue(initialExactAt >= 0)
        assertTrue(initialRevokeAt > initialExactAt)
        assertTrue(initialStateAt > initialRevokeAt)
        assertTrue(connector.contains("message = \"Opening badge USB serial\""))
        assertTrue(connector.contains("message = \"No readable USB serial endpoint found\""))
        assertTrue(connector.contains("message = \"Could not open USB badge\""))
        assertTrue(connector.contains("message = \"Could not claim USB badge interface\""))
    }

    @Test
    fun ambiguity_and_expiry_terminal_commits_are_selection_atomic() {
        val repository = source("data/badge/BadgeUsbRepository.kt")
        val ambiguity = repository
            .substringAfter("private fun failAmbiguousUsbReconnect(")
            .substringBefore("private suspend fun runUsbReconnectScheduler(")
        val ambiguityMutexAt = ambiguity.indexOf("connectionMutex.withLock")
        val ambiguityBarrierAt = ambiguity.indexOf("usbReconnectSelectionGate.withBarrier")
        val ambiguityExactAt = ambiguity.indexOf("usbReconnectOperationSlot.current() !== operation")
        val ambiguityErrorAt = ambiguity.indexOf("reportAmbiguousBadgeDevices(candidates)")
        val ambiguityClearAt = ambiguity.indexOf("clearUsbReconnectOperationLocked(operation)")
        assertTrue(ambiguityMutexAt >= 0)
        assertTrue(ambiguityBarrierAt > ambiguityMutexAt)
        assertTrue(ambiguityExactAt > ambiguityBarrierAt)
        assertTrue(ambiguityErrorAt > ambiguityExactAt)
        assertTrue(ambiguityClearAt > ambiguityErrorAt)

        val expiry = repository
            .substringAfter("private suspend fun expireUsbReconnect(")
            .substringBefore("private fun registerReceiverIfNeeded()")
        val expiryMutexAt = expiry.indexOf("connectionMutex.withLock")
        val expiryBarrierAt = expiry.indexOf("usbReconnectSelectionGate.withBarrier")
        val expiryExactAt = expiry.indexOf("usbReconnectOperationSlot.current() !== operation")
        val expiryErrorAt = expiry.indexOf("reduceBadgeUsbTerminalError(")
        val expiryClearAt = expiry.indexOf("clearUsbReconnectOperationLocked(operation)", expiryErrorAt)
        assertTrue(expiryMutexAt >= 0)
        assertTrue(expiryBarrierAt > expiryMutexAt)
        assertTrue(expiryExactAt > expiryBarrierAt)
        assertTrue(expiryErrorAt > expiryExactAt)
        assertTrue(expiryClearAt > expiryErrorAt)
    }

    @Test
    fun reconnect_completion_apis_share_one_state_machine() {
        val liveness = source("data/badge/BadgeUsbLiveness.kt")
        val operationGate = liveness
            .substringAfter("internal class BadgeUsbReconnectOperationGate")
            .substringBefore("internal enum class BadgeUsbStatusPollDecision")
        val compatibilityApi = operationGate
            .substringAfter("fun completeAndClearIfActive(")
            .substringBefore("\n    @Synchronized\n    fun completeHandshakeAndClearIfActive(")

        assertTrue(
            "Compatibility completion must delegate to the production handshake completion API",
            compatibilityApi.contains("completeHandshakeAndClearIfActive("),
        )
    }

    private fun assertInvocationTimeOwnerCapture(block: String) {
        val captureAt = block.indexOf("val expectedOwner = verifiedUsbOwnerKey")
        val launchAt = block.indexOf("scope.launch")
        assertTrue("missing immutable USB owner snapshot", captureAt >= 0)
        assertTrue("USB owner must be captured before coroutine launch", launchAt > captureAt)
        assertFalse(
            "Coroutine must not late-capture a replacement USB owner",
            block.substring(launchAt).contains("verifiedUsbOwnerKey"),
        )
    }

    private fun assertTerminalOrdering(
        label: String,
        body: String,
        exactMarker: String,
        requireLocalMutex: Boolean,
    ) {
        val mutexAt = body.indexOf("connectionMutex.withLock")
        val barrierAt = body.indexOf("usbReconnectSelectionGate.withBarrier")
        val exactAt = body.indexOf(exactMarker)
        val revokeAt = body.indexOf("revokeUsbSessionLocked(")
        val stateAt = body.indexOf("setState")
        val closeAt = body.indexOf("closeDetachedUsbResources(")
        if (requireLocalMutex) {
            assertTrue("$label must acquire connectionMutex", mutexAt >= 0)
            assertTrue("$label must acquire selection after connectionMutex", barrierAt > mutexAt)
        } else {
            assertTrue("$label must acquire selection", barrierAt >= 0)
        }
        assertTrue("$label must exactly revalidate inside selection", exactAt > barrierAt)
        assertTrue("$label must revoke inside selection", revokeAt > exactAt)
        assertTrue("$label must publish state atomically with revoke", stateAt > revokeAt)
        assertTrue("$label must physically close after the selection transaction", closeAt > stateAt)
    }

    private fun functionBody(source: String, marker: String): String {
        val markerAt = source.indexOf(marker)
        assertTrue("Missing function marker: $marker", markerAt >= 0)
        val bodyStart = source.indexOf('{', markerAt)
        assertTrue("Missing function body for: $marker", bodyStart >= 0)
        return balancedBraceBlock(source, bodyStart)
    }

    private fun balancedBlocksForCall(source: String, marker: String): List<String> {
        val blocks = mutableListOf<String>()
        var searchFrom = 0
        while (true) {
            val markerAt = source.indexOf(marker, searchFrom)
            if (markerAt < 0) return blocks
            val bodyStart = source.indexOf('{', markerAt + marker.length)
            assertTrue("Missing block after: $marker", bodyStart >= 0)
            val block = balancedBraceBlock(source, bodyStart)
            blocks += block
            searchFrom = bodyStart + block.length
        }
    }

    private fun balancedBraceBlock(source: String, bodyStart: Int): String {
        var depth = 0
        var index = bodyStart
        var quote: Char? = null
        var escaped = false
        var lineComment = false
        var blockComment = false
        while (index < source.length) {
            val current = source[index]
            val next = source.getOrNull(index + 1)
            if (lineComment) {
                if (current == '\n') lineComment = false
                index += 1
                continue
            }
            if (blockComment) {
                if (current == '*' && next == '/') {
                    blockComment = false
                    index += 2
                } else {
                    index += 1
                }
                continue
            }
            if (quote != null) {
                if (escaped) {
                    escaped = false
                } else if (current == '\\') {
                    escaped = true
                } else if (current == quote) {
                    quote = null
                }
                index += 1
                continue
            }
            if (current == '/' && next == '/') {
                lineComment = true
                index += 2
                continue
            }
            if (current == '/' && next == '*') {
                blockComment = true
                index += 2
                continue
            }
            if (current == '"' || current == '\'') {
                quote = current
                index += 1
                continue
            }
            if (current == '{') depth += 1
            if (current == '}') {
                depth -= 1
                if (depth == 0) return source.substring(bodyStart, index + 1)
            }
            index += 1
        }
        throw AssertionError("Unbalanced block starting at $bodyStart")
    }

    private fun source(relativePath: String): String {
        val candidates = listOf(
            File("src/main/java/com/friendorfoe/$relativePath"),
            File("app/src/main/java/com/friendorfoe/$relativePath"),
            File("android/app/src/main/java/com/friendorfoe/$relativePath"),
        )
        return candidates.first { it.isFile }.readText()
    }
}
