package com.friendorfoe.presentation.welcome

import androidx.compose.foundation.layout.*
import androidx.compose.material3.Icon
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Flight
import androidx.compose.material.icons.filled.Map
import androidx.compose.material.icons.filled.Shield
import androidx.compose.ui.graphics.vector.ImageVector
import com.friendorfoe.presentation.components.FofDisclosure
import android.content.Intent
import android.net.Uri
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.defaultMinSize
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp

private const val GAMECHANGERS_URL = "https://gamechangersai.org"
private const val REPOSITORY_URL = "https://github.com/lnxgod/friendorfoe"

data class WelcomeActions(
    val onGetStarted: () -> Unit = {},
    val onOpenLink: (String) -> Unit = {},
)

@Composable
fun WelcomeScreen(onGetStarted: () -> Unit) {
    val context = LocalContext.current
    WelcomeContent(
        actions = WelcomeActions(
            onGetStarted = onGetStarted,
            onOpenLink = { url ->
                runCatching {
                    context.startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(url)))
                }
            },
        ),
    )
}

@Composable
fun WelcomeContent(actions: WelcomeActions) {
    Column(Modifier.fillMaxSize().testTag("welcome_frame")
        .windowInsetsPadding(androidx.compose.foundation.layout.WindowInsets.safeDrawing)
        .padding(horizontal = 24.dp, vertical = 16.dp)) {
        Column(Modifier.weight(1f).verticalScroll(rememberScrollState()).testTag("welcome_scroll"),
            verticalArrangement = Arrangement.spacedBy(20.dp)) {
            Text("Friend or Foe", style = MaterialTheme.typography.headlineLarge, fontWeight = FontWeight.Bold)
            Text("Aircraft, drones and nearby signals", style = MaterialTheme.typography.titleMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant)
            WelcomeFeature(Icons.Default.Flight, "Nearby", "See what is close, with distance first.")
            WelcomeFeature(Icons.Default.Map, "Map & trails", "Follow received positions and review flight paths.")
            WelcomeFeature(Icons.Default.Shield, "Privacy", "Inspect nearby signals and their evidence.")
            Text("Observations are evidence, not proof of identity, intent, or ownership.",
                style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.testTag("welcome_scope"))
            FofDisclosure("Data & permissions", tag = "welcome_data") {
                WelcomeFact("History may store observations and phone coordinates locally.")
                WelcomeFact("Network features may exchange location or detection data with the service you use.")
                WelcomeFact("Android asks for access when a feature needs it.")
                WelcomeFact("Coverage depends on nearby signals, permissions and configured services.")
                WelcomeLinkRow("GameChangers") { actions.onOpenLink(GAMECHANGERS_URL) }
                WelcomeLinkRow("GitHub Repository") { actions.onOpenLink(REPOSITORY_URL) }
            }
        }
        Text("Permissions are requested when you use a feature.",
            style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.padding(vertical = 12.dp))
        Button(onClick = actions.onGetStarted, modifier = Modifier.fillMaxWidth()
            .defaultMinSize(minHeight = 56.dp).testTag("welcome_get_started")) {
            Text("Get started", style = MaterialTheme.typography.titleMedium)
        }
    }
}

@Composable
private fun WelcomeFeature(icon: ImageVector, title: String, detail: String) {
    Row(horizontalArrangement = Arrangement.spacedBy(16.dp), verticalAlignment = Alignment.CenterVertically) {
        Icon(icon, contentDescription = null, tint = MaterialTheme.colorScheme.primary, modifier = Modifier.size(28.dp))
        Column {
            Text(title, style = MaterialTheme.typography.titleMedium, fontWeight = FontWeight.SemiBold)
            Text(detail, style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
    }
}

@Composable
private fun WelcomeFact(text: String) {
    Text(text, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant,
        modifier = Modifier.fillMaxWidth().padding(bottom = 8.dp))
}

@Composable
private fun WelcomeLinkRow(label: String, onClick: () -> Unit) {
    androidx.compose.material3.TextButton(onClick = onClick) { Text(label) }
}
