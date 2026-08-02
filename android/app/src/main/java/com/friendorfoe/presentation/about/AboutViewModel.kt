package com.friendorfoe.presentation.about

import androidx.lifecycle.ViewModel
import androidx.lifecycle.SavedStateHandle
import androidx.lifecycle.viewModelScope
import com.friendorfoe.data.AppVersion
import com.friendorfoe.data.BackendEndpoint
import com.friendorfoe.data.DetectionPrefs
import com.friendorfoe.data.DetectionSettings
import com.friendorfoe.data.isUpdateAvailable
import com.friendorfoe.data.repository.AppUpdateMetadata
import com.friendorfoe.data.repository.AppUpdateRepository
import com.friendorfoe.data.repository.BackendSessionHealthRepository
import com.friendorfoe.data.repository.SessionHealth
import com.friendorfoe.data.repository.SkyObjectRepository
import com.friendorfoe.presentation.permissions.AppFeature
import com.friendorfoe.presentation.permissions.PermissionStateSource
import com.friendorfoe.presentation.permissions.PermissionUiState
import com.friendorfoe.presentation.permissions.isUsable
import dagger.hilt.android.lifecycle.HiltViewModel
import java.util.concurrent.atomic.AtomicLong
import javax.inject.Inject
import javax.inject.Singleton
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Job
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch

private const val BACKEND_URL_ERROR = "Enter a complete http:// or https:// URL"

enum class InfoSettingKey {
    ADS_B,
    BLE_REMOTE_ID,
    WIFI_REMOTE_ID,
    PHONE_PRIVACY_SCAN,
    STALKER,
    ULTRASONIC,
    WIFI_ANOMALY,
    PRIVACY_ALERTS,
    DRONE_ALERTS,
    HELICOPTER_ALERTS,
    MILITARY_ALERTS,
    POLICE_ALERTS,
    SENSOR_BACKEND,
    BACKEND_ONLY,
}

enum class InfoSourceKey {
    ADS_B,
    BLE_REMOTE_ID,
    WIFI_REMOTE_ID,
    PHONE_PRIVACY_SCAN,
    ULTRASONIC,
    SENSOR_BACKEND,
    NOTIFICATION_DELIVERY,
}

data class InfoSourceStatus(
    val key: InfoSourceKey,
    val label: String,
    val configured: Boolean,
    /** Null means the app has configuration but no runtime proof of operation. */
    val effective: Boolean?,
    val statusText: String,
    val detail: String? = null,
)

sealed interface ConnectionTestState {
    data object Idle : ConnectionTestState
    data class Checking(val endpoint: BackendEndpoint) : ConnectionTestState
    data class Connected(
        val endpoint: BackendEndpoint,
        val serverVersion: String?,
    ) : ConnectionTestState
    data class Failed(
        val endpoint: BackendEndpoint,
        val message: String,
    ) : ConnectionTestState
}

sealed interface UpdateUiState {
    data object Idle : UpdateUiState
    data object Checking : UpdateUiState
    data class UpToDate(val installed: AppVersion) : UpdateUiState
    data class Available(val remote: AppUpdateMetadata) : UpdateUiState
    data class Failed(val message: String) : UpdateUiState
}

data class InfoUiState(
    val settings: DetectionSettings = DetectionSettings.defaults(),
    val permissionStates: Map<AppFeature, PermissionUiState> = emptyMap(),
    val sourceStatus: List<InfoSourceStatus> = emptyList(),
    val backendUrlDraft: String = DetectionSettings.defaults().backendUrl,
    val backendUrlError: String? = null,
    val backendUrlCanSave: Boolean = true,
    val backendUrlCanTest: Boolean = true,
    val connection: ConnectionTestState = ConnectionTestState.Idle,
    val sessionHealth: SessionHealth = SessionHealth.Untested,
    val calibrationEntryAvailable: Boolean = false,
    val installedVersion: AppVersion = AppVersion(null, ""),
    val updateState: UpdateUiState = UpdateUiState.Idle,
) {
    // Transitional names keep callers source-compatible while InfoContent replaces the old screen.
    val backendValidationError: String? get() = backendUrlError
    val connectionStatus: ConnectionTestState get() = connection
}

data class PendingInfoPermissionSetting(
    val key: InfoSettingKey,
    val requestLaunched: Boolean,
)

typealias InfoSettingsUiState = InfoUiState

