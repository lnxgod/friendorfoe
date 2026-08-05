package com.friendorfoe.data.preferences

import android.content.Context
import androidx.annotation.VisibleForTesting
import androidx.datastore.preferences.core.booleanPreferencesKey
import androidx.datastore.preferences.core.edit
import androidx.datastore.preferences.core.stringPreferencesKey
import androidx.datastore.preferences.core.stringSetPreferencesKey
import androidx.datastore.preferences.preferencesDataStore
import dagger.hilt.android.qualifiers.ApplicationContext
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.map
import javax.inject.Inject
import javax.inject.Singleton

private val Context.fofDataStore by preferencesDataStore(name = "fof_app_state")

private val TOP_LEVEL_ROUTES = setOf(
    "ar_view", "map_view", "list_view", "privacy", "badge", "history", "info"
)

internal const val DEFAULT_TOP_LEVEL_ROUTE = "info"

internal fun normalLaunchRoute(): String = DEFAULT_TOP_LEVEL_ROUTE

internal fun sanitizeTopLevelRoute(route: String?): String =
    route?.takeIf(TOP_LEVEL_ROUTES::contains) ?: DEFAULT_TOP_LEVEL_ROUTE

@Singleton
class AppPreferencesRepository @Inject constructor(
    @ApplicationContext private val context: Context,
) : AppPreferences {
    private val onboarding = booleanPreferencesKey("onboarding_complete")
    private val lastRoute = stringPreferencesKey("last_top_level_route")
    private val ignored = stringSetPreferencesKey("ignored_finding_keys")
    private val requested = stringSetPreferencesKey("requested_permissions")

    override val launchState: Flow<AppLaunchState> = context.fofDataStore.data.map { prefs ->
        if (prefs[onboarding] != true) AppLaunchState.NeedsOnboarding
        else AppLaunchState.Ready(normalLaunchRoute())
    }

    override val ignoredFindingKeys: Flow<Set<String>> = context.fofDataStore.data
        .map { it[ignored].orEmpty() }

    override val requestedPermissions: Flow<Set<String>> = context.fofDataStore.data
        .map { it[requested].orEmpty() }

    override suspend fun setOnboardingComplete() {
        context.fofDataStore.edit {
            it[onboarding] = true
            it[lastRoute] = normalLaunchRoute()
        }
    }

    override suspend fun setLastTopLevelRoute(route: String) {
        context.fofDataStore.edit {
            it[lastRoute] = sanitizeTopLevelRoute(route)
        }
    }

    override suspend fun ignoreFinding(key: FindingPreferenceKey) {
        context.fofDataStore.edit {
            it[ignored] = it[ignored].orEmpty() + key.encoded
        }
    }

    override suspend fun restoreFinding(key: FindingPreferenceKey) {
        context.fofDataStore.edit {
            it[ignored] = it[ignored].orEmpty() - key.encoded
        }
    }

    override suspend fun markPermissionsRequested(permissions: Set<String>) {
        if (permissions.isEmpty()) return
        context.fofDataStore.edit {
            it[requested] = it[requested].orEmpty() + permissions
        }
    }

    @VisibleForTesting
    suspend fun resetForInstrumentation() {
        context.fofDataStore.edit { it.clear() }
    }
}
