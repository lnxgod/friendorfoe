package com.friendorfoe.presentation.watch

import android.Manifest
import android.annotation.SuppressLint
import android.app.*
import android.content.Intent
import android.content.pm.PackageManager
import android.content.pm.ServiceInfo
import android.location.Location
import android.location.LocationListener
import android.location.LocationManager
import android.os.IBinder
import android.os.SystemClock
import androidx.core.app.NotificationCompat
import androidx.core.app.NotificationManagerCompat
import androidx.core.app.ServiceCompat
import androidx.core.content.ContextCompat
import androidx.core.location.LocationManagerCompat
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.ProcessLifecycleOwner
import com.friendorfoe.R
import com.friendorfoe.data.DetectionPrefs
import com.friendorfoe.data.repository.SkyObjectRepository
import com.friendorfoe.data.repository.validatedLocationAccuracyMeters
import com.friendorfoe.presentation.MainActivity
import com.friendorfoe.presentation.alerts.SkyAlertNotifier
import com.friendorfoe.detection.AircraftFeedPhase
import dagger.hilt.android.AndroidEntryPoint
import kotlinx.coroutines.*
import kotlinx.coroutines.flow.combine
import javax.inject.Inject

@AndroidEntryPoint
class NearbyWatchService : Service(), LocationListener {
    @Inject lateinit var repository: SkyObjectRepository
    @Inject lateinit var notifier: SkyAlertNotifier
    @Inject lateinit var preferences: DetectionPrefs
    @Inject lateinit var controller: WatchModeController
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
    private lateinit var locationManager: LocationManager
    private var started = false
    private var stopMessage: String? = null
    private var lastLocationElapsedNanos: Long? = null
    private val manager get() = getSystemService(NotificationManager::class.java)

    override fun onBind(intent: Intent?): IBinder? = null