fun calibrationEntryAvailable(
    backendEnabled: Boolean,
    endpoint: BackendEndpoint,
    health: SessionHealth,
): Boolean = backendEnabled && health == SessionHealth.Healthy(endpoint)

interface InfoSettingsStore {
    val settings: StateFlow<DetectionSettings>
    fun set(key: InfoSettingKey, enabled: Boolean)
    fun saveBackendEndpoint(endpoint: BackendEndpoint)
}

@Singleton
class AndroidInfoSettingsStore @Inject constructor(
    private val detectionPrefs: DetectionPrefs,
    private val skyObjectRepository: SkyObjectRepository,
) : InfoSettingsStore {
    override val settings: StateFlow<DetectionSettings> = detectionPrefs.settings

    override fun set(key: InfoSettingKey, enabled: Boolean) {
        val previousSettings = settings.value

        when (key) {
            InfoSettingKey.ADS_B -> detectionPrefs.adsbEnabled = enabled
            InfoSettingKey.BLE_REMOTE_ID -> detectionPrefs.bleRidEnabled = enabled
            InfoSettingKey.WIFI_REMOTE_ID -> detectionPrefs.wifiEnabled = enabled
            InfoSettingKey.PHONE_PRIVACY_SCAN ->
                skyObjectRepository.setPrivacyDetectionEnabled(enabled)
            InfoSettingKey.STALKER -> detectionPrefs.stalkerDetectionEnabled = enabled
            InfoSettingKey.ULTRASONIC -> detectionPrefs.ultrasonicEnabled = enabled
            InfoSettingKey.WIFI_ANOMALY -> detectionPrefs.wifiAnomalyEnabled = enabled
            InfoSettingKey.PRIVACY_ALERTS -> detectionPrefs.privacyNotificationsEnabled = enabled
            InfoSettingKey.DRONE_ALERTS -> detectionPrefs.droneAlertsEnabled = enabled
            InfoSettingKey.HELICOPTER_ALERTS -> detectionPrefs.helicopterAlertsEnabled = enabled
            InfoSettingKey.MILITARY_ALERTS -> detectionPrefs.militaryAlertsEnabled = enabled
            InfoSettingKey.POLICE_ALERTS -> detectionPrefs.policeAlertsEnabled = enabled
            InfoSettingKey.SENSOR_BACKEND -> detectionPrefs.sensorBackendEnabled = enabled
            InfoSettingKey.BACKEND_ONLY -> detectionPrefs.backendOnlyMode = enabled
        }
        if (shouldRestartSkySourcesForInfoSetting(key, enabled, previousSettings)) {
            skyObjectRepository.restartDetectionSources()
        }
    }

    override fun saveBackendEndpoint(endpoint: BackendEndpoint) {
        detectionPrefs.backendUrl = endpoint.baseUrl
    }
}

internal fun shouldRestartSkySourcesForInfoSetting(
    key: InfoSettingKey,
    enabled: Boolean,
    previousSettings: DetectionSettings,
): Boolean = key in setOf(
    InfoSettingKey.ADS_B,
    InfoSettingKey.BLE_REMOTE_ID,
    InfoSettingKey.WIFI_REMOTE_ID,
    InfoSettingKey.BACKEND_ONLY,
) || (
    key == InfoSettingKey.SENSOR_BACKEND &&
        !enabled &&
        previousSettings.backendOnlyMode
)

internal fun DetectionSettings.withSetting(
    key: InfoSettingKey,
    enabled: Boolean,
): DetectionSettings = when (key) {
    InfoSettingKey.ADS_B -> copy(adsbEnabled = enabled)
    InfoSettingKey.BLE_REMOTE_ID -> copy(bleRidEnabled = enabled)
    InfoSettingKey.WIFI_REMOTE_ID -> copy(wifiEnabled = enabled)
    InfoSettingKey.PHONE_PRIVACY_SCAN -> copy(phonePrivacyScanEnabled = enabled)
    InfoSettingKey.STALKER -> copy(stalkerEnabled = enabled)
    InfoSettingKey.ULTRASONIC -> copy(ultrasonicEnabled = enabled)
    InfoSettingKey.WIFI_ANOMALY -> copy(wifiAnomalyEnabled = enabled)
    InfoSettingKey.PRIVACY_ALERTS -> copy(privacyNotificationsEnabled = enabled)
    InfoSettingKey.DRONE_ALERTS -> copy(droneAlertsEnabled = enabled)
    InfoSettingKey.HELICOPTER_ALERTS -> copy(helicopterAlertsEnabled = enabled)
    InfoSettingKey.MILITARY_ALERTS -> copy(militaryAlertsEnabled = enabled)
    InfoSettingKey.POLICE_ALERTS -> copy(policeAlertsEnabled = enabled)
    InfoSettingKey.SENSOR_BACKEND -> copy(
        sensorBackendEnabled = enabled,
        backendOnlyMode = backendOnlyMode && enabled,
    )
    InfoSettingKey.BACKEND_ONLY -> copy(
        backendOnlyMode = enabled && sensorBackendEnabled,
    )
}

