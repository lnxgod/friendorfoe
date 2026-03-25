package com.friendorfoe.data

import android.content.Context
import android.content.SharedPreferences
import dagger.hilt.android.qualifiers.ApplicationContext
import javax.inject.Inject
import javax.inject.Singleton

/**
 * SharedPreferences-backed toggle for smart glasses / privacy device detection.
 * Off by default — opt-in feature.
 */
@Singleton
class GlassesDetectionPrefs @Inject constructor(
    @ApplicationContext context: Context
) {
    private val prefs: SharedPreferences =
        context.getSharedPreferences("fof_settings", Context.MODE_PRIVATE)

    companion object {
        private const val KEY_ENABLED = "glasses_detection_enabled"
    }

    var isEnabled: Boolean
        get() = prefs.getBoolean(KEY_ENABLED, false)
        set(value) = prefs.edit().putBoolean(KEY_ENABLED, value).apply()
}
