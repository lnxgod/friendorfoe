package com.friendorfoe.presentation.ar

import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier

/**
 * AR Viewfinder screen - the main screen of the app.
 *
 * TODO: Implement in AR overlay task:
 * - CameraX preview as background
 * - Sensor fusion for device orientation
 * - Floating color-coded labels on detected sky objects
 * - Status bar showing counts and sensor health
 * - Tap handling to navigate to detail
 *
 * @param onObjectTapped Callback when a sky object label is tapped
 */
@Composable
fun ArViewScreen(
    onObjectTapped: (String) -> Unit
) {
    Box(
        modifier = Modifier.fillMaxSize(),
        contentAlignment = Alignment.Center
    ) {
        Text("AR Viewfinder - Coming Soon")
        // TODO: Replace with CameraX preview + AR overlay
    }
}
