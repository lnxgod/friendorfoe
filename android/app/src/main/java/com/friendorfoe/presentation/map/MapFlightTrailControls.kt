package com.friendorfoe.presentation.map

import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.material3.*
import androidx.compose.runtime.Composable
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
    Column(Modifier.fillMaxWidth().padding(horizontal = 12.dp)) {
        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
            Text("Flight trails", style = MaterialTheme.typography.titleSmall, modifier = Modifier.padding(top = 12.dp))
            TextButton(onClick = onFit, enabled = state.trails.isNotEmpty(), modifier = Modifier.testTag("fit_flight_trails")) {
                Text("Fit trails")
            }
        }
        Row(Modifier.horizontalScroll(rememberScrollState()), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            FlightTrailWindow.entries.forEach { option ->
                FilterChip(selected = window == option, onClick = { onWindow(option) }, label = { Text(option.label) },
                    modifier = Modifier.testTag("flight_trails_${option.name}"))
            }
        }
        if (window != FlightTrailWindow.OFF) {
            when {
                state.error -> TextButton(onClick = onRetry) { Text("Couldn't load trails · Retry") }
                state.loading -> Text("Loading recorded paths…", style = MaterialTheme.typography.bodySmall)
                state.trails.isEmpty() -> Text("No recorded paths match this view yet. Keep aircraft detection running.",
                    style = MaterialTheme.typography.bodySmall)
                else -> Text(
                    "${state.trails.size} recorded ${if (state.trails.size == 1) "path" else "paths"}${if (state.trails.size == 40) " (up to 40 shown)" else ""} · dots mark last received positions. Tap a dot to review.",
                    style = MaterialTheme.typography.bodySmall,
                )
            }
            Spacer(Modifier.height(8.dp))
        }
    }
}
