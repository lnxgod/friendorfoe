package com.friendorfoe.presentation.welcome

import android.content.Intent
import android.net.Uri
import androidx.compose.foundation.clickable
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
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextDecoration
import androidx.compose.ui.unit.dp
import com.friendorfoe.presentation.components.FofSection

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
    Column(
        modifier = Modifier
            .fillMaxSize()
            .testTag("welcome_scroll")
            .verticalScroll(rememberScrollState())
            .padding(horizontal = 24.dp, vertical = 20.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.spacedBy(14.dp),
    ) {
        Text(
            text = "Friend or Foe",
            style = MaterialTheme.typography.headlineLarge,
            fontWeight = FontWeight.Bold,
            color = MaterialTheme.colorScheme.primary,
        )
        Text(
            text = "Inspect nearby aircraft, broadcast drone signals, and supported privacy observations.",
            style = MaterialTheme.typography.bodyLarge,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            textAlign = TextAlign.Center,
        )

        Button(
            onClick = actions.onGetStarted,
            modifier = Modifier
                .fillMaxWidth()
                .defaultMinSize(minHeight = 56.dp)
                .testTag("welcome_get_started"),
            shape = MaterialTheme.shapes.small,
        ) {
            Text(
                text = "Get Started",
                style = MaterialTheme.typography.titleMedium,
                fontWeight = FontWeight.Bold,
            )
        }

        FofSection(
            title = "What this app can tell you",
            modifier = Modifier.testTag("welcome_scope"),
        ) {
            WelcomeFact("Observations are evidence, not proof of identity, intent, or ownership.")
            WelcomeFact(
                "Coverage depends on nearby signals, available data, granted permissions, and configured services.",
            )
        }

        FofSection(title = "Data & permissions") {
            WelcomeFact("History may store observations and phone coordinates locally.")
            WelcomeFact(
                "Network features may exchange location or detection data with the service you use.",
            )
            WelcomeFact(
                "Android asks for access when a feature needs it, not all at once during welcome.",
            )
        }

        FofSection(title = "Optional links") {
            WelcomeLinkRow(
                label = "GameChangers",
                onClick = { actions.onOpenLink(GAMECHANGERS_URL) },
            )
            WelcomeLinkRow(
                label = "GitHub Repository",
                onClick = { actions.onOpenLink(REPOSITORY_URL) },
            )
        }
    }
}

@Composable
private fun WelcomeFact(text: String) {
    Text(
        text = text,
        style = MaterialTheme.typography.bodyMedium,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
        modifier = Modifier.fillMaxWidth(),
    )
}

@Composable
private fun WelcomeLinkRow(label: String, onClick: () -> Unit) {
    Text(
        text = label,
        style = MaterialTheme.typography.bodyMedium,
        fontWeight = FontWeight.Medium,
        color = MaterialTheme.colorScheme.primary,
        textDecoration = TextDecoration.Underline,
        modifier = Modifier
            .fillMaxWidth()
            .defaultMinSize(minHeight = 48.dp)
            .clickable(onClick = onClick)
            .padding(vertical = 12.dp),
    )
}
