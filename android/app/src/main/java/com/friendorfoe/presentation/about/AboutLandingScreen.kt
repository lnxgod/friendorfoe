package com.friendorfoe.presentation.about

import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.defaultMinSize
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.material3.Button
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import com.friendorfoe.BuildConfig
import com.friendorfoe.presentation.badge.BadgeMarkGold
import com.friendorfoe.presentation.badge.BadgeMarkIcon
import com.friendorfoe.presentation.components.FofActionRow
import com.friendorfoe.presentation.components.FofSection

data class AboutLandingActions(
    val onOpenSettings: () -> Unit = {},
    val onOpenReference: () -> Unit = {},
    val onContactSupport: () -> Unit = {},
    val onOpenGithub: () -> Unit = {},
)

@Composable
fun AboutLandingScreen(
    actions: AboutLandingActions,
    modifier: Modifier = Modifier,
) {
    Column(
        modifier = modifier
            .fillMaxSize()
            .testTag("about_landing")
            .verticalScroll(rememberScrollState())
            .padding(horizontal = 20.dp, vertical = 16.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Icon(
            imageVector = BadgeMarkIcon,
            contentDescription = null,
            tint = BadgeMarkGold,
            modifier = Modifier.size(64.dp).testTag("about_triforce"),
        )
        Text(
            text = "Friend or Foe",
            style = MaterialTheme.typography.headlineLarge,
            fontWeight = FontWeight.Bold,
            textAlign = TextAlign.Center,
        )
        Text(
            text = "Inspect nearby aircraft, broadcast drone signals, and supported privacy observations.",
            style = MaterialTheme.typography.bodyLarge,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            textAlign = TextAlign.Center,
        )
        Text(
            text = "Were you at our DEF CON talk? Thank you for coming—we're glad you're here.",
            style = MaterialTheme.typography.titleSmall,
            color = MaterialTheme.colorScheme.primary,
            textAlign = TextAlign.Center,
        )
        FofSection(title = "Use observations carefully") {
            Text(
                text = "Observations are evidence, not proof of identity, intent, or ownership.",
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
        Button(
            onClick = actions.onOpenSettings,
            modifier = Modifier
                .fillMaxWidth()
                .defaultMinSize(minHeight = 56.dp)
                .testTag("about_app_settings"),
        ) {
            Text("App settings", fontWeight = FontWeight.Bold)
        }
        FofSection(title = "Helpful links") {
            FofActionRow(
                title = "Reference guide",
                description = "Browse aircraft, drones, and category guidance",
                onClick = actions.onOpenReference,
                modifier = Modifier.testTag("about_reference"),
            )
            FofActionRow(
                title = "Contact & feedback",
                description = "lnxgod@gmail.com",
                trailingLabel = "Email",
                onClick = actions.onContactSupport,
                modifier = Modifier.testTag("about_contact"),
            )
            FofActionRow(
                title = "GitHub repository",
                description = "View the project source and releases",
                onClick = actions.onOpenGithub,
                modifier = Modifier.testTag("about_github"),
            )
        }
        Text(
            text = "Version ${BuildConfig.VERSION_NAME}",
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
}
