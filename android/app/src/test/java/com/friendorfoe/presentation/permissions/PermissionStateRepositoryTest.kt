package com.friendorfoe.presentation.permissions

import android.Manifest
import com.friendorfoe.data.preferences.AppLaunchState
import com.friendorfoe.data.preferences.AppPreferences
import com.friendorfoe.data.preferences.FindingPreferenceKey
import java.util.concurrent.atomic.AtomicInteger
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.async
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.flow
import kotlinx.coroutines.flow.flowOf
import kotlinx.coroutines.launch
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Test

class PermissionStateRepositoryTest {

    @Test
    fun refreshUsesPersistedHistoryToSeparateFirstAndPermanentDenial() = runTest {
        val preferences = FakeAppPreferences()
        val repository = PermissionStateRepository(
            platform = FakePlatformPermissionEvidence(sdkInt = 35),
            preferences = preferences,
        )

        repository.refresh(rationaleByPermission = emptyMap())
        assertEquals(PermissionUiState.Denied, repository.stateFor(AppFeature.AR_CAMERA))

        preferences.markPermissionsRequested(setOf(Manifest.permission.CAMERA))
        repository.refresh(rationaleByPermission = emptyMap())
        assertEquals(
            PermissionUiState.PermanentlyDenied,
            repository.stateFor(AppFeature.AR_CAMERA),
        )
    }

    @Test
    fun refreshAfterSettingsImmediatelyPublishesNewPlatformTruth() = runTest {
        val platform = FakePlatformPermissionEvidence(sdkInt = 35)
        val repository = PermissionStateRepository(platform, FakeAppPreferences())

        repository.refresh(rationaleByPermission = emptyMap())
        assertEquals(PermissionUiState.Denied, repository.stateFor(AppFeature.IR_CAMERA))

        platform.granted += Manifest.permission.CAMERA
        repository.refresh(rationaleByPermission = emptyMap())

        assertEquals(PermissionUiState.Granted, repository.stateFor(AppFeature.IR_CAMERA))
    }

    @Test
    fun approximateLocationIsUsableButStillExposesItsReducedPrecision() = runTest {
        val platform = FakePlatformPermissionEvidence(
            sdkInt = 35,
            granted = mutableSetOf(Manifest.permission.ACCESS_COARSE_LOCATION),
        )
        val repository = PermissionStateRepository(platform, FakeAppPreferences())

        repository.refresh(rationaleByPermission = emptyMap())

        assertEquals(
            PermissionUiState.Approximate,
            repository.stateFor(AppFeature.AR_MAP_LOCATION),
        )
    }

    @Test
    fun globallyDisabledNotificationsOverrideGrantedRuntimePermission() = runTest {
        val platform = FakePlatformPermissionEvidence(
            sdkInt = 35,
            granted = mutableSetOf(Manifest.permission.POST_NOTIFICATIONS),
            notificationsEnabled = false,
        )
        val repository = PermissionStateRepository(platform, FakeAppPreferences())

        repository.refresh(rationaleByPermission = emptyMap())

        assertEquals(
            PermissionUiState.NotificationsBlocked,
            repository.stateFor(AppFeature.PRIVACY_ALERTS),
        )
        assertEquals(
            PermissionUiState.NotificationsBlocked,
            repository.stateFor(AppFeature.SKY_ALERTS),
        )
    }

    @Test
    fun eachNotificationFeatureUsesOnlyItsOwnedChannel() = runTest {
        val platform = FakePlatformPermissionEvidence(
            sdkInt = 35,
            granted = mutableSetOf(Manifest.permission.POST_NOTIFICATIONS),
            channelStates = mutableMapOf(
                PRIVACY_ALERT_CHANNEL_ID to false,
                SKY_ALERT_CHANNEL_ID to true,
            ),
        )
        val repository = PermissionStateRepository(platform, FakeAppPreferences())

        repository.refresh(rationaleByPermission = emptyMap())

        assertEquals(
            PermissionUiState.NotificationChannelBlocked,
            repository.stateFor(AppFeature.PRIVACY_ALERTS),
        )
        assertEquals(
            PermissionUiState.Granted,
            repository.stateFor(AppFeature.SKY_ALERTS),
        )
    }

    @Test
    fun missingPermissionSetMatchesTheCompleteApi35CollectorContract() {
        val platform = FakePlatformPermissionEvidence(
            sdkInt = 35,
            granted = mutableSetOf(
                Manifest.permission.BLUETOOTH_SCAN,
                Manifest.permission.NEARBY_WIFI_DEVICES,
            ),
        )
        val repository = PermissionStateRepository(platform, FakeAppPreferences())

        assertEquals(
            setOf(
                Manifest.permission.BLUETOOTH_CONNECT,
                Manifest.permission.ACCESS_FINE_LOCATION,
                Manifest.permission.ACCESS_COARSE_LOCATION,
            ),
            repository.missingPermissionsFor(AppFeature.PHONE_PRIVACY_SCAN),
        )
    }