private data class BackendDraft(
    val text: String,
    val persistedText: String,
    val error: String?,
) {
    val dirty: Boolean get() = text.trim() != persistedText.trim()
}

@HiltViewModel
class AboutViewModel @Inject constructor(
    private val settingsStore: InfoSettingsStore,
    private val sessionHealthRepository: BackendSessionHealthRepository,
    private val appUpdateRepository: AppUpdateRepository,
    private val installedVersion: AppVersion,
    private val permissionStateSource: PermissionStateSource,
    private val savedStateHandle: SavedStateHandle,
) : ViewModel() {
    private val initialSettings = settingsStore.settings.value
    private val backendDraft = MutableStateFlow(
        BackendDraft(
            text = initialSettings.backendUrl,
            persistedText = initialSettings.backendUrl,
            error = initialSettings.backendUrl.validationError(),
        ),
    )
    private val updateState = MutableStateFlow<UpdateUiState>(UpdateUiState.Idle)
    private val _pendingPermissionSetting = MutableStateFlow(readPendingPermissionSetting())
    val pendingPermissionSetting: StateFlow<PendingInfoPermissionSetting?> =
        _pendingPermissionSetting
    private val updateGeneration = AtomicLong(0L)
    private var updateJob: Job? = null

    init {
        reconcileExistingSession(initialSettings)
        var previousBackendConfig = initialSettings.backendConfig()
        viewModelScope.launch {
            settingsStore.settings.collect { settings ->
                val nextBackendConfig = settings.backendConfig()
                if (nextBackendConfig != previousBackendConfig) {
                    sessionHealthRepository.invalidate()
                    previousBackendConfig = nextBackendConfig
                }
                backendDraft.update { draft ->
                    if (draft.dirty) {
                        draft.copy(persistedText = settings.backendUrl)
                    } else {
                        BackendDraft(
                            text = settings.backendUrl,
                            persistedText = settings.backendUrl,
                            error = settings.backendUrl.validationError(),
                        )
                    }
                }
            }
        }
        viewModelScope.launch {
            permissionStateSource.states.collect { permissionStates ->
                val pending = _pendingPermissionSetting.value ?: return@collect
                if (!pending.requestLaunched) return@collect
                val feature = permissionFeatureForSetting(pending.key) ?: return@collect
                val state = permissionStates[feature] ?: return@collect
                if (state.isUsable()) resolvePendingPermission(feature, state)
            }
        }
    }

    private val settingsAndPermissions = combine(
        settingsStore.settings,
        permissionStateSource.states,
    ) { settings, permissions -> settings to permissions }

    val uiState: StateFlow<InfoUiState> = combine(
        settingsAndPermissions,
        backendDraft,
        sessionHealthRepository.health,
        sessionHealthRepository.serverVersion,
        updateState,
    ) { settingsWithPermissions, draft, health, serverVersion, update ->
        projectInfoUiState(
            settings = settingsWithPermissions.first,
            permissionStates = settingsWithPermissions.second,
            draft = draft,
            health = health,
            serverVersion = serverVersion,
            update = update,
        )
    }.stateIn(
        scope = viewModelScope,
        started = SharingStarted.Eagerly,
        initialValue = projectInfoUiState(
            settings = initialSettings,
            permissionStates = permissionStateSource.states.value,
            draft = backendDraft.value,
            health = sessionHealthRepository.health.value,
            serverVersion = sessionHealthRepository.serverVersion.value,
            update = updateState.value,
        ),
    )

    fun setSetting(key: InfoSettingKey, enabled: Boolean) {
        settingsStore.set(key, enabled)
        if (key == InfoSettingKey.SENSOR_BACKEND && !enabled) {
            sessionHealthRepository.invalidate()
        }
    }

    fun beginPermissionEnable(key: InfoSettingKey) {
        requireNotNull(permissionFeatureForSetting(key)) {
            "$key is not backed by a contextual permission"
        }
        persistPendingPermission(PendingInfoPermissionSetting(key, requestLaunched = false))
    }

    fun markPermissionRequestLaunched() {
        val pending = _pendingPermissionSetting.value ?: return
        persistPendingPermission(pending.copy(requestLaunched = true))
    }

    fun cancelPendingPermission() {
        persistPendingPermission(null)
    }

    fun resolvePendingPermission(
        feature: AppFeature,
        state: PermissionUiState,
    ) {
        val pending = _pendingPermissionSetting.value ?: return
        if (permissionFeatureForSetting(pending.key) != feature) return
        if (state.isUsable()) settingsStore.set(pending.key, true)
        persistPendingPermission(null)
    }

    fun editBackendUrl(raw: String) {
        backendDraft.update { draft ->
            draft.copy(text = raw, error = raw.validationError())
        }
    }

    fun saveBackendUrl() {
        val endpoint = BackendEndpoint.parse(backendDraft.value.text).getOrElse {
            backendDraft.update { it.copy(error = BACKEND_URL_ERROR) }
            return
        }
        val oldEndpoint = BackendEndpoint.parse(settingsStore.settings.value.backendUrl).getOrNull()
        settingsStore.saveBackendEndpoint(endpoint)
        backendDraft.value = BackendDraft(
            text = endpoint.baseUrl,
            persistedText = endpoint.baseUrl,
            error = null,
        )
        if (oldEndpoint != endpoint) {
            sessionHealthRepository.invalidate()
        }
    }

    fun testConnection() {
        val settings = settingsStore.settings.value
        val endpoint = BackendEndpoint.parse(settings.backendUrl).getOrElse {
            backendDraft.update { it.copy(error = BACKEND_URL_ERROR) }
            sessionHealthRepository.invalidate()
            return
        }
        if (!settings.sensorBackendEnabled) {
            sessionHealthRepository.invalidate()
            return
        }
        backendDraft.update { it.copy(error = null) }
        sessionHealthRepository.check(endpoint, enabled = true)
    }

    fun refreshCalibrationAvailability() = testConnection()

    fun checkForUpdates() {
        updateJob?.cancel()
        val generation = updateGeneration.incrementAndGet()
        updateState.value = UpdateUiState.Checking
        updateJob = viewModelScope.launch {
            val result = try {
                appUpdateRepository.latest()
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (failure: Exception) {
                Result.failure(failure)
            }
            currentCoroutineContext().ensureActive()
            if (updateGeneration.get() != generation) return@launch
            updateState.value = result.fold(
                onSuccess = { metadata ->
                    if (isUpdateAvailable(installedVersion, metadata.version)) {
                        UpdateUiState.Available(metadata)
                    } else {
                        UpdateUiState.UpToDate(installedVersion)
                    }
                },
                onFailure = { UpdateUiState.Failed("Could not check for updates") },
            )
        }
    }

    fun checkForUpdatesIfIdle() {
        if (updateState.value == UpdateUiState.Idle) checkForUpdates()
    }

    // Compatibility wrappers for the previous AboutScreen while its UI is replaced.
    fun setAdsbEnabled(enabled: Boolean) = setSetting(InfoSettingKey.ADS_B, enabled)
    fun setBleRidEnabled(enabled: Boolean) = setSetting(InfoSettingKey.BLE_REMOTE_ID, enabled)
    fun setWifiEnabled(enabled: Boolean) = setSetting(InfoSettingKey.WIFI_REMOTE_ID, enabled)
    fun setPrivacyEnabled(enabled: Boolean) = setSetting(InfoSettingKey.PHONE_PRIVACY_SCAN, enabled)
    fun setStalkerEnabled(enabled: Boolean) = setSetting(InfoSettingKey.STALKER, enabled)
    fun setUltrasonicEnabled(enabled: Boolean) = setSetting(InfoSettingKey.ULTRASONIC, enabled)
    fun setWifiAnomalyEnabled(enabled: Boolean) = setSetting(InfoSettingKey.WIFI_ANOMALY, enabled)
    fun setPrivacyNotificationsEnabled(enabled: Boolean) =
        setSetting(InfoSettingKey.PRIVACY_ALERTS, enabled)
    fun setDroneAlertsEnabled(enabled: Boolean) = setSetting(InfoSettingKey.DRONE_ALERTS, enabled)
    fun setHelicopterAlertsEnabled(enabled: Boolean) =
        setSetting(InfoSettingKey.HELICOPTER_ALERTS, enabled)
    fun setMilitaryAlertsEnabled(enabled: Boolean) =
        setSetting(InfoSettingKey.MILITARY_ALERTS, enabled)
    fun setPoliceAlertsEnabled(enabled: Boolean) = setSetting(InfoSettingKey.POLICE_ALERTS, enabled)
    fun setSensorBackendEnabled(enabled: Boolean) =
        setSetting(InfoSettingKey.SENSOR_BACKEND, enabled)
    fun setBackendOnlyMode(enabled: Boolean) = setSetting(InfoSettingKey.BACKEND_ONLY, enabled)
    fun setBackendUrl(raw: String): Result<BackendEndpoint> = BackendEndpoint.parse(raw).also { result ->
        editBackendUrl(raw)
        if (result.isSuccess) saveBackendUrl()
    }

    private fun reconcileExistingSession(settings: DetectionSettings) {
        val endpoint = BackendEndpoint.parse(settings.backendUrl).getOrNull()
        val healthEndpoint = sessionHealthRepository.health.value.endpointOrNull()
        if (!settings.sensorBackendEnabled || endpoint == null ||
            (healthEndpoint != null && healthEndpoint != endpoint)
        ) {
            sessionHealthRepository.invalidate()
        }
    }

    private fun readPendingPermissionSetting(): PendingInfoPermissionSetting? {
        val name = savedStateHandle.get<String>(PENDING_PERMISSION_SETTING_KEY) ?: return null
        val key = InfoSettingKey.entries.firstOrNull { it.name == name } ?: return null
        return PendingInfoPermissionSetting(
            key = key,
            requestLaunched = savedStateHandle[PENDING_PERMISSION_LAUNCHED_KEY] ?: false,
        )
    }

    private fun persistPendingPermission(pending: PendingInfoPermissionSetting?) {
        savedStateHandle[PENDING_PERMISSION_SETTING_KEY] = pending?.key?.name
        savedStateHandle[PENDING_PERMISSION_LAUNCHED_KEY] = pending?.requestLaunched ?: false
        _pendingPermissionSetting.value = pending
    }

    private fun projectInfoUiState(
        settings: DetectionSettings,
        permissionStates: Map<AppFeature, PermissionUiState>,
        draft: BackendDraft,
        health: SessionHealth,
        serverVersion: String?,
        update: UpdateUiState,
    ): InfoUiState {
        val configuredEndpoint = BackendEndpoint.parse(settings.backendUrl).getOrNull()
        val connection = connectionState(
            backendEnabled = settings.sensorBackendEnabled,
            configuredEndpoint = configuredEndpoint,
            health = health,
            serverVersion = serverVersion,
        )
        return InfoUiState(
            settings = settings,
            permissionStates = permissionStates,
            sourceStatus = sourceStatus(
                settings,
                configuredEndpoint,
                health,
                permissionStates,
            ),
            backendUrlDraft = draft.text,
            backendUrlError = draft.error,
            backendUrlCanSave = BackendEndpoint.parse(draft.text).isSuccess,
            backendUrlCanTest = settings.sensorBackendEnabled &&
                configuredEndpoint != null && !draft.dirty,
            connection = connection,
            sessionHealth = health,
            calibrationEntryAvailable = configuredEndpoint?.let { endpoint ->
                calibrationEntryAvailable(settings.sensorBackendEnabled, endpoint, health)
            } ?: false,
            installedVersion = installedVersion,
            updateState = update,
        )
    }
}

private const val PENDING_PERMISSION_SETTING_KEY = "pending_info_permission_setting"
private const val PENDING_PERMISSION_LAUNCHED_KEY = "pending_info_permission_launched"

private fun String.validationError(): String? =
    if (BackendEndpoint.parse(this).isSuccess) null else BACKEND_URL_ERROR

private fun DetectionSettings.backendConfig(): Pair<Boolean, BackendEndpoint?> =
    sensorBackendEnabled to BackendEndpoint.parse(backendUrl).getOrNull()

private fun SessionHealth.endpointOrNull(): BackendEndpoint? = when (this) {
    SessionHealth.Untested -> null
    is SessionHealth.Checking -> endpoint
    is SessionHealth.Healthy -> endpoint
    is SessionHealth.Failed -> endpoint
}

private fun connectionState(
    backendEnabled: Boolean,
    configuredEndpoint: BackendEndpoint?,
    health: SessionHealth,
    serverVersion: String?,
): ConnectionTestState {
    if (!backendEnabled || configuredEndpoint == null) return ConnectionTestState.Idle
    return when (health) {
        SessionHealth.Untested -> ConnectionTestState.Idle
        is SessionHealth.Checking -> if (health.endpoint == configuredEndpoint) {
            ConnectionTestState.Checking(configuredEndpoint)
        } else {
            ConnectionTestState.Idle
        }
        is SessionHealth.Healthy -> if (health.endpoint == configuredEndpoint) {
            ConnectionTestState.Connected(configuredEndpoint, serverVersion)
        } else {
            ConnectionTestState.Idle
        }
        is SessionHealth.Failed -> if (health.endpoint == configuredEndpoint) {
            ConnectionTestState.Failed(configuredEndpoint, "Connection failed")
        } else {
            ConnectionTestState.Idle
        }
    }
}

internal fun sourceStatus(
    settings: DetectionSettings,
    endpoint: BackendEndpoint?,
    health: SessionHealth,
    permissionStates: Map<AppFeature, PermissionUiState>,
): List<InfoSourceStatus> = listOf(
    neutralSource(
        InfoSourceKey.ADS_B,
        "ADS-B",
        settings.adsbEnabled,
        "Aircraft data source",
    ),
    permissionAwareSource(
        InfoSourceKey.BLE_REMOTE_ID,
        "BLE Remote ID",
        settings.bleRidEnabled,
        permissionStates[AppFeature.LOCAL_RADIO_DISCOVERY] ?: PermissionUiState.Loading,
        "Bluetooth and nearby Wi-Fi access",
    ),
    permissionAwareSource(
        InfoSourceKey.WIFI_REMOTE_ID,
        "Wi-Fi Remote ID",
        settings.wifiEnabled,
        permissionStates[AppFeature.LOCAL_RADIO_DISCOVERY] ?: PermissionUiState.Loading,
        "Nearby Wi-Fi and location-protected scan results",
    ),
    permissionAwareSource(
        InfoSourceKey.PHONE_PRIVACY_SCAN,
        "Phone privacy scan",
        settings.phonePrivacyScanEnabled,
        permissionStates[AppFeature.PHONE_PRIVACY_SCAN] ?: PermissionUiState.Loading,
        "Local BLE and Wi-Fi collectors",
    ),
    permissionAwareSource(
        InfoSourceKey.ULTRASONIC,
        "Ultrasonic",
        settings.ultrasonicEnabled,
        permissionStates[AppFeature.ULTRASONIC] ?: PermissionUiState.Loading,
        "Microphone access for high-frequency sampling",
    ),
    backendSource(settings.sensorBackendEnabled, endpoint, health),
    permissionAwareSource(
        InfoSourceKey.NOTIFICATION_DELIVERY,
        "Notification delivery",
        settings.anyAlertsEnabled(),
        configuredNotificationState(settings, permissionStates),
        "Android permission, global delivery, and each configured alert channel",
    ),
)

internal fun permissionFeatureForSetting(key: InfoSettingKey): AppFeature? = when (key) {
    InfoSettingKey.BLE_REMOTE_ID,
    InfoSettingKey.WIFI_REMOTE_ID,
    -> AppFeature.LOCAL_RADIO_DISCOVERY

    InfoSettingKey.PHONE_PRIVACY_SCAN -> AppFeature.PHONE_PRIVACY_SCAN
    InfoSettingKey.ULTRASONIC -> AppFeature.ULTRASONIC
    InfoSettingKey.PRIVACY_ALERTS -> AppFeature.PRIVACY_ALERTS
    InfoSettingKey.DRONE_ALERTS,
    InfoSettingKey.HELICOPTER_ALERTS,
    InfoSettingKey.MILITARY_ALERTS,
    InfoSettingKey.POLICE_ALERTS,
    -> AppFeature.SKY_ALERTS

    InfoSettingKey.ADS_B,
    InfoSettingKey.STALKER,
    InfoSettingKey.WIFI_ANOMALY,
    InfoSettingKey.SENSOR_BACKEND,
    InfoSettingKey.BACKEND_ONLY,
    -> null
}

internal fun isInfoSettingInteractive(
    key: InfoSettingKey,
    settings: DetectionSettings,
    phonePrivacyPermission: PermissionUiState,
): Boolean = when (key) {
    InfoSettingKey.STALKER,
    InfoSettingKey.WIFI_ANOMALY,
    -> settings.phonePrivacyScanEnabled &&
        !settings.backendOnlyMode &&
        phonePrivacyPermission.isUsable()

    else -> true
}

internal fun infoSettingDisabledReason(
    key: InfoSettingKey,
    settings: DetectionSettings,
    phonePrivacyPermission: PermissionUiState,
): String? {
    if (key == InfoSettingKey.BACKEND_ONLY && !settings.sensorBackendEnabled) {
        return "Enable Sensor backend connection first."
    }
    if (key != InfoSettingKey.STALKER && key != InfoSettingKey.WIFI_ANOMALY) return null
    return when {
        !settings.phonePrivacyScanEnabled || !phonePrivacyPermission.isUsable() ->
            "Requires Phone privacy scan."
        settings.backendOnlyMode -> "Unavailable in backend-only mode."
        else -> null
    }
}

private fun neutralSource(
    key: InfoSourceKey,
    label: String,
    configured: Boolean,
    detail: String,
): InfoSourceStatus = InfoSourceStatus(
    key = key,
    label = label,
    configured = configured,
    effective = null,
    statusText = if (configured) "Configured" else "Off",
    detail = detail,
)

private fun permissionAwareSource(
    key: InfoSourceKey,
    label: String,
    configured: Boolean,
    permissionState: PermissionUiState,
    detail: String,
): InfoSourceStatus = InfoSourceStatus(
    key = key,
    label = label,
    configured = configured,
    effective = when {
        !configured -> null
        permissionState == PermissionUiState.Loading -> null
        else -> permissionState.isUsable()
    },
    statusText = when {
        !configured -> "Off"
        permissionState == PermissionUiState.Loading -> "Checking"
        permissionState.isUsable() -> "Ready"
        else -> "Permission needed"
    },
    detail = detail,
)

private fun DetectionSettings.anyAlertsEnabled(): Boolean =
    privacyNotificationsEnabled || droneAlertsEnabled || helicopterAlertsEnabled ||
        militaryAlertsEnabled || policeAlertsEnabled

private fun configuredNotificationState(
    settings: DetectionSettings,
    permissionStates: Map<AppFeature, PermissionUiState>,
): PermissionUiState {
    val configuredStates = buildList {
        if (settings.privacyNotificationsEnabled) {
            add(permissionStates[AppFeature.PRIVACY_ALERTS] ?: PermissionUiState.Loading)
        }
        if (
            settings.droneAlertsEnabled || settings.helicopterAlertsEnabled ||
            settings.militaryAlertsEnabled || settings.policeAlertsEnabled
        ) {
            add(permissionStates[AppFeature.SKY_ALERTS] ?: PermissionUiState.Loading)
        }
    }
    return configuredStates.firstOrNull { !it.isUsable() }
        ?: configuredStates.firstOrNull()
        ?: PermissionUiState.Granted
}

private fun backendSource(
    configured: Boolean,
    endpoint: BackendEndpoint?,
    health: SessionHealth,
): InfoSourceStatus {
    val matchingHealth = endpoint != null && health.endpointOrNull() == endpoint
    val status = when {
        !configured -> "Off"
        endpoint == null -> "Needs setup"
        !matchingHealth -> "Configured"
        health is SessionHealth.Checking -> "Checking"
        health is SessionHealth.Healthy -> "Connected"
        health is SessionHealth.Failed -> "Connection failed"
        else -> "Configured"
    }
    return InfoSourceStatus(
        key = InfoSourceKey.SENSOR_BACKEND,
        label = "Configured backend",
        configured = configured,
        effective = when {
            matchingHealth && health is SessionHealth.Healthy -> true
            matchingHealth && health is SessionHealth.Failed -> false
            else -> null
        },
        statusText = status,
        detail = endpoint?.baseUrl ?: "Enter a complete backend URL",
    )
}
