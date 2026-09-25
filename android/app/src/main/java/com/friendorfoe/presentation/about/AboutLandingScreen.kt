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
import com.friendorfoe.presentation.components.FofDisclosure
import com.friendorfoe.presentation.components.FofSection

data class AboutLandingActions(
    val onOpenSettings: () -> Unit = {},
    val onOpenReference: () -> Unit = {},
    val onContactSupport: () -> Unit = {},
    val onOpenGithub: () -> Unit = {},
    val onCheckForUpdates: () -> Unit = {},
    val onOpenUpdate: (String) -> Unit = {},
    val onOpenHistory: () -> Unit = {},
    val onOpenBadge: () -> Unit = {},
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
        Spacer(Modifier.height(20.dp))
        FofActionRow("App settings", "Alerts, detection and connections", onClick = actions.onOpenSettings,
            modifier = Modifier.testTag("about_app_settings"))
        HorizontalDivider()
        FofActionRow("History", "Saved observations and flight paths", onClick = actions.onOpenHistory,
            modifier = Modifier.testTag("more_history"))
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
        FofDisclosure("About & support", tag = "more_support") {
            FofActionRow("Contact & feedback", "lnxgod@gmail.com", trailingLabel = "Email",
                onClick = actions.onContactSupport, modifier = Modifier.testTag("about_contact"))
            FofActionRow("GitHub repository", "Source code and releases", onClick = actions.onOpenGithub,
                modifier = Modifier.testTag("about_github"))
            Text("Observations are evidence, not proof of identity, intent, or ownership.",
                style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
    }
}
