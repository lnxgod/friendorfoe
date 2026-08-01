package com.friendorfoe.presentation.permissions

import android.Manifest
import androidx.lifecycle.SavedStateHandle
import com.friendorfoe.data.preferences.AppLaunchState
import com.friendorfoe.data.preferences.AppPreferences
import com.friendorfoe.data.preferences.FindingPreferenceKey
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.flowOf
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Test

class PermissionRequestCoordinatorTest {

    @Test
    fun requestHistoryIsDurableBeforeTheSystemDialogLaunches() = runTest {
        val events = mutableListOf<String>()
        val preferences = RecordingPreferences(events)
        val launcher = PermissionLauncher { permissions ->
            events += "launch:${permissions.joinToString()}"
        }

        requestFeaturePermissions(
            missing = setOf(
                Manifest.permission.NEARBY_WIFI_DEVICES,
                Manifest.permission.BLUETOOTH_SCAN,
            ),
            preferences = preferences,
            launcher = launcher,
        )

        assertEquals(
            listOf(
                "persist:${Manifest.permission.BLUETOOTH_SCAN}, ${Manifest.permission.NEARBY_WIFI_DEVICES}",
                "launch:${Manifest.permission.BLUETOOTH_SCAN}, ${Manifest.permission.NEARBY_WIFI_DEVICES}",
            ),
            events,
        )
    }

    @Test
    fun alreadyGrantedFeatureDoesNotWriteHistoryOrLaunchADialog() = runTest {
        val events = mutableListOf<String>()

        requestFeaturePermissions(
            missing = emptySet(),
            preferences = RecordingPreferences(events),
            launcher = PermissionLauncher { events += "launch" },
        )

        assertEquals(emptyList<String>(), events)
    }

    @Test
    fun deniedEnableOpensExplanationWithoutCommittingTheSetting() {
        assertEquals(
            PermissionToggleAction.ShowExplanation,
            permissionToggleAction(
                configuredChecked = false,
                effectiveChecked = false,
                requestedChecked = true,
                permissionState = PermissionUiState.Denied,
            ),
        )
    }

    @Test
    fun disablingAlwaysCommitsEvenWhenPermissionIsMissing() {
        assertEquals(
            PermissionToggleAction.Commit(false),
            permissionToggleAction(
                configuredChecked = true,
                effectiveChecked = true,
                requestedChecked = false,
                permissionState = PermissionUiState.PermanentlyDenied,
            ),
        )
    }

    @Test
    fun grantedAndApproximateEnableRequestsCommitImmediately() {
        assertEquals(
            PermissionToggleAction.Commit(true),
            permissionToggleAction(
                configuredChecked = false,
                effectiveChecked = false,
                requestedChecked = true,
                permissionState = PermissionUiState.Granted,
            ),
        )
        assertEquals(
            PermissionToggleAction.Commit(true),
            permissionToggleAction(
                configuredChecked = false,
                effectiveChecked = false,
                requestedChecked = true,
                permissionState = PermissionUiState.Approximate,
            ),
        )
    }

    @Test
    fun storedOnButPermissionMissingCanBeExplicitlyDisabledFromTheEffectiveOffRow() {
        assertEquals(
            PermissionToggleAction.Commit(false),
            permissionToggleAction(
                configuredChecked = true,
                effectiveChecked = false,
                requestedChecked = true,
                permissionState = PermissionUiState.Denied,
            ),
        )
    }

    @Test
    fun activeFeatureRequestSurvivesActivityRecreation() {
        val savedState = SavedStateHandle()
        PendingFeatureRequestStore(savedState).begin(AppFeature.PHONE_PRIVACY_SCAN)

        val recreated = PendingFeatureRequestStore(savedState)

        assertEquals(AppFeature.PHONE_PRIVACY_SCAN, recreated.pendingFeature())
        recreated.clear()
        assertEquals(null, PendingFeatureRequestStore(savedState).pendingFeature())
    }
}

private class RecordingPreferences(
    private val events: MutableList<String>,
) : AppPreferences {
    override val launchState: Flow<AppLaunchState> = flowOf(AppLaunchState.NeedsOnboarding)
    override val ignoredFindingKeys: Flow<Set<String>> = flowOf(emptySet())
    override val requestedPermissions = MutableStateFlow(emptySet<String>())

    override suspend fun setOnboardingComplete() = Unit
    override suspend fun setLastTopLevelRoute(route: String) = Unit
    override suspend fun ignoreFinding(key: FindingPreferenceKey) = Unit
    override suspend fun restoreFinding(key: FindingPreferenceKey) = Unit

    override suspend fun markPermissionsRequested(permissions: Set<String>) {
        requestedPermissions.value += permissions
        events += "persist:${permissions.sorted().joinToString()}"
    }
}
