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
}
