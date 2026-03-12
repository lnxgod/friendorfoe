package com.friendorfoe.presentation.detail

import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier

/**
 * Detail screen showing full information about a specific sky object.
 *
 * TODO: Implement in presentation task:
 * - Aircraft photo (if available)
 * - Full identity: airline, registration, model
 * - Route: origin -> destination
 * - Current stats: altitude, speed, heading
 * - Distance from user
 * - Detection source and confidence indicator
 * - For drones: operator location, manufacturer info
 *
 * @param objectId The unique ID of the sky object to display
 * @param onBack Callback to navigate back
 */
@Composable
fun DetailScreen(
    objectId: String,
    onBack: () -> Unit
) {
    Box(
        modifier = Modifier.fillMaxSize(),
        contentAlignment = Alignment.Center
    ) {
        Text("Detail View for: $objectId - Coming Soon")
        // TODO: Replace with full detail card UI
    }
}
