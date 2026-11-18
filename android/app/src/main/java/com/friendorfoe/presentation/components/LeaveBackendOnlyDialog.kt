package com.friendorfoe.presentation.components

import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable

@Composable
internal fun LeaveBackendOnlyDialog(onConfirm: () -> Unit, onDismiss: () -> Unit) {
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("Leave backend-only mode?") },
        text = {
            Text("Phone scanning turns off backend-only mode. Other enabled phone sensors can resume, " +
                "including ultrasonic sampling if microphone access is allowed.")
        },
        confirmButton = { TextButton(onClick = onConfirm) { Text("Use phone scanning") } },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Keep backend-only mode") } },
    )
}
