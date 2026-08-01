package com.friendorfoe.presentation.about

import androidx.lifecycle.SavedStateHandle
import com.friendorfoe.data.AppVersion
import com.friendorfoe.data.BackendEndpoint
import com.friendorfoe.data.DetectionSettings
import com.friendorfoe.data.remote.BackendHealthClient
import com.friendorfoe.data.remote.BackendHealthResponse
import com.friendorfoe.data.repository.AppUpdateMetadata
import com.friendorfoe.data.repository.AppUpdateRepository
import com.friendorfoe.data.repository.BackendSessionHealthRepository
import com.friendorfoe.data.repository.SessionHealth
import com.friendorfoe.presentation.permissions.AppFeature
import com.friendorfoe.presentation.permissions.PermissionUiState
import com.friendorfoe.presentation.permissions.PermissionStateSource
import com.friendorfoe.test.MainDispatcherRule
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.test.advanceUntilIdle
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test

@OptIn(ExperimentalCoroutinesApi::class)
class AboutViewModelTest {
    @get:Rule
    val mainDispatcherRule = MainDispatcherRule()

    @Test
    fun invalidBackendNeverSavesAndChangingTheSavedEndpointClearsSessionEvidence() = runTest {
        val endpointA = endpoint("http://badge-lab:8000/")
        val endpointB = endpoint("https://field-kit.example/")
        val settings = FakeInfoSettingsStore(DetectionSettings.defaults().copy(
            sensorBackendEnabled = true,
            backendUrl = endpointA.baseUrl,
        ))
        val session = sessionRepository()
        session.recordConnected(endpointA, "1.0")
        val viewModel = viewModel(settings, session)
        advanceUntilIdle()
        assertTrue(viewModel.uiState.value.calibrationEntryAvailable)

        viewModel.editBackendUrl("not-a-url")
        viewModel.saveBackendUrl()
        advanceUntilIdle()

        assertEquals("Enter a complete http:// or https:// URL", viewModel.uiState.value.backendUrlError)
        assertNull(settings.lastSavedEndpoint)
        assertEquals(SessionHealth.Healthy(endpointA), session.health.value)

        viewModel.editBackendUrl(endpointB.baseUrl)
        viewModel.saveBackendUrl()
        advanceUntilIdle()

        assertEquals(endpointB, settings.lastSavedEndpoint)
        assertEquals(SessionHealth.Untested, session.health.value)
        assertFalse(viewModel.uiState.value.calibrationEntryAvailable)
    }

    @Test
    fun ordinaryConnectionTestUsesExactEndpointAndUnlocksCalibrationForThisSession() = runTest {
        val configured = endpoint("https://field-kit.example:8443/")
        var checkedEndpoint: BackendEndpoint? = null
        val settings = FakeInfoSettingsStore(DetectionSettings.defaults().copy(
            sensorBackendEnabled = true,
            backendUrl = configured.baseUrl,
        ))
        val session = BackendSessionHealthRepository(
            healthClient = BackendHealthClient { endpoint ->
                checkedEndpoint = endpoint
                BackendHealthResponse("ok", "0.65.0")
            },
            scope = this,
        )
        val viewModel = viewModel(settings, session)

        viewModel.testConnection()
        advanceUntilIdle()

        assertEquals(configured, checkedEndpoint)
        assertEquals(
            ConnectionTestState.Connected(configured, "0.65.0"),
            viewModel.uiState.value.connection,
        )
        assertTrue(viewModel.uiState.value.calibrationEntryAvailable)
    }

    @Test
    fun exactHealthySessionEvidenceSurvivesReturningToInfo() = runTest {
        val configured = endpoint("http://badge-lab:8000/")
        val settings = FakeInfoSettingsStore(DetectionSettings.defaults().copy(
            sensorBackendEnabled = true,
            backendUrl = configured.baseUrl,
        ))
        val session = sessionRepository()
        session.recordConnected(configured, "1.0")

        val first = viewModel(settings, session)
        val second = viewModel(settings, session)
        advanceUntilIdle()

        assertTrue(first.uiState.value.calibrationEntryAvailable)
        assertTrue(second.uiState.value.calibrationEntryAvailable)
        assertEquals(SessionHealth.Healthy(configured), second.uiState.value.sessionHealth)
    }

    @Test
    fun updateStateOnlyAnnouncesAGenuinelyNewerRelease() = runTest {
        val settings = FakeInfoSettingsStore(DetectionSettings.defaults())
        val sameVersion = AppUpdateMetadata(
            AppVersion(null, "0.64.65"),
            "https://github.com/lnxgod/friendorfoe/releases/tag/0.64.65",
        )
        val viewModel = viewModel(
            settings = settings,
            session = sessionRepository(),
            updateRepository = FixedUpdateRepository(Result.success(sameVersion)),
            installed = AppVersion(108, "0.64.65-privacy-beacons"),
        )

        viewModel.checkForUpdates()
        advanceUntilIdle()

        assertEquals(
            UpdateUiState.UpToDate(AppVersion(108, "0.64.65-privacy-beacons")),
            viewModel.uiState.value.updateState,
        )
    }

