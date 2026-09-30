package com.friendorfoe.presentation.alerts

import android.app.NotificationManager
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import dagger.hilt.android.AndroidEntryPoint
import javax.inject.Inject

@AndroidEntryPoint
class SkyAlertActionReceiver : BroadcastReceiver() {
    @Inject lateinit var controls: SkyAlertControls
    override fun onReceive(context: Context, intent: Intent) {
        when (intent.action) {
            MUTE -> intent.getStringExtra(SkyAlertRoute.OBJECT_ID)?.let(controls::mute)
            SNOOZE -> controls.snooze()
            else -> return
        }
        val manager = context.getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        if (intent.action == SNOOZE) {
            manager.activeNotifications.filter { it.notification.channelId == "sky_alerts" }.forEach { manager.cancel(it.id) }
        } else manager.cancel(intent.getIntExtra("notification_id", -1))
    }
    companion object {
        const val MUTE = "com.friendorfoe.MUTE_SKY_OBJECT"
        const val SNOOZE = "com.friendorfoe.SNOOZE_SKY_ALERTS"
    }
}
