package com.friendorfoe.presentation.about

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.friendorfoe.data.BackendEndpoint
import com.friendorfoe.data.DetectionPrefs
import com.friendorfoe.data.DetectionSettings
import com.friendorfoe.data.remote.SensorMapApiService
import com.friendorfoe.data.repository.SkyObjectRepository
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch
import javax.inject.Inject

data class InfoSettingsUiState(
    val settings: DetectionSettings = DetectionSettings.defaults(),
    val backendValidationError: String? = null,
    val connectionStatus: ConnectionTestState = ConnectionTestState.Idle,
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

internal class BackendConnectionTester(
    private val scope: CoroutineScope,
    private val connectionStatus: MutableStateFlow<ConnectionTestState>,
    private val fetchServerVersion: suspend () -> String?,
) {
    private var job: Job? = null

    fun test(endpoint: BackendEndpoint) {
        job?.cancel()
        connectionStatus.value = ConnectionTestState.Checking(endpoint)
        job = scope.launch {
            try {
                val version = fetchServerVersion()
                currentCoroutineContext().ensureActive()
                connectionStatus.value = ConnectionTestState.Connected(endpoint, version)
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (failure: Exception) {
                currentCoroutineContext().ensureActive()
                connectionStatus.value = ConnectionTestState.Failed(
                    endpoint = endpoint,
                    message = failure.message?.take(80) ?: "Connection failed",
                )
            }
        }
    }

    fun reset() {
        job?.cancel()
        job = null
        connectionStatus.value = ConnectionTestState.Idle
    }
}

@HiltViewModel
class AboutViewModel @Inject constructor(
    private val detectionPrefs: DetectionPrefs,
    private val skyObjectRepository: SkyObjectRepository,
    private val sensorMapApiService: SensorMapApiService
) : ViewModel() {

    private val backendValidationError = MutableStateFlow<String?>(null)
    private val connectionStatus = MutableStateFlow<ConnectionTestState>(ConnectionTestState.Idle)
    private val connectionTester = BackendConnectionTester(
        scope = viewModelScope,
        connectionStatus = connectionStatus,
        fetchServerVersion = { sensorMapApiService.getHealth().version },
    )

    val uiState: StateFlow<InfoSettingsUiState> = combine(
        detectionPrefs.settings,
        backendValidationError,
        connectionStatus,
    ) { settings, error, connection ->
        InfoSettingsUiState(settings, error, connection)
    }.stateIn(
        scope = viewModelScope,
        started = SharingStarted.Eagerly,
        initialValue = InfoSettingsUiState(),
    )

    fun setAdsbEnabled(enabled: Boolean) {
        detectionPrefs.adsbEnabled = enabled
        skyObjectRepository.restartDetectionSources()
    }
    fun setBleRidEnabled(enabled: Boolean) {
        detectionPrefs.bleRidEnabled = enabled
        skyObjectRepository.restartDetectionSources()
    }
    fun setWifiEnabled(enabled: Boolean) {
        detectionPrefs.wifiEnabled = enabled
        skyObjectRepository.restartDetectionSources()
    }
    fun setPrivacyEnabled(enabled: Boolean) {
        skyObjectRepository.setPrivacyDetectionEnabled(enabled)
    }
    fun setStalkerEnabled(enabled: Boolean) {
        detectionPrefs.stalkerDetectionEnabled = enabled
        skyObjectRepository.restartDetectionSources()
    }
    fun setUltrasonicEnabled(enabled: Boolean) {
        detectionPrefs.ultrasonicEnabled = enabled
        skyObjectRepository.restartDetectionSources()
    }
    fun setWifiAnomalyEnabled(enabled: Boolean) { detectionPrefs.wifiAnomalyEnabled = enabled }
    fun setPrivacyNotificationsEnabled(enabled: Boolean) {
        detectionPrefs.privacyNotificationsEnabled = enabled
    }
    fun setDroneAlertsEnabled(enabled: Boolean) { detectionPrefs.droneAlertsEnabled = enabled }
    fun setHelicopterAlertsEnabled(enabled: Boolean) { detectionPrefs.helicopterAlertsEnabled = enabled }
    fun setMilitaryAlertsEnabled(enabled: Boolean) { detectionPrefs.militaryAlertsEnabled = enabled }
    fun setPoliceAlertsEnabled(enabled: Boolean) { detectionPrefs.policeAlertsEnabled = enabled }
    fun setSensorBackendEnabled(enabled: Boolean) { detectionPrefs.sensorBackendEnabled = enabled }
    fun setBackendUrl(raw: String): Result<BackendEndpoint> =
        BackendEndpoint.parse(raw).onSuccess { endpoint ->
            detectionPrefs.backendUrl = endpoint.baseUrl
            backendValidationError.value = null
            connectionTester.reset()
        }.onFailure { failure ->
            backendValidationError.value =
                failure.message ?: "Enter a complete http:// or https:// URL"
        }
    fun setBackendOnlyMode(enabled: Boolean) {
        detectionPrefs.backendOnlyMode = enabled
        skyObjectRepository.restartDetectionSources()
    }

    fun testConnection() {
        connectionTester.reset()
        val endpoint = BackendEndpoint.parse(detectionPrefs.backendUrl).getOrElse { failure ->
            backendValidationError.value =
                failure.message ?: "Enter a complete http:// or https:// URL"
            connectionStatus.value = ConnectionTestState.Idle
            return
        }
        backendValidationError.value = null
        connectionTester.test(endpoint)
    }
}
