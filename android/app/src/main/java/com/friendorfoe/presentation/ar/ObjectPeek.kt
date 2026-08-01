package com.friendorfoe.presentation.ar

import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.friendorfoe.domain.model.DetectionSource
import com.friendorfoe.presentation.components.FofActionRow

internal fun objectPeekEvidence(source: DetectionSource?): String = when (source) {
    DetectionSource.ADS_B -> "ADS-B radio match"
    DetectionSource.REMOTE_ID -> "Remote ID radio match"
    DetectionSource.WIFI_NAN -> "Remote ID radio match (Wi-Fi NaN)"
    DetectionSource.WIFI_BEACON -> "Remote ID radio match (Wi-Fi Beacon)"
    DetectionSource.WIFI -> "Wi-Fi observation"
    null -> "No radio match is currently available"
}

data class ObjectPeekState(
    val objectId: String,
    val title: String,
    val evidence: String,
    val canCapture: Boolean,
)

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun ObjectPeek(
    state: ObjectPeekState,
    onInspect: () -> Unit,
    onCapture: () -> Unit,
    onFullDetails: () -> Unit,
    onDismiss: () -> Unit,
) {
    ModalBottomSheet(onDismissRequest = onDismiss) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 20.dp, vertical = 8.dp),
        ) {
            Text(state.title, style = MaterialTheme.typography.titleMedium)
            Text(
                state.evidence,
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(top = 4.dp, bottom = 8.dp),
            )
            FofActionRow(
                title = "Inspect",
                description = "Open zoom without saving",
                onClick = onInspect,
            )
            FofActionRow(
                title = "Capture",
                description = "Take a photo and review it before saving",
                enabled = state.canCapture,
                onClick = onCapture,
            )
            FofActionRow(
                title = "Full details",
                description = "Open identification details",
                onClick = onFullDetails,
            )
        }
    }
}
