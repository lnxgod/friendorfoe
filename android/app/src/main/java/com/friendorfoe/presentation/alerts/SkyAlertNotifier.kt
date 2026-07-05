package com.friendorfoe.presentation.alerts

import android.Manifest
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import androidx.core.app.NotificationCompat
import androidx.core.app.NotificationManagerCompat
import androidx.core.content.ContextCompat
import com.friendorfoe.R
import com.friendorfoe.data.DetectionPrefs
import com.friendorfoe.domain.model.SkyObject
import com.friendorfoe.presentation.MainActivity
import dagger.hilt.android.qualifiers.ApplicationContext
import javax.inject.Inject
import javax.inject.Singleton

@Singleton
class SkyAlertNotifier @Inject constructor(
    @ApplicationContext private val context: Context,
    private val detectionPrefs: DetectionPrefs
) {
    companion object {
        private const val CHANNEL_ID = "sky_alerts"
    }

    private val policy = SkyAlertPolicy()

    fun notifyObject(skyObject: SkyObject) {
        val settings = SkyAlertSettings(
            droneAlertsEnabled = detectionPrefs.droneAlertsEnabled,
            helicopterAlertsEnabled = detectionPrefs.helicopterAlertsEnabled,
            militaryAlertsEnabled = detectionPrefs.militaryAlertsEnabled,
            policeAlertsEnabled = detectionPrefs.policeAlertsEnabled
        )
        val candidate = SkyAlertPolicy.candidateFor(skyObject, settings) ?: return
        notifyCandidate(candidate)
    }

    private fun notifyCandidate(candidate: SkyAlertCandidate) {
        if (!policy.shouldNotify(candidate)) return
        if (!hasNotificationPermission()) return

        ensureChannel()
        val intent = Intent(context, MainActivity::class.java).apply {
            flags = Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TOP
        }
        val pendingIntent = PendingIntent.getActivity(
            context,
            0,
            intent,
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        )
        val notification = NotificationCompat.Builder(context, CHANNEL_ID)
            .setSmallIcon(R.drawable.ic_launcher_foreground)
            .setContentTitle(candidate.title)
            .setContentText(candidate.body)
            .setStyle(NotificationCompat.BigTextStyle().bigText(candidate.body))
            .setContentIntent(pendingIntent)
            .setAutoCancel(true)
            .setPriority(NotificationCompat.PRIORITY_HIGH)
            .setCategory(NotificationCompat.CATEGORY_ALARM)
            .build()

        NotificationManagerCompat.from(context).notify(
            candidate.key.hashCode() and Int.MAX_VALUE,
            notification
        )
    }

    private fun hasNotificationPermission(): Boolean {
        return Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU ||
            ContextCompat.checkSelfPermission(context, Manifest.permission.POST_NOTIFICATIONS) ==
            PackageManager.PERMISSION_GRANTED
    }

    private fun ensureChannel() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) return
        val manager = context.getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        if (manager.getNotificationChannel(CHANNEL_ID) != null) return
        val channel = NotificationChannel(
            CHANNEL_ID,
            "Nearby object alerts",
            NotificationManager.IMPORTANCE_HIGH
        ).apply {
            description = "Drone, helicopter, military, police, and emergency proximity alerts"
        }
        manager.createNotificationChannel(channel)
    }
}
