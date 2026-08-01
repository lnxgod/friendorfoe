package com.friendorfoe.data.preferences

import kotlinx.coroutines.flow.Flow

interface AppPreferences {
    val launchState: Flow<AppLaunchState>
    val ignoredFindingKeys: Flow<Set<String>>

    suspend fun setOnboardingComplete()
    suspend fun setLastTopLevelRoute(route: String)
    suspend fun ignoreFinding(key: FindingPreferenceKey)
    suspend fun restoreFinding(key: FindingPreferenceKey)
}

sealed interface AppLaunchState {
    data object Loading : AppLaunchState
    data object NeedsOnboarding : AppLaunchState
    data class Ready(val startRoute: String) : AppLaunchState
}

data class FindingPreferenceKey private constructor(
    val source: String,
    val stableId: String,
) {
    val encoded: String = "$source\u001F$stableId"

    companion object {
        fun create(source: String, stableId: String): FindingPreferenceKey? =
            if (source.isBlank() || stableId.isBlank()) null else FindingPreferenceKey(source, stableId)

        fun decode(encoded: String): FindingPreferenceKey? {
            val parts = encoded.split('\u001F', limit = 2)
            return if (parts.size == 2) create(parts[0], parts[1]) else null
        }
    }
}
