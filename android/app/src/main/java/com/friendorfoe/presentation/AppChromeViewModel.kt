package com.friendorfoe.presentation

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.friendorfoe.data.preferences.AppLaunchState
import com.friendorfoe.data.preferences.AppPreferences
import dagger.hilt.android.lifecycle.HiltViewModel
import javax.inject.Inject
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch

data class AppChromeUiState(
    val launchState: AppLaunchState = AppLaunchState.Loading,
)

@HiltViewModel
class AppChromeViewModel @Inject constructor(
    private val appPreferences: AppPreferences,
) : ViewModel() {
    val uiState = appPreferences.launchState
        .map(::AppChromeUiState)
        .stateIn(viewModelScope, SharingStarted.Eagerly, AppChromeUiState())

    fun completeOnboarding() = viewModelScope.launch {
        appPreferences.setOnboardingComplete()
    }

    fun recordTopLevelRoute(route: String) = viewModelScope.launch {
        appPreferences.setLastTopLevelRoute(route)
    }
}
