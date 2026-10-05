package com.friendorfoe.data

import android.content.ContextWrapper
import android.content.SharedPreferences
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class DetectionPrefsTest {
    @Test
    fun newAndExistingPriorityInstallsDefaultToDistanceOrderedGroups() {
        assertTrue(DetectionSettings.defaults().groupAircraftByType)
        assertTrue(DetectionPrefs(TestContext(TestSharedPreferences())).groupAircraftByType)
        assertTrue(DetectionPrefs(TestContext(TestSharedPreferences(mapOf("list_nearest_first" to false)))).groupAircraftByType)
    }

    @Test
    fun explicitLegacyFlatViewIsPreservedUntilUserChoosesGrouping() {
        val storage = TestSharedPreferences(mapOf("list_nearest_first" to true))
        val prefs = DetectionPrefs(TestContext(storage))
        assertFalse(prefs.groupAircraftByType)
        prefs.groupAircraftByType = true
        assertTrue(prefs.settings.value.groupAircraftByType)
        assertTrue(DetectionPrefs(TestContext(storage)).groupAircraftByType)
        prefs.groupAircraftByType = false
        assertFalse(DetectionPrefs(TestContext(storage)).groupAircraftByType)
    }

    @Test
    fun missingPrivacyPreferenceDefaultsToEnabled() {
        val prefs = DetectionPrefs(TestContext(TestSharedPreferences()))

        assertTrue(prefs.settings.value.phonePrivacyScanEnabled)
    }

    @Test
    fun explicitFalsePrivacyPreferenceRemainsDisabled() {
        val preferences = TestSharedPreferences(
            mapOf("glasses_detection_enabled" to false),
        )
        val prefs = DetectionPrefs(TestContext(preferences))

        assertFalse(prefs.settings.value.phonePrivacyScanEnabled)
    }

    @Test
    fun missingBackendPreferenceDefaultsToDisabled() {
        val prefs = DetectionPrefs(TestContext(TestSharedPreferences()))

        assertFalse(prefs.settings.value.sensorBackendEnabled)
    }

    @Test
    fun explicitBackendPreferenceValuesRemainAuthoritative() {
        val enabled = DetectionPrefs(TestContext(TestSharedPreferences(
            mapOf("sensor_backend_enabled" to true),
        )))
        val disabled = DetectionPrefs(TestContext(TestSharedPreferences(
            mapOf("sensor_backend_enabled" to false),
        )))

        assertTrue(enabled.settings.value.sensorBackendEnabled)
        assertFalse(disabled.settings.value.sensorBackendEnabled)
    }

    @Test
    fun settingsStateUpdatesImmediatelyAfterPreferenceWrite() {
        val prefs = DetectionPrefs(TestContext(TestSharedPreferences()))

        prefs.adsbEnabled = false

        assertFalse(prefs.settings.value.adsbEnabled)
    }
}

private class TestContext(
    private val preferences: SharedPreferences,
) : ContextWrapper(null) {
    override fun getSharedPreferences(name: String?, mode: Int): SharedPreferences = preferences
}

private class TestSharedPreferences(
    initialValues: Map<String, Any> = emptyMap(),
) : SharedPreferences {
    private val values = initialValues.toMutableMap()

    override fun getAll(): Map<String, *> = values

    override fun getString(key: String?, defValue: String?): String? =
        values[key] as? String ?: defValue

    override fun getStringSet(key: String?, defValues: Set<String>?): Set<String>? =
        @Suppress("UNCHECKED_CAST")
        ((values[key] as? Set<String>) ?: defValues)

    override fun getInt(key: String?, defValue: Int): Int = values[key] as? Int ?: defValue

    override fun getLong(key: String?, defValue: Long): Long = values[key] as? Long ?: defValue

    override fun getFloat(key: String?, defValue: Float): Float = values[key] as? Float ?: defValue

    override fun getBoolean(key: String?, defValue: Boolean): Boolean =
        values[key] as? Boolean ?: defValue

    override fun contains(key: String?): Boolean = key in values

    override fun edit(): SharedPreferences.Editor = object : SharedPreferences.Editor {
        private val updates = mutableMapOf<String, Any?>()
        private var clearFirst = false

        override fun putString(key: String, value: String?) = apply { updates[key] = value }

        override fun putStringSet(key: String, values: Set<String>?) =
            apply { updates[key] = values }

        override fun putInt(key: String, value: Int) = apply { updates[key] = value }

        override fun putLong(key: String, value: Long) = apply { updates[key] = value }

        override fun putFloat(key: String, value: Float) = apply { updates[key] = value }

        override fun putBoolean(key: String, value: Boolean) = apply { updates[key] = value }

        override fun remove(key: String) = apply { updates[key] = null }

        override fun clear() = apply { clearFirst = true }

        override fun commit(): Boolean {
            applyChanges()
            return true
        }

        override fun apply() = applyChanges()

        private fun applyChanges() {
            if (clearFirst) values.clear()
            updates.forEach { (key, value) ->
                if (value == null) values.remove(key) else values[key] = value
            }
        }
    }

    override fun registerOnSharedPreferenceChangeListener(
        listener: SharedPreferences.OnSharedPreferenceChangeListener?,
    ) = Unit

    override fun unregisterOnSharedPreferenceChangeListener(
        listener: SharedPreferences.OnSharedPreferenceChangeListener?,
    ) = Unit
}
