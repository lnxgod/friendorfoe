package com.friendorfoe.calibration

import com.google.gson.Gson
import com.google.gson.JsonObject
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.RequestBody.Companion.toRequestBody
import java.util.concurrent.TimeUnit
import javax.inject.Inject
import javax.inject.Singleton

/**
 * Tiny HTTP client for the backend's /detections/calibrate/walk/ endpoints.
 * endpoints. We deliberately bypass Retrofit here so that the base URL
 * + bearer token can change at runtime without rebuilding a Retrofit
 * instance, and so the X-Cal-Token header is set per-request.
 */
@Singleton
class CalibrationApi @Inject constructor() {

    private val gson = Gson()
    private val client = OkHttpClient.Builder()
        .connectTimeout(8, TimeUnit.SECONDS)
        .readTimeout(8, TimeUnit.SECONDS)
        .build()
    private val jsonMedia = "application/json; charset=utf-8".toMediaType()

    private fun buildBase(rawUrl: String): String {
        // Tolerate trailing slash differences; backend uses /detections/...
        return rawUrl.trimEnd('/').let { if (it.startsWith("http")) it else "http://$it" }
    }

    private fun call(method: String, base: String, path: String, token: String,
                     body: Map<String, Any?>?): Result<JsonObject> = runCatching {
        val req = Request.Builder()
            .url("${buildBase(base)}$path")
            .header("X-Cal-Token", token)
            .header("Content-Type", "application/json")
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
            gson.fromJson(text, JsonObject::class.java) ?: JsonObject()
        }
    }

    suspend fun walkStart(baseUrl: String, token: String,
                          operatorLabel: String,
                          txPowerDbm: Double? = null): Result<JsonObject> {
        val body = mutableMapOf<String, Any?>("operator_label" to operatorLabel)
        if (txPowerDbm != null) body["tx_power_dbm"] = txPowerDbm
        return call("POST", baseUrl, "/detections/calibrate/walk/start", token, body)
    }

    suspend fun walkSample(baseUrl: String, token: String,
                           sessionId: String, lat: Double, lon: Double,
                           tsMs: Long, accuracyM: Float?): Result<JsonObject> =
        call("POST", baseUrl, "/detections/calibrate/walk/sample", token, mapOf(
            "session_id" to sessionId,
            "lat" to lat,
            "lon" to lon,
            "ts_ms" to tsMs,
            "accuracy_m" to accuracyM,
        ))

    suspend fun walkFeedback(baseUrl: String, token: String,
                             sessionId: String): Result<JsonObject> =
        call("GET", baseUrl, "/detections/calibrate/walk/feedback?session_id=$sessionId&window_s=10",
             token, null)

    suspend fun walkEnd(baseUrl: String, token: String,
                        sessionId: String): Result<JsonObject> =
        call("POST", baseUrl, "/detections/calibrate/walk/end", token, mapOf(
            "session_id" to sessionId,
        ))

    /** Returns the fleet sensor list — id, label, lat, lon, online age.
     *  Drives the "tap which sensor you're at" cards on the calibration
     *  screen. Doubles as a connectivity preflight for the operator. */
    suspend fun walkSensors(baseUrl: String, token: String): Result<JsonObject> =
        call("GET", baseUrl, "/detections/calibrate/walk/sensors", token, null)

    /** Real-time "where does the fleet think I am vs my GPS" snapshot —
     *  drives the Calibrate screen's convergence card + "OK to move"
     *  indicator. Backend uses the session's tracking_id (FP:CAL-...)
     *  so the triangulator gets a clean lock on the phone's BLE beacon. */
    suspend fun walkMyPosition(baseUrl: String, token: String,
                                sessionId: String): Result<JsonObject> =
        call("GET", baseUrl, "/detections/calibrate/walk/my-position?session_id=$sessionId",
             token, null)

    /** Operator stood next to a sensor and tapped its "I'm here" button.
     *  Backend pins the OLS RSSI_REF anchor and returns a sanity result
     *  the UI shows as a green / yellow / red badge for that sensor. */
    suspend fun walkCheckpoint(baseUrl: String, token: String,
                               sessionId: String, sensorId: String,
                               lat: Double, lon: Double,
                               accuracyM: Float?, tsMs: Long): Result<JsonObject> =
        call("POST", baseUrl, "/detections/calibrate/walk/checkpoint", token, mapOf(
            "session_id" to sessionId,
            "sensor_id" to sensorId,
            "lat" to lat,
            "lon" to lon,
            "accuracy_m" to accuracyM,
            "ts_ms" to tsMs,
        ))
}
