package com.friendorfoe.presentation.ar

import android.content.Context
import android.content.Intent
import android.hardware.SensorManager
import android.location.LocationManager
import android.provider.Settings
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Explore
import androidx.compose.material.icons.filled.LocationOff
import androidx.compose.material.icons.filled.WifiOff
import androidx.compose.material3.Icon
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.friendorfoe.presentation.permissions.PermissionUiState
import com.friendorfoe.presentation.permissions.isUsable

private data class ArStatusBanner(
    val message: String,
    val action: (() -> Unit)?,
    val color: Color,
)

/**
 * Renders camera AR plus non-blocking status for optional system capabilities.
 *
 * Camera permission is owned by the route. Location is optional here, and local-radio
 * permissions remain owned by their Info toggles rather than being requested a second time.
 */
@Composable
fun PermissionHandler(
    viewModel: ArViewModel,
    locationPermissionState: PermissionUiState = PermissionUiState.Granted,
    onRequestLocation: () -> Unit = {},
    onOpenLocationSettings: () -> Unit = {},
    content: @Composable () -> Unit,
) {
    val context = LocalContext.current
    val locationManager = context.getSystemService(Context.LOCATION_SERVICE) as LocationManager
    val gpsEnabled = runCatching {
        locationManager.isProviderEnabled(LocationManager.GPS_PROVIDER) ||
            locationManager.isProviderEnabled(LocationManager.NETWORK_PROVIDER)
    }.getOrDefault(false)
    val isOnline by viewModel.isOnline.collectAsStateWithLifecycle()
    val sensorAccuracy by viewModel.sensorAccuracy.collectAsStateWithLifecycle()

    ArSystemStatusFrame(
        gpsEnabled = gpsEnabled,
        locationPermissionState = locationPermissionState,
        isOnline = isOnline,
        sensorAccuracy = sensorAccuracy,
        onRequestLocation = onRequestLocation,
        onOpenLocationSettings = onOpenLocationSettings,
        onOpenSystemLocationSettings = {
            runCatching {
                context.startActivity(Intent(Settings.ACTION_LOCATION_SOURCE_SETTINGS))
            }
        },
        content = content,
    )
}

@Composable
internal fun ArSystemStatusFrame(
    gpsEnabled: Boolean,
    locationPermissionState: PermissionUiState,
    isOnline: Boolean,
    sensorAccuracy: Int,
    onRequestLocation: () -> Unit,
    onOpenLocationSettings: () -> Unit,
    onOpenSystemLocationSettings: () -> Unit,
    content: @Composable () -> Unit,
) {
    Box(modifier = Modifier.fillMaxSize()) {
        content()

        var bannerOffset = 0.dp

        val locationStatus = when (locationPermissionState) {
            PermissionUiState.Granted -> null
            PermissionUiState.Approximate -> ArStatusBanner(
                message = "Approximate location — tap for precise distance and bearing",
                action = onRequestLocation,
                color = Color(0xFFFF9800),
            )
            PermissionUiState.Denied -> ArStatusBanner(
                message = "Location is optional — tap to add distance and bearing",
                action = onRequestLocation,
                color = Color(0xFFFF9800),
            )
            PermissionUiState.Loading -> ArStatusBanner(
                message = "Checking optional location access",
                action = null,
                color = Color(0xFF546E7A),
            )
            else -> ArStatusBanner(
                message = "Location is off — tap to open app settings",
                action = onOpenLocationSettings,
                color = Color(0xFFFF9800),
            )
        }
        locationStatus?.let { status ->
            OverlayBanner(
                icon = Icons.Filled.LocationOff,
                text = status.message,
                color = status.color,
                onClick = status.action,
                modifier = Modifier.align(Alignment.TopCenter).padding(top = bannerOffset),
            )
            bannerOffset += 40.dp
        }

        if (locationPermissionState.isUsable() && !gpsEnabled) {
            OverlayBanner(
                icon = Icons.Filled.LocationOff,
                text = "Location services are off — camera AR still works; tap to enable positioning",
                color = Color(0xFFFF9800),
                onClick = onOpenSystemLocationSettings,
                modifier = Modifier.align(Alignment.TopCenter).padding(top = bannerOffset),
            )
            bannerOffset += 40.dp
        }

        if (!isOnline) {
            OverlayBanner(
                icon = Icons.Filled.WifiOff,
                text = "No internet — showing cached data; online updates are paused",
                color = Color(0xFFF44336),
                modifier = Modifier
                    .align(Alignment.TopCenter)
                    .padding(top = bannerOffset),
            )
            bannerOffset += 40.dp
        }

        if (sensorAccuracy <= SensorManager.SENSOR_STATUS_ACCURACY_LOW) {
            OverlayBanner(
                icon = Icons.Filled.Explore,
                text = "Compass needs calibration — wave your phone in a figure-8 pattern",
                color = Color(0xFFFF9800),
                modifier = Modifier
                    .align(Alignment.TopCenter)
                    .padding(top = bannerOffset),
            )
        }
    }
}

/**
 * Non-blocking banner overlay shown at the top of the AR view for warnings
 * that do not prevent the app from functioning.
 */
@Composable
private fun OverlayBanner(
    icon: ImageVector,
    text: String,
    color: Color,
    modifier: Modifier = Modifier,
    onClick: (() -> Unit)? = null
) {
    Row(
        modifier = modifier
            .fillMaxWidth()
            .background(color.copy(alpha = 0.85f))
            .then(if (onClick != null) Modifier.clickable(onClick = onClick) else Modifier)
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
