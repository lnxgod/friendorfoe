package com.friendorfoe.data

import android.content.Context
import android.content.SharedPreferences
import dagger.hilt.android.qualifiers.ApplicationContext
import javax.inject.Inject
import javax.inject.Singleton

/**
 * SharedPreferences-backed toggles for all detection sources.
 * All default to ON.
 */
@Singleton
class DetectionPrefs @Inject constructor(
    @ApplicationContext context: Context
) {
    private val prefs: SharedPreferences =
        context.getSharedPreferences("fof_settings", Context.MODE_PRIVATE)

    companion object {
        private const val KEY_ADSB = "detection_adsb_enabled"
        private const val KEY_BLE_RID = "detection_ble_rid_enabled"
        private const val KEY_WIFI = "detection_wifi_enabled"
        private const val KEY_PRIVACY = "glasses_detection_enabled"
        private const val KEY_STALKER = "detection_stalker_enabled"
        private const val KEY_ULTRASONIC = "detection_ultrasonic_enabled"
        private const val KEY_WIFI_ANOMALY = "detection_wifi_anomaly_enabled"
        private const val KEY_IGNORED_MACS = "privacy_ignored_macs"
        private const val KEY_SENSOR_BACKEND = "sensor_backend_enabled"
        private const val KEY_BACKEND_URL = "sensor_backend_url"
        private const val KEY_BACKEND_ONLY = "sensor_backend_only_mode"
        private const val DEFAULT_BACKEND_URL = "http://192.168.42.235:8000/"
    }

    var adsbEnabled: Boolean
        get() = prefs.getBoolean(KEY_ADSB, true)
        set(value) = prefs.edit().putBoolean(KEY_ADSB, value).apply()

    var bleRidEnabled: Boolean
        get() = prefs.getBoolean(KEY_BLE_RID, true)
        set(value) = prefs.edit().putBoolean(KEY_BLE_RID, value).apply()

    var wifiEnabled: Boolean
        get() = prefs.getBoolean(KEY_WIFI, true)
        set(value) = prefs.edit().putBoolean(KEY_WIFI, value).apply()

    var privacyEnabled: Boolean
        get() = prefs.getBoolean(KEY_PRIVACY, true)
        set(value) = prefs.edit().putBoolean(KEY_PRIVACY, value).apply()

    var stalkerDetectionEnabled: Boolean
        get() = prefs.getBoolean(KEY_STALKER, true)
        set(value) = prefs.edit().putBoolean(KEY_STALKER, value).apply()

    var ultrasonicEnabled: Boolean
        get() = prefs.getBoolean(KEY_ULTRASONIC, false) // OFF by default — uses microphone
        set(value) = prefs.edit().putBoolean(KEY_ULTRASONIC, value).apply()

    var wifiAnomalyEnabled: Boolean
        get() = prefs.getBoolean(KEY_WIFI_ANOMALY, true)
        set(value) = prefs.edit().putBoolean(KEY_WIFI_ANOMALY, value).apply()

    /** Sensor backend (ESP32 network) — enabled by default */
    var sensorBackendEnabled: Boolean
        get() = prefs.getBoolean(KEY_SENSOR_BACKEND, true)
        set(value) = prefs.edit().putBoolean(KEY_SENSOR_BACKEND, value).apply()

    /** Backend URL — configurable */
    var backendUrl: String
        get() = prefs.getString(KEY_BACKEND_URL, DEFAULT_BACKEND_URL) ?: DEFAULT_BACKEND_URL
        set(value) = prefs.edit().putString(KEY_BACKEND_URL, value).apply()

    /** Backend-only mode — disable all local detection, rely solely on ESP32 sensors */
    var backendOnlyMode: Boolean
        get() = prefs.getBoolean(KEY_BACKEND_ONLY, true)  // ON by default
        set(value) = prefs.edit().putBoolean(KEY_BACKEND_ONLY, value).apply()

    /** MACs the user has dismissed / marked as not a threat. Cached in memory for hot-path BLE checks. */
    @Volatile private var cachedIgnoredMacs: Set<String>? = null

    fun getIgnoredMacs(): Set<String> {
        cachedIgnoredMacs?.let { return it }
        val csv = prefs.getString(KEY_IGNORED_MACS, "") ?: ""
        val set = if (csv.isBlank()) emptySet() else csv.split(",").toSet()
        cachedIgnoredMacs = set
        return set
    }

    fun ignoreMac(mac: String) {
        val current = getIgnoredMacs().toMutableSet()
        current.add(mac)
        prefs.edit().putString(KEY_IGNORED_MACS, current.joinToString(",")).apply()
        cachedIgnoredMacs = current
    }

    fun unignoreMac(mac: String) {
        val current = getIgnoredMacs().toMutableSet()
        current.remove(mac)
        prefs.edit().putString(KEY_IGNORED_MACS, current.joinToString(",")).apply()
        cachedIgnoredMacs = current
    }
}
