package com.friendorfoe.detection

enum class PrivacyDetectionOrigin { ANDROID, BADGE, BACKEND, WIFI }

enum class BleInvestigationMode { GATT, PASSIVE_CAPTURE }

data class BleInvestigationTarget(
    val mode: BleInvestigationMode,
    val mac: String?,
    val entityKey: String,
    val observedAtElapsedMs: Long,
    val origin: PrivacyDetectionOrigin,
)

internal fun elapsedRealtimeMs(): Long = try {
    android.os.SystemClock.elapsedRealtime()
} catch (_: RuntimeException) {
    System.nanoTime() / 1_000_000L
}
