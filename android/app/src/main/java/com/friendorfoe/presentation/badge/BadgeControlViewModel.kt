package com.friendorfoe.presentation.badge

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.friendorfoe.data.badge.BadgeDisplayPolicy
import com.friendorfoe.data.badge.BadgeDisplayNavAction
import com.friendorfoe.data.badge.BadgeTheme
import com.friendorfoe.data.badge.BadgeThemeProfileStore
import com.friendorfoe.data.badge.BadgeThreatEntity
import com.friendorfoe.data.badge.BadgeUsbRepository
import com.friendorfoe.detection.BleInvestigationRoute
import com.friendorfoe.detection.elapsedRealtimeMs
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.stateIn
import javax.inject.Inject

@HiltViewModel
class BadgeControlViewModel @Inject constructor(
    private val repository: BadgeUsbRepository,
    private val profiles: BadgeThemeProfileStore,
) : ViewModel() {
    val badgeState = repository.state
    val themeProfiles = profiles.profiles
    val investigation = combine(
        repository.investigation,
        sharedBadgeInvestigationSession.currentRequestId,
    ) { repositoryResult, requestId ->
        repositoryResult?.takeIf { it.requestId == requestId }
    }.stateIn(
        scope = viewModelScope,
        started = SharingStarted.WhileSubscribed(5_000),
        initialValue = sharedBadgeInvestigationSession.visibleResult(repository.investigation.value),
    )

    fun grantUsbAccess() = repository.requestConnection()
    fun refresh() = repository.requestStatus()
    fun displayNav(action: BadgeDisplayNavAction) = repository.displayNav(action)
    fun setMode(mode: String) = repository.setMode(mode)
    fun applyTheme(theme: BadgeTheme) = repository.applyBadgeTheme(theme)
    fun resetTheme() = repository.resetBadgeTheme()
    fun applyDisplayPolicy(policy: BadgeDisplayPolicy) = repository.applyDisplayPolicy(policy)
    fun resetDisplayPolicy() = repository.resetDisplayPolicy()
    fun createProfile(name: String, theme: BadgeTheme) = profiles.create(name, theme)
    fun renameProfile(id: String, name: String) = profiles.rename(id, name)
    fun replaceProfile(id: String, theme: BadgeTheme) = profiles.replace(id, theme)
    fun deleteProfile(id: String) = profiles.delete(id)

    fun canInvestigate(entity: BadgeThreatEntity): Boolean {
        val nowElapsedMs = elapsedRealtimeMs()
        return entity.badgeInvestigationTarget(nowElapsedMs) != null &&
            badgeInvestigationStartAllowed(
                state = repository.state.value,
                repositoryResult = repository.investigation.value,
            )
    }

    fun investigate(entity: BadgeThreatEntity): Boolean {
        val nowElapsedMs = elapsedRealtimeMs()
        if (!badgeInvestigationStartAllowed(
                state = repository.state.value,
                repositoryResult = repository.investigation.value,
            )
        ) return false

        val requestId = sharedBadgeInvestigationSession.nextRequestId(nowElapsedMs)
        val request = entity.badgeInvestigationRequest(nowElapsedMs, requestId) ?: return false
        check(request.route == BleInvestigationRoute.BADGE)

        val accepted = repository.investigateBle(request)
        sharedBadgeInvestigationSession.recordRepositoryResult(
            requestId = requestId,
            repositoryResult = repository.investigation.value,
        )
        return accepted
    }

    fun cancelInvestigation() {
        sharedBadgeInvestigationSession
            .activeCancelRequestId(repository.investigation.value)
            ?.let { requestId -> repository.cancelBleInvestigation(requestId) }
    }

    fun execute(action: BadgeDangerAction) = when (action) {
        BadgeDangerAction.REBOOT -> repository.rebootBadge()
    }
}
