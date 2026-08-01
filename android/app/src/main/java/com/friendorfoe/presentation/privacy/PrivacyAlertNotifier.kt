package com.friendorfoe.presentation.privacy

import android.Manifest
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import androidx.core.app.NotificationCompat
import androidx.core.app.NotificationManagerCompat
import androidx.core.content.ContextCompat
import com.friendorfoe.R
import com.friendorfoe.data.DetectionPrefs
import com.friendorfoe.presentation.MainActivity
import dagger.hilt.android.qualifiers.ApplicationContext
import javax.inject.Inject

internal enum class PrivacyNotificationDeliveryState {
    AVAILABLE,
    RUNTIME_PERMISSION_MISSING,
    GLOBAL_DISABLED,
    CHANNEL_BLOCKED,
}

internal fun privacyNotificationDeliveryState(
    hasRuntimePermission: Boolean,
    globallyEnabled: Boolean,
    channelImportance: Int?,
): PrivacyNotificationDeliveryState = when {
    !hasRuntimePermission -> PrivacyNotificationDeliveryState.RUNTIME_PERMISSION_MISSING
    !globallyEnabled -> PrivacyNotificationDeliveryState.GLOBAL_DISABLED
    channelImportance == NotificationManager.IMPORTANCE_NONE ->
        PrivacyNotificationDeliveryState.CHANNEL_BLOCKED
    else -> PrivacyNotificationDeliveryState.AVAILABLE
}

class PrivacyAlertNotifier internal constructor(
    private val notificationsEnabled: () -> Boolean,
    private val deliveryState: () -> PrivacyNotificationDeliveryState,
    private val ids: PrivacyNotificationIdStore,
    private val post: (PrivacyFinding, PrivacyNotificationRoute) -> Boolean,
) : PrivacyAlertPublisher {
    @Inject
    constructor(
        @ApplicationContext context: Context,
        detectionPrefs: DetectionPrefs,
        ids: PrivacyNotificationIdStore,
    ) : this(
        notificationsEnabled = { detectionPrefs.privacyNotificationsEnabled },
        deliveryState = { androidPrivacyNotificationDeliveryState(context) },
        ids = ids,
        post = { finding, target -> postPrivacyNotification(context, finding, target) },
    )

    override fun publish(finding: PrivacyFinding): Boolean {
        if (!notificationsEnabled() ||
            deliveryState() != PrivacyNotificationDeliveryState.AVAILABLE
        ) {
            return false
        }
        val key = finding.routableKey ?: return false
        val target = runCatching { PrivacyNotificationRoute.from(key, ids) }
            .getOrElse { return false }
        return runCatching { post(finding, target) }.getOrDefault(false)
    }
}

private fun androidPrivacyNotificationDeliveryState(
    context: Context,
): PrivacyNotificationDeliveryState {
    val hasRuntimePermission = Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU ||
        ContextCompat.checkSelfPermission(context, Manifest.permission.POST_NOTIFICATIONS) ==
        PackageManager.PERMISSION_GRANTED
    val globallyEnabled = NotificationManagerCompat.from(context).areNotificationsEnabled()
    val channelImportance = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
        val manager = context.getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        manager.getNotificationChannel(PRIVACY_ALERT_CHANNEL_ID)?.importance
    } else {
        null
    }
    return privacyNotificationDeliveryState(
        hasRuntimePermission = hasRuntimePermission,
        globallyEnabled = globallyEnabled,
        channelImportance = channelImportance,
    )
}

private fun postPrivacyNotification(
    context: Context,
    finding: PrivacyFinding,
    target: PrivacyNotificationRoute,
): Boolean {
    ensurePrivacyAlertChannel(context)
    val intent = Intent(context, MainActivity::class.java).apply {
        flags = Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TOP
        data = Uri.parse(target.dataUri)
        setPackage(context.packageName)
        putExtra(PrivacyNotificationRoute.EXTRA_ROUTE, target.route)
    }
    val pendingIntent = PendingIntent.getActivity(
        context,
        target.pendingIntentId,
        intent,
        PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE,
    )
    val body = finding.evidence
        ?: "${finding.source.userLabel()} · ${finding.category.label}"
    val notification = NotificationCompat.Builder(context, PRIVACY_ALERT_CHANNEL_ID)
        .setSmallIcon(R.drawable.ic_launcher_foreground)
        .setContentTitle(finding.title)
        .setContentText(body)
        .setStyle(NotificationCompat.BigTextStyle().bigText(body))
        .setContentIntent(pendingIntent)
        .setAutoCancel(true)
        .setPriority(NotificationCompat.PRIORITY_HIGH)
        .setCategory(NotificationCompat.CATEGORY_ALARM)
        .build()
    NotificationManagerCompat.from(context).notify(target.pendingIntentId, notification)
    return true
}

private fun ensurePrivacyAlertChannel(context: Context) {
    if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) return
    val manager = context.getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
    if (manager.getNotificationChannel(PRIVACY_ALERT_CHANNEL_ID) != null) return
    manager.createNotificationChannel(
        NotificationChannel(
            PRIVACY_ALERT_CHANNEL_ID,
            "Privacy alerts",
            NotificationManager.IMPORTANCE_HIGH,
        ).apply {
            description = "Current high-risk Privacy findings"
        },
    )
}

private const val PRIVACY_ALERT_CHANNEL_ID = "privacy_alerts"
