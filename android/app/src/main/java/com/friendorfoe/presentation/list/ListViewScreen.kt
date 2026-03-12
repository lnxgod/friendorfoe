package com.friendorfoe.presentation.list

import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier

/**
 * List View screen showing all detected sky objects sorted by distance.
 *
 * TODO: Implement in presentation task:
 * - LazyColumn of detected aircraft and drones
 * - Color-coded category indicators
 * - Distance, altitude, and direction info
 * - Tap to navigate to detail card
 * - Pull-to-refresh
 *
 * @param onObjectTapped Callback when a list item is tapped
 */
@Composable
fun ListViewScreen(
    onObjectTapped: (String) -> Unit
) {
    Box(
        modifier = Modifier.fillMaxSize(),
        contentAlignment = Alignment.Center
    ) {
        Text("List View - Coming Soon")
        // TODO: Replace with LazyColumn of SkyObject items
    }
}
