package com.friendorfoe.presentation.about

import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.layout.*
import androidx.compose.material3.*
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import com.friendorfoe.BuildConfig
import com.friendorfoe.presentation.components.FofActionRow
import com.friendorfoe.presentation.components.FofSection
import com.friendorfoe.presentation.components.PublisherCredit

data class AboutLandingActions(
    val onOpenSettings: () -> Unit = {},
    val onOpenReference: () -> Unit = {},
    val onOpenSource: (String) -> Unit = {},
    val onCheckForUpdates: () -> Unit = {},
    val onOpenUpdate: () -> Unit = {},
    val onOpenProbes: () -> Unit = {},
    val onOpenSavedFlights: () -> Unit = {},
    val onOpenHistory: () -> Unit = {},
    val onOpenBadge: () -> Unit = {},
    val onOpenPublisher: (String) -> Unit = {},
)

@Composable
fun AboutLandingScreen(
    actions: AboutLandingActions,
    installedVersionName: String = BuildConfig.VERSION_NAME,
    updateState: UpdateUiState = UpdateUiState.Idle,
    modifier: Modifier = Modifier,
) {
    Column(
        modifier.fillMaxSize().testTag("about_landing")
            .verticalScroll(rememberScrollState()).padding(horizontal = 20.dp, vertical = 20.dp),
    ) {
        Text("More", style = MaterialTheme.typography.headlineMedium, fontWeight = FontWeight.SemiBold)
        Text("Friend or Foe", style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant)
        Spacer(Modifier.height(8.dp))
        PublisherCredit(actions.onOpenPublisher)
        Spacer(Modifier.height(12.dp))
        FofActionRow("App settings", "Alerts, detection and connections", onClick = actions.onOpenSettings,
            modifier = Modifier.testTag("about_app_settings"))
        HorizontalDivider()
        FofActionRow("Wi-Fi probes", "See which networks nearby devices request", onClick = actions.onOpenProbes,
            modifier = Modifier.testTag("more_probes"))
        HorizontalDivider()
        FofActionRow("History", "Saved observations and flight paths", onClick = actions.onOpenHistory,
            modifier = Modifier.testTag("more_history"))
        HorizontalDivider()
        FofActionRow("Saved flights", "Flights you chose to keep", onClick = actions.onOpenSavedFlights,
            modifier = Modifier.testTag("more_saved_flights"))
        HorizontalDivider()
        FofActionRow("Badge", "Connect and manage your scanner", onClick = actions.onOpenBadge,
            modifier = Modifier.testTag("more_badge"))
        HorizontalDivider()
        FofActionRow("Reference guide", "Identify aircraft and drones", onClick = actions.onOpenReference,
            modifier = Modifier.testTag("about_reference"))
        Spacer(Modifier.height(16.dp))
        FofSection("Version & updates") {
            Text("Version $installedVersionName", style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant)
            AppUpdateRow(updateState, actions.onCheckForUpdates, actions.onOpenUpdate, testTagPrefix = "about")
        }
        FofActionRow(
            title = "Source code on GitHub",
            description = "Browse the Friend or Foe project",
            onClick = { actions.onOpenSource("https://github.com/lnxgod/friendorfoe") },
            modifier = Modifier.testTag("about_github"),
        )
        Text("Observations are evidence, not proof of identity, intent, or ownership.",
            style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
    }
}