    @Test
    fun blockedOlderRefreshCannotOverwriteNewerPlatformTruth() = runTest {
        val firstRefreshSubscribed = CompletableDeferred<Unit>()
        val releaseFirstRefresh = CompletableDeferred<Unit>()
        val subscriptions = AtomicInteger(0)
        val requestedHistory = flow {
            if (subscriptions.incrementAndGet() == 1) {
                firstRefreshSubscribed.complete(Unit)
                releaseFirstRefresh.await()
            }
            emit(emptySet<String>())
        }
        val platform = FakePlatformPermissionEvidence(sdkInt = 35)
        val repository = PermissionStateRepository(
            platform = platform,
            preferences = FakeAppPreferences(requestedPermissionsFlow = requestedHistory),
        )

        val older = launch { repository.refresh(rationaleByPermission = emptyMap()) }
        firstRefreshSubscribed.await()
        platform.granted += Manifest.permission.CAMERA

        val newer = launch { repository.refresh(rationaleByPermission = emptyMap()) }
        newer.join()
        assertEquals(PermissionUiState.Granted, repository.stateFor(AppFeature.AR_CAMERA))

        releaseFirstRefresh.complete(Unit)
        older.join()

        assertEquals(PermissionUiState.Granted, repository.stateFor(AppFeature.AR_CAMERA))
    }

    @Test
    fun mutableRationaleInputIsSnapshottedBeforePreferencesCanSuspend() = runTest {
        val subscribed = CompletableDeferred<Unit>()
        val release = CompletableDeferred<Unit>()
        val requestedHistory = flow {
            subscribed.complete(Unit)
            release.await()
            emit(setOf(Manifest.permission.CAMERA))
        }
        val repository = PermissionStateRepository(
            platform = FakePlatformPermissionEvidence(sdkInt = 35),
            preferences = FakeAppPreferences(requestedPermissionsFlow = requestedHistory),
        )
        val rationales = mutableMapOf(Manifest.permission.CAMERA to true)

        val refresh = launch {
            repository.refresh(rationaleByPermission = rationales)
        }
        subscribed.await()
        rationales[Manifest.permission.CAMERA] = false
        release.complete(Unit)
        refresh.join()

        assertEquals(PermissionUiState.Denied, repository.stateFor(AppFeature.AR_CAMERA))
    }

    @Test
    fun grantedRequestCallbackUsesItsOwnResultWhileNewerResumeRefreshIsStillBlocked() = runTest {
        val firstSubscribed = CompletableDeferred<Unit>()
        val secondSubscribed = CompletableDeferred<Unit>()
        val releaseFirst = CompletableDeferred<Unit>()
        val releaseSecond = CompletableDeferred<Unit>()
        val subscriptions = AtomicInteger(0)
        val requestedHistory = flow {
            when (subscriptions.incrementAndGet()) {
                1 -> {
                    firstSubscribed.complete(Unit)
                    releaseFirst.await()
                }
                2 -> {
                    secondSubscribed.complete(Unit)
                    releaseSecond.await()
                }
            }
            emit(emptySet<String>())
        }
        val repository = PermissionStateRepository(
            platform = FakePlatformPermissionEvidence(
                sdkInt = 35,
                granted = mutableSetOf(Manifest.permission.CAMERA),
            ),
            preferences = FakeAppPreferences(requestedPermissionsFlow = requestedHistory),
        )

        val callbackRefresh = async {
            repository.refresh(rationaleByPermission = emptyMap())
        }
        firstSubscribed.await()
        val resumeRefresh = async {
            repository.refresh(rationaleByPermission = emptyMap())
        }
        secondSubscribed.await()

        releaseFirst.complete(Unit)
        val callbackResult = callbackRefresh.await()

        assertEquals(PermissionUiState.Granted, callbackResult[AppFeature.AR_CAMERA])
        assertEquals(PermissionUiState.Loading, repository.stateFor(AppFeature.AR_CAMERA))

        releaseSecond.complete(Unit)
        resumeRefresh.await()
        assertEquals(PermissionUiState.Granted, repository.stateFor(AppFeature.AR_CAMERA))
    }
}

private class FakePlatformPermissionEvidence(
    override val sdkInt: Int,
    val granted: MutableSet<String> = mutableSetOf(),
    var notificationsEnabled: Boolean = true,
    val channelStates: MutableMap<String, Boolean> = mutableMapOf(),
) : PlatformPermissionEvidence {
    override fun isGranted(permission: String): Boolean = permission in granted
    override fun notificationsEnabled(): Boolean = notificationsEnabled
    override fun channelEnabled(channelId: String): Boolean = channelStates[channelId] ?: true
}

private class FakeAppPreferences(
    initialRequestedPermissions: Set<String> = emptySet(),
    requestedPermissionsFlow: Flow<Set<String>>? = null,
) : AppPreferences {
    override val launchState: Flow<AppLaunchState> = flowOf(AppLaunchState.NeedsOnboarding)
    override val ignoredFindingKeys: Flow<Set<String>> = flowOf(emptySet())
    private val mutableRequestedPermissions = MutableStateFlow(initialRequestedPermissions)
    override val requestedPermissions: Flow<Set<String>> =
        requestedPermissionsFlow ?: mutableRequestedPermissions

    override suspend fun setOnboardingComplete() = Unit
    override suspend fun setLastTopLevelRoute(route: String) = Unit
    override suspend fun ignoreFinding(key: FindingPreferenceKey) = Unit
    override suspend fun restoreFinding(key: FindingPreferenceKey) = Unit

    override suspend fun markPermissionsRequested(permissions: Set<String>) {
        mutableRequestedPermissions.value += permissions
    }
}
