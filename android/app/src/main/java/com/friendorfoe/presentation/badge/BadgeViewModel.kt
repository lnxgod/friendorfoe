package com.friendorfoe.presentation.badge

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.friendorfoe.data.badge.BadgeCapability
import com.friendorfoe.data.badge.BadgeCapabilitySupport
import com.friendorfoe.data.badge.BadgeCommand
import com.friendorfoe.data.badge.BadgeCommandOutcome
import com.friendorfoe.data.badge.BadgeConnectionEvidence
import com.friendorfoe.data.badge.BadgeConnectionPhase
import com.friendorfoe.data.badge.BadgeControlPort
import com.friendorfoe.data.badge.BadgeControlStatus
import com.friendorfoe.data.badge.BadgeDisplayAction
import com.friendorfoe.data.badge.BadgeDisplayPolicy
import com.friendorfoe.data.badge.BadgeNetworkMode
import com.friendorfoe.data.badge.BadgeRepositoryState
import com.friendorfoe.data.badge.BadgeTheme
import com.friendorfoe.data.badge.BadgeTransport
import com.friendorfoe.data.badge.aged
import com.friendorfoe.data.badge.badgeCapability
import com.friendorfoe.data.badge.expectedRuntimeMode
import com.friendorfoe.data.badge.firmwareHash
import com.friendorfoe.data.badge.payloadSizeOrNull
import com.friendorfoe.data.time.MonotonicClock
import dagger.hilt.android.lifecycle.HiltViewModel
import java.util.concurrent.atomic.AtomicLong
import javax.inject.Inject
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.mapNotNull
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import kotlinx.coroutines.withTimeoutOrNull

interface BadgeViewModelContract {
    fun refresh()
    fun reconnect()
    fun updateTheme(transform: (BadgeTheme) -> BadgeTheme)
    fun updatePolicy(transform: (BadgeDisplayPolicy) -> BadgeDisplayPolicy)
    fun updateNetworkMode(mode: BadgeNetworkMode)
    fun useFirmwareDefaultsInDraft()
    fun revertDraft()
    fun applyChanges()
    fun navigateDisplay(action: BadgeDisplayAction)
    fun requestRecovery(action: BadgeRecoveryAction)
    fun cancelRecovery()
    fun confirmRecovery()
}

