package com.friendorfoe.calibration

enum class BackendReachabilityState {
    Unknown,
    Reachable,
    Unreachable,
}

enum class CalibrationAuthState {
    Unknown,
    Valid,
    Invalid,
}

enum class SensorLoadState {
    Unknown,
    Ready,
    Empty,
    Unavailable,
}

enum class PreflightPhase {
    Idle,
    Health,
    Sensors,
    StartWalk,
}

enum class PreflightCheckStatus {
    Ok,
    Pending,
    Error,
}

data class PreflightChecklistItem(
    val label: String,
    val status: PreflightCheckStatus,
    val detail: String,
)

val CalibrationViewModel.State.preflightReady: Boolean
    get() = backendReachability == BackendReachabilityState.Reachable &&
        calibrationAuthState == CalibrationAuthState.Valid &&
        sensorLoadState == SensorLoadState.Ready

private const val ZERO_HEARING_WARNING_AFTER_MS = 15_000L

fun CalibrationViewModel.State.ssidDisplay(): String = when {
    networkTransport == NetworkTransportState.Wifi && !currentSsid.isNullOrBlank() -> currentSsid
    networkTransport == NetworkTransportState.Wifi -> "WiFi connected (SSID unavailable)"
    else -> "not associated"
}

fun CalibrationViewModel.State.preflightChecklist(): List<PreflightChecklistItem> {
    val networkDetail = when (networkTransport) {
        NetworkTransportState.Wifi -> ssidDisplay()
        NetworkTransportState.Other -> "Using non-WiFi transport"
        NetworkTransportState.Offline -> "No active network transport"
    }
    val backendDetail = when (backendReachability) {
        BackendReachabilityState.Reachable -> backendVersion?.let { "Reachable · $it" } ?: "Reachable"
        BackendReachabilityState.Unreachable -> "Backend unreachable"
        BackendReachabilityState.Unknown -> "Run Test connectivity"
    }
    val tokenDetail = when (calibrationAuthState) {
        CalibrationAuthState.Valid -> "Token accepted"
        CalibrationAuthState.Invalid -> "X-Cal-Token rejected"
        CalibrationAuthState.Unknown -> "Token not checked yet"
    }
    val sensorDetail = when (sensorLoadState) {
        SensorLoadState.Ready -> "${availableSensors.size} sensors loaded"
        SensorLoadState.Empty -> "Backend returned no sensors"
        SensorLoadState.Unavailable -> "Sensor list unavailable"
        SensorLoadState.Unknown -> "Sensor list not loaded"
    }
    return listOf(
        PreflightChecklistItem(
            label = "Network",
            status = when (networkTransport) {
                NetworkTransportState.Offline -> PreflightCheckStatus.Error
                else -> PreflightCheckStatus.Ok
            },
            detail = networkDetail,
        ),
        PreflightChecklistItem(
            label = "Backend",
            status = when (backendReachability) {
                BackendReachabilityState.Reachable -> PreflightCheckStatus.Ok
                BackendReachabilityState.Unreachable -> PreflightCheckStatus.Error
                BackendReachabilityState.Unknown -> PreflightCheckStatus.Pending
            },
            detail = backendDetail,
        ),
        PreflightChecklistItem(
            label = "Token",
            status = when (calibrationAuthState) {
                CalibrationAuthState.Valid -> PreflightCheckStatus.Ok
                CalibrationAuthState.Invalid -> PreflightCheckStatus.Error
                CalibrationAuthState.Unknown -> PreflightCheckStatus.Pending
            },
            detail = tokenDetail,
        ),
        PreflightChecklistItem(
            label = "Sensors",
            status = when (sensorLoadState) {
                SensorLoadState.Ready -> PreflightCheckStatus.Ok
                SensorLoadState.Empty, SensorLoadState.Unavailable -> PreflightCheckStatus.Error
                SensorLoadState.Unknown -> PreflightCheckStatus.Pending
            },
            detail = sensorDetail,
        ),
    )
}

fun CalibrationViewModel.State.preflightFailureReason(): String? = when {
    backendReachability == BackendReachabilityState.Unreachable ->
        "Backend unreachable — test connectivity first."
    calibrationAuthState == CalibrationAuthState.Invalid ->
        "Calibration token rejected — update X-Cal-Token."
    sensorLoadState == SensorLoadState.Unavailable ->
        "Sensor list unavailable — retry the connectivity check."
    sensorLoadState == SensorLoadState.Empty ->
        "No sensors loaded from backend."
    !preflightReady ->
        "Run Test connectivity before starting a walk."
    else -> null
}

fun CalibrationViewModel.State.preflightFailureSummary(): String? {
    val phase = lastPreflightFailurePhase ?: return null
    val detail = lastPreflightFailureDetail ?: return null
    return "${phase.name.lowercase()}: $detail"
}

fun CalibrationViewModel.State.zeroHearingWarning(nowMs: Long = System.currentTimeMillis()): String? {
    val startedAt = walkStartedAtMs ?: return null
    if (!isWalking || !beaconOnAir || heardSensorCount > 0) {
        return null
    }
    if ((nowMs - startedAt) < ZERO_HEARING_WARNING_AFTER_MS) {
        return null
    }
    return "Walk session is live and the beacon is on air, but no fleet sensor has heard it yet. Check BLE advertise support/permissions or scanner firmware handling of the calibration UUID."
}
