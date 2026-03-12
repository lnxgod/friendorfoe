package com.friendorfoe.presentation.ar

import android.Manifest
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.hardware.SensorManager
import android.location.LocationManager
import android.os.Build
import android.provider.Settings
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Bluetooth
import androidx.compose.material.icons.filled.CameraAlt
import androidx.compose.material.icons.filled.Explore
import androidx.compose.material.icons.filled.LocationOff
import androidx.compose.material.icons.filled.WifiOff
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Icon
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.core.content.ContextCompat
import androidx.lifecycle.compose.collectAsStateWithLifecycle

/**
 * Composable gate that checks runtime permissions and system state before
 * showing the AR view.
 *
 * Checks performed (in order):
 * 1. CAMERA permission -- required, AR cannot function without it
 * 2. ACCESS_FINE_LOCATION permission -- required for GPS positioning
 * 3. BLUETOOTH_SCAN permission (API 31+) -- optional, needed for Remote ID
 * 4. GPS enabled in system settings -- shows prompt if disabled
 * 5. Internet connectivity -- shows offline indicator if unavailable
 * 6. Compass calibration -- shows calibration prompt when magnetometer accuracy is low
 *
 * Once all required permissions are granted and GPS is enabled, the [content]
 * composable (typically [ArViewScreen]) is rendered.
 *
 * Uses [rememberLauncherForActivityResult] from activity-compose (already a
 * project dependency) to request permissions without pulling in the
 * Accompanist library.
 *
 * @param viewModel The [ArViewModel] for accessing sensor accuracy and connectivity state
 * @param content The AR content to display once permissions are satisfied
 */
@Composable
fun PermissionHandler(
    viewModel: ArViewModel,
    content: @Composable () -> Unit
) {
    val context = LocalContext.current

    // --- Permission state tracking ---
    var cameraGranted by remember {
        mutableStateOf(
            ContextCompat.checkSelfPermission(context, Manifest.permission.CAMERA) ==
                PackageManager.PERMISSION_GRANTED
        )
    }
    var locationGranted by remember {
        mutableStateOf(
            ContextCompat.checkSelfPermission(context, Manifest.permission.ACCESS_FINE_LOCATION) ==
                PackageManager.PERMISSION_GRANTED
        )
    }
    var bluetoothGranted by remember {
        mutableStateOf(
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                ContextCompat.checkSelfPermission(context, Manifest.permission.BLUETOOTH_SCAN) ==
                    PackageManager.PERMISSION_GRANTED
            } else {
                true // Not needed below API 31
            }
        )
    }

    // Track whether the initial bulk request has been launched
    var hasRequestedPermissions by remember { mutableStateOf(false) }

    // --- Permission launchers ---

    val permissionLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions()
    ) { results ->
        cameraGranted = results[Manifest.permission.CAMERA] == true
        locationGranted = results[Manifest.permission.ACCESS_FINE_LOCATION] == true
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            bluetoothGranted = results[Manifest.permission.BLUETOOTH_SCAN] == true
        }
    }

    // Launch permission request on first composition
    LaunchedEffect(Unit) {
        if (!hasRequestedPermissions && (!cameraGranted || !locationGranted || !bluetoothGranted)) {
            hasRequestedPermissions = true
            val permissions = buildList {
                if (!cameraGranted) add(Manifest.permission.CAMERA)
                if (!locationGranted) add(Manifest.permission.ACCESS_FINE_LOCATION)
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S && !bluetoothGranted) {
                    add(Manifest.permission.BLUETOOTH_SCAN)
                }
            }
            if (permissions.isNotEmpty()) {
                permissionLauncher.launch(permissions.toTypedArray())
            }
        }
    }

    // --- Gate: Camera permission ---
    if (!cameraGranted) {
        PermissionBlockedScreen(
            icon = Icons.Filled.CameraAlt,
            title = "Camera Access Required",
            message = "Friend or Foe needs camera access to show the live AR viewfinder " +
                "with aircraft and drone overlays. The camera feed stays on your device.\n\n" +
                "If the permission dialog did not appear, please grant camera access in Settings.",
            buttonText = "Grant Camera Access",
            onButtonClick = {
                // Try requesting again; if the system won't show the dialog,
                // the user can tap again and we'll open settings.
                if (hasRequestedPermissions) {
                    openAppSettings(context)
                } else {
                    permissionLauncher.launch(arrayOf(Manifest.permission.CAMERA))
                }
            }
        )
        return
    }

    // --- Gate: Location permission ---
    if (!locationGranted) {
        PermissionBlockedScreen(
            icon = Icons.Filled.LocationOff,
            title = "Location Access Required",
            message = "Friend or Foe needs your precise location to calculate the bearing " +
                "and distance to aircraft and drones in the sky around you.\n\n" +
                "If the permission dialog did not appear, please grant location access in Settings.",
            buttonText = "Grant Location Access",
            onButtonClick = {
                if (hasRequestedPermissions) {
                    openAppSettings(context)
                } else {
                    permissionLauncher.launch(arrayOf(Manifest.permission.ACCESS_FINE_LOCATION))
                }
            }
        )
        return
    }

    // --- Check: GPS enabled ---
    val locationManager = context.getSystemService(Context.LOCATION_SERVICE) as LocationManager
    val gpsEnabled = locationManager.isProviderEnabled(LocationManager.GPS_PROVIDER)

    if (!gpsEnabled) {
        PermissionBlockedScreen(
            icon = Icons.Filled.LocationOff,
            title = "GPS is Disabled",
            message = "Please enable GPS / Location Services in your device settings. " +
                "Friend or Foe needs a GPS fix to calculate positions of aircraft relative to you.",
            buttonText = "Open Location Settings",
            onButtonClick = {
                context.startActivity(Intent(Settings.ACTION_LOCATION_SOURCE_SETTINGS))
            }
        )
        return
    }

    // --- All required permissions granted, GPS enabled. Show AR content. ---
    Box(modifier = Modifier.fillMaxSize()) {
        content()

        // Calculate vertical offset for stacking banners
        var bannerOffset = 0.dp

        // --- Overlay: Bluetooth not granted (non-blocking warning) ---
        if (!bluetoothGranted) {
            OverlayBanner(
                icon = Icons.Filled.Bluetooth,
                text = "Bluetooth not available -- Remote ID drone scanning disabled",
                color = Color(0xFFFF9800),
                modifier = Modifier
                    .align(Alignment.TopCenter)
                    .padding(top = bannerOffset)
            )
            bannerOffset += 40.dp
        }

        // --- Overlay: Offline indicator (non-blocking warning) ---
        val isOnline by viewModel.isOnline.collectAsStateWithLifecycle()
        if (!isOnline) {
            OverlayBanner(
                icon = Icons.Filled.WifiOff,
                text = "No internet -- showing cached data, ADS-B updates paused",
                color = Color(0xFFF44336),
                modifier = Modifier
                    .align(Alignment.TopCenter)
                    .padding(top = bannerOffset)
            )
            bannerOffset += 40.dp
        }

        // --- Overlay: Compass calibration needed (non-blocking warning) ---
        val sensorAccuracy by viewModel.sensorAccuracy.collectAsStateWithLifecycle()
        if (sensorAccuracy <= SensorManager.SENSOR_STATUS_ACCURACY_LOW) {
            OverlayBanner(
                icon = Icons.Filled.Explore,
                text = "Compass needs calibration -- wave your phone in a figure-8 pattern",
                color = Color(0xFFFF9800),
                modifier = Modifier
                    .align(Alignment.TopCenter)
                    .padding(top = bannerOffset)
            )
        }
    }
}

