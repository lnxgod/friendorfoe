package com.friendorfoe.calibration

import com.friendorfoe.test.MainDispatcherRule
import com.google.gson.Gson
import com.google.gson.JsonObject
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.test.advanceUntilIdle
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test

@OptIn(ExperimentalCoroutinesApi::class)
class CalibrationViewModelTest {

    @get:Rule
    val mainDispatcherRule = MainDispatcherRule()

    @Test
    fun refreshConnectivity_withHealthyBackendAndSensors_marksPreflightReady() = runTest {
        val backend = FakeCalibrationBackend(
            healthResult = successJson("""{"status":"ok","version":"0.63.8-calibrate-runtime"}"""),
            walkSensorsResult = successJson(
                """
                {"sensors":[
                  {"device_id":"gate","name":"Gate","lat":37.0,"lon":-122.0,"online":true,"age_s":1.0}
                ]}
                """.trimIndent()
            ),
        )
        val platform = FakeCalibrationPlatform(
            networkSnapshot = CalibrationNetworkSnapshot(
                transport = NetworkTransportState.Wifi,
                ssid = "fof-mesh",
            )
        )
        val viewModel = CalibrationViewModel(
            prefs = FakeCalibrationSettingsStore(),
            advertiser = FakeCalibrationAdvertiser(),
            api = backend,
            platform = platform,
        )

        viewModel.refreshConnectivity()
        advanceUntilIdle()

        val state = viewModel.state.value
        assertEquals(BackendReachabilityState.Reachable, state.backendReachability)
        assertEquals(CalibrationAuthState.Valid, state.calibrationAuthState)
        assertEquals(SensorLoadState.Ready, state.sensorLoadState)
        assertEquals("0.63.8-calibrate-runtime", state.backendVersion)
        assertTrue(state.preflightReady)
        assertNull(state.errorMessage)
    }

    @Test
    fun refreshConnectivity_withHealthyBackendAnd401_marksAuthFailed() = runTest {
        val backend = FakeCalibrationBackend(
            healthResult = successJson("""{"status":"ok","version":"0.63.6"}"""),
            walkSensorsResult = Result.failure(IllegalStateException("HTTP 401: no token")),
        )
        val viewModel = CalibrationViewModel(
            prefs = FakeCalibrationSettingsStore(calibrationToken = CalibrationApi.DEFAULT_TOKEN),
            advertiser = FakeCalibrationAdvertiser(),
            api = backend,
            platform = FakeCalibrationPlatform(),
        )

        viewModel.refreshConnectivity()
        advanceUntilIdle()

        val state = viewModel.state.value
        assertEquals(CalibrationViewModel.BackendStatus.AuthFailed, state.backendStatus)
        assertEquals(BackendReachabilityState.Reachable, state.backendReachability)
        assertEquals(CalibrationAuthState.Invalid, state.calibrationAuthState)
        assertEquals(SensorLoadState.Unknown, state.sensorLoadState)
        assertEquals("Calibration token rejected — update X-Cal-Token.", state.errorMessage)
    }

    @Test
    fun refreshConnectivity_withHealthFailure_marksBackendUnreachable() = runTest {
        val viewModel = CalibrationViewModel(
            prefs = FakeCalibrationSettingsStore(),
            advertiser = FakeCalibrationAdvertiser(),
            api = FakeCalibrationBackend(
                healthResult = Result.failure(IllegalStateException("timeout")),
            ),
            platform = FakeCalibrationPlatform(),
        )

        viewModel.refreshConnectivity()
        advanceUntilIdle()

        val state = viewModel.state.value
        assertEquals(CalibrationViewModel.BackendStatus.Unreachable, state.backendStatus)
        assertEquals(BackendReachabilityState.Unreachable, state.backendReachability)
        assertEquals("Backend unreachable: timeout", state.errorMessage)
        assertTrue(!state.preflightReady)
    }

    @Test
    fun refreshConnectivity_withWifiButHiddenSsid_doesNotRenderNotAssociated() = runTest {
        val viewModel = CalibrationViewModel(
            prefs = FakeCalibrationSettingsStore(),
            advertiser = FakeCalibrationAdvertiser(),
            api = FakeCalibrationBackend(
                healthResult = successJson("""{"status":"ok","version":"0.63.6"}"""),
                walkSensorsResult = successJson("""{"sensors":[{"device_id":"gate","name":"Gate","lat":1.0,"lon":2.0,"online":true}]}"""),
            ),
            platform = FakeCalibrationPlatform(
                networkSnapshot = CalibrationNetworkSnapshot(
                    transport = NetworkTransportState.Wifi,
                    ssid = null,
                )
            ),
        )

        viewModel.refreshConnectivity()
        advanceUntilIdle()

        val state = viewModel.state.value
        assertEquals(NetworkTransportState.Wifi, state.networkTransport)
        assertNull(state.currentSsid)
        assertEquals("WiFi connected (SSID unavailable)", state.ssidDisplay())
    }

