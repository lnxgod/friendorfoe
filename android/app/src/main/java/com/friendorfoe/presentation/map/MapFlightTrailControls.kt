package com.friendorfoe.presentation.map

import androidx.compose.foundation.layout.*
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.ExpandMore
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.unit.dp

@Composable
internal fun MapFlightTrailControls(
    window: FlightTrailWindow,
    state: MapFlightTrailsState,
    onWindow: (FlightTrailWindow) -> Unit,
    onFit: () -> Unit,
    onRetry: () -> Unit,
) {
    var menuOpen by remember { mutableStateOf(false) }
    Column(Modifier.fillMaxWidth().padding(horizontal = 16.dp)) {
        Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
            Box(Modifier.weight(1f)) {
                TextButton(onClick = { menuOpen = true }, modifier = Modifier.testTag("flight_trails_menu")) {
                    Text("Trails · ${window.label}")
                    Icon(Icons.Default.ExpandMore, contentDescription = null)
                }
                DropdownMenu(expanded = menuOpen, onDismissRequest = { menuOpen = false }) {
                    FlightTrailWindow.entries.forEach { option ->
                        DropdownMenuItem(
                            text = { Text(option.label) },
                            onClick = { onWindow(option); menuOpen = false },
                            leadingIcon = { RadioButton(selected = window == option, onClick = null) },
                            modifier = Modifier.testTag("flight_trails_${option.name}"),
                        )
                    }
                }
            }
            TextButton(onClick = onFit, enabled = state.trails.isNotEmpty(), modifier = Modifier.testTag("fit_flight_trails")) {
                Text("Fit trails")
            }
        }
        if (window != FlightTrailWindow.OFF) {
            if (state.error) {
                TextButton(onClick = onRetry) { Text("Couldn't load trails · Retry") }
            } else {
                Text(when {
                    state.loading -> "Loading recorded paths…"
                    state.trails.isEmpty() -> "No recorded paths yet. Keep aircraft detection running."
                    else -> "${state.trails.size} paths · tap an endpoint to review"
                }, style = MaterialTheme.typography.labelSmall, color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.padding(start = 12.dp, bottom = 8.dp))
            }
        }
    }
}
