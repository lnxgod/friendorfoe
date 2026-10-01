package com.friendorfoe.presentation.probes

import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.horizontalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Modifier
import androidx.compose.ui.Alignment
import androidx.compose.ui.hapticfeedback.HapticFeedbackType
import androidx.compose.ui.platform.LocalHapticFeedback
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.platform.LocalUriHandler
import com.friendorfoe.data.probes.PROBE_FLASHER_URL
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.hilt.navigation.compose.hiltViewModel
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.compose.ui.platform.LocalLifecycleOwner
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.friendorfoe.data.remote.ProbeTransmitterDto
import com.friendorfoe.presentation.components.FofDisclosure

@Composable
fun ProbeScreen(onBack: () -> Unit, onSettings: () -> Unit, onBadge: () -> Unit,
                viewModel: ProbeViewModel = hiltViewModel()) {
    val state by viewModel.state.collectAsStateWithLifecycle()
    val lifecycle = LocalLifecycleOwner.current.lifecycle
    DisposableEffect(lifecycle, viewModel) {
        val observer = LifecycleEventObserver { _, _ ->
            viewModel.setActive(lifecycle.currentState.isAtLeast(Lifecycle.State.STARTED))
        }
        lifecycle.addObserver(observer)
        viewModel.setActive(lifecycle.currentState.isAtLeast(Lifecycle.State.STARTED))
        onDispose { lifecycle.removeObserver(observer); viewModel.setActive(false) }
    }
    ProbeContent(state, ProbeActions(onBack, onSettings, onBadge, viewModel::retry,
        viewModel::selectSensor, viewModel::query, viewModel::minimumSignal,
        viewModel::directedOnly, viewModel::strongestFirst, viewModel::announce, viewModel::selectUsb))
}

