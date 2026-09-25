package com.friendorfoe.presentation.about

import androidx.compose.foundation.layout.Column
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import com.friendorfoe.presentation.components.FofActionRow

@Composable
internal fun AppUpdateRow(
    update: UpdateUiState,
    onCheck: () -> Unit,
    onOpen: (String) -> Unit,
    testTagPrefix: String,
) {
    val checkTag = "${testTagPrefix}_check_updates"
    when (update) {
        UpdateUiState.Idle -> FofActionRow(
            title = "App updates",
            description = "Check for a newer Android release",
            trailingLabel = "Check",
            onClick = onCheck,
            modifier = Modifier.testTag(checkTag),
        )
        UpdateUiState.Checking -> FofActionRow(
            title = "Checking for updates",
            description = "Checking GitHub releases",
            trailingLabel = "Checking…",
            enabled = false,
            onClick = onCheck,
            modifier = Modifier.testTag(checkTag),
        )
        is UpdateUiState.UpToDate -> FofActionRow(
            title = "Up to date",
            description = "No newer release available",
            trailingLabel = "Check again",
            onClick = onCheck,
            modifier = Modifier.testTag(checkTag),
        )
        is UpdateUiState.Available -> Column {
            FofActionRow(
                title = "Update available",
                description = "Version ${update.remote.version.name}",
                trailingLabel = if (update.remote.apkUrl != null) "Download APK" else "Open",
                onClick = { onOpen(update.remote.apkUrl ?: update.remote.releaseUrl) },
                modifier = Modifier.testTag("${testTagPrefix}_open_update"),
            )
            if (update.remote.apkUrl != null) FofActionRow(
                title = "Release notes",
                description = "See changes and installation information",
                trailingLabel = "View",
                onClick = { onOpen(update.remote.releaseUrl) },
                modifier = Modifier.testTag("${testTagPrefix}_release_notes"),
            )
        }
        is UpdateUiState.Failed -> FofActionRow(
            title = update.message,
            description = "Try again in a moment.",
            trailingLabel = "Retry",
            onClick = onCheck,
            modifier = Modifier.testTag(checkTag),
        )
    }
}
