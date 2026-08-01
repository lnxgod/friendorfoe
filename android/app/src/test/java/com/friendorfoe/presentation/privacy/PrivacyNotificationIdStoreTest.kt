package com.friendorfoe.presentation.privacy

import android.content.SharedPreferences
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.CountDownLatch
import kotlin.concurrent.thread
import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test

class PrivacyNotificationIdStoreTest {
    @Test
    fun idsSurviveStoreReconstructionAndSkipPersistedCollisions() {
        val preferences = InMemorySharedPreferences()
        val firstKey = PrivacyFindingKey(PrivacySourceKind.PHONE_BLE, "mac:one")
        val secondKey = PrivacyFindingKey(PrivacySourceKind.WIFI_ANALYSIS, "bssid:two")
        val firstStore = SharedPreferencesPrivacyNotificationIdStore(preferences)

        val firstId = firstStore.idFor(firstKey)
        preferences.edit().putInt("next_id", firstId).commit()
        val reconstructed = SharedPreferencesPrivacyNotificationIdStore(preferences)

        assertEquals(firstId, reconstructed.idFor(firstKey))
        assertTrue(reconstructed.idFor(secondKey) > 0)
        assertTrue(reconstructed.idFor(secondKey) != firstId)
    }

    @Test
    fun concurrentAllocationNeverReusesAPositiveId() {
        val store = SharedPreferencesPrivacyNotificationIdStore(InMemorySharedPreferences())
        val start = CountDownLatch(1)
        val ids = ConcurrentHashMap.newKeySet<Int>()
        val workers = (1..32).map { index ->
            thread(start = true) {
                start.await()
                ids += store.idFor(
                    PrivacyFindingKey(PrivacySourceKind.BACKEND, "entity:$index"),
                )
            }
        }

        start.countDown()
        workers.forEach(Thread::join)

        assertEquals(32, ids.size)
        assertTrue(ids.all { it > 0 })
    }

    @Test
    fun failedAtomicCommitThrowsInsteadOfReturningAnUnpersistedId() {
        val store = SharedPreferencesPrivacyNotificationIdStore(
            InMemorySharedPreferences(failCommits = true),
        )

        assertThrows(IllegalStateException::class.java) {
            store.idFor(PrivacyFindingKey(PrivacySourceKind.BADGE_AP, "entity:42"))
        }
    }

    @Test
    fun persistedIdentityKeysContainNoXmlUnsafeControlCharacters() {
        val preferences = InMemorySharedPreferences()
        val store = SharedPreferencesPrivacyNotificationIdStore(preferences)

        store.idFor(PrivacyFindingKey(PrivacySourceKind.BADGE_USB, "entity:42"))

        assertTrue(
            preferences.all.keys.all { key -> key.none(Char::isISOControl) },
        )
    }

    private class InMemorySharedPreferences(
        private val failCommits: Boolean = false,
    ) : SharedPreferences {
        private val values = linkedMapOf<String, Any?>()

        override fun getAll(): Map<String, *> = synchronized(values) { values.toMap() }
        override fun getString(key: String, defValue: String?): String? =
            synchronized(values) { values[key] as? String ?: defValue }
        override fun getStringSet(key: String, defValues: Set<String>?): Set<String>? =
            synchronized(values) {
                @Suppress("UNCHECKED_CAST")
                (values[key] as? Set<String>)?.toSet() ?: defValues
            }
        override fun getInt(key: String, defValue: Int): Int =
            synchronized(values) { values[key] as? Int ?: defValue }
        override fun getLong(key: String, defValue: Long): Long =
            synchronized(values) { values[key] as? Long ?: defValue }
        override fun getFloat(key: String, defValue: Float): Float =
            synchronized(values) { values[key] as? Float ?: defValue }
        override fun getBoolean(key: String, defValue: Boolean): Boolean =
            synchronized(values) { values[key] as? Boolean ?: defValue }
        override fun contains(key: String): Boolean = synchronized(values) { key in values }
        override fun edit(): SharedPreferences.Editor = Editor()
        override fun registerOnSharedPreferenceChangeListener(
            listener: SharedPreferences.OnSharedPreferenceChangeListener,
        ) = Unit
        override fun unregisterOnSharedPreferenceChangeListener(
            listener: SharedPreferences.OnSharedPreferenceChangeListener,
        ) = Unit

        private inner class Editor : SharedPreferences.Editor {
            private val updates = linkedMapOf<String, Any?>()
            private val removals = linkedSetOf<String>()
            private var clear = false

            override fun putString(key: String, value: String?) = apply { updates[key] = value }
            override fun putStringSet(key: String, values: Set<String>?) = apply {
                updates[key] = values?.toSet()
            }
            override fun putInt(key: String, value: Int) = apply { updates[key] = value }
            override fun putLong(key: String, value: Long) = apply { updates[key] = value }
            override fun putFloat(key: String, value: Float) = apply { updates[key] = value }
            override fun putBoolean(key: String, value: Boolean) = apply { updates[key] = value }
            override fun remove(key: String) = apply { removals += key }
            override fun clear() = apply { clear = true }

            override fun commit(): Boolean {
                if (failCommits) return false
                synchronized(this@InMemorySharedPreferences.values) {
                    if (clear) this@InMemorySharedPreferences.values.clear()
                    removals.forEach(this@InMemorySharedPreferences.values::remove)
                    updates.forEach { (key, value) ->
                        if (value == null) this@InMemorySharedPreferences.values.remove(key)
                        else this@InMemorySharedPreferences.values[key] = value
                    }
                }
                return true
            }

            override fun apply() {
                commit()
            }
        }
    }
}