data class ProbeActions(
    val onBack: () -> Unit = {},
    val onSettings: () -> Unit = {},
    val onBadge: () -> Unit = {},
    val onRetry: () -> Unit = {},
    val onSelectSensor: (String) -> Unit = {},
    val onQuery: (String) -> Unit = {},
    val onMinimumSignal: (Int?) -> Unit = {},
    val onDirectedOnly: (Boolean) -> Unit = {},
    val onStrongestFirst: (Boolean) -> Unit = {},
    val onAnnounce: (Boolean) -> Unit = {},
    val onSelectUsb: (Boolean) -> Unit = {},
)

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun ProbeContent(state: ProbeUiState, actions: ProbeActions) {
    var chooseScanner by rememberSaveable { mutableStateOf(false) }
    val snackbar = remember { SnackbarHostState() }
    val haptics = LocalHapticFeedback.current
    val uriHandler = LocalUriHandler.current
    LaunchedEffect(state.snapshotElapsedMs, state.newActivity) {
        state.newActivity?.let { message ->
            haptics.performHapticFeedback(HapticFeedbackType.LongPress)
            snackbar.showSnackbar(message)
        }
    }
    Scaffold(
        topBar = { TopAppBar(title = { Text("Wi-Fi probes") }, navigationIcon = {
            IconButton(onClick = actions.onBack) { Icon(Icons.AutoMirrored.Filled.ArrowBack, "Back") }
        }, actions = {
            IconButton(onClick = actions.onRetry, enabled = state.enabled && !state.loading) {
                Icon(Icons.Default.Refresh, "Refresh probes")
            }
        }) },
        snackbarHost = { SnackbarHost(snackbar) },
    ) { padding ->
        LazyColumn(Modifier.fillMaxSize().padding(padding).testTag("probe_screen"),
            contentPadding = PaddingValues(horizontal = 20.dp, vertical = 12.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp)) {
            item {
                Text("See what nearby devices are searching for.", style = MaterialTheme.typography.titleMedium)
                Text("Plug a probe scanner into your phone, or choose a scanner reporting to your backend.",
                    style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
            item {
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    FilterChip(state.usbSource, { actions.onSelectUsb(true) }, { Text("USB scanner") }, modifier = Modifier.testTag("probe_source_usb"))
                    FilterChip(!state.usbSource, { actions.onSelectUsb(false) }, { Text("Backend scanner") }, modifier = Modifier.testTag("probe_source_backend"))
                }
            }
            if (state.usbSource) item {
                Surface(color = MaterialTheme.colorScheme.surfaceContainerHigh, shape = MaterialTheme.shapes.medium) {
                    Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                        Text(if (state.usb.connected) "Scanner connected" else "USB Wi-Fi probe scanner", fontWeight = FontWeight.SemiBold)
                        Text(state.usb.message, style = MaterialTheme.typography.bodyMedium)
                        if (!state.usb.connected) {
                            Button(onClick = actions.onRetry, enabled = !state.usb.connecting) { Text("Connect USB") }
                            TextButton(onClick = { uriHandler.openUri(PROBE_FLASHER_URL) }) { Text("Get scanner firmware") }
                            Text("First flash the ESP32-S3 in Chrome or Edge on a computer. Then connect its native USB port to this phone. Scanning runs while this screen is open.", style = MaterialTheme.typography.bodySmall)
                        } else if (state.usb.dropped > 0) {
                            Text("Busy radio · ${state.usb.dropped} reports skipped by sampling or USB limits since scanner boot.", style = MaterialTheme.typography.bodySmall)
                        }
                    }
                }
            }
            if (!state.enabled && !state.usbSource) {
                item {
                    Surface(color = MaterialTheme.colorScheme.surfaceContainerHigh, shape = MaterialTheme.shapes.medium) {
                        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                            Text("Connect a probe-capable scanner", fontWeight = FontWeight.SemiBold)
                            Text("Use a Friend or Foe ESP32 scanner near you with backend reporting enabled. Then enable Sensor backend in App settings and select that scanner here.")
                            Button(onClick = actions.onSettings) { Text("Open app settings") }
                            TextButton(onClick = actions.onBadge) { Text("Badge connection") }
                        }
                    }
                }
            } else {
                if (!state.usbSource) item {
                    OutlinedButton(onClick = { chooseScanner = true }, modifier = Modifier.fillMaxWidth().testTag("probe_scanner")) {
                        Text(state.sensorId ?: "Choose the scanner near you")
                    }
                    val observer = state.snapshot.observers.firstOrNull { it.sensorId == state.sensorId }
                    val observerAge = observer?.ageSeconds?.plus(state.elapsedSeconds)
                    Text(when {
                        state.error != null -> state.error
                        state.loading && state.snapshotElapsedMs == null -> "Connecting…"
                        state.sensorId == null -> "Select one scanner to keep remote observations out of your results."
                        state.stale -> "Updates delayed · retained reports below"
                        observerAge == null || observerAge > 60 -> "Scanner is quiet or offline · retained reports below"
                        else -> "Refreshing every 5 seconds · scanner reporting"
                    }, style = MaterialTheme.typography.bodySmall, color = if (state.error != null)
                        MaterialTheme.colorScheme.error else MaterialTheme.colorScheme.onSurfaceVariant)
                }
                if (state.sensorId != null) {
                    item {
                        OutlinedTextField(value = state.query, onValueChange = actions.onQuery,
                            label = { Text("Find a network or MAC address") }, singleLine = true,
                            modifier = Modifier.fillMaxWidth().testTag("probe_search"))
                    }
                    item {
                        Row(Modifier.horizontalScroll(rememberScrollState()), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                            FilterChip(state.directedOnly, { actions.onDirectedOnly(!state.directedOnly) }, { Text("Named networks only") })
                            FilterChip(state.strongestFirst, { actions.onStrongestFirst(!state.strongestFirst) }, { Text("Strongest first") })
                        }
                        FofDisclosure("Signal & activity alerts", tag = "probe_options", summary =
                            state.minimumRssi?.let { "Signal at least $it dBm" } ?: "All signal strengths · newest first") {
                            Text("Signal is measured at the scanner; walls and antennas affect it. It does not tell you distance.", style = MaterialTheme.typography.bodySmall)
                            Row(Modifier.horizontalScroll(rememberScrollState()), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                                listOf(null to "All", -80 to "−80 dBm+", -65 to "−65 dBm+").forEach { (minimum, label) ->
                                    FilterChip(state.minimumRssi == minimum, { actions.onMinimumSignal(minimum) }, { Text(label) })
                                }
                            }
                            Row(verticalAlignment = Alignment.CenterVertically) {
                                Column(Modifier.weight(1f)) {
                                    Text("Alert me here")
                                    Text("Vibrate and show new addresses or network searches while this screen is open.", style = MaterialTheme.typography.bodySmall)
                                }
                                Switch(state.announceNew, actions.onAnnounce, Modifier.testTag("probe_announce"))
                            }
                        }
                    }
                    item {
                        Text("${state.rows.size} observed addresses · last 5 minutes", style = MaterialTheme.typography.labelLarge)
                        Text("Reports are sampled, not a count of every Wi-Fi packet.", style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant)
                    }
                    if (state.rows.isEmpty() && !state.loading) item {
                        Text(if (state.error != null) "No current reports available." else if (state.query.isNotBlank() || state.minimumRssi != null || state.directedOnly)
                            "No reports match these filters." else "No recent probe reports from this scanner.",
                            modifier = Modifier.padding(vertical = 16.dp))
                    }
                    items(state.rows, key = { "${it.sensorId}/${it.mac}" }) { row -> ProbeRow(row, state.elapsedSeconds) }
                }
            }
            item {
                FofDisclosure("What a probe means", tag = "probe_explanation") {
                    Text("A named request asks whether a network with that name is available. A wildcard request asks for any network. Neither proves an attack, a connection, or ownership.", style = MaterialTheme.typography.bodySmall)
                    Spacer(Modifier.height(8.dp))
                    Text("MAC addresses can rotate. One address is not necessarily one device or person. Capture covers only the scanner’s supported bands and the channel it is listening on. Older firmware may report only selected probes.", style = MaterialTheme.typography.bodySmall)
                }
            }
        }
    }
    if (chooseScanner) AlertDialog(onDismissRequest = { chooseScanner = false }, title = { Text("Scanner near you") },
        text = {
            LazyColumn(Modifier.heightIn(max = 360.dp)) {
                if (state.snapshot.observers.isEmpty()) item { Text("No reporting scanners yet. Check the scanner’s backend connection.") }
                items(state.snapshot.observers, key = { it.sensorId }) { observer ->
                    TextButton(onClick = { actions.onSelectSensor(observer.sensorId); chooseScanner = false }, modifier = Modifier.fillMaxWidth()) {
                        Column(Modifier.fillMaxWidth()) {
                            Text(observer.sensorId)
                            Text(probeAgeLabel((observer.ageSeconds ?: Double.NaN) + state.elapsedSeconds), style = MaterialTheme.typography.bodySmall)
                        }
                    }
                }
            }
        }, confirmButton = { TextButton(onClick = { chooseScanner = false }) { Text("Done") } })
}

