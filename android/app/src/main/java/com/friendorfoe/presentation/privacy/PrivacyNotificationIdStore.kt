package com.friendorfoe.presentation.privacy

import android.content.Context
import android.content.SharedPreferences
import dagger.hilt.android.qualifiers.ApplicationContext
import com.friendorfoe.presentation.navigation.encodeRouteSegment
import javax.inject.Inject
import javax.inject.Singleton

fun interface PrivacyNotificationIdStore {
    fun idFor(key: PrivacyFindingKey): Int
}

@Singleton
class SharedPreferencesPrivacyNotificationIdStore internal constructor(
    private val preferences: SharedPreferences,
) : PrivacyNotificationIdStore {
    @Inject
    constructor(@ApplicationContext context: Context) : this(
        context.getSharedPreferences(PREFERENCES_NAME, Context.MODE_PRIVATE),
    )

    private val lock = Any()

    override fun idFor(key: PrivacyFindingKey): Int = synchronized(lock) {
        val storageKey = "$ID_PREFIX${encodeRouteSegment(key.encoded)}"
        preferences.getInt(storageKey, 0).takeIf { it > 0 }?.let {
            return@synchronized it
        }

        val used = preferences.all.asSequence()
            .filter { (storedKey, value) -> storedKey.startsWith(ID_PREFIX) && value is Int }
            .mapNotNull { (_, value) -> (value as? Int)?.takeIf { it > 0 } }
            .toHashSet()
        var candidate = preferences.getInt(NEXT_ID, 1).takeIf { it > 0 } ?: 1
        while (candidate in used) candidate = nextPositive(candidate)
        val next = nextPositive(candidate)
        check(
            preferences.edit()
                .putInt(storageKey, candidate)
                .putInt(NEXT_ID, next)
                .commit(),
        ) { "Could not persist a unique Privacy notification ID" }
        candidate
    }

    private fun nextPositive(value: Int): Int = if (value == Int.MAX_VALUE) 1 else value + 1

    private companion object {
        const val PREFERENCES_NAME = "privacy_notification_ids"
        const val ID_PREFIX = "id_"
        const val NEXT_ID = "next_id"
    }
}
