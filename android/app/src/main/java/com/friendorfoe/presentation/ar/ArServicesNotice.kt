package com.friendorfoe.presentation.ar

import android.content.Context
import android.content.pm.PackageManager
import android.hardware.Sensor
import android.hardware.SensorManager
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import com.friendorfoe.presentation.about.openUri

// Required notice and link targets: https://developers.google.com/ar/develop/privacy-requirements
internal const val ARCORE_NOTICE = "This application runs on Google Play Services for AR (ARCore), " +
    "which is provided by Google and governed by the Google Privacy Policy."
internal const val ARCORE_SERVICES_URL = "https://play.google.com/store/apps/details?id=com.google.ar.core"
internal const val GOOGLE_PRIVACY_URL = "https://policies.google.com/privacy"

@Composable
internal fun ArServicesDisclosure(onOpenLink: (String) -> Unit) {
    Column {
        Text(ARCORE_NOTICE, style = MaterialTheme.typography.bodyMedium)
        TextButton(onClick = { onOpenLink(ARCORE_SERVICES_URL) }) { Text("Google Play Services for AR") }
        TextButton(onClick = { onOpenLink(GOOGLE_PRIVACY_URL) }) { Text("Google Privacy Policy") }
    }
}

@Composable
internal fun ArServicesEntry(onUseNearby: () -> Unit, content: @Composable () -> Unit) {
    val context = LocalContext.current
    val unavailable = remember(context) {
        val sensors = context.getSystemService(SensorManager::class.java)
        when {
            !context.packageManager.hasSystemFeature(PackageManager.FEATURE_CAMERA) ->
                "The AR view needs a rear camera."
            sensors == null || (sensors.getDefaultSensor(Sensor.TYPE_ROTATION_VECTOR) == null &&
                sensors.getDefaultSensor(Sensor.TYPE_GAME_ROTATION_VECTOR) == null &&
                (sensors.getDefaultSensor(Sensor.TYPE_ACCELEROMETER) == null ||
                    sensors.getDefaultSensor(Sensor.TYPE_MAGNETIC_FIELD) == null)) ->
                "This device does not have the orientation sensors used by the AR view."
            else -> null
        }
    }
    if (unavailable != null) {
        Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(24.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp)) {
            Text("AR unavailable", style = MaterialTheme.typography.headlineSmall)
            Text(unavailable)
            Button(onClick = onUseNearby) { Text("Use Nearby instead") }
        }
        return
    }
    val prefs = remember(context) { context.getSharedPreferences("ar_notices", Context.MODE_PRIVATE) }
    var acknowledged by remember(prefs) { mutableStateOf(prefs.getBoolean("google_ar_notice_v1", false)) }
    ArServicesEntryContent(
        acknowledged = acknowledged,
        onContinue = {
            prefs.edit().putBoolean("google_ar_notice_v1", true).apply()
            acknowledged = true
        },
        onUseNearby = onUseNearby,
        onOpenLink = context::openUri,
        content = content,
    )
}

@Composable
internal fun ArServicesEntryContent(
    acknowledged: Boolean,
    onContinue: () -> Unit,
    onUseNearby: () -> Unit,
    onOpenLink: (String) -> Unit,
    content: @Composable () -> Unit,
) {
    if (acknowledged) {
        content()
        return
    }
    Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(24.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp)) {
        Text("Using the AR camera", style = MaterialTheme.typography.headlineSmall)
        Text("On supported phones, AR uses Google’s service to align observations with the camera. " +
            "Google collects identifiers, usage and diagnostic information when the service is used.")
        ArServicesDisclosure(onOpenLink)
        Button(onClick = onContinue) { Text("Continue to AR") }
        TextButton(onClick = onUseNearby) { Text("Use Nearby instead") }
    }
}