@HiltViewModel
class BadgeViewModel @Inject constructor(
    private val port: BadgeControlPort,
    private val clock: MonotonicClock,
) : ViewModel(), BadgeViewModelContract {
    private val _uiState = MutableStateFlow(BadgeUiState())
    val uiState: StateFlow<BadgeUiState> = _uiState.asStateFlow()

    private var currentIdentity: BadgeConnectionIdentity? = null
    private var applyJob: Job? = null
    private val applyTransaction = AtomicLong(0)
    private var recoveryJob: Job? = null

    init {
        consumeRepositoryState(port.state.value)
        viewModelScope.launch {
            port.state.collect(::consumeRepositoryState)
        }
    }

    override fun refresh() {
        port.refreshStatus()
    }

    override fun reconnect() {
        port.requestConnection()
    }

    override fun updateTheme(transform: (BadgeTheme) -> BadgeTheme) {
        _uiState.update { state ->
            if (state.mutationLocked) return@update state
            val next = state.draftTheme?.let(transform) ?: return@update state
            state.copy(
                draftTheme = next,
                applyState = state.applyState.copy(
                    theme = BadgeSectionApplyResult(
                        section = BadgeConfigSection.THEME,
                        phase = if (next == state.appliedTheme) {
                            BadgeApplyPhase.CLEAN
                        } else {
                            BadgeApplyPhase.DIRTY
                        },
                    ),
                ),
            )
        }
    }

    override fun updatePolicy(transform: (BadgeDisplayPolicy) -> BadgeDisplayPolicy) {
        _uiState.update { state ->
            if (state.mutationLocked) return@update state
            val next = state.draftPolicy?.let(transform) ?: return@update state
            state.copy(
                draftPolicy = next,
                applyState = state.applyState.copy(
                    policy = BadgeSectionApplyResult(
                        section = BadgeConfigSection.DISPLAY_POLICY,
                        phase = if (next == state.appliedPolicy) {
                            BadgeApplyPhase.CLEAN
                        } else {
                            BadgeApplyPhase.DIRTY
                        },
                    ),
                ),
            )
        }
    }

    override fun updateNetworkMode(mode: BadgeNetworkMode) {
        _uiState.update { state ->
            if (state.mutationLocked || state.appliedNetworkMode == null) return@update state
            state.copy(
                draftNetworkMode = mode,
                applyState = state.applyState.copy(
                    network = BadgeSectionApplyResult(
                        section = BadgeConfigSection.NETWORK_MODE,
                        phase = if (mode == state.appliedNetworkMode) {
                            BadgeApplyPhase.CLEAN
                        } else {
                            BadgeApplyPhase.DIRTY
                        },
                    ),
                ),
            )
        }
    }

    override fun useFirmwareDefaultsInDraft() {
        _uiState.update { state ->
            if (state.mutationLocked) return@update state
            val nextTheme = state.firmwareDefaultThemeDraftOrNull()
                ?.takeIf { it != state.draftTheme }
            val nextPolicy = state.firmwareDefaultPolicyDraftOrNull()
                ?.takeIf { it != state.draftPolicy }
            if (nextTheme == null && nextPolicy == null) return@update state
            state.copy(
                draftTheme = nextTheme ?: state.draftTheme,
                draftPolicy = nextPolicy ?: state.draftPolicy,
                applyState = state.applyState.copy(
                    theme = nextTheme?.let {
                        BadgeSectionApplyResult(
                            section = BadgeConfigSection.THEME,
                            phase = if (it != state.appliedTheme) {
                                BadgeApplyPhase.DIRTY
                            } else {
                                BadgeApplyPhase.CLEAN
                            },
                        )
                    } ?: state.applyState.theme,
                    policy = nextPolicy?.let {
                        BadgeSectionApplyResult(
                            section = BadgeConfigSection.DISPLAY_POLICY,
                            phase = if (it != state.appliedPolicy) {
                                BadgeApplyPhase.DIRTY
                            } else {
                                BadgeApplyPhase.CLEAN
                            },
                        )
                    } ?: state.applyState.policy,
                ),
            )
        }
    }

    override fun revertDraft() {
        _uiState.update { state ->
            if (state.mutationLocked) return@update state
            val revertTheme = state.themeDirty &&
                state.appliedTheme != null && state.draftTheme != null
            val revertPolicy = state.policyDirty &&
                state.appliedPolicy != null && state.draftPolicy != null
            val revertNetwork = state.networkDirty &&
                state.appliedNetworkMode != null && state.draftNetworkMode != null
            if (!revertTheme && !revertPolicy && !revertNetwork) return@update state
            state.copy(
                draftTheme = if (revertTheme) state.appliedTheme else state.draftTheme,
                draftPolicy = if (revertPolicy) state.appliedPolicy else state.draftPolicy,
                draftNetworkMode = if (revertNetwork) {
                    state.appliedNetworkMode
                } else {
                    state.draftNetworkMode
                },
                applyState = state.applyState.copy(
                    theme = if (revertTheme) {
                        BadgeSectionApplyResult(BadgeConfigSection.THEME)
                    } else {
                        state.applyState.theme
                    },
                    policy = if (revertPolicy) {
                        BadgeSectionApplyResult(BadgeConfigSection.DISPLAY_POLICY)
                    } else {
                        state.applyState.policy
                    },
                    network = if (revertNetwork) {
                        BadgeSectionApplyResult(BadgeConfigSection.NETWORK_MODE)
                    } else {
                        state.applyState.network
                    },
                ),
            )
        }
    }

    override fun applyChanges() {
        if (applyJob?.isActive == true) return
        val snapshot = _uiState.value
        if (!snapshot.canApply) return
        val identity = snapshot.connection.identityOrNull() ?: return
        val submittedTheme = snapshot.draftTheme.takeIf { snapshot.themeDirty }
        val submittedPolicy = snapshot.draftPolicy.takeIf { snapshot.policyDirty }
        val submittedNetwork = snapshot.draftNetworkMode.takeIf { snapshot.networkDirty }

        val claimedState = snapshot.copy(
            applyInFlight = true,
            applyState = BadgeApplyState(
                theme = pendingResult(
                    BadgeConfigSection.THEME,
                    submittedTheme?.firmwareHash(),
                ).takeIf { submittedTheme != null }
                    ?: BadgeSectionApplyResult(BadgeConfigSection.THEME),
                policy = pendingResult(
                    BadgeConfigSection.DISPLAY_POLICY,
                    submittedPolicy?.firmwareHash(),
                ).takeIf { submittedPolicy != null }
                    ?: BadgeSectionApplyResult(BadgeConfigSection.DISPLAY_POLICY),
                network = pendingResult(BadgeConfigSection.NETWORK_MODE)
                    .takeIf { submittedNetwork != null }
                    ?: BadgeSectionApplyResult(BadgeConfigSection.NETWORK_MODE),
            ),
            recoveryAvailability = recoveryAvailability(
                connection = snapshot.connection,
                recovery = snapshot.recovery,
                applyInFlight = true,
            ),
        )
        if (!_uiState.compareAndSet(snapshot, claimedState)) return

        val transaction = applyTransaction.incrementAndGet()
        val job = viewModelScope.launch(start = CoroutineStart.LAZY) {
            try {
                if (submittedTheme != null && identityIsCurrent(identity)) {
                    applyTheme(identity, submittedTheme)
                }
                if (submittedPolicy != null && identityIsCurrent(identity)) {
                    applyPolicy(identity, submittedPolicy)
                }
                if (submittedNetwork != null && identityIsCurrent(identity)) {
                    applyNetwork(identity, submittedNetwork)
                }
            } finally {
                if (identityIsCurrent(identity)) {
                    _uiState.update { state ->
                        if (state.connection.identityOrNull() == identity) {
                            state.copy(
                                applyInFlight = false,
                                recoveryAvailability = recoveryAvailability(
                                    connection = state.connection,
                                    recovery = state.recovery,
                                    applyInFlight = false,
                                ),
                            )
                        } else {
                            state
                        }
                    }
                }
                if (applyTransaction.get() == transaction) {
                    applyJob = null
                }
            }
        }
        applyJob = job
        job.start()
    }

    override fun navigateDisplay(action: BadgeDisplayAction) {
        val snapshot = _uiState.value
        if (snapshot.mutationLocked ||
            snapshot.displayNavigationSupport[action] != BadgeCapabilitySupport.SUPPORTED
        ) {
            return
        }
        val identity = snapshot.connection.identityOrNull() ?: return
        viewModelScope.launch {
            val outcome = executeSafely(BadgeCommand.NavigateDisplay(action))
            if (!identityIsCurrent(identity)) return@launch
            _uiState.update { state ->
                if (state.connection.identityOrNull() == identity) {
                    state.copy(displayNavigationResult = outcome)
                } else {
                    state
                }
            }
        }
    }

    override fun requestRecovery(action: BadgeRecoveryAction) {
        if (recoveryJob?.isActive == true) return
        _uiState.update { state ->
            val availability = state.recoveryAvailability[action]
            val connection = state.connection
            if (state.mutationLocked || availability?.enabled != true ||
                connection.transport != BadgeTransport.USB_SERIAL ||
                connection.targetId.isNullOrBlank() ||
                connection.transportGeneration == null
            ) {
                return@update state
            }
            val recovery = BadgeRecoveryState(
                action = action,
                targetId = connection.targetId,
                targetTransportGeneration = connection.transportGeneration,
                phase = BadgeRecoveryPhase.CONFIRMING,
            )
            state.copy(
                recovery = recovery,
                recoveryAvailability = recoveryAvailability(
                    connection = connection,
                    recovery = recovery,
                    applyInFlight = state.applyInFlight,
                ),
            )
        }
    }

    override fun cancelRecovery() {
        _uiState.update { state ->
            if (state.recovery.phase != BadgeRecoveryPhase.CONFIRMING) return@update state
            val recovery = BadgeRecoveryState()
            state.copy(
                recovery = recovery,
                recoveryAvailability = recoveryAvailability(
                    connection = state.connection,
                    recovery = recovery,
                    applyInFlight = state.applyInFlight,
                ),
            )
        }
    }

    override fun confirmRecovery() {
        if (recoveryJob?.isActive == true) return
        val snapshot = _uiState.value
        val recovery = snapshot.recovery
        val action = recovery.action ?: return
        val identity = snapshot.connection.identityOrNull() ?: return
        if (recovery.phase != BadgeRecoveryPhase.CONFIRMING ||
            identity.transport != BadgeTransport.USB_SERIAL ||
            recovery.targetId != identity.targetId ||
            recovery.targetTransportGeneration != identity.transportGeneration ||
            badgeCapability(snapshot.connection, action.capability) !=
            BadgeCapabilitySupport.SUPPORTED
        ) {
            return
        }

        val pendingRecovery = snapshot.recovery.copy(phase = BadgeRecoveryPhase.PENDING)
        val claimedState = snapshot.copy(
            recovery = pendingRecovery,
            recoveryAvailability = recoveryAvailability(
                connection = snapshot.connection,
                recovery = pendingRecovery,
                applyInFlight = snapshot.applyInFlight,
            ),
        )
        if (!_uiState.compareAndSet(snapshot, claimedState)) return

        val job = viewModelScope.launch(start = CoroutineStart.LAZY) {
            val outcome = executeSafely(action.command)
            if (!identityIsCurrent(identity)) return@launch
            val guidance = "Reconnect and refresh badge status"
            val next = when (outcome) {
                is BadgeCommandOutcome.Acknowledged -> BadgeRecoveryState(
                    action = action,
                    targetId = identity.targetId,
                    targetTransportGeneration = identity.transportGeneration,
                    phase = BadgeRecoveryPhase.ACKNOWLEDGED,
                    message = outcome.acknowledgement.message,
                    reconnectGuidance = guidance,
                )
                is BadgeCommandOutcome.Failed -> BadgeRecoveryState(
                    action = action,
                    targetId = identity.targetId,
                    targetTransportGeneration = identity.transportGeneration,
                    phase = BadgeRecoveryPhase.FAILED,
                    message = outcome.message,
                    reconnectGuidance = guidance,
                )
                is BadgeCommandOutcome.Unsupported -> BadgeRecoveryState(
                    action = action,
                    targetId = identity.targetId,
                    targetTransportGeneration = identity.transportGeneration,
                    phase = BadgeRecoveryPhase.FAILED,
                    message = outcome.reason,
                    reconnectGuidance = guidance,
                )
                is BadgeCommandOutcome.Accepted -> BadgeRecoveryState(
                    action = action,
                    targetId = identity.targetId,
                    targetTransportGeneration = identity.transportGeneration,
                    phase = BadgeRecoveryPhase.NOT_VERIFIED,
                    message = outcome.message,
                    reconnectGuidance = guidance,
                )
                BadgeCommandOutcome.TimedOut -> BadgeRecoveryState(
                    action = action,
                    targetId = identity.targetId,
                    targetTransportGeneration = identity.transportGeneration,
                    phase = BadgeRecoveryPhase.NOT_VERIFIED,
                    message = "Badge recovery acknowledgement timed out",
                    reconnectGuidance = guidance,
                )
            }
            _uiState.update { state ->
                if (state.connection.identityOrNull() == identity &&
                    state.recovery.phase == BadgeRecoveryPhase.PENDING
                ) {
                    state.copy(
                        recovery = next,
                        recoveryAvailability = recoveryAvailability(
                            connection = state.connection,
                            recovery = next,
                            applyInFlight = state.applyInFlight,
                        ),
                    )
                } else {
                    state
                }
            }
            recoveryJob = null
        }
        recoveryJob = job
        job.start()
    }

    private fun consumeRepositoryState(repository: BadgeRepositoryState) {
        val connection = effectiveConnection(repository.connection)
        val nextIdentity = connection.identityOrNull()
        val identityChanged = currentIdentity != null && currentIdentity != nextIdentity
        if (identityChanged) {
            applyTransaction.incrementAndGet()
            applyJob?.cancel()
            recoveryJob?.cancel()
            applyJob = null
            recoveryJob = null
        }
        currentIdentity = nextIdentity

        val capabilities = BadgeCapability.entries.associateWith { capability ->
            badgeCapability(connection, capability)
        }
        val navigation = BadgeDisplayAction.entries.associateWith { action ->
            val command = BadgeCommand.NavigateDisplay(action)
            badgeCapability(
                connection,
                BadgeCapability.DISPLAY_NAV,
                command.payloadSizeOrNull(),
            )
        }
        val status = repository.controlStatus.takeIf {
            isValidLiveStatus(connection, it, nextIdentity)
        }

        _uiState.update { previous ->
            val reset = if (identityChanged) BadgeUiState() else previous
            var next = reset.copy(
                connection = connection,
                capabilities = capabilities,
                displayNavigationSupport = navigation,
                controlStatus = status,
                displayNavigationResult = reset.displayNavigationResult.takeUnless {
                    identityChanged
                },
                recovery = reset.recovery.takeUnless { identityChanged }
                    ?: BadgeRecoveryState(),
            )
            if (status != null) {
                next = reconcileReadbacks(next, status)
            } else {
                next = clearUnsafeReadbacks(next)
            }
            val recovery = next.recovery.takeUnless {
                it.phase == BadgeRecoveryPhase.CONFIRMING &&
                    !recoveryConfirmationIsValid(connection, it)
            } ?: BadgeRecoveryState()
            next.copy(
                recovery = recovery,
                recoveryAvailability = recoveryAvailability(
                    connection = connection,
                    recovery = recovery,
                    applyInFlight = next.applyInFlight,
                ),
            )
        }
    }

    private fun recoveryConfirmationIsValid(
        connection: BadgeConnectionEvidence,
        recovery: BadgeRecoveryState,
    ): Boolean {
        val action = recovery.action ?: return false
        return connection.transport == BadgeTransport.USB_SERIAL &&
            recovery.targetId == connection.targetId &&
            recovery.targetTransportGeneration == connection.transportGeneration &&
            badgeCapability(connection, action.capability) == BadgeCapabilitySupport.SUPPORTED
    }

    private fun reconcileReadbacks(
        state: BadgeUiState,
        status: BadgeControlStatus,
    ): BadgeUiState {
        var next = state
        next = when {
            !status.themeReadback.isEditable -> invalidateThemeReadback(
                next,
                status.themeReadback.issue ?: "Theme readback is unavailable",
            )
            else -> {
                val hadNoAppliedBaseline = next.appliedTheme == null
                val retainedDraft = next.draftTheme.takeIf { next.themeDirty }
                val applied = status.themeReadback.value
                val draft = retainedDraft ?: applied
                next.copy(
                    appliedTheme = applied,
                    draftTheme = draft,
                    applyState = next.applyState.copy(
                        theme = if (shouldNormalizeMatchingReadback(
                                hadNoAppliedBaseline = hadNoAppliedBaseline,
                                retainedDraftPresent = retainedDraft != null,
                                draftMatchesApplied = draft == applied,
                                phase = next.applyState.theme.phase,
                            )
                        ) {
                            BadgeSectionApplyResult(BadgeConfigSection.THEME)
                        } else {
                            next.applyState.theme
                        },
                    ),
                )
            }
        }
        next = when {
            !status.policyReadback.isEditable -> invalidatePolicyReadback(
                next,
                status.policyReadback.issue ?: "Display policy readback is unavailable",
            )
            else -> {
                val hadNoAppliedBaseline = next.appliedPolicy == null
                val retainedDraft = next.draftPolicy.takeIf { next.policyDirty }
                val applied = status.policyReadback.value
                val draft = retainedDraft ?: applied
                next.copy(
                    appliedPolicy = applied,
                    draftPolicy = draft,
                    applyState = next.applyState.copy(
                        policy = if (shouldNormalizeMatchingReadback(
                                hadNoAppliedBaseline = hadNoAppliedBaseline,
                                retainedDraftPresent = retainedDraft != null,
                                draftMatchesApplied = draft == applied,
                                phase = next.applyState.policy.phase,
                            )
                        ) {
                            BadgeSectionApplyResult(BadgeConfigSection.DISPLAY_POLICY)
                        } else {
                            next.applyState.policy
                        },
                    ),
                )
            }
        }
        next = when {
            !status.networkModeReadback.isEditable -> invalidateNetworkReadback(
                next,
                status.networkModeReadback.issue ?: "Network mode readback is unavailable",
            )
            else -> {
                val hadNoAppliedBaseline = next.appliedNetworkMode == null
                val retainedDraft = next.draftNetworkMode.takeIf { next.networkDirty }
                val applied = status.networkModeReadback.value
                val draft = retainedDraft ?: applied
                next.copy(
                    appliedNetworkMode = applied,
                    draftNetworkMode = draft,
                    applyState = next.applyState.copy(
                        network = if (shouldNormalizeMatchingReadback(
                                hadNoAppliedBaseline = hadNoAppliedBaseline,
                                retainedDraftPresent = retainedDraft != null,
                                draftMatchesApplied = draft == applied,
                                phase = next.applyState.network.phase,
                            )
                        ) {
                            BadgeSectionApplyResult(BadgeConfigSection.NETWORK_MODE)
                        } else {
                            next.applyState.network
                        },
                    ),
                )
            }
        }
        return next
    }

    private fun shouldNormalizeMatchingReadback(
        hadNoAppliedBaseline: Boolean,
        retainedDraftPresent: Boolean,
        draftMatchesApplied: Boolean,
        phase: BadgeApplyPhase,
    ): Boolean = retainedDraftPresent && draftMatchesApplied &&
        (phase == BadgeApplyPhase.DIRTY ||
            (hadNoAppliedBaseline && phase == BadgeApplyPhase.FAILED))

    private fun clearUnsafeReadbacks(state: BadgeUiState): BadgeUiState =
        invalidateNetworkReadback(
            invalidatePolicyReadback(
                invalidateThemeReadback(state, "Theme proof requires fresh badge status"),
                "Display policy proof requires fresh badge status",
            ),
            "Network mode proof requires fresh badge status",
        )

    private fun invalidateThemeReadback(
        state: BadgeUiState,
        message: String,
    ): BadgeUiState {
        val retainedDraft = state.draftTheme.takeIf { state.themeDirty }
        return state.copy(
            appliedTheme = null,
            draftTheme = retainedDraft,
            applyState = state.applyState.copy(
                theme = state.applyState.theme.invalidateSuccessfulProof(message),
            ),
        )
    }

    private fun invalidatePolicyReadback(
        state: BadgeUiState,
        message: String,
    ): BadgeUiState {
        val retainedDraft = state.draftPolicy.takeIf { state.policyDirty }
        return state.copy(
            appliedPolicy = null,
            draftPolicy = retainedDraft,
            applyState = state.applyState.copy(
                policy = state.applyState.policy.invalidateSuccessfulProof(message),
            ),
        )
    }

    private fun invalidateNetworkReadback(
        state: BadgeUiState,
        message: String,
    ): BadgeUiState {
        val retainedDraft = state.draftNetworkMode.takeIf { state.networkDirty }
        return state.copy(
            appliedNetworkMode = null,
            draftNetworkMode = retainedDraft,
            applyState = state.applyState.copy(
                network = state.applyState.network.invalidateSuccessfulProof(message),
            ),
        )
    }

    private suspend fun applyTheme(identity: BadgeConnectionIdentity, submitted: BadgeTheme) {
        val baseline = baselineReceipt(identity) ?: run {
            updateThemeResult(identity, notVerified(BadgeConfigSection.THEME, "Fresh badge status is required"))
            return
        }
        val expectedHash = submitted.firmwareHash()
        val outcome = executeSafely(BadgeCommand.ApplyTheme(submitted))
        if (!identityIsCurrent(identity)) return
        when (outcome) {
            is BadgeCommandOutcome.Failed -> {
                updateThemeResult(identity, failed(BadgeConfigSection.THEME, outcome.message, expectedHash))
                return
            }
            is BadgeCommandOutcome.Unsupported -> {
                updateThemeResult(identity, unsupported(BadgeConfigSection.THEME, outcome.reason, expectedHash))
                return
            }
            BadgeCommandOutcome.TimedOut -> {
                updateThemeResult(identity, notVerified(BadgeConfigSection.THEME, "Badge command timed out", expectedHash))
                return
            }
            is BadgeCommandOutcome.Acknowledged -> {
                val acknowledgementHash = outcome.acknowledgement.themeHash
                if (acknowledgementHash != null && acknowledgementHash != expectedHash) {
                    updateThemeResult(
                        identity,
                        notVerified(
                            BadgeConfigSection.THEME,
                            "Theme acknowledgement hash did not match",
                            expectedHash,
                            acknowledgementHash,
                        ),
                    )
                    port.refreshStatus()
                    return
                }
                updateThemeResult(identity, BadgeSectionApplyResult(
                    section = BadgeConfigSection.THEME,
                    phase = if (acknowledgementHash == null) {
                        BadgeApplyPhase.ACCEPTED
                    } else {
                        BadgeApplyPhase.ACKNOWLEDGED
                    },
                    message = outcome.acknowledgement.message,
                    expectedHash = expectedHash,
                    acknowledgementHash = acknowledgementHash,
                ))
            }
            is BadgeCommandOutcome.Accepted -> updateThemeResult(
                identity,
                BadgeSectionApplyResult(
                    section = BadgeConfigSection.THEME,
                    phase = BadgeApplyPhase.ACCEPTED,
                    message = outcome.message,
                    expectedHash = expectedHash,
                ),
            )
        }
        port.refreshStatus()
        val status = awaitNewStatus(identity, baseline) { candidate ->
            candidate.themeReadback.isEditable &&
                candidate.themeReadback.value == submitted &&
                candidate.themeReadback.hash == expectedHash
        }
        if (!identityIsCurrent(identity)) return
        val explicitHash = (outcome as? BadgeCommandOutcome.Acknowledged)
            ?.acknowledgement?.themeHash
        val acknowledgementHash = explicitHash ?: status?.themeReadback?.hash
        val readbackHash = status?.themeReadback?.hash
        val verified = acknowledgementHash == expectedHash &&
            status?.themeReadback?.isEditable == true &&
            status.themeReadback.value == submitted &&
            readbackHash == expectedHash
        val result = if (verified) {
            BadgeSectionApplyResult(
                section = BadgeConfigSection.THEME,
                phase = BadgeApplyPhase.VERIFIED,
                message = "Theme verified on badge",
                expectedHash = expectedHash,
                acknowledgementHash = acknowledgementHash,
                readbackHash = readbackHash,
            )
        } else {
            notVerified(
                BadgeConfigSection.THEME,
                "Theme was not verified by a fresh matching readback",
                expectedHash,
                acknowledgementHash,
                readbackHash,
            )
        }
        updateThemeResult(
            identity,
            result,
            submitted.takeIf { status != null },
            status?.receivedAtElapsedMs,
        )
    }

    private suspend fun applyPolicy(
        identity: BadgeConnectionIdentity,
        submitted: BadgeDisplayPolicy,
    ) {
        val baseline = baselineReceipt(identity) ?: run {
            updatePolicyResult(identity, notVerified(BadgeConfigSection.DISPLAY_POLICY, "Fresh badge status is required"))
            return
        }
        val expectedHash = submitted.firmwareHash()
        val outcome = executeSafely(BadgeCommand.ApplyPolicy(submitted))
        if (!identityIsCurrent(identity)) return
        when (outcome) {
            is BadgeCommandOutcome.Failed -> {
                updatePolicyResult(identity, failed(BadgeConfigSection.DISPLAY_POLICY, outcome.message, expectedHash))
                return
            }
            is BadgeCommandOutcome.Unsupported -> {
                updatePolicyResult(identity, unsupported(BadgeConfigSection.DISPLAY_POLICY, outcome.reason, expectedHash))
                return
            }
            BadgeCommandOutcome.TimedOut -> {
                updatePolicyResult(identity, notVerified(BadgeConfigSection.DISPLAY_POLICY, "Badge command timed out", expectedHash))
                return
            }
            is BadgeCommandOutcome.Acknowledged -> {
                val acknowledgementHash = outcome.acknowledgement.policyHash
                if (acknowledgementHash != null && acknowledgementHash != expectedHash) {
                    updatePolicyResult(
                        identity,
                        notVerified(
                            BadgeConfigSection.DISPLAY_POLICY,
                            "Policy acknowledgement hash did not match",
                            expectedHash,
                            acknowledgementHash,
                        ),
                    )
                    port.refreshStatus()
                    return
                }
                updatePolicyResult(identity, BadgeSectionApplyResult(
                    section = BadgeConfigSection.DISPLAY_POLICY,
                    phase = if (acknowledgementHash == null) {
                        BadgeApplyPhase.ACCEPTED
                    } else {
                        BadgeApplyPhase.ACKNOWLEDGED
                    },
                    message = outcome.acknowledgement.message,
                    expectedHash = expectedHash,
                    acknowledgementHash = acknowledgementHash,
                ))
            }
            is BadgeCommandOutcome.Accepted -> updatePolicyResult(
                identity,
                BadgeSectionApplyResult(
                    section = BadgeConfigSection.DISPLAY_POLICY,
                    phase = BadgeApplyPhase.ACCEPTED,
                    message = outcome.message,
                    expectedHash = expectedHash,
                ),
            )
        }
        port.refreshStatus()
        val explicitHash = (outcome as? BadgeCommandOutcome.Acknowledged)
            ?.acknowledgement?.policyHash
        val status = awaitPolicyProof(
            identity = identity,
            baselineReceipt = baseline,
            submitted = submitted,
            expectedHash = expectedHash,
            explicitAcknowledgementHash = explicitHash,
        )
        if (!identityIsCurrent(identity)) return
        val acknowledgementHash = explicitHash ?: status?.policyReadback?.hash
        val readbackHash = status?.policyReadback?.hash
        val appliedOnBadge = acknowledgementHash == expectedHash &&
            status?.policyReadback?.isEditable == true &&
            status.policyReadback.value == submitted &&
            readbackHash == expectedHash
        val phase = when {
            !appliedOnBadge -> BadgeApplyPhase.NOT_VERIFIED
            scannersVerified(status, expectedHash) -> BadgeApplyPhase.VERIFIED_ON_SCANNERS
            else -> BadgeApplyPhase.APPLIED_ON_BADGE
        }
        val result = BadgeSectionApplyResult(
            section = BadgeConfigSection.DISPLAY_POLICY,
            phase = phase,
            message = when (phase) {
                BadgeApplyPhase.VERIFIED_ON_SCANNERS -> "Policy verified on badge and scanners"
                BadgeApplyPhase.APPLIED_ON_BADGE -> "Policy applied on badge; scanner proof unavailable"
                else -> "Policy was not verified by a fresh matching readback"
            },
            expectedHash = expectedHash,
            acknowledgementHash = acknowledgementHash,
            readbackHash = readbackHash,
        )
        updatePolicyResult(
            identity,
            result,
            submitted.takeIf { status != null },
            status?.receivedAtElapsedMs,
        )
    }

    private suspend fun awaitPolicyProof(
        identity: BadgeConnectionIdentity,
        baselineReceipt: Long,
        submitted: BadgeDisplayPolicy,
        expectedHash: Long,
        explicitAcknowledgementHash: Long?,
    ): BadgeControlStatus? {
        var lastFreshStatus: BadgeControlStatus? = null
        var appliedOnBadgeStatus: BadgeControlStatus? = null
        val completedStatus = withTimeoutOrNull(COMMAND_PROOF_TIMEOUT_MS) {
            port.state.mapNotNull { repository ->
                validStatusFor(repository, identity)?.takeIf {
                    it.receivedAtElapsedMs > baselineReceipt
                }
            }.first { status ->
                lastFreshStatus = status
                val acknowledgementHash = explicitAcknowledgementHash
                    ?: status.policyReadback.hash
                val appliedOnBadge = acknowledgementHash == expectedHash &&
                    status.policyReadback.isEditable &&
                    status.policyReadback.value == submitted &&
                    status.policyReadback.hash == expectedHash
                if (!appliedOnBadge) return@first false

                appliedOnBadgeStatus = status
                val scannerProof = scannersVerified(status, expectedHash)
                updatePolicyResult(
                    identity,
                    BadgeSectionApplyResult(
                        section = BadgeConfigSection.DISPLAY_POLICY,
                        phase = if (scannerProof) {
                            BadgeApplyPhase.VERIFIED_ON_SCANNERS
                        } else {
                            BadgeApplyPhase.APPLIED_ON_BADGE
                        },
                        message = if (scannerProof) {
                            "Policy verified on badge and scanners"
                        } else {
                            "Policy applied on badge; waiting for scanner proof"
                        },
                        expectedHash = expectedHash,
                        acknowledgementHash = acknowledgementHash,
                        readbackHash = status.policyReadback.hash,
                    ),
                    submitted,
                    status.receivedAtElapsedMs,
                )
                val connectedScanners = status.scanners.filter { it.connected }
                connectedScanners.isEmpty() || scannerProof
            }
        }
        return completedStatus ?: appliedOnBadgeStatus ?: lastFreshStatus
    }

    private suspend fun applyNetwork(
        identity: BadgeConnectionIdentity,
        submitted: BadgeNetworkMode,
    ) {
        val baseline = baselineReceipt(identity) ?: run {
            updateNetworkResult(identity, notVerified(BadgeConfigSection.NETWORK_MODE, "Fresh badge status is required"))
            return
        }
        val outcome = executeSafely(BadgeCommand.SetNetworkMode(submitted))
        if (!identityIsCurrent(identity)) return
        when (outcome) {
            is BadgeCommandOutcome.Failed -> {
                updateNetworkResult(identity, failed(BadgeConfigSection.NETWORK_MODE, outcome.message))
                return
            }
            is BadgeCommandOutcome.Unsupported -> {
                updateNetworkResult(identity, unsupported(BadgeConfigSection.NETWORK_MODE, outcome.reason))
                return
            }
            BadgeCommandOutcome.TimedOut -> {
                updateNetworkResult(identity, notVerified(BadgeConfigSection.NETWORK_MODE, "Badge command timed out"))
                return
            }
            is BadgeCommandOutcome.Accepted -> {
                updateNetworkResult(
                    identity,
                    BadgeSectionApplyResult(
                        section = BadgeConfigSection.NETWORK_MODE,
                        phase = BadgeApplyPhase.NOT_VERIFIED,
                        message = "Network mode transport acceptance is not proof",
                    ),
                )
                port.refreshStatus()
                return
            }
            is BadgeCommandOutcome.Acknowledged -> {
                val acknowledgement = outcome.acknowledgement
                if (acknowledgement.networkApplied != true ||
                    acknowledgement.runtimeNetworkMode != submitted.expectedRuntimeMode()
                ) {
                    updateNetworkResult(
                        identity,
                        notVerified(
                            BadgeConfigSection.NETWORK_MODE,
                            "Network acknowledgement did not match the requested runtime mode",
                        ),
                    )
                    port.refreshStatus()
                    return
                }
                updateNetworkResult(
                    identity,
                    BadgeSectionApplyResult(
                        section = BadgeConfigSection.NETWORK_MODE,
                        phase = BadgeApplyPhase.ACKNOWLEDGED,
                        message = acknowledgement.message,
                    ),
                )
            }
        }
        port.refreshStatus()
        val status = awaitNewStatus(identity, baseline) { candidate ->
            candidate.networkModeReadback.isEditable &&
                candidate.networkModeReadback.value == submitted
        }
        if (!identityIsCurrent(identity)) return
        val verified = status?.networkModeReadback?.isEditable == true &&
            status.networkModeReadback.value == submitted
        val result = if (verified) {
            BadgeSectionApplyResult(
                section = BadgeConfigSection.NETWORK_MODE,
                phase = BadgeApplyPhase.VERIFIED,
                message = "Network mode verified on badge",
            )
        } else {
            notVerified(
                BadgeConfigSection.NETWORK_MODE,
                "Network mode was not verified by a fresh matching readback",
            )
        }
        updateNetworkResult(
            identity,
            result,
            submitted.takeIf { status != null },
            status?.receivedAtElapsedMs,
        )
    }

    private fun baselineReceipt(identity: BadgeConnectionIdentity): Long? =
        validStatusFor(port.state.value, identity)?.receivedAtElapsedMs

    private suspend fun awaitNewStatus(
        identity: BadgeConnectionIdentity,
        baselineReceipt: Long,
        matchesSubmitted: (BadgeControlStatus) -> Boolean,
    ): BadgeControlStatus? {
        var latestFreshStatus: BadgeControlStatus? = null
        val matchingStatus = withTimeoutOrNull(COMMAND_PROOF_TIMEOUT_MS) {
            port.state.mapNotNull { repository ->
                validStatusFor(repository, identity)?.takeIf {
                    it.receivedAtElapsedMs > baselineReceipt
                }
            }.first { status ->
                latestFreshStatus = status
                matchesSubmitted(status)
            }
        }
        return matchingStatus ?: latestFreshStatus
    }

    private fun validStatusFor(
        repository: BadgeRepositoryState,
        identity: BadgeConnectionIdentity,
    ): BadgeControlStatus? {
        val connection = effectiveConnection(repository.connection)
        if (connection.identityOrNull() != identity) return null
        return repository.controlStatus.takeIf {
            isValidLiveStatus(connection, it, identity)
        }
    }

    private fun isValidLiveStatus(
        connection: BadgeConnectionEvidence,
        status: BadgeControlStatus?,
        identity: BadgeConnectionIdentity?,
    ): Boolean = status != null &&
        identity != null &&
        connection.phase == BadgeConnectionPhase.LIVE &&
        connection.lastValidStatusAtElapsedMs == status.receivedAtElapsedMs

    private fun identityIsCurrent(identity: BadgeConnectionIdentity): Boolean =
        port.state.value.connection.identityOrNull() == identity

    private fun effectiveConnection(connection: BadgeConnectionEvidence): BadgeConnectionEvidence =
        if (connection.phase == BadgeConnectionPhase.LIVE) {
            connection.aged(clock.nowElapsedMs())
        } else {
            connection
        }

    private suspend fun executeSafely(command: BadgeCommand): BadgeCommandOutcome =
        try {
            port.execute(command)
        } catch (cancelled: CancellationException) {
            throw cancelled
        } catch (error: Exception) {
            BadgeCommandOutcome.Failed(error.message ?: "Badge command failed")
        }

    private fun updateThemeResult(
        identity: BadgeConnectionIdentity,
        result: BadgeSectionApplyResult,
        applied: BadgeTheme? = null,
        proofReceipt: Long? = null,
    ) {
        _uiState.update { state ->
            if (state.connection.identityOrNull() != identity) return@update state
            val liveProof = if (applied != null && proofReceipt != null) {
                currentLiveProofStatus(identity, proofReceipt)
            } else {
                null
            }
            val canRestore = applied == null || (
                liveProof?.themeReadback?.isEditable == true &&
                    liveProof.themeReadback.value == applied &&
                    liveProof.themeReadback.hash == (result.expectedHash ?: applied.firmwareHash())
                )
            val safeResult = if (canRestore) {
                result
            } else {
                result.invalidatedProof(
                    message = "Theme proof is no longer current",
                    currentReadbackHash = liveProof?.themeReadback?.hash,
                )
            }
            val currentApplied = liveProof?.themeReadback
                ?.takeIf { it.isEditable }
                ?.value
            val nextApplied = when {
                applied == null -> state.appliedTheme
                canRestore -> applied
                else -> currentApplied
            }
            val nextDraft = when {
                applied == null -> state.draftTheme
                canRestore -> applied
                currentApplied != null -> applied
                else -> null
            }
            state.copy(
                appliedTheme = nextApplied,
                draftTheme = nextDraft,
                applyState = state.applyState.copy(theme = safeResult),
            )
        }
    }

    private fun updatePolicyResult(
        identity: BadgeConnectionIdentity,
        result: BadgeSectionApplyResult,
        applied: BadgeDisplayPolicy? = null,
        proofReceipt: Long? = null,
    ) {
        _uiState.update { state ->
            if (state.connection.identityOrNull() != identity) return@update state
            val liveProof = if (applied != null && proofReceipt != null) {
                currentLiveProofStatus(identity, proofReceipt)
            } else {
                null
            }
            val expectedHash = result.expectedHash ?: applied?.firmwareHash()
            val canRestore = applied == null || (
                liveProof?.policyReadback?.isEditable == true &&
                    liveProof.policyReadback.value == applied &&
                    liveProof.policyReadback.hash == expectedHash
                )
            val safeResult = when {
                !canRestore -> result.invalidatedProof(
                    message = "Policy proof is no longer current",
                    currentReadbackHash = liveProof?.policyReadback?.hash,
                )
                applied == null || liveProof == null || expectedHash == null -> result
                scannersVerified(liveProof, expectedHash) -> result.copy(
                    phase = BadgeApplyPhase.VERIFIED_ON_SCANNERS,
                    message = "Policy verified on badge and scanners",
                    readbackHash = liveProof.policyReadback.hash,
                )
                else -> result.copy(
                    phase = BadgeApplyPhase.APPLIED_ON_BADGE,
                    message = "Policy applied on badge; scanner proof unavailable",
                    readbackHash = liveProof.policyReadback.hash,
                )
            }
            val currentApplied = liveProof?.policyReadback
                ?.takeIf { it.isEditable }
                ?.value
            val nextApplied = when {
                applied == null -> state.appliedPolicy
                canRestore -> applied
                else -> currentApplied
            }
            val nextDraft = when {
                applied == null -> state.draftPolicy
                canRestore -> applied
                currentApplied != null -> applied
                else -> null
            }
            state.copy(
                appliedPolicy = nextApplied,
                draftPolicy = nextDraft,
                applyState = state.applyState.copy(policy = safeResult),
            )
        }
    }

    private fun updateNetworkResult(
        identity: BadgeConnectionIdentity,
        result: BadgeSectionApplyResult,
        applied: BadgeNetworkMode? = null,
        proofReceipt: Long? = null,
    ) {
        _uiState.update { state ->
            if (state.connection.identityOrNull() != identity) return@update state
            val liveProof = if (applied != null && proofReceipt != null) {
                currentLiveProofStatus(identity, proofReceipt)
            } else {
                null
            }
            val canRestore = applied == null || (
                liveProof?.networkModeReadback?.isEditable == true &&
                    liveProof.networkModeReadback.value == applied
                )
            val safeResult = if (canRestore) {
                result
            } else {
                result.invalidatedProof("Network mode proof is no longer current")
            }
            val currentApplied = liveProof?.networkModeReadback
                ?.takeIf { it.isEditable }
                ?.value
            val nextApplied = when {
                applied == null -> state.appliedNetworkMode
                canRestore -> applied
                else -> currentApplied
            }
            val nextDraft = when {
                applied == null -> state.draftNetworkMode
                canRestore -> applied
                currentApplied != null -> applied
                else -> null
            }
            state.copy(
                appliedNetworkMode = nextApplied,
                draftNetworkMode = nextDraft,
                applyState = state.applyState.copy(network = safeResult),
            )
        }
    }

    private fun currentLiveProofStatus(
        identity: BadgeConnectionIdentity,
        minimumReceipt: Long,
    ): BadgeControlStatus? = validStatusFor(port.state.value, identity)?.takeIf {
        it.receivedAtElapsedMs >= minimumReceipt
    }

    private fun recoveryAvailability(
        connection: BadgeConnectionEvidence,
        recovery: BadgeRecoveryState,
        applyInFlight: Boolean,
    ): Map<BadgeRecoveryAction, BadgeRecoveryAvailability> =
        BadgeRecoveryAction.entries.associateWith { action ->
            when {
                applyInFlight -> BadgeRecoveryAvailability(
                    false,
                    "Badge changes are being applied",
                )
                recovery.phase == BadgeRecoveryPhase.CONFIRMING -> BadgeRecoveryAvailability(
                    false,
                    "Recovery confirmation pending",
                )
                recovery.recoveryRequiresReconnectFor(connection) -> BadgeRecoveryAvailability(
                    false,
                    recovery.reconnectGuidance ?: "Reconnect and refresh badge status",
                )
                connection.transport == BadgeTransport.USB_SERIAL &&
                    (connection.usbCandidateCount != 1 ||
                        connection.targetId.isNullOrBlank() ||
                        connection.transportGeneration == null) -> BadgeRecoveryAvailability(
                    false,
                    "Connect exactly one badge over USB before recovery",
                )
                connection.transport != BadgeTransport.USB_SERIAL ||
                    badgeCapability(connection, action.capability) !=
                    BadgeCapabilitySupport.SUPPORTED -> BadgeRecoveryAvailability(
                    false,
                    "Verified direct USB is required",
                )
                else -> BadgeRecoveryAvailability(true, "")
            }
        }

    private fun scannersVerified(status: BadgeControlStatus?, expectedHash: Long): Boolean {
        val connected = status?.scanners?.filter { it.connected }.orEmpty()
        return connected.isNotEmpty() && connected.all {
            it.displayPolicyAckHash == expectedHash
        }
    }

    private companion object {
        const val COMMAND_PROOF_TIMEOUT_MS = 5_000L
    }
}

