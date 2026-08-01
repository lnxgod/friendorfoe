package com.friendorfoe.data

import android.content.Context
import android.content.SharedPreferences
import com.friendorfoe.calibration.CalibrationSettingsStore
import dagger.hilt.android.qualifiers.ApplicationContext
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import javax.inject.Inject
import javax.inject.Singleton

data class DetectionSettings(
    val adsbEnabled: Boolean,
    val bleRidEnabled: Boolean,
    val wifiEnabled: Boolean,
    val phonePrivacyScanEnabled: Boolean,
    val stalkerEnabled: Boolean,
    val ultrasonicEnabled: Boolean,
    val wifiAnomalyEnabled: Boolean,
    val privacyNotificationsEnabled: Boolean,
    val droneAlertsEnabled: Boolean,
    val helicopterAlertsEnabled: Boolean,
    val militaryAlertsEnabled: Boolean,
    val policeAlertsEnabled: Boolean,
    val sensorBackendEnabled: Boolean,
    val backendOnlyMode: Boolean,
    val backendUrl: String,
) {
    companion object {
        fun defaults() = DetectionSettings(
            adsbEnabled = true,
            bleRidEnabled = true,
            wifiEnabled = true,
            phonePrivacyScanEnabled = false,
            stalkerEnabled = true,
            ultrasonicEnabled = false,
            wifiAnomalyEnabled = true,
            privacyNotificationsEnabled = false,
            droneAlertsEnabled = false,
            helicopterAlertsEnabled = false,
            militaryAlertsEnabled = false,
            policeAlertsEnabled = false,
            sensorBackendEnabled = true,
            backendOnlyMode = false,
            backendUrl = "http://fof-server.local:8000/",
        )
    }
}

/**
 * SharedPreferences-backed toggles for all detection sources.
 */
