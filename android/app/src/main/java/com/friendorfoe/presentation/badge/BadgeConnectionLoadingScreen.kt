package com.friendorfoe.presentation.badge

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.unit.dp

@Composable
fun BadgeConnectionLoadingScreen() {
    Column(
        Modifier.fillMaxSize().padding(16.dp).testTag("screen_badge"),
        verticalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        Text("Badge", style = MaterialTheme.typography.headlineSmall)
        Text("Checking for a verified Friend or Foe badge connection")
        LinearProgressIndicator(Modifier.fillMaxWidth())
    }
}
