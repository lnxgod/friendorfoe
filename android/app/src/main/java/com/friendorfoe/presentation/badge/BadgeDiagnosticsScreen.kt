package com.friendorfoe.presentation.badge

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.hilt.navigation.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle

@Composable
fun BadgeDiagnosticsRoute(
    onBack: () -> Unit,
    viewModel: BadgeViewModel = hiltViewModel(),
) {
    val state by viewModel.uiState.collectAsStateWithLifecycle()
    BadgeDiagnosticsContent(
        state = state,
        onBack = onBack,
        onRefresh = viewModel::refresh,
    )
}

@Composable
fun BadgeDiagnosticsContent(
    state: BadgeUiState,
    onBack: () -> Unit,
    onRefresh: () -> Unit,
) {
    Column(
        Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(16.dp)
            .testTag("screen_badge_diagnostics"),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Row(
            Modifier.fillMaxWidth(),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            IconButton(
                onClick = onBack,
                modifier = Modifier.heightIn(min = 48.dp),
            ) {
                Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "Back")
            }
            Column(Modifier.weight(1f)) {
                Text(
                    "Badge diagnostics",
                    style = MaterialTheme.typography.headlineSmall,
                    fontWeight = FontWeight.SemiBold,
                )
                Text(
                    "Read-only device evidence",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            IconButton(
                onClick = onRefresh,
                modifier = Modifier.heightIn(min = 48.dp),
            ) {
                Icon(Icons.Default.Refresh, contentDescription = "Refresh diagnostics")
            }
        }

        BadgeSectionCard {
            Text("Connection", style = MaterialTheme.typography.titleMedium)
            DiagnosticLine("Transport ${transportLabel(state.connection.transport)}")
            DiagnosticLine("Target ${state.connection.targetId ?: "none"}")
            DiagnosticLine(
                "Phase ${state.connection.phase.name.lowercase().replace('_', ' ')}",
            )
            DiagnosticLine("Protocol ${state.connection.protocolVersion ?: "unknown"}")
        }

        val status = state.controlStatus
        if (status == null) {
            BadgeSectionCard {
                Text("No fresh device diagnostics are available.")
            }
        } else {
            BadgeSectionCard {
                Text("Firmware and configuration", style = MaterialTheme.typography.titleMedium)
                DiagnosticLine("Firmware ${status.version}")
                DiagnosticLine("Theme hash ${hashCode32(status.themeReadback.hash)}")
                DiagnosticLine("Policy hash ${hashCode32(status.policyReadback.hash)}")
                DiagnosticLine(
                    "Persisted network ${status.networkModeReadback.value?.wireValue ?: "unknown"}",
                )
                DiagnosticLine(
                    "Runtime network ${status.reporting.networkMode.ifBlank { "unknown" }}",
                )
                DiagnosticLine("Safe mode ${if (status.safeMode) "on" else "off"}")
                DiagnosticLine("Reset ${status.resetReason.ifBlank { "unknown" }}")
                DiagnosticLine("Crash count ${status.crashCount}")
                DiagnosticLine("Recovery mode ${status.recoveryMode.ifBlank { "unknown" }}")
            }

            BadgeSectionCard {
                Text("Memory", style = MaterialTheme.typography.titleMedium)
                status.stackFreeBytes.forEach { (name, bytes) ->
                    DiagnosticLine("Stack $name: $bytes B")
                }
                DiagnosticLine("Internal heap free: ${status.heapInternalFreeBytes} B")
                DiagnosticLine("Internal heap minimum: ${status.heapInternalMinimumFreeBytes} B")
                DiagnosticLine("PSRAM free: ${status.psramFreeBytes} B")
            }

            BadgeSectionCard {
                Text("Scanners", style = MaterialTheme.typography.titleMedium)
                if (status.scanners.isEmpty()) {
                    DiagnosticLine("No scanners reported")
                }
                status.scanners.forEach { scanner ->
                    DiagnosticLine(
                        "Scanner ${scanner.slot} · ${scanner.health.ifBlank { "unknown" }}",
                    )
                    DiagnosticLine("Connected ${scanner.connected}")
                    DiagnosticLine("Scan profile ${scanner.scanProfile.ifBlank { "unknown" }}")
                    DiagnosticLine("Policy hash ${hashCode32(scanner.displayPolicyHash)}")
                    DiagnosticLine(
                        "Policy acknowledgement ${hashCode32(scanner.displayPolicyAckHash)}",
                    )
                    DiagnosticLine("Target ${scanner.targetVersion.ifBlank { "unknown" }}")
                    DiagnosticLine("OTA ${scanner.otaState.ifBlank { "unknown" }}")
                    DiagnosticLine(
                        "Last firmware error: ${scanner.lastFirmwareError.ifBlank { "none" }}",
                    )
                }
            }
        }
    }
}

@Composable
private fun DiagnosticLine(text: String) {
    Text(
        text,
        style = MaterialTheme.typography.bodySmall,
        fontFamily = FontFamily.Monospace,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
    )
}
