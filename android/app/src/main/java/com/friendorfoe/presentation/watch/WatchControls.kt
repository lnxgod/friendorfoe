package com.friendorfoe.presentation.watch

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.location.LocationManager
import android.net.Uri
import android.os.Build
import android.provider.Settings
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.unit.dp
import androidx.core.app.NotificationManagerCompat
import androidx.core.content.ContextCompat
import androidx.core.location.LocationManagerCompat
import androidx.hilt.navigation.compose.hiltViewModel
import androidx.lifecycle.ViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.friendorfoe.presentation.alerts.SkyAlertControls
import dagger.hilt.android.lifecycle.HiltViewModel
import javax.inject.Inject

@HiltViewModel
class WatchControlsViewModel @Inject constructor(val watch: WatchModeController, val alerts: SkyAlertControls) : ViewModel()

@Composable
fun WatchControls(viewModel: WatchControlsViewModel = hiltViewModel()) {
    val context = LocalContext.current
    val watch by viewModel.watch.state.collectAsStateWithLifecycle()
    val alerts by viewModel.alerts.state.collectAsStateWithLifecycle()
    var open by rememberSaveable { mutableStateOf(false) }
    var error by remember { mutableStateOf<String?>(null) }
    fun has(permission: String) = ContextCompat.checkSelfPermission(context, permission) == PackageManager.PERMISSION_GRANTED
    fun startIfReady() {
        error = when {
            !has(Manifest.permission.ACCESS_FINE_LOCATION) && !has(Manifest.permission.ACCESS_COARSE_LOCATION) -> "Allow location access to start watching."
            !LocationManagerCompat.isLocationEnabled(context.getSystemService(LocationManager::class.java)) -> "Turn on Android location to start watching."
            !NotificationManagerCompat.from(context).areNotificationsEnabled() -> "Allow notifications so you can see and stop watch mode."
            else -> null
        }
        if (error == null) viewModel.watch.start()
    }
    val permissions = rememberLauncherForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) { startIfReady() }
    TextButton(onClick = { open = true }, modifier = Modifier.testTag("watch_controls")) {
        Text(if (watch.active) "Watching" else if (watch.starting) "Starting…" else "Watch")
    }
    val nowMs by produceState(System.currentTimeMillis(), open) {
        while (open) { kotlinx.coroutines.delay(1000); value = System.currentTimeMillis() }
    }
    if (open) AlertDialog(
        onDismissRequest = { open = false },
        title = { Text("Watch nearby") },
        text = { Column(Modifier.verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(12.dp)) {
            Text("Keep nearby aircraft and permitted local radio sources running with the screen off. Uses location, mobile data, and battery until you stop.")
            Text(watch.message ?: if (watch.active) "Watching nearby" else "Watch mode is off")
            error?.let { Text(it, color = MaterialTheme.colorScheme.error) }
            if (error != null || watch.message?.startsWith("Couldn't") == true || watch.message?.startsWith("Watch stopped") == true) {
                TextButton(onClick = { context.startActivity(Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS, Uri.parse("package:${context.packageName}"))) }) { Text("App permissions") }
                TextButton(onClick = { context.startActivity(Intent(Settings.ACTION_LOCATION_SOURCE_SETTINGS)) }) { Text("Android location settings") }
            }
            HorizontalDivider()
            Text("Aircraft & drone alerts", style = MaterialTheme.typography.titleSmall)
            Text("Watch mode follows your notification settings. Starting a watch does not turn alert categories on.", style = MaterialTheme.typography.bodySmall)
            if (alerts.snoozedUntilMs > nowMs) {
                Text("Alerts snoozed until ${java.time.Instant.ofEpochMilli(alerts.snoozedUntilMs).atZone(java.time.ZoneId.systemDefault()).format(java.time.format.DateTimeFormatter.ofPattern("HH:mm"))}")
                TextButton(onClick = viewModel.alerts::resume) { Text("Resume alerts") }
            } else TextButton(onClick = viewModel.alerts::snooze) { Text("Snooze alerts for 30 minutes") }
            if (alerts.mutedObjectIds.isNotEmpty()) {
                Text("${alerts.mutedObjectIds.size} muted aircraft or drones")
                TextButton(onClick = viewModel.alerts::clearMuted) { Text("Unmute all") }
            }
        } },
        confirmButton = {
            TextButton(enabled = !watch.starting, onClick = {
                if (watch.active) viewModel.watch.stop() else {
                    val needed = buildList {
                        if (!has(Manifest.permission.ACCESS_FINE_LOCATION) && !has(Manifest.permission.ACCESS_COARSE_LOCATION)) {
                            add(Manifest.permission.ACCESS_FINE_LOCATION); add(Manifest.permission.ACCESS_COARSE_LOCATION)
                        }
                        if (Build.VERSION.SDK_INT >= 33 && !has(Manifest.permission.POST_NOTIFICATIONS)) add(Manifest.permission.POST_NOTIFICATIONS)
                    }
                    if (needed.isEmpty()) startIfReady() else permissions.launch(needed.toTypedArray())
                }
            }, modifier = Modifier.testTag("watch_toggle")) { Text(if (watch.active) "Stop watching" else "Start watching") }
        },
        dismissButton = { TextButton(onClick = { open = false }) { Text("Done") } },
    )
}
