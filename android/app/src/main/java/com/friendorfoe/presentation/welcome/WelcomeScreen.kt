package com.friendorfoe.presentation.welcome

import androidx.compose.foundation.layout.*
import androidx.compose.material3.Icon
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Flight
import androidx.compose.material.icons.filled.Map
import androidx.compose.material.icons.filled.Shield
import androidx.compose.ui.graphics.vector.ImageVector
import com.friendorfoe.presentation.about.openUri
import com.friendorfoe.presentation.components.FofDisclosure
import com.friendorfoe.presentation.components.PublisherCredit
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


data class WelcomeActions(
    val onGetStarted: () -> Unit = {},
    val onOpenPublisher: (String) -> Unit = {},
)

@Composable
fun WelcomeScreen(onGetStarted: () -> Unit) {
    val context = LocalContext.current
    WelcomeContent(WelcomeActions(onGetStarted = onGetStarted, onOpenPublisher = context::openUri))
}

@Composable
fun WelcomeContent(actions: WelcomeActions) {
    Column(Modifier.fillMaxSize().testTag("welcome_frame")
        .windowInsetsPadding(androidx.compose.foundation.layout.WindowInsets.safeDrawing)
        .padding(horizontal = 24.dp, vertical = 16.dp)) {
        Column(Modifier.weight(1f).verticalScroll(rememberScrollState()).testTag("welcome_scroll"),
            verticalArrangement = Arrangement.spacedBy(20.dp)) {
            Text("Friend or Foe", style = MaterialTheme.typography.headlineLarge, fontWeight = FontWeight.Bold)
            Text("Explore nearby devices and aircraft", style = MaterialTheme.typography.titleMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant)
            PublisherCredit(actions.onOpenPublisher)
            WelcomeFeature(Icons.Default.Shield, "Privacy", "Look for personal recorders, cameras and trackers.")
            WelcomeFeature(Icons.Default.Flight, "Nearby", "See aircraft and drones sorted by distance.")
            WelcomeFeature(Icons.Default.Map, "Map & trails", "Follow aircraft and review flight paths.")
            Text("Observations are evidence, not proof of identity, intent, or ownership.",
                style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.testTag("welcome_scope"))
            FofDisclosure("Data & permissions", tag = "welcome_data") {
                WelcomeFact("History may store observations and phone coordinates locally.")
                WelcomeFact("Network features may exchange location or detection data with the service you use.")
                WelcomeFact("Android asks for access when a feature needs it.")
                WelcomeFact("Coverage depends on nearby signals, permissions and configured services.")
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
