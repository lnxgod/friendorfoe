package com.friendorfoe.calibration

import android.util.Log
import com.friendorfoe.data.remote.SensorMapApiService
import com.google.gson.Gson
import com.google.gson.JsonObject
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.RequestBody.Companion.toRequestBody
import java.util.concurrent.TimeUnit
import javax.inject.Inject
import javax.inject.Named
import javax.inject.Singleton

/**
 * Tiny HTTP client for the backend's /detections/calibrate/walk/ endpoints.
 * endpoints. We deliberately bypass Retrofit here so that the base URL
 * + bearer token can change at runtime without rebuilding a Retrofit
 * instance, and so the X-Cal-Token header is set per-request.
 */
@Singleton
class CalibrationApi @Inject constructor(
    @Named("backendClient") sharedBackendClient: OkHttpClient,
    private val sensorMapApiService: SensorMapApiService,
) : CalibrationBackend {

    private val gson = Gson()
    private val client = sharedBackendClient.newBuilder()
        // Calibration keeps the backend connection open a bit longer than
        // the rest of the app because operators will roam between APs.
        .connectTimeout(20, TimeUnit.SECONDS)
        .readTimeout(20, TimeUnit.SECONDS)
        .writeTimeout(20, TimeUnit.SECONDS)
        .callTimeout(30, TimeUnit.SECONDS)
        .retryOnConnectionFailure(true)
        .build()
    private val jsonMedia = "application/json; charset=utf-8".toMediaType()
    companion object {
        const val DEFAULT_TOKEN = "chompchomp"
        private const val TAG = "CalibrationApi"
    }

    private fun buildBase(rawUrl: String): String {
        // Tolerate trailing slash differences; backend uses /detections/...
        return rawUrl.trim().trimEnd('/').let { if (it.startsWith("http")) it else "http://$it" }
    }

    private suspend fun call(
        phase: String,
        method: String,
        base: String,
        path: String,
        token: String?,
        body: Map<String, Any?>?,
    ): Result<JsonObject> = withContext(Dispatchers.IO) {
        runCatching {
            val url = "${buildBase(base)}$path"
            Log.i(TAG, "phase=$phase method=$method url=$url start")
            val req = Request.Builder()
                .url(url)
                .header("Content-Type", "application/json")
            if (!token.isNullOrBlank()) {
                req.header("X-Cal-Token", token)
            }
            when (method) {
                "GET"  -> req.get()
                "POST" -> req.post(gson.toJson(body ?: emptyMap<String, Any>()).toRequestBody(jsonMedia))
                else   -> error("Unsupported method $method")
            }
            client.newCall(req.build()).execute().use { resp ->
                val text = resp.body?.string().orEmpty()
                if (!resp.isSuccessful) {
                    error("HTTP ${resp.code}: ${text.take(200)}")
                }
                Log.i(TAG, "phase=$phase method=$method url=$url success code=${resp.code}")
                gson.fromJson(text, JsonObject::class.java) ?: JsonObject()
            }
        }.onFailure { error ->
            val url = "${buildBase(base)}$path"
            Log.w(
                TAG,
                "phase=$phase method=$method url=$url failure=${error::class.java.simpleName}:${error.message?.take(160)}"
            )
        }
    }

    override suspend fun health(baseUrl: String): Result<JsonObject> =
        runCatching {
            Log.i(TAG, "phase=health method=GET url=${buildBase(baseUrl)}/health start")
            val health = sensorMapApiService.getHealth()
            Log.i(TAG, "phase=health method=GET url=${buildBase(baseUrl)}/health success status=${health.status} version=${health.version}")
            JsonObject().apply {
                addProperty("status", health.status)
                addProperty("version", health.version)
                addProperty("redis", health.redis)
                addProperty("database", health.database)
            }
        }.onFailure { error ->
            Log.w(
                TAG,
                "phase=health method=GET url=${buildBase(baseUrl)}/health failure=${error::class.java.simpleName}:${error.message?.take(160)}"
            )
        }

    override suspend fun walkStart(baseUrl: String, token: String,
                                   operatorLabel: String,
                                   txPowerDbm: Double?): Result<JsonObject> {
        val body = mutableMapOf<String, Any?>("operator_label" to operatorLabel)
        if (txPowerDbm != null) body["tx_power_dbm"] = txPowerDbm
        return call("walk_start", "POST", baseUrl, "/detections/calibrate/walk/start", token, body)
    }

    override suspend fun walkSample(baseUrl: String, token: String,
                                    sessionId: String, lat: Double, lon: Double,
                                    tsMs: Long, accuracyM: Float?): Result<JsonObject> =
        call("walk_sample", "POST", baseUrl, "/detections/calibrate/walk/sample", token, mapOf(
            "session_id" to sessionId,
            "lat" to lat,
            "lon" to lon,
            "ts_ms" to tsMs,
            "accuracy_m" to accuracyM,
        ))

    override suspend fun walkFeedback(baseUrl: String, token: String,
                                      sessionId: String): Result<JsonObject> =
        call("walk_feedback", "GET", baseUrl, "/detections/calibrate/walk/feedback?session_id=$sessionId&window_s=10",
             token, null)

    override suspend fun walkEnd(baseUrl: String, token: String,
                                 sessionId: String,
                                 provisionalFit: JsonObject?,
                                 applyRequested: Boolean): Result<JsonObject> =
        call("walk_end", "POST", baseUrl, "/detections/calibrate/walk/end", token, mapOf(
            "session_id" to sessionId,
            "provisional_fit" to provisionalFit,
            "apply_requested" to applyRequested,
        ))

    override suspend fun walkAbort(
        baseUrl: String,
        token: String,
        sessionId: String,
        reason: String,
    ): Result<JsonObject> =
        call("walk_abort", "POST", baseUrl, "/detections/calibrate/walk/abort", token, mapOf(
            "session_id" to sessionId,
            "reason" to reason,
        ))

    /** Returns the fleet sensor list — id, label, lat, lon, online age.
     *  Drives the "tap which sensor you're at" cards on the calibration
     *  screen. Doubles as a connectivity preflight for the operator. */
    override suspend fun walkSensors(baseUrl: String, token: String): Result<JsonObject> =
        call("walk_sensors", "GET", baseUrl, "/detections/calibrate/walk/sensors", token, null)

    /** Real-time "where does the fleet think I am vs my GPS" snapshot —
     *  drives the Calibrate screen's convergence card + "OK to move"
     *  indicator. Backend uses the session's tracking_id (FP:CAL-...)
     *  so the triangulator gets a clean lock on the phone's BLE beacon. */
    override suspend fun walkMyPosition(baseUrl: String, token: String,
                                        sessionId: String): Result<JsonObject> =
        call("walk_my_position", "GET", baseUrl, "/detections/calibrate/walk/my-position?session_id=$sessionId",
             token, null)

    /** Operator stood next to a sensor and tapped its "I'm here" button.
     *  Backend pins the OLS RSSI_REF anchor and returns a sanity result
     *  the UI shows as a green / yellow / red badge for that sensor. */
    override suspend fun walkCheckpoint(baseUrl: String, token: String,
                                        sessionId: String, sensorId: String,
                                        lat: Double, lon: Double,
                                        accuracyM: Float?, tsMs: Long,
                                        anchorSource: String): Result<JsonObject> =
        call("walk_checkpoint", "POST", baseUrl, "/detections/calibrate/walk/checkpoint", token, mapOf(
            "session_id" to sessionId,
            "sensor_id" to sensorId,
            "lat" to lat,
            "lon" to lon,
            "accuracy_m" to accuracyM,
            "ts_ms" to tsMs,
            "anchor_source" to anchorSource,
        ))
}