/**
 * Full-screen blocking UI shown when a required permission is denied.
 *
 * Displays an icon, title, explanation message, and a single action button
 * that either re-requests the permission or opens system settings.
 */
@Composable
private fun PermissionBlockedScreen(
    icon: ImageVector,
    title: String,
    message: String,
    buttonText: String,
    onButtonClick: () -> Unit
) {
    Box(
        modifier = Modifier
            .fillMaxSize()
            .background(Color(0xFF1A1A2E)),
        contentAlignment = Alignment.Center
    ) {
        Column(
            modifier = Modifier
                .padding(32.dp)
                .fillMaxWidth(),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.spacedBy(16.dp)
        ) {
            Icon(
                imageVector = icon,
                contentDescription = null,
                tint = Color(0xFFFF9800),
                modifier = Modifier.size(64.dp)
            )

            Text(
                text = title,
                color = Color.White,
                fontSize = 22.sp,
                fontWeight = FontWeight.Bold,
                textAlign = TextAlign.Center
            )

            Text(
                text = message,
                color = Color.White.copy(alpha = 0.8f),
                fontSize = 15.sp,
                textAlign = TextAlign.Center,
                lineHeight = 22.sp
            )

            Spacer(modifier = Modifier.height(8.dp))

            Button(
                onClick = onButtonClick,
                colors = ButtonDefaults.buttonColors(
                    containerColor = Color(0xFF2196F3)
                ),
                shape = RoundedCornerShape(8.dp)
            ) {
                Text(
                    text = buttonText,
                    fontSize = 16.sp,
                    fontWeight = FontWeight.Medium,
                    modifier = Modifier.padding(horizontal = 16.dp, vertical = 4.dp)
                )
            }
        }
    }
}

/**
 * Non-blocking banner overlay shown at the top of the AR view for warnings
 * that do not prevent the app from functioning (e.g., no Bluetooth, offline, compass).
 */
@Composable
private fun OverlayBanner(
    icon: ImageVector,
    text: String,
    color: Color,
    modifier: Modifier = Modifier
) {
    Row(
        modifier = modifier
            .fillMaxWidth()
            .background(color.copy(alpha = 0.85f))
            .padding(horizontal = 12.dp, vertical = 8.dp),
        verticalAlignment = Alignment.CenterVertically
    ) {
        Icon(
            imageVector = icon,
            contentDescription = null,
            tint = Color.White,
            modifier = Modifier.size(18.dp)
        )
        Spacer(modifier = Modifier.width(8.dp))
        Text(
            text = text,
            color = Color.White,
            fontSize = 12.sp,
            fontWeight = FontWeight.Medium
        )
    }
}

/**
 * Opens the app-specific settings page so the user can manually grant
 * permissions that were previously denied with "Don't ask again".
 */
private fun openAppSettings(context: Context) {
    val intent = Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS).apply {
        data = android.net.Uri.fromParts("package", context.packageName, null)
    }
    context.startActivity(intent)
}
