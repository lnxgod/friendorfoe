package com.friendorfoe.presentation.about

import androidx.compose.foundation.Image
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material3.Button
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import com.friendorfoe.R
import com.friendorfoe.presentation.components.FofActionRow
import com.friendorfoe.presentation.components.FofSecondaryScreenHeader
import com.friendorfoe.presentation.components.PublisherCredit

internal const val PROJECT_SOURCE_URL = "https://github.com/lnxgod/friendorfoe"
private const val DEF_CON_TALK_URL = "https://www.youtube.com/watch?v=VxlqV-vBvy4"
private const val PUBLISHER_MISSION_URL = "https://gamechangersai.org/about.html"

@Composable
internal fun ProjectAboutScreen(onBack: () -> Unit, onOpenLink: (String) -> Unit) {
    Column(Modifier.fillMaxSize().testTag("project_about")) {
        FofSecondaryScreenHeader(title = "About", onBack = onBack)
        Column(
            Modifier.weight(1f).fillMaxWidth().verticalScroll(rememberScrollState())
                .padding(horizontal = 20.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
        ) {
            Column(
                Modifier.widthIn(max = 540.dp).fillMaxWidth().padding(bottom = 24.dp),
                verticalArrangement = Arrangement.spacedBy(12.dp),
            ) {
                Image(
                    painterResource(R.drawable.friend_or_foe_badge),
                    contentDescription = "Friend or Foe by GameChangers AI, with the triangular scanner badge",
                    modifier = Modifier.fillMaxWidth().aspectRatio(1024f / 500f).clip(MaterialTheme.shapes.large),
                )
                Text("DEF CON 34 · Packet Hacking Village", style = MaterialTheme.typography.labelLarge,
                    color = MaterialTheme.colorScheme.primary)
                Text("Why couldn’t I see my own drone?", style = MaterialTheme.typography.headlineSmall,
                    fontWeight = FontWeight.SemiBold, modifier = Modifier.semantics { heading() })
                Text(
                    "When their Android app missed their own drone, Will Hatzer and Charles Grow " +
                        "followed its radio signals. Hear how that experiment shaped Friend or Foe and its scanner badge.",
                    style = MaterialTheme.typography.bodyMedium,
                )
                Button(onClick = { onOpenLink(DEF_CON_TALK_URL) },
                    contentPadding = PaddingValues(horizontal = 16.dp, vertical = 10.dp)) {
                    Icon(Icons.Default.PlayArrow, contentDescription = null, modifier = Modifier.size(20.dp))
                    Spacer(Modifier.width(8.dp))
                    Text("Watch the talk on YouTube")
                }
                HorizontalDivider()
                PublisherCredit(onOpenLink)
                Text(
                    "GameChangers AI is a nonprofit helping children and adults in rural communities " +
                        "learn AI by making games, building tools and testing their own ideas.",
                    style = MaterialTheme.typography.bodyMedium,
                )
                FofActionRow("About GameChangers AI", "Our mission and how we teach",
                    onClick = { onOpenLink(PUBLISHER_MISSION_URL) })
                HorizontalDivider()
                FofActionRow("Source code on GitHub", "Explore the Android app and badge hardware",
                    onClick = { onOpenLink(PROJECT_SOURCE_URL) })
            }
        }
    }
}