    @Test
    fun updateFailureUsesNeutralRecoveryCopy() = runTest {
        val viewModel = viewModel(
            settings = FakeInfoSettingsStore(DetectionSettings.defaults()),
            session = sessionRepository(),
            updateRepository = FixedUpdateRepository(Result.failure(IllegalStateException("raw body"))),
        )

        viewModel.checkForUpdates()
        advanceUntilIdle()

        assertEquals(
            UpdateUiState.Failed("Could not check for updates"),
            viewModel.uiState.value.updateState,
        )
    }

    @Test
    fun sourceStatusCallsOutMissingRuntimeAndNotificationDelivery() {
        val statuses = sourceStatus(
            settings = DetectionSettings.defaults().copy(
                phonePrivacyScanEnabled = true,
                privacyNotificationsEnabled = true,
            ),
            endpoint = endpoint("http://fof-server.local:8000/"),
            health = SessionHealth.Untested,
            permissionStates = mapOf(
                AppFeature.LOCAL_RADIO_DISCOVERY to PermissionUiState.Granted,
                AppFeature.PHONE_PRIVACY_SCAN to PermissionUiState.Denied,
                AppFeature.ULTRASONIC to PermissionUiState.Denied,
                AppFeature.PRIVACY_ALERTS to PermissionUiState.NotificationsBlocked,
                AppFeature.SKY_ALERTS to PermissionUiState.Granted,
            ),
        )

        val privacy = statuses.single {
            it.key == InfoSourceKey.PHONE_PRIVACY_SCAN
        }
        val notifications = statuses.single {
            it.key == InfoSourceKey.NOTIFICATION_DELIVERY
        }
        assertEquals("Permission needed", privacy.statusText)
        assertEquals("Permission needed", notifications.statusText)
        assertFalse(privacy.effective ?: true)
        assertFalse(notifications.effective ?: true)
    }

    @Test
    fun permissionBackedSettingsMapOnlyToTheFeatureTheyActuallyNeed() {
        assertEquals(
            AppFeature.LOCAL_RADIO_DISCOVERY,
            permissionFeatureForSetting(InfoSettingKey.BLE_REMOTE_ID),
        )
        assertEquals(
            AppFeature.LOCAL_RADIO_DISCOVERY,
            permissionFeatureForSetting(InfoSettingKey.WIFI_REMOTE_ID),
        )
        assertEquals(
            AppFeature.PHONE_PRIVACY_SCAN,
            permissionFeatureForSetting(InfoSettingKey.PHONE_PRIVACY_SCAN),
        )
        assertEquals(
            AppFeature.PRIVACY_ALERTS,
            permissionFeatureForSetting(InfoSettingKey.PRIVACY_ALERTS),
        )
        listOf(
            InfoSettingKey.DRONE_ALERTS,
            InfoSettingKey.HELICOPTER_ALERTS,
            InfoSettingKey.MILITARY_ALERTS,
            InfoSettingKey.POLICE_ALERTS,
        ).forEach {
            assertEquals(AppFeature.SKY_ALERTS, permissionFeatureForSetting(it))
        }
        assertEquals(AppFeature.ULTRASONIC, permissionFeatureForSetting(InfoSettingKey.ULTRASONIC))
        assertNull(permissionFeatureForSetting(InfoSettingKey.ADS_B))
        assertNull(permissionFeatureForSetting(InfoSettingKey.STALKER))
    }

    @Test
    fun followerAndWifiAnomalyControlsWaitForAnEffectivePhoneScan() {
        val configured = DetectionSettings.defaults().copy(
            phonePrivacyScanEnabled = true,
            backendOnlyMode = false,
        )

        assertFalse(
            isInfoSettingInteractive(
                InfoSettingKey.STALKER,
                configured,
                PermissionUiState.Denied,
            )
        )
        assertFalse(
            isInfoSettingInteractive(
                InfoSettingKey.WIFI_ANOMALY,
                configured.copy(phonePrivacyScanEnabled = false),
                PermissionUiState.Granted,
            )
        )
        assertTrue(
            isInfoSettingInteractive(
                InfoSettingKey.STALKER,
                configured,
                PermissionUiState.Granted,
            )
        )
        assertFalse(
            isInfoSettingInteractive(
                InfoSettingKey.WIFI_ANOMALY,
                configured.copy(backendOnlyMode = true),
                PermissionUiState.Granted,
            )
        )
        assertEquals(
            "Requires Phone privacy scan.",
            infoSettingDisabledReason(
                InfoSettingKey.STALKER,
                configured.copy(phonePrivacyScanEnabled = false),
                PermissionUiState.Granted,
            ),
        )
        assertEquals(
            "Unavailable in backend-only mode.",
            infoSettingDisabledReason(
                InfoSettingKey.WIFI_ANOMALY,
                configured.copy(backendOnlyMode = true),
                PermissionUiState.Granted,
            ),
        )
    }

