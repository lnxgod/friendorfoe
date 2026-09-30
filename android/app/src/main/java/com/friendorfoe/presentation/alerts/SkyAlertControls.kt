package com.friendorfoe.presentation.alerts

import android.content.Context
import dagger.hilt.android.qualifiers.ApplicationContext
import javax.inject.Inject
import javax.inject.Singleton
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.asStateFlow

data class SkyAlertControlState(val snoozedUntilMs: Long = 0, val mutedObjectIds: Set<String> = emptySet()) {
    fun allows(objectId: String, nowMs: Long) = nowMs >= snoozedUntilMs && objectId !in mutedObjectIds
}

@Singleton
class SkyAlertControls @Inject constructor(@ApplicationContext context: Context) {
    private val prefs = context.getSharedPreferences("sky_alert_controls", Context.MODE_PRIVATE)
    private fun snapshot() = SkyAlertControlState(prefs.getLong("snoozed_until", 0), prefs.getStringSet("muted_ids", emptySet()).orEmpty().toSet())
    private val mutableState = MutableStateFlow(snapshot())
    val state = mutableState.asStateFlow()
    @Synchronized fun mute(objectId: String) {
        if (!SkyAlertRoute.validObjectId(objectId)) return
        prefs.edit().putStringSet("muted_ids", snapshot().mutedObjectIds + objectId).apply()
        mutableState.value = snapshot()
    }
    @Synchronized fun snooze() {
        prefs.edit().putLong("snoozed_until", System.currentTimeMillis() + 30 * 60_000L).apply()
        mutableState.value = snapshot()
    }
    @Synchronized fun resume() {
        prefs.edit().remove("snoozed_until").apply()
        mutableState.value = snapshot()
    }
    @Synchronized fun clearMuted() {
        prefs.edit().remove("muted_ids").apply()
        mutableState.value = snapshot()
    }
}

object SkyAlertRoute {
    const val OPEN = "com.friendorfoe.OPEN_SKY_OBJECT"
    const val OBJECT_ID = "sky_object_id"
    fun validObjectId(value: String?) = value != null && value.isNotBlank() && value.length <= 256 && value.none(Char::isISOControl)
    fun parse(action: String?, objectId: String?): String? = objectId?.takeIf { action == OPEN && validObjectId(it) }
}
