package com.friendorfoe.presentation.privacy

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
import com.friendorfoe.detection.BleTracker
import com.friendorfoe.detection.GlassesDetection
import com.friendorfoe.detection.UltrasonicDetector
import com.friendorfoe.detection.WifiAnomalyDetector
import com.friendorfoe.presentation.MainActivity
import dagger.hilt.android.qualifiers.ApplicationContext
import javax.inject.Inject
import javax.inject.Singleton

@Singleton
class PrivacyAlertNotifier @Inject constructor(
    @ApplicationContext private val context: Context,
    private val detectionPrefs: DetectionPrefs
) {
    companion object {
        private const val CHANNEL_ID = "privacy_alerts"
    }

    private val policy = PrivacyAlertPolicy()

    fun notifyDetection(detection: GlassesDetection) {
        PrivacyAlertPolicy.fromDetection(detection)?.let(::notifyCandidate)
    }

    fun notifyWifiAnomaly(anomaly: WifiAnomalyDetector.WifiAnomaly) {
        notifyCandidate(
            PrivacyAlertPolicy.wifiAnomaly(
                type = anomaly.type,
                ssid = anomaly.ssid,
                details = anomaly.details,
                threatLevel = anomaly.threatLevel,
                bssids = anomaly.bssids
            )
        )
    }

    fun notifyUltrasonic(alert: UltrasonicDetector.UltrasonicAlert) {
        notifyCandidate(
            PrivacyAlertPolicy.ultrasonic(
                frequencyHz = alert.frequencyHz,
                snrDb = alert.snrDb,
                persistenceFrames = alert.persistenceFrames
            )
        )
    }

    fun notifyStalker(alert: BleTracker.StalkerAlert) {
        if (alert.threatLevel < 2) return
        val label = alert.device.deviceName
            ?: alert.device.deviceType
            ?: alert.device.mac
        notifyCandidate(
            PrivacyAlertPolicy.stalker(
                mac = alert.device.mac,
                label = label,
                reason = alert.reason,
                threatLevel = alert.threatLevel
            )
        )
    }

    private fun notifyCandidate(candidate: PrivacyAlertCandidate) {
        if (!detectionPrefs.privacyNotificationsEnabled) return
        if (!policy.shouldNotify(candidate, detectionPrefs.getIgnoredMacs())) return
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

        NotificationManagerCompat.from(context).notify(candidate.key.hashCode() and Int.MAX_VALUE, notification)
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
            "Privacy alerts",
            NotificationManager.IMPORTANCE_HIGH
        ).apply {
            description = "High-risk privacy, tracker, and WiFi attack alerts"
        }
        manager.createNotificationChannel(channel)
    }
}
