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
    onOpen: () -> Unit,
    testTagPrefix: String,
) {
    val checkTag = "${testTagPrefix}_check_updates"
    when (update) {
        UpdateUiState.Idle -> FofActionRow(
            title = "App updates",
            description = "Check Google Play for an update",
            trailingLabel = "Check",
            onClick = onCheck,
            modifier = Modifier.testTag(checkTag),
        )
        UpdateUiState.Checking -> FofActionRow(
            title = "Checking for updates",
            description = "Contacting Google Play",
            trailingLabel = "Checking…",
            enabled = false,
            onClick = onCheck,
            modifier = Modifier.testTag(checkTag),
        )
        is UpdateUiState.UpToDate -> FofActionRow(
            title = "Up to date",
            description = "Google Play has no update for this installation",
            trailingLabel = "Check again",
            onClick = onCheck,
            modifier = Modifier.testTag(checkTag),
        )
        UpdateUiState.Available -> FofActionRow(
            title = "Update available",
            description = "Open Friend or Foe in Google Play to update",
            trailingLabel = "Update",
            onClick = onOpen,
            modifier = Modifier.testTag("${testTagPrefix}_open_update"),
        )
        is UpdateUiState.Failed -> Column {
            FofActionRow(
                title = update.message,
                description = "Google Play could not check this installation.",
                trailingLabel = "Retry",
                onClick = onCheck,
                modifier = Modifier.testTag(checkTag),
            )
            FofActionRow(
                title = "View in Google Play",
                description = "Open the official store listing",
                trailingLabel = "Open",
                onClick = onOpen,
                modifier = Modifier.testTag("${testTagPrefix}_play_listing"),
            )
        }
    }
}