    @Test
    fun pendingPermissionEnableSurvivesRecreationAndCommitsAfterGrant() = runTest {
        val settings = FakeInfoSettingsStore(
            DetectionSettings.defaults().copy(phonePrivacyScanEnabled = false)
        )
        val savedState = SavedStateHandle()
        val deniedPermissions = AppFeature.entries.associateWith { feature ->
            if (feature == AppFeature.PHONE_PRIVACY_SCAN) PermissionUiState.Denied
            else PermissionUiState.Granted
        }
        val first = viewModel(
            settings = settings,
            session = sessionRepository(),
            savedStateHandle = savedState,
            permissionStates = deniedPermissions,
        )

        first.beginPermissionEnable(InfoSettingKey.PHONE_PRIVACY_SCAN)
        first.markPermissionRequestLaunched()

        val recreated = viewModel(
            settings = settings,
            session = sessionRepository(),
            savedStateHandle = savedState,
            permissionStates = deniedPermissions,
        )
        assertEquals(
            InfoSettingKey.PHONE_PRIVACY_SCAN,
            recreated.pendingPermissionSetting.value?.key,
        )

        recreated.resolvePendingPermission(
            feature = AppFeature.PHONE_PRIVACY_SCAN,
            state = PermissionUiState.Granted,
        )
        advanceUntilIdle()

        assertEquals(
            InfoSettingKey.PHONE_PRIVACY_SCAN to true,
            settings.writes.last(),
        )
        assertNull(recreated.pendingPermissionSetting.value)
    }

    @Test
    fun onlyCollectorTopologySettingsRequestASkyRestart() {
        assertTrue(shouldRestartSkySourcesForInfoSetting(InfoSettingKey.ADS_B))
        assertTrue(shouldRestartSkySourcesForInfoSetting(InfoSettingKey.BLE_REMOTE_ID))
        assertTrue(shouldRestartSkySourcesForInfoSetting(InfoSettingKey.WIFI_REMOTE_ID))
        assertTrue(shouldRestartSkySourcesForInfoSetting(InfoSettingKey.BACKEND_ONLY))
        assertFalse(shouldRestartSkySourcesForInfoSetting(InfoSettingKey.STALKER))
        assertFalse(shouldRestartSkySourcesForInfoSetting(InfoSettingKey.ULTRASONIC))
    }

    private fun viewModel(
        settings: InfoSettingsStore,
        session: BackendSessionHealthRepository,
        updateRepository: AppUpdateRepository = FixedUpdateRepository(Result.failure(Exception())),
        installed: AppVersion = AppVersion(108, "0.64.65"),
        permissionStates: Map<AppFeature, PermissionUiState> =
            AppFeature.entries.associateWith { PermissionUiState.Granted },
        savedStateHandle: SavedStateHandle = SavedStateHandle(),
    ) = AboutViewModel(
        settingsStore = settings,
        sessionHealthRepository = session,
        appUpdateRepository = updateRepository,
        installedVersion = installed,
        permissionStateSource = FixedPermissionStateSource(permissionStates),
        savedStateHandle = savedStateHandle,
    )

    private fun sessionRepository() = BackendSessionHealthRepository(
        healthClient = BackendHealthClient { BackendHealthResponse("ok", "test") },
        scope = kotlinx.coroutines.CoroutineScope(mainDispatcherRule.dispatcher),
    )

    private fun endpoint(raw: String) = BackendEndpoint.parse(raw).getOrThrow()
}

private class FixedPermissionStateSource(
    initial: Map<AppFeature, PermissionUiState>,
) : PermissionStateSource {
    override val states = MutableStateFlow(initial)
}

private class FakeInfoSettingsStore(initial: DetectionSettings) : InfoSettingsStore {
    private val mutableSettings = MutableStateFlow(initial)
    override val settings: StateFlow<DetectionSettings> = mutableSettings
    val writes = mutableListOf<Pair<InfoSettingKey, Boolean>>()
    var lastSavedEndpoint: BackendEndpoint? = null

    override fun set(key: InfoSettingKey, enabled: Boolean) {
        writes += key to enabled
        mutableSettings.value = settings.value.withSetting(key, enabled)
    }

    override fun saveBackendEndpoint(endpoint: BackendEndpoint) {
        lastSavedEndpoint = endpoint
        mutableSettings.value = settings.value.copy(backendUrl = endpoint.baseUrl)
    }
}

private class FixedUpdateRepository(
    private val result: Result<AppUpdateMetadata>,
) : AppUpdateRepository {
    override suspend fun latest(): Result<AppUpdateMetadata> = result
}