    @Test
    fun refreshConnectivity_successAfterFailure_clearsStaleErrorBanner() = runTest {
        val backend = FakeCalibrationBackend(
            healthResult = Result.failure(IllegalStateException("timeout")),
        )
        val viewModel = CalibrationViewModel(
            prefs = FakeCalibrationSettingsStore(),
            advertiser = FakeCalibrationAdvertiser(),
            api = backend,
            platform = FakeCalibrationPlatform(),
        )

        viewModel.refreshConnectivity()
        advanceUntilIdle()
        assertEquals("Backend unreachable: timeout", viewModel.state.value.errorMessage)
        assertEquals(PreflightPhase.Health, viewModel.state.value.lastPreflightFailurePhase)
        assertEquals("timeout", viewModel.state.value.lastPreflightFailureDetail)

        backend.healthResult = successJson("""{"status":"ok","version":"0.63.6"}""")
        backend.walkSensorsResult = successJson("""{"sensors":[{"device_id":"pool","name":"Pool","lat":1.0,"lon":2.0,"online":true}]}""")

        viewModel.refreshConnectivity()
        advanceUntilIdle()

        val state = viewModel.state.value
        assertNull(state.errorMessage)
        assertEquals(SensorLoadState.Ready, state.sensorLoadState)
        assertNull(state.lastPreflightFailurePhase)
        assertNull(state.lastPreflightFailureDetail)
        assertTrue(state.preflightReady)
    }

    private fun successJson(body: String): Result<JsonObject> =
        Result.success(Gson().fromJson(body, JsonObject::class.java))
}

private class FakeCalibrationSettingsStore(
    override var backendUrl: String = "http://fof-server.local:8000/",
    override var calibrationToken: String = CalibrationApi.DEFAULT_TOKEN,
    override var operatorLabel: String = "Test phone",
) : CalibrationSettingsStore

private class FakeCalibrationAdvertiser : CalibrationAdvertiser {
    override fun start(serviceUuid: String, onError: (String) -> Unit): Boolean = true
    override fun stop() = Unit
}

private class FakeCalibrationPlatform(
    private val bluetoothEnabled: Boolean = true,
    var networkSnapshot: CalibrationNetworkSnapshot = CalibrationNetworkSnapshot(),
    private val locationFailure: Exception? = null,
) : CalibrationPlatform {
    override fun isBluetoothEnabled(): Boolean = bluetoothEnabled
    override fun currentNetworkSnapshot(): CalibrationNetworkSnapshot = networkSnapshot
    override fun requestLocationUpdates(listener: android.location.LocationListener) {
        locationFailure?.let { throw it }
    }
    override fun removeLocationUpdates(listener: android.location.LocationListener) = Unit
}

private class FakeCalibrationBackend(
    var healthResult: Result<JsonObject> = Result.success(JsonObject()),
    var walkSensorsResult: Result<JsonObject> = Result.success(JsonObject()),
    private val walkStartResult: Result<JsonObject> = Result.success(JsonObject()),
) : CalibrationBackend {
    override suspend fun health(baseUrl: String): Result<JsonObject> = healthResult
    override suspend fun walkStart(
        baseUrl: String,
        token: String,
        operatorLabel: String,
        txPowerDbm: Double?,
    ): Result<JsonObject> = walkStartResult

    override suspend fun walkSample(
        baseUrl: String,
        token: String,
        sessionId: String,
        lat: Double,
        lon: Double,
        tsMs: Long,
        accuracyM: Float?,
    ): Result<JsonObject> = Result.success(JsonObject())

    override suspend fun walkFeedback(baseUrl: String, token: String, sessionId: String): Result<JsonObject> =
        Result.success(JsonObject())

    override suspend fun walkEnd(baseUrl: String, token: String, sessionId: String): Result<JsonObject> =
        Result.success(JsonObject())

    override suspend fun walkSensors(baseUrl: String, token: String): Result<JsonObject> = walkSensorsResult

    override suspend fun walkMyPosition(baseUrl: String, token: String, sessionId: String): Result<JsonObject> =
        Result.success(JsonObject())

    override suspend fun walkCheckpoint(
        baseUrl: String,
        token: String,
        sessionId: String,
        sensorId: String,
        lat: Double,
        lon: Double,
        accuracyM: Float?,
        tsMs: Long,
    ): Result<JsonObject> = Result.success(JsonObject())
}
