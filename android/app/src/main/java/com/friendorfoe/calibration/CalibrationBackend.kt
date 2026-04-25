package com.friendorfoe.calibration

import com.google.gson.JsonObject

interface CalibrationBackend {
    suspend fun health(baseUrl: String): Result<JsonObject>
    suspend fun walkStart(
        baseUrl: String,
        token: String,
        operatorLabel: String,
        txPowerDbm: Double? = null,
    ): Result<JsonObject>
    suspend fun walkSample(
        baseUrl: String,
        token: String,
        sessionId: String,
        lat: Double,
        lon: Double,
        tsMs: Long,
        accuracyM: Float?,
    ): Result<JsonObject>
    suspend fun walkFeedback(baseUrl: String, token: String, sessionId: String): Result<JsonObject>
    suspend fun walkEnd(
        baseUrl: String,
        token: String,
        sessionId: String,
        provisionalFit: JsonObject?,
        applyRequested: Boolean = true,
    ): Result<JsonObject>
    suspend fun walkAbort(
        baseUrl: String,
        token: String,
        sessionId: String,
        reason: String,
    ): Result<JsonObject>
    suspend fun walkSensors(baseUrl: String, token: String): Result<JsonObject>
    suspend fun walkMyPosition(baseUrl: String, token: String, sessionId: String): Result<JsonObject>
    suspend fun walkCheckpoint(
        baseUrl: String,
        token: String,
        sessionId: String,
        sensorId: String,
        lat: Double,
        lon: Double,
        accuracyM: Float?,
        tsMs: Long,
        anchorSource: String = "phone_gps",
    ): Result<JsonObject>
}
