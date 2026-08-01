package com.friendorfoe.presentation

import com.friendorfoe.data.preferences.AppLaunchState
import com.friendorfoe.data.preferences.AppPreferences
import com.friendorfoe.data.preferences.FindingPreferenceKey
import com.friendorfoe.test.MainDispatcherRule
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.test.advanceUntilIdle
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Rule
import org.junit.Test

@OptIn(ExperimentalCoroutinesApi::class)
class AppChromeViewModelTest {

    @get:Rule
    val mainDispatcherRule = MainDispatcherRule()

    @Test
    fun launchStateAndOnboardingCompletionFollowPreferences() = runTest {
        val preferences = FakeAppPreferences(AppLaunchState.NeedsOnboarding)
        val viewModel = AppChromeViewModel(preferences)

        assertEquals(
            AppLaunchState.NeedsOnboarding,
            viewModel.uiState.first { it.launchState !is AppLaunchState.Loading }.launchState
        )

        viewModel.completeOnboarding()
        advanceUntilIdle()

        assertEquals(1, preferences.onboardingCompletionCount)

        preferences.launchState.value = AppLaunchState.Ready("privacy")

        advanceUntilIdle()

        assertEquals(AppLaunchState.Ready("privacy"), viewModel.uiState.value.launchState)
    }
}

private class FakeAppPreferences(initialLaunchState: AppLaunchState) : AppPreferences {
    override val launchState = MutableStateFlow(initialLaunchState)
    override val ignoredFindingKeys = MutableStateFlow(emptySet<String>())
    override val requestedPermissions = MutableStateFlow(emptySet<String>())
    var onboardingCompletionCount = 0

    override suspend fun setOnboardingComplete() {
        onboardingCompletionCount++
    }

    override suspend fun setLastTopLevelRoute(route: String) = Unit

    override suspend fun ignoreFinding(key: FindingPreferenceKey) = Unit

    override suspend fun restoreFinding(key: FindingPreferenceKey) = Unit

    override suspend fun markPermissionsRequested(permissions: Set<String>) {
        requestedPermissions.value += permissions
    }
}