private data class BadgeConnectionIdentity(
    val transport: BadgeTransport,
    val transportGeneration: Long,
    val targetId: String,
)

private fun BadgeRecoveryState.recoveryRequiresReconnectFor(
    connection: BadgeConnectionEvidence,
): Boolean = phase in setOf(
    BadgeRecoveryPhase.PENDING,
    BadgeRecoveryPhase.ACKNOWLEDGED,
    BadgeRecoveryPhase.NOT_VERIFIED,
    BadgeRecoveryPhase.FAILED,
) && targetId == connection.targetId &&
    targetTransportGeneration == connection.transportGeneration

private fun BadgeConnectionEvidence.identityOrNull(): BadgeConnectionIdentity? {
    val concreteTransport = transport ?: return null
    val generation = transportGeneration ?: return null
    val target = targetId?.takeIf { it.isNotBlank() } ?: return null
    return BadgeConnectionIdentity(concreteTransport, generation, target)
}

private fun pendingResult(
    section: BadgeConfigSection,
    expectedHash: Long? = null,
) = BadgeSectionApplyResult(
    section = section,
    phase = BadgeApplyPhase.PENDING,
    expectedHash = expectedHash,
)

private fun notVerified(
    section: BadgeConfigSection,
    message: String,
    expectedHash: Long? = null,
    acknowledgementHash: Long? = null,
    readbackHash: Long? = null,
) = BadgeSectionApplyResult(
    section = section,
    phase = BadgeApplyPhase.NOT_VERIFIED,
    message = message,
    expectedHash = expectedHash,
    acknowledgementHash = acknowledgementHash,
    readbackHash = readbackHash,
)

private fun failed(
    section: BadgeConfigSection,
    message: String,
    expectedHash: Long? = null,
) = BadgeSectionApplyResult(
    section = section,
    phase = BadgeApplyPhase.FAILED,
    message = message,
    expectedHash = expectedHash,
)

private fun unsupported(
    section: BadgeConfigSection,
    message: String,
    expectedHash: Long? = null,
) = BadgeSectionApplyResult(
    section = section,
    phase = BadgeApplyPhase.UNSUPPORTED,
    message = message,
    expectedHash = expectedHash,
)

private fun BadgeSectionApplyResult.invalidatedProof(
    message: String,
    currentReadbackHash: Long? = null,
) = copy(
    phase = BadgeApplyPhase.NOT_VERIFIED,
    message = message,
    readbackHash = currentReadbackHash,
)

private fun BadgeSectionApplyResult.invalidateSuccessfulProof(
    message: String,
): BadgeSectionApplyResult = when (phase) {
    BadgeApplyPhase.APPLIED_ON_BADGE,
    BadgeApplyPhase.VERIFIED,
    BadgeApplyPhase.VERIFIED_ON_SCANNERS -> invalidatedProof(message)
    else -> this
}