    @SuppressLint("MissingPermission")
    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == STOP) { stopSelf(); return START_NOT_STICKY }
        if (started) return START_NOT_STICKY
        locationManager = getSystemService(LocationManager::class.java)
        try {
            check(locationAllowed() && LocationManagerCompat.isLocationEnabled(locationManager)) { "Location unavailable" }
            check(NotificationManagerCompat.from(this).areNotificationsEnabled()) { "Notifications disabled" }
            manager.createNotificationChannel(NotificationChannel(CHANNEL, "Nearby watch", NotificationManager.IMPORTANCE_LOW))
            check(manager.getNotificationChannel(CHANNEL)?.importance != NotificationManager.IMPORTANCE_NONE) { "Watch notification blocked" }
            ServiceCompat.startForeground(this, ID, notification("Waiting for a location fix"), ServiceInfo.FOREGROUND_SERVICE_TYPE_LOCATION)
            started = true
            controller.started()
            for (provider in listOf(LocationManager.GPS_PROVIDER, LocationManager.NETWORK_PROVIDER)) {
                if (locationManager.isProviderEnabled(provider) && (provider != LocationManager.GPS_PROVIDER ||
                    ContextCompat.checkSelfPermission(this, Manifest.permission.ACCESS_FINE_LOCATION) == PackageManager.PERMISSION_GRANTED)) {
                    locationManager.requestLocationUpdates(provider, 5_000L, 10f, this)
                    locationManager.getLastKnownLocation(provider)?.let(::onLocationChanged)
                }
            }
            scope.launch {
                combine(repository.skyObjects, preferences.settings) { objects, _ -> objects }.collect { objects ->
                    if (freshWatchLocation(lastLocationElapsedNanos, SystemClock.elapsedRealtimeNanos())) objects.forEach(notifier::notifyObject)
                }
            }
            scope.launch {
                while (isActive) {
                    if (!locationAllowed() || !LocationManagerCompat.isLocationEnabled(locationManager) ||
                        !NotificationManagerCompat.from(this@NearbyWatchService).areNotificationsEnabled() ||
                        manager.getNotificationChannel(CHANNEL)?.importance == NotificationManager.IMPORTANCE_NONE) {
                        stopMessage = "Watch stopped: restore location and notification access to start again."
                        stopSelf(); break
                    }
                    val settings = preferences.settings.value
                    val aircraft = if (!settings.adsbEnabled) "Aircraft off" else when (repository.aircraftFeedState.value.phase) {
                        AircraftFeedPhase.LIVE -> "Aircraft live"
                        AircraftFeedPhase.RECONNECTING -> "Aircraft reconnecting"
                        AircraftFeedPhase.OFFLINE -> "Aircraft offline"
                        else -> "Aircraft connecting"
                    }
                    val localEnabled = !settings.backendOnlyMode && (settings.bleRidEnabled || settings.wifiEnabled)
                    val description = if (!freshWatchLocation(lastLocationElapsedNanos, SystemClock.elapsedRealtimeNanos()))
                        "Waiting for a fresh location · alerts paused" else "$aircraft · local radio ${if (localEnabled) "enabled" else "off"}"
                    controller.update(description, freshWatchLocation(lastLocationElapsedNanos, SystemClock.elapsedRealtimeNanos()))
                    manager.notify(ID, notification(description))
                    delay(5_000)
                }
            }
        } catch (_: Exception) {
            stopMessage = "Couldn't start watch mode. Check location and notification access."
            stopSelf()
        }
        return START_NOT_STICKY
    }

    override fun onLocationChanged(location: Location) {
        if (!freshWatchLocation(location.elapsedRealtimeNanos, SystemClock.elapsedRealtimeNanos())) return
        if (lastLocationElapsedNanos?.let { location.elapsedRealtimeNanos < it } == true) return
        if (!location.latitude.isFinite() || location.latitude !in -90.0..90.0 ||
            !location.longitude.isFinite() || location.longitude !in -180.0..180.0 ||
            (location.latitude == 0.0 && location.longitude == 0.0)) return
        lastLocationElapsedNanos = location.elapsedRealtimeNanos
        repository.ensureStarted(location.latitude, location.longitude, location.validatedLocationAccuracyMeters())
    }

    override fun onProviderEnabled(provider: String) {}
    override fun onProviderDisabled(provider: String) {}
    @Deprecated("Legacy location callback")
    override fun onStatusChanged(provider: String?, status: Int, extras: android.os.Bundle?) {}

    private fun locationAllowed() = listOf(Manifest.permission.ACCESS_FINE_LOCATION, Manifest.permission.ACCESS_COARSE_LOCATION)
        .any { ContextCompat.checkSelfPermission(this, it) == PackageManager.PERMISSION_GRANTED }

    private fun notification(message: String): Notification {
        val open = PendingIntent.getActivity(this, ID, Intent(this, MainActivity::class.java), PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE)
        val stop = PendingIntent.getService(this, ID, Intent(this, NearbyWatchService::class.java).setAction(STOP), PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE)
        return NotificationCompat.Builder(this, CHANNEL).setSmallIcon(R.drawable.ic_launcher_foreground)
            .setContentTitle("Watching nearby").setContentText(message).setStyle(NotificationCompat.BigTextStyle().bigText(message))
            .setContentIntent(open).setOngoing(true).setOnlyAlertOnce(true)
            .setForegroundServiceBehavior(NotificationCompat.FOREGROUND_SERVICE_IMMEDIATE)
            .addAction(0, "Stop watching", stop).build()
    }

    override fun onDestroy() {
        scope.cancel()
        if (::locationManager.isInitialized) locationManager.removeUpdates(this)
        if (!ProcessLifecycleOwner.get().lifecycle.currentState.isAtLeast(Lifecycle.State.STARTED)) repository.stop()
        stopForeground(STOP_FOREGROUND_REMOVE)
        controller.stopped(stopMessage)
        super.onDestroy()
    }
    companion object {
        const val CHANNEL = "nearby_watch"
        const val ID = 730241
        const val STOP = "com.friendorfoe.STOP_NEARBY_WATCH"
    }
}

fun freshWatchLocation(fixElapsedNanos: Long?, nowElapsedNanos: Long): Boolean =
    fixElapsedNanos != null && fixElapsedNanos > 0 && nowElapsedNanos - fixElapsedNanos in 0..120_000_000_000L
