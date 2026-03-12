package com.friendorfoe.presentation.history

import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier

/**
 * History screen showing past detection records from the Room database.
 *
 * TODO: Implement in presentation task:
 * - LazyColumn of HistoryEntity items
 * - Date grouping headers
 * - Filter by type (aircraft/drone)
 * - Delete/clear history actions
 * - Tap to view detail
 *
 * @param onEntryTapped Callback when a history entry is tapped
 */
@Composable
fun HistoryScreen(
    onEntryTapped: (String) -> Unit
) {
    Box(
        modifier = Modifier.fillMaxSize(),
        contentAlignment = Alignment.Center
    ) {
        Text("History - Coming Soon")
        // TODO: Replace with LazyColumn of history entries
    }
}