@Singleton
class DetectionPrefs @Inject constructor(
    @ApplicationContext context: Context
) : CalibrationSettingsStore {
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
        private const val KEY_PRIVACY_NOTIFICATIONS = "privacy_notifications_enabled"
        private const val KEY_DRONE_ALERTS = "alert_drones_enabled"
        private const val KEY_HELICOPTER_ALERTS = "alert_helicopters_enabled"
        private const val KEY_MILITARY_ALERTS = "alert_military_enabled"
        private const val KEY_POLICE_ALERTS = "alert_police_enabled"
        private const val KEY_IGNORED_MACS = "privacy_ignored_macs"
        private const val KEY_IGNORED_IDENTITIES = "privacy_ignored_identities_v2"
        private const val KEY_SENSOR_BACKEND = "sensor_backend_enabled"
        private const val KEY_BACKEND_URL = "sensor_backend_url"
        private const val KEY_BACKEND_ONLY = "sensor_backend_only_mode"
        private const val KEY_CAL_TOKEN = "fof_calibration_token"
        private const val KEY_OPERATOR_LABEL = "fof_calibration_operator"
        private const val DEFAULT_BACKEND_URL = "http://fof-server.local:8000/"
    }

    private val _settings = MutableStateFlow(snapshot())
    val settings: StateFlow<DetectionSettings> = _settings.asStateFlow()

    private val listener = SharedPreferences.OnSharedPreferenceChangeListener { _, _ ->
        _settings.value = snapshot()
    }

    init {
        prefs.registerOnSharedPreferenceChangeListener(listener)
    }

    private fun snapshot() = DetectionSettings(
        adsbEnabled = adsbEnabled,
        bleRidEnabled = bleRidEnabled,
        wifiEnabled = wifiEnabled,
        phonePrivacyScanEnabled = privacyEnabled,
        stalkerEnabled = stalkerDetectionEnabled,
        ultrasonicEnabled = ultrasonicEnabled,
        wifiAnomalyEnabled = wifiAnomalyEnabled,
        privacyNotificationsEnabled = privacyNotificationsEnabled,
        droneAlertsEnabled = droneAlertsEnabled,
        helicopterAlertsEnabled = helicopterAlertsEnabled,
        militaryAlertsEnabled = militaryAlertsEnabled,
        policeAlertsEnabled = policeAlertsEnabled,
        sensorBackendEnabled = sensorBackendEnabled,
        backendOnlyMode = backendOnlyMode,
        backendUrl = backendUrl,
    )

    private val ignoredIdentityStore = IgnoredIdentityStore(
        readStructured = {
            prefs.getStringSet(KEY_IGNORED_IDENTITIES, null)?.toSet()
        },
        readLegacyCsv = {
            prefs.getString(KEY_IGNORED_MACS, null)
        },
        persistStructured = { identities ->
            prefs.edit()
                .putStringSet(KEY_IGNORED_IDENTITIES, identities.toSet())
                .remove(KEY_IGNORED_MACS)
                .apply()
        },
    )

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
        get() = prefs.getBoolean(KEY_PRIVACY, false)
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

    var privacyNotificationsEnabled: Boolean
        get() = prefs.getBoolean(KEY_PRIVACY_NOTIFICATIONS, false)
        set(value) = prefs.edit().putBoolean(KEY_PRIVACY_NOTIFICATIONS, value).apply()

    var droneAlertsEnabled: Boolean
        get() = prefs.getBoolean(KEY_DRONE_ALERTS, false)
        set(value) = prefs.edit().putBoolean(KEY_DRONE_ALERTS, value).apply()

    var helicopterAlertsEnabled: Boolean
        get() = prefs.getBoolean(KEY_HELICOPTER_ALERTS, false)
        set(value) = prefs.edit().putBoolean(KEY_HELICOPTER_ALERTS, value).apply()

    var militaryAlertsEnabled: Boolean
        get() = prefs.getBoolean(KEY_MILITARY_ALERTS, false)
        set(value) = prefs.edit().putBoolean(KEY_MILITARY_ALERTS, value).apply()

    var policeAlertsEnabled: Boolean
        get() = prefs.getBoolean(KEY_POLICE_ALERTS, false)
        set(value) = prefs.edit().putBoolean(KEY_POLICE_ALERTS, value).apply()

    /** Sensor backend (ESP32 network) — enabled by default */
    var sensorBackendEnabled: Boolean
        get() = prefs.getBoolean(KEY_SENSOR_BACKEND, true)
        set(value) = prefs.edit().putBoolean(KEY_SENSOR_BACKEND, value).apply()

    /** Backend URL — configurable */
    override var backendUrl: String
        get() = prefs.getString(KEY_BACKEND_URL, DEFAULT_BACKEND_URL) ?: DEFAULT_BACKEND_URL
        set(value) = prefs.edit().putString(KEY_BACKEND_URL, value).apply()

    /** Backend-only mode — phone sensors off; ESP32/API/badge feeds remain available. */
    var backendOnlyMode: Boolean
        get() = prefs.getBoolean(KEY_BACKEND_ONLY, false)
        set(value) = prefs.edit().putBoolean(KEY_BACKEND_ONLY, value).apply()

    /** Bearer token for the calibration walk endpoints.
     *  Default matches the backend's `_DEV_DEFAULT_CAL_TOKEN` so a
     *  fresh install + fresh backend Just Work. Operator can overwrite
     *  via the Calibrate screen if they've pinned FOF_CAL_TOKEN in prod. */
    override var calibrationToken: String
        get() = prefs.getString(KEY_CAL_TOKEN, "chompchomp") ?: "chompchomp"
        set(value) = prefs.edit().putString(KEY_CAL_TOKEN, value).apply()

    /** Display name shown to operators reviewing calibration history. */
    override var operatorLabel: String
        get() = prefs.getString(KEY_OPERATOR_LABEL, "") ?: ""
        set(value) = prefs.edit().putString(KEY_OPERATOR_LABEL, value).apply()

    /** Canonical privacy identities dismissed by the user. */
    fun getIgnoredIdentities(): Set<String> = ignoredIdentityStore.get()

    /** Retained for callers that predate stable fingerprints. */
    fun getIgnoredMacs(): Set<String> = getIgnoredIdentities()

    fun ignoreIdentities(identities: Iterable<String>) {
        ignoredIdentityStore.add(identities)
    }

    fun ignoreMac(mac: String) {
        ignoreIdentities(setOf(mac))
    }

    fun unignoreIdentities(identities: Iterable<String>) {
        ignoredIdentityStore.remove(identities)
    }

    fun unignoreMac(mac: String) {
        unignoreIdentities(setOf(mac))
    }
}
