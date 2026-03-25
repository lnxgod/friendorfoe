package com.friendorfoe.presentation.welcome

import android.content.Intent
import android.net.Uri
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ElevatedCard
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextDecoration
import androidx.compose.ui.unit.dp
import com.friendorfoe.BuildConfig
import com.google.gson.JsonParser
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import okhttp3.OkHttpClient
import okhttp3.Request

private sealed class UpdateState {
    data object Idle : UpdateState()
    data object Loading : UpdateState()
    data class UpToDate(val version: String) : UpdateState()
    data class UpdateAvailable(val version: String, val url: String) : UpdateState()
    data class Error(val message: String) : UpdateState()
}

@Composable
fun WelcomeScreen(onGetStarted: () -> Unit) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    var updateState by remember { mutableStateOf<UpdateState>(UpdateState.Idle) }

    Column(
        modifier = Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(24.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.spacedBy(16.dp)
    ) {
        Spacer(modifier = Modifier.height(32.dp))

        // App title
        Text(
            text = "Friend or Foe",
            style = MaterialTheme.typography.headlineLarge,
            fontWeight = FontWeight.Bold,
            color = MaterialTheme.colorScheme.primary
        )

        Text(
            text = "Real-time aircraft & drone identification",
            style = MaterialTheme.typography.bodyLarge,
            color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.7f),
            textAlign = TextAlign.Center
        )

        Spacer(modifier = Modifier.height(8.dp))

        // Thank you card
        SectionCard(title = "Thanks for trying Friend or Foe!") {
            Text(
                text = "Point your phone at the sky to identify aircraft and drones " +
                    "using ADS-B, FAA Remote ID, and WiFi detection with augmented reality.",
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.7f)
            )
        }

        // Links card
        SectionCard(title = "Links") {
            Text(
                text = "GameChangers",
                style = MaterialTheme.typography.bodyMedium,
                fontWeight = FontWeight.Medium,
                color = MaterialTheme.colorScheme.primary,
                textDecoration = TextDecoration.Underline,
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(vertical = 4.dp)
                    .clickable {
                        context.startActivity(
                            Intent(Intent.ACTION_VIEW, Uri.parse("https://gamechangersai.org"))
                        )
                    }
            )

            Spacer(modifier = Modifier.height(4.dp))

            Text(
                text = "GitHub Repository",
                style = MaterialTheme.typography.bodyMedium,
                fontWeight = FontWeight.Medium,
                color = MaterialTheme.colorScheme.primary,
                textDecoration = TextDecoration.Underline,
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(vertical = 4.dp)
                    .clickable {
                        context.startActivity(
                            Intent(
                                Intent.ACTION_VIEW,
                                Uri.parse("https://github.com/lnxgod/friendorfoe")
                            )
                        )
                    }
            )
        }

        // Check for updates
        SectionCard(title = "Updates") {
            Text(
                text = "Version ${BuildConfig.VERSION_NAME}",
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.7f)
            )

            Spacer(modifier = Modifier.height(8.dp))

            Button(
                onClick = {
                    updateState = UpdateState.Loading
                    scope.launch {
                        updateState = checkForUpdates()
                    }
                },
                enabled = updateState !is UpdateState.Loading,
                modifier = Modifier.fillMaxWidth()
            ) {
                if (updateState is UpdateState.Loading) {
                    CircularProgressIndicator(
                        modifier = Modifier.size(18.dp),
                        strokeWidth = 2.dp,
                        color = MaterialTheme.colorScheme.onPrimary
                    )
                } else {
                    Text("Check for Updates")
                }
            }

            // Update result
            when (val state = updateState) {
                is UpdateState.UpToDate -> {
                    Spacer(modifier = Modifier.height(8.dp))
                    Text(
                        text = "You're on the latest version!",
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.primary
                    )
                }
                is UpdateState.UpdateAvailable -> {
                    Spacer(modifier = Modifier.height(8.dp))
                    Text(
                        text = "Version ${state.version} available",
                        style = MaterialTheme.typography.bodyMedium,
                        fontWeight = FontWeight.Medium
                    )
                    Text(
                        text = "Download from GitHub Releases",
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.primary,
                        textDecoration = TextDecoration.Underline,
                        modifier = Modifier
                            .padding(top = 4.dp)
                            .clickable {
                                context.startActivity(
                                    Intent(Intent.ACTION_VIEW, Uri.parse(state.url))
                                )
                            }
                    )
                }
                is UpdateState.Error -> {
                    Spacer(modifier = Modifier.height(8.dp))
                    Text(
                        text = state.message,
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.error
                    )
                }
                else -> { /* Idle or Loading — no extra text */ }
            }
        }

        Spacer(modifier = Modifier.height(16.dp))

        // Get Started button
        Button(
            onClick = onGetStarted,
            modifier = Modifier
                .fillMaxWidth()
                .height(56.dp),
            shape = RoundedCornerShape(12.dp),
            colors = ButtonDefaults.buttonColors(
                containerColor = MaterialTheme.colorScheme.primary
            )
        ) {
            Text(
                text = "Get Started",
                style = MaterialTheme.typography.titleMedium,
                fontWeight = FontWeight.Bold
            )
        }

        Spacer(modifier = Modifier.height(24.dp))
    }
}

@Composable
private fun SectionCard(title: String, content: @Composable () -> Unit) {
    ElevatedCard(
        modifier = Modifier.fillMaxWidth(),
        shape = MaterialTheme.shapes.medium,
        colors = CardDefaults.elevatedCardColors(
            containerColor = MaterialTheme.colorScheme.surfaceContainerLow
        ),
        elevation = CardDefaults.elevatedCardElevation(defaultElevation = 1.dp)
    ) {
        Column(modifier = Modifier.padding(16.dp)) {
            Text(
                text = title,
                style = MaterialTheme.typography.titleSmall,
                fontWeight = FontWeight.Bold,
                color = MaterialTheme.colorScheme.primary
            )
            Spacer(modifier = Modifier.height(8.dp))
            HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
            Spacer(modifier = Modifier.height(8.dp))
            content()
        }
    }
}

private suspend fun checkForUpdates(): UpdateState {
    return withContext(Dispatchers.IO) {
        try {
            val client = OkHttpClient()
            val request = Request.Builder()
                .url("https://api.github.com/repos/lnxgod/friendorfoe/releases/latest")
                .header("Accept", "application/vnd.github.v3+json")
                .build()
            val response = client.newCall(request).execute()
            if (!response.isSuccessful) {
                return@withContext UpdateState.Error("Couldn't check — try again later")
            }
            val body = response.body?.string()
                ?: return@withContext UpdateState.Error("Couldn't check — try again later")

            val json = JsonParser.parseString(body).asJsonObject
            val tagName = json.get("tag_name")?.asString
                ?: return@withContext UpdateState.Error("Couldn't check — try again later")
            val htmlUrl = json.get("html_url")?.asString
                ?: "https://github.com/lnxgod/friendorfoe/releases"

            val latestVersion = tagName.removePrefix("v")
            val currentVersion = BuildConfig.VERSION_NAME

            if (latestVersion == currentVersion) {
                UpdateState.UpToDate(currentVersion)
            } else {
                UpdateState.UpdateAvailable(latestVersion, htmlUrl)
            }
        } catch (_: Exception) {
            UpdateState.Error("Couldn't check — try again later")
        }
    }
}
