package com.friendorfoe.data

import android.content.ContextWrapper
import android.content.SharedPreferences
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class DetectionPrefsTest {
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
}

private class TestContext(
    private val preferences: SharedPreferences,
) : ContextWrapper(null) {
    override fun getSharedPreferences(name: String?, mode: Int): SharedPreferences = preferences
}

private class TestSharedPreferences(
    private val values: Map<String, Any> = emptyMap(),
) : SharedPreferences {
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

    override fun edit(): SharedPreferences.Editor = error("Editing is not used by these tests")

    override fun registerOnSharedPreferenceChangeListener(
        listener: SharedPreferences.OnSharedPreferenceChangeListener?,
    ) = Unit

    override fun unregisterOnSharedPreferenceChangeListener(
        listener: SharedPreferences.OnSharedPreferenceChangeListener?,
    ) = Unit
}
