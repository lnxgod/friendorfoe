package com.friendorfoe.data

import android.content.Context
import androidx.test.core.app.ApplicationProvider
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeout
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class DetectionPrefsObservableTest {
    @Test
    fun publicWritesPublishNewSnapshotsFromRealSharedPreferences() = runBlocking {
        val context = ApplicationProvider.getApplicationContext<Context>()
        val sharedPreferences = context.getSharedPreferences("fof_settings", Context.MODE_PRIVATE)
        val keys = listOf(
            "glasses_detection_enabled",
            "sensor_backend_url",
            "alert_drones_enabled",
            "sensor_backend_enabled",
            "sensor_backend_only_mode",
        )
        val originalValues = keys.associateWith { key -> sharedPreferences.all[key] }
        val originallyPresent = keys.associateWith(sharedPreferences::contains)

        try {
            sharedPreferences.edit()
                .putBoolean("sensor_backend_enabled", false)
                .putBoolean("sensor_backend_only_mode", true)
                .commit()

            val prefs = DetectionPrefs(context)
            assertFalse(prefs.settings.value.sensorBackendEnabled)
            assertFalse(prefs.settings.value.backendOnlyMode)
            assertFalse(sharedPreferences.getBoolean("sensor_backend_only_mode", true))

            val privacyValue = !prefs.settings.value.phonePrivacyScanEnabled
            prefs.privacyEnabled = privacyValue
            assertEquals(
                privacyValue,
                withTimeout(2_000) {
                    prefs.settings.first { it.phonePrivacyScanEnabled == privacyValue }
                }.phonePrivacyScanEnabled,
            )

            val backendValue = "https://observable.example:8443/"
            prefs.backendUrl = backendValue
            assertEquals(
                backendValue,
                withTimeout(2_000) {
                    prefs.settings.first { it.backendUrl == backendValue }
                }.backendUrl,
            )

            val alertValue = !prefs.settings.value.droneAlertsEnabled
            prefs.droneAlertsEnabled = alertValue
            assertEquals(
                alertValue,
                withTimeout(2_000) {
                    prefs.settings.first { it.droneAlertsEnabled == alertValue }
                }.droneAlertsEnabled,
            )

            prefs.sensorBackendEnabled = true
            assertTrue(
                withTimeout(2_000) {
                    prefs.settings.first { it.sensorBackendEnabled }
                }.sensorBackendEnabled,
            )

            prefs.backendOnlyMode = true
            assertTrue(sharedPreferences.getBoolean("sensor_backend_only_mode", false))
            assertTrue(
                withTimeout(2_000) {
                    prefs.settings.first { it.backendOnlyMode }
                }.backendOnlyMode,
            )

            prefs.sensorBackendEnabled = false
            withTimeout(2_000) {
                prefs.settings.first {
                    !it.sensorBackendEnabled && !it.backendOnlyMode
                }
            }.also { snapshot ->
                assertFalse(snapshot.sensorBackendEnabled)
                assertFalse(snapshot.backendOnlyMode)
            }
            assertFalse(sharedPreferences.getBoolean("sensor_backend_only_mode", true))
        } finally {
            val editor = sharedPreferences.edit()
            keys.forEach { key ->
                if (originallyPresent.getValue(key)) {
                    when (val value = originalValues[key]) {
                        is Boolean -> editor.putBoolean(key, value)
                        is String -> editor.putString(key, value)
                        else -> error("Unsupported original preference value for $key")
                    }
                } else {
                    editor.remove(key)
                }
            }
            editor.commit()
        }
    }
}