@Composable
private fun ProbeRow(row: ProbeTransmitterDto, elapsedSeconds: Double) {
    var showAll by rememberSaveable(row.mac, row.sensorId) { mutableStateOf(false) }
    Surface(color = MaterialTheme.colorScheme.surfaceContainerLow, shape = MaterialTheme.shapes.medium,
        modifier = Modifier.fillMaxWidth().testTag("probe_${row.mac}")) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
            Text(row.mac, style = MaterialTheme.typography.titleSmall, fontFamily = FontFamily.Monospace)
            Text("${probeAgeLabel(row.ageSeconds + elapsedSeconds)} · ${row.rssi?.let { "$it dBm" } ?: "Signal unavailable"}" +
                (row.channel?.let { " · Ch $it" } ?: ""), style = MaterialTheme.typography.labelMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant)
            if (row.targets.isNotEmpty()) {
                Text("Searching for", style = MaterialTheme.typography.labelSmall, color = MaterialTheme.colorScheme.primary)
                // Render SSIDs as text, preserving case, commas, and spaces from the request.
                row.targets.take(if (showAll) Int.MAX_VALUE else 3).forEach { target ->
                    Text(target.ssid, fontWeight = FontWeight.SemiBold)
                }
                if (row.targets.size > 3) TextButton(onClick = { showAll = !showAll }) {
                    Text(if (showAll) "Show fewer networks" else "Show all ${row.targets.size} networks")
                }
            }
            if (row.wildcardReports > 0) Text("Any network · wildcard scan", style = MaterialTheme.typography.bodyMedium)
            if (row.unknownReports > 0) Text("Network name not captured", style = MaterialTheme.typography.bodySmall)
            Text("${row.reports} sampled reports" + if (row.locallyAdministered) " · Private/local MAC" else "",
                style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
    }
}
