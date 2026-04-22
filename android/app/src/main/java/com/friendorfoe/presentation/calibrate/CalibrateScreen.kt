package com.friendorfoe.presentation.calibrate

import android.content.Intent
import android.provider.Settings
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.ArrowBack
import androidx.compose.material.icons.filled.CheckCircle
import androidx.compose.material.icons.filled.LocationOn
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material.icons.filled.Warning
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.collectAsState
import androidx.compose.ui.Alignment
import androidx.compose.ui.BiasAlignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalLifecycleOwner
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.unit.dp
import androidx.hilt.navigation.compose.hiltViewModel
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import com.friendorfoe.calibration.CalibrationViewModel
import com.friendorfoe.calibration.CalibrationViewModel.BackendStatus
import com.friendorfoe.calibration.CalibrationViewModel.CheckpointResult
import com.friendorfoe.calibration.CalibrationViewModel.MyPosition
import com.friendorfoe.calibration.CalibrationViewModel.SensorInfo
import com.friendorfoe.calibration.CalibrationViewModel.SensorReading
import com.friendorfoe.data.remote.CalibrationModelDto
import com.friendorfoe.data.remote.EventDto
import com.friendorfoe.data.remote.NodeDto
import com.friendorfoe.data.remote.ProbeDeviceDto

private val OK_GREEN     = Color(0xFF4CAF50)
private val WARN_AMBER   = Color(0xFFFFB300)
private val ERROR_RED    = Color(0xFFE53935)
private val NEUTRAL_GREY = Color(0xFF616161)

private enum class CalibrateTab(val label: String) {
    Walk("Walk"),
    Nodes("Nodes"),
    Probes("Probes"),
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun CalibrateScreen(
    onBack: () -> Unit,
    viewModel: CalibrationViewModel = hiltViewModel(),
    consoleViewModel: CalibrateConsoleViewModel = hiltViewModel(),
) {
    val state by viewModel.state.collectAsState()
    val consoleState by consoleViewModel.state.collectAsState()
    val context = LocalContext.current
    val lifecycleOwner = LocalLifecycleOwner.current
    var selectedTab by remember { mutableStateOf(CalibrateTab.Walk) }

    // Refresh BT state + reach connectivity check whenever the screen
    // returns to the foreground (operator may have just enabled BT or
    // edited the URL/token).
    DisposableEffect(lifecycleOwner) {
        val obs = LifecycleEventObserver { _, event ->
            if (event == Lifecycle.Event.ON_RESUME) {
                viewModel.refreshBluetoothState()
                viewModel.refreshConnectivity()
                consoleViewModel.startPolling()
            } else if (event == Lifecycle.Event.ON_PAUSE) {
                consoleViewModel.stopPolling()
            }
        }
        lifecycleOwner.lifecycle.addObserver(obs)
        onDispose {
            lifecycleOwner.lifecycle.removeObserver(obs)
            consoleViewModel.stopPolling()
        }
    }

    // Cache the granted-permissions set so we don't rebuild the launcher
    // on every recomposition.
    var pendingStartAfterGrant by remember { mutableStateOf(false) }
    val permissionLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions()
    ) { result ->
        val missing = viewModel.missingPermissions(
            result.filterValues { it }.keys
        )
        if (missing.isEmpty() && pendingStartAfterGrant) {
            pendingStartAfterGrant = false
            viewModel.startWalk()
        } else if (missing.isNotEmpty()) {
            pendingStartAfterGrant = false
        }
    }

    // Proactive permission prompt on screen entry — if they're already
    // granted this is a no-op; if not, the operator sees the OS dialog
    // immediately instead of halfway through tapping Start Walk. Runs
    // once per screen entry via a stable LaunchedEffect key.
    LaunchedEffect(Unit) {
        val needed = listOf(
            android.Manifest.permission.ACCESS_FINE_LOCATION,
            android.Manifest.permission.BLUETOOTH_ADVERTISE,
            android.Manifest.permission.BLUETOOTH_SCAN,
            android.Manifest.permission.BLUETOOTH_CONNECT,
        )
        val missing = viewModel.missingPermissions(granted = emptySet())
        if (missing.isNotEmpty()) {
            permissionLauncher.launch(needed.toTypedArray())
        }
    }

    fun requestStart() {
        val granted = mutableSetOf<String>()
        // Build a "currently granted" map by re-asking for everything we
        // need; the launcher returns success for already-granted perms.
        val needed = listOf(
            android.Manifest.permission.ACCESS_FINE_LOCATION,
            android.Manifest.permission.BLUETOOTH_ADVERTISE,
            android.Manifest.permission.BLUETOOTH_SCAN,
            android.Manifest.permission.BLUETOOTH_CONNECT,
        )
        val missing = viewModel.missingPermissions(granted)  // empty grant set → returns full list
        if (missing.isEmpty()) {
            viewModel.startWalk()
        } else {
            pendingStartAfterGrant = true
            permissionLauncher.launch(needed.toTypedArray())
        }
    }

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text("Triangulation Calibration") },
                navigationIcon = {
                    IconButton(onClick = onBack) {
                        Icon(Icons.Default.ArrowBack, contentDescription = "Back")
                    }
                },
                actions = {
                    IconButton(onClick = {
                        viewModel.refreshConnectivity()
                        consoleViewModel.refreshNow()
                    }) {
                        Icon(Icons.Default.Refresh, contentDescription = "Refresh")
                    }
                },
            )
        }
    ) { padding ->
        Column(
            modifier = Modifier
                .padding(padding)
                .verticalScroll(rememberScrollState())
                .padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            Text(
                "Walk the property while this screen is open. The phone " +
                "broadcasts a known BLE beacon; sensors report what they " +
                "hear; the backend fits a per-sensor path-loss model. " +
                "Visit each sensor and tap its 'I'm here' button to " +
                "anchor the fit and verify its registered coordinates.",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )

            TabRow(selectedTabIndex = selectedTab.ordinal) {
                CalibrateTab.entries.forEach { tab ->
                    Tab(
                        selected = selectedTab == tab,
                        onClick = { selectedTab = tab },
                        text = { Text(tab.label) }
                    )
                }
            }

            // ── Bluetooth-off banner ─────────────────────────────────
            if (!state.bluetoothEnabled) {
                Surface(color = MaterialTheme.colorScheme.errorContainer,
                        shape = RoundedCornerShape(8.dp),
                        modifier = Modifier.fillMaxWidth()) {
                    Row(Modifier.padding(12.dp), verticalAlignment = Alignment.CenterVertically) {
                        Icon(Icons.Default.Warning, contentDescription = null,
                             tint = MaterialTheme.colorScheme.onErrorContainer)
                        Spacer(Modifier.width(8.dp))
                        Column(Modifier.weight(1f)) {
                            Text("Bluetooth is off",
                                 style = MaterialTheme.typography.titleSmall,
                                 color = MaterialTheme.colorScheme.onErrorContainer)
                            Text("Calibration needs to advertise BLE — please enable Bluetooth.",
                                 style = MaterialTheme.typography.bodySmall,
                                 color = MaterialTheme.colorScheme.onErrorContainer)
                        }
                        TextButton(onClick = {
                            context.startActivity(Intent(Settings.ACTION_BLUETOOTH_SETTINGS))
                        }) { Text("Settings") }
                    }
                }
            }

            // ── WiFi / connectivity banner ────────────────────────────
            // Property likely spans multiple APs — operator may need to
            // switch networks mid-walk. Always visible so they can jump
            // to Settings quickly; shows queued-sample count so they can
            // see the offline buffer filling/draining during roam.
            Surface(
                color = when {
                    state.queuedCount > 0 -> WARN_AMBER.copy(alpha = 0.15f)
                    state.backendStatus == BackendStatus.Ok -> OK_GREEN.copy(alpha = 0.12f)
                    else -> MaterialTheme.colorScheme.surfaceVariant
                },
                shape = RoundedCornerShape(8.dp),
                modifier = Modifier.fillMaxWidth(),
            ) {
                Row(Modifier.padding(12.dp), verticalAlignment = Alignment.CenterVertically) {
                    Column(Modifier.weight(1f)) {
                        Text(
                            when {
                                state.queuedCount > 0 ->
                                    "Roaming — ${state.queuedCount} queued, will sync when you reach WiFi"
                                state.backendStatus == BackendStatus.Ok ->
                                    "Backend reachable · syncing live"
                                state.backendStatus == BackendStatus.AuthFailed ->
                                    "Auth failed — check X-Cal-Token"
                                state.backendStatus == BackendStatus.Unreachable ->
                                    "Backend unreachable — switch to a closer WiFi network"
                                else -> "Connectivity unknown"
                            },
                            style = MaterialTheme.typography.bodySmall,
                            fontWeight = FontWeight.SemiBold,
                        )
                        Text(
                            "WiFi: " + (state.currentSsid ?: "not associated"),
                            style = MaterialTheme.typography.labelSmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                            fontFamily = FontFamily.Monospace,
                        )
                    }
                    TextButton(onClick = {
                        // Drops the operator on Android's WiFi settings
                        // where they can tap a different saved network.
                        // NEW_TASK flag isn't needed from a UI context but
                        // keeps this safe if the call ever hops threads.
                        val intent = Intent(Settings.ACTION_WIFI_SETTINGS)
                            .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                        try { context.startActivity(intent) }
                        catch (_: Exception) {
                            // Some OEMs (rare) hide WIFI_SETTINGS; fall
                            // back to the global settings index.
                            context.startActivity(Intent(Settings.ACTION_SETTINGS))
                        }
                    }) { Text("Switch WiFi") }
                }
            }

            // ── Settings ──────────────────────────────────────────────
            Surface(
                color = MaterialTheme.colorScheme.surfaceVariant,
                shape = RoundedCornerShape(8.dp),
            ) {
                Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Text("Backend", style = MaterialTheme.typography.labelLarge,
                             modifier = Modifier.weight(1f))
                        StatusDot(state.backendStatus)
                        Spacer(Modifier.width(6.dp))
                        Text(when (state.backendStatus) {
                            BackendStatus.Ok           -> "ok"
                            BackendStatus.AuthFailed   -> "401 — check token"
                            BackendStatus.Unreachable  -> "unreachable"
                            BackendStatus.Unknown      -> "unknown"
                        }, style = MaterialTheme.typography.labelSmall,
                           color = MaterialTheme.colorScheme.onSurfaceVariant)
                    }
                    OutlinedTextField(
                        value = state.backendUrl,
                        onValueChange = viewModel::setBackendUrl,
                        label = { Text("URL") },
                        placeholder = { Text("http://192.168.42.235:8000/") },
                        singleLine = true,
                        enabled = !state.isWalking,
                        modifier = Modifier.fillMaxWidth(),
                    )
                    OutlinedTextField(
                        value = state.token,
                        onValueChange = viewModel::setToken,
                        label = { Text("X-Cal-Token") },
                        placeholder = { Text("FOF_CAL_TOKEN from backend env") },
                        singleLine = true,
                        enabled = !state.isWalking,
                        visualTransformation = PasswordVisualTransformation(),
                        modifier = Modifier.fillMaxWidth(),
                    )
                    OutlinedTextField(
                        value = state.operatorLabel,
                        onValueChange = viewModel::setOperatorLabel,
                        label = { Text("Operator label") },
                        placeholder = { Text("Bill's Pixel") },
                        singleLine = true,
                        enabled = !state.isWalking,
                        modifier = Modifier.fillMaxWidth(),
                    )
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        TextButton(onClick = { viewModel.refreshConnectivity() }) {
                            Text("Test connectivity")
                        }
                        Spacer(Modifier.width(4.dp))
                        TextButton(onClick = { viewModel.resetTokenToDefault() }) {
                            // Manual escape hatch when the auto-recovery
                            // didn't fire (e.g. backend was briefly down
                            // during screen entry so the 401 retry never
                            // ran). Resets to `chompchomp` + re-tests.
                            Text("Reset token")
                        }
                    }
                }
            }

            when (selectedTab) {
                CalibrateTab.Walk -> {
                    // ── Walk control ──────────────────────────────────
                    Row(
                        verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.spacedBy(8.dp),
                    ) {
                        if (!state.isWalking) {
                            Button(
                                onClick = { requestStart() },
                                enabled = state.bluetoothEnabled,
                                modifier = Modifier.weight(1f),
                            ) { Text("Start walk") }
                        } else {
                            Button(
                                onClick = { viewModel.endWalk() },
                                colors = ButtonDefaults.buttonColors(
                                    containerColor = if (state.sessionReadiness.readyOverall) OK_GREEN
                                    else MaterialTheme.colorScheme.error,
                                    contentColor = MaterialTheme.colorScheme.onError,
                                ),
                                modifier = Modifier.weight(1f),
                            ) {
                                Text(
                                    if (state.sessionReadiness.readyOverall)
                                        "Session ready — finish + apply fit"
                                    else "Stop walk + apply fit"
                                )
                            }
                        }
                    }

                    if (state.isWalking) {
                        val sr = state.sessionReadiness
                        val pctReady = if (sr.sensorsTotal > 0) {
                            sr.sensorsReady.toFloat() / sr.sensorsTotal
                        } else 0f
                        val bannerColor = when {
                            sr.readyOverall -> OK_GREEN.copy(alpha = 0.18f)
                            sr.sensorsReady > 0 -> WARN_AMBER.copy(alpha = 0.15f)
                            else -> NEUTRAL_GREY.copy(alpha = 0.15f)
                        }
                        Surface(
                            color = bannerColor,
                            shape = RoundedCornerShape(8.dp),
                            modifier = Modifier.fillMaxWidth(),
                        ) {
                            Column(
                                Modifier.padding(12.dp),
                                verticalArrangement = Arrangement.spacedBy(4.dp),
                            ) {
                                Row(verticalAlignment = Alignment.CenterVertically) {
                                    val icon = if (sr.readyOverall) Icons.Default.CheckCircle else Icons.Default.Warning
                                    val iconColor = if (sr.readyOverall) OK_GREEN else WARN_AMBER
                                    Icon(icon, contentDescription = null, tint = iconColor)
                                    Spacer(Modifier.width(8.dp))
                                    Text(
                                        if (sr.readyOverall) "Ready — you can stop any time"
                                        else "Keep walking — ${sr.sensorsReady}/${sr.sensorsTotal} sensors ready (need ${sr.minRequired})",
                                        fontWeight = FontWeight.SemiBold,
                                        style = MaterialTheme.typography.bodyMedium,
                                    )
                                }
                                LinearProgressIndicator(
                                    progress = { pctReady },
                                    color = if (sr.readyOverall) OK_GREEN else WARN_AMBER,
                                    trackColor = MaterialTheme.colorScheme.surfaceVariant,
                                    modifier = Modifier.fillMaxWidth().height(6.dp),
                                )
                            }
                        }
                    }

                    CalibrationModelCard(consoleState.calibrationModel)

                    if (state.isWalking) {
                        ConvergenceCard(state.myPosition)
                    }

                    Surface(
                        color = MaterialTheme.colorScheme.surfaceVariant,
                        shape = RoundedCornerShape(8.dp),
                    ) {
                        Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                            Row(verticalAlignment = Alignment.CenterVertically) {
                                Text(
                                    "Sensors — walk to each + tap 'I'm here'",
                                    style = MaterialTheme.typography.labelLarge,
                                    modifier = Modifier.weight(1f),
                                )
                                if (state.checkpointResults.isNotEmpty()) {
                                    Text(
                                        "${state.checkpointResults.size}/${state.availableSensors.size} touched",
                                        style = MaterialTheme.typography.labelSmall,
                                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                                    )
                                }
                            }
                            if (state.availableSensors.isEmpty()) {
                                Text(
                                    "No sensors loaded. Configure backend + token, then tap Refresh ↻ above.",
                                    style = MaterialTheme.typography.bodySmall,
                                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                                )
                            } else {
                                val readingById = state.sensorsHearingMe.associateBy { it.sensorId }
                                val sortedSensors = state.availableSensors.sortedWith(
                                    compareBy<SensorInfo> {
                                        readingById[it.deviceId]?.gpsDistanceM ?: Double.MAX_VALUE
                                    }.thenByDescending {
                                        readingById[it.deviceId]?.rssi ?: Int.MIN_VALUE
                                    }.thenBy { it.name }
                                )
                                val candidateId = sortedSensors.firstOrNull()?.deviceId
                                sortedSensors.forEach { sensor ->
                                    SensorCard(
                                        sensor = sensor,
                                        reading = readingById[sensor.deviceId],
                                        result = state.checkpointResults[sensor.deviceId],
                                        walking = state.isWalking,
                                        highlighted = sensor.deviceId == candidateId,
                                        onMarkHere = { viewModel.markAtSensor(sensor) },
                                    )
                                }
                            }
                        }
                    }

                    Surface(
                        color = MaterialTheme.colorScheme.surfaceVariant,
                        shape = RoundedCornerShape(8.dp),
                    ) {
                        Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                            Text("Live status", style = MaterialTheme.typography.labelLarge)
                            InfoRow("Walking", if (state.isWalking) "yes" else "no")
                            InfoRow("Session", state.sessionId ?: "—")
                            InfoRow("BLE UUID", state.advertiseUuid?.take(13)?.plus("…") ?: "—")
                            InfoRow(
                                "Phone GPS",
                                state.phoneLat?.let {
                                    "%.5f, %.5f ±%.0f m".format(
                                        it,
                                        state.phoneLon ?: 0.0,
                                        state.gpsAccuracyM ?: 0f,
                                    )
                                } ?: "no fix"
                            )
                            InfoRow("Trace points", state.tracePoints.toString())
                            InfoRow("Sensor samples", state.samplesTotal.toString())
                            InfoRow(
                                "Model source",
                                consoleState.calibrationModel?.activeModelSource ?: "defaults"
                            )
                            InfoRow(
                                "Model trust",
                                when {
                                    consoleState.calibrationModel?.isTrusted == true -> "trusted"
                                    consoleState.calibrationModel?.isActive == true -> "active, untrusted"
                                    else -> "defaults only"
                                }
                            )
                        }
                    }

                    Surface(
                        color = MaterialTheme.colorScheme.surfaceVariant,
                        shape = RoundedCornerShape(8.dp),
                    ) {
                        Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                            Text(
                                "Sensors hearing this phone (last 10 s)",
                                style = MaterialTheme.typography.labelLarge,
                            )
                            if (state.sensorsHearingMe.isEmpty()) {
                                Text(
                                    if (state.isWalking) "No sensor sightings yet — keep walking."
                                    else "Start a walk to see live sensor RSSI.",
                                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                                    style = MaterialTheme.typography.bodySmall,
                                )
                            } else {
                                state.sensorsHearingMe.sortedWith(
                                    compareBy<SensorReading> { it.gpsDistanceM ?: Double.MAX_VALUE }
                                        .thenByDescending { it.rssi ?: Int.MIN_VALUE }
                                ).forEach { sensor ->
                                    val rssiVal = sensor.rssi
                                    val rssiColor = when {
                                        rssiVal == null -> MaterialTheme.colorScheme.onSurfaceVariant
                                        rssiVal > -55 -> ERROR_RED
                                        rssiVal > -75 -> WARN_AMBER
                                        else -> MaterialTheme.colorScheme.onSurfaceVariant
                                    }
                                    Row(
                                        verticalAlignment = Alignment.CenterVertically,
                                        modifier = Modifier.fillMaxWidth(),
                                    ) {
                                        Text(
                                            sensor.sensorId,
                                            style = MaterialTheme.typography.bodySmall,
                                            fontFamily = FontFamily.Monospace,
                                            modifier = Modifier.weight(1f),
                                        )
                                        Text(
                                            rssiVal?.let { "$it dBm" } ?: "—",
                                            color = rssiColor,
                                            fontWeight = FontWeight.SemiBold,
                                            style = MaterialTheme.typography.bodySmall,
                                            fontFamily = FontFamily.Monospace,
                                        )
                                        Spacer(Modifier.width(8.dp))
                                        Text(
                                            "${sensor.samplesInWindow} smp",
                                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                                            style = MaterialTheme.typography.bodySmall,
                                            fontFamily = FontFamily.Monospace,
                                        )
                                        Spacer(Modifier.width(8.dp))
                                        Text(
                                            sensor.gpsDistanceM?.let { "%.0fm".format(it) } ?: "—",
                                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                                            style = MaterialTheme.typography.bodySmall,
                                            fontFamily = FontFamily.Monospace,
                                        )
                                    }
                                }
                            }
                        }
                    }

                    state.fitResult?.let { fit ->
                        Surface(
                            color = if (state.fitApplied == true) {
                                Color(0xFF1B5E20).copy(alpha = 0.20f)
                            } else MaterialTheme.colorScheme.surfaceVariant,
                            shape = RoundedCornerShape(8.dp),
                        ) {
                            Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                                Text(
                                    if (state.fitApplied == true) "Calibration applied"
                                    else "Calibration NOT applied (fit too weak)",
                                    style = MaterialTheme.typography.labelLarge,
                                    color = if (state.fitApplied == true) OK_GREEN else MaterialTheme.colorScheme.error,
                                )
                                InfoRow("Global RSSI_REF", fit.get("global_rssi_ref")?.toString() ?: "—")
                                InfoRow("Global path-loss n", fit.get("global_path_loss_exponent")?.toString() ?: "—")
                                InfoRow("Global R²", fit.get("global_r_squared")?.toString() ?: "—")
                                InfoRow("Trace points", fit.get("trace_points")?.toString() ?: "—")
                                InfoRow("Total samples", fit.get("samples_total")?.toString() ?: "—")
                                InfoRow("Sensors checkpointed", fit.get("checkpointed_sensor_count")?.toString() ?: "—")
                            }
                        }
                    }
                }

                CalibrateTab.Nodes -> NodesTabContent(consoleState.nodes, consoleState.lastRefreshMs)
                CalibrateTab.Probes -> ProbesTabContent(
                    probes = consoleState.probes,
                    events = consoleState.events,
                    onAckEvent = consoleViewModel::ackEvent,
                )
            }

            // ── Messages ──────────────────────────────────────────────
            state.errorMessage?.let { msg ->
                Surface(color = MaterialTheme.colorScheme.errorContainer,
                        shape = RoundedCornerShape(8.dp),
                        modifier = Modifier.fillMaxWidth()) {
                    Row(Modifier.padding(12.dp), verticalAlignment = Alignment.CenterVertically) {
                        Text(msg, color = MaterialTheme.colorScheme.onErrorContainer,
                             modifier = Modifier.weight(1f))
                        TextButton(onClick = { viewModel.clearMessages() }) { Text("OK") }
                    }
                }
            }
            state.infoMessage?.let { msg ->
                Surface(color = MaterialTheme.colorScheme.tertiaryContainer,
                        shape = RoundedCornerShape(8.dp),
                        modifier = Modifier.fillMaxWidth()) {
                    Text(msg, modifier = Modifier.padding(12.dp),
                         color = MaterialTheme.colorScheme.onTertiaryContainer,
                         style = MaterialTheme.typography.bodySmall)
                }
            }
            consoleState.errorMessage?.let { msg ->
                Surface(
                    color = WARN_AMBER.copy(alpha = 0.18f),
                    shape = RoundedCornerShape(8.dp),
                    modifier = Modifier.fillMaxWidth(),
                ) {
                    Text(
                        msg,
                        modifier = Modifier.padding(12.dp),
                        color = MaterialTheme.colorScheme.onSurface,
                        style = MaterialTheme.typography.bodySmall,
                    )
                }
            }
        }
    }
}

@Composable
private fun ConvergenceCard(pos: MyPosition) {
    // Color the banner based on the most actionable state: green when
    // fully locked + ok to move, amber while converging / standing still,
    // red when the fleet can't hear us, grey while waiting for GPS.
    val color = when {
        pos.okToMove -> OK_GREEN
        pos.sensorCount == 0 -> ERROR_RED
        pos.errorM != null -> WARN_AMBER
        else -> NEUTRAL_GREY
    }
    val headline = when {
        pos.okToMove -> "OK to move — fleet is locked on you"
        pos.sensorCount == 0 -> "No sensors hearing you yet"
        pos.phoneLat == null -> "Waiting for GPS fix"
        pos.triangulatedLat == null -> "Waiting for triangulation"
        !pos.standingStill -> "Walking — sampling across positions"
        pos.errorM != null && pos.errorM >= pos.convergenceTargetM ->
            "Stand still — converging (error %.0f m, target < %.0f m)".format(
                pos.errorM, pos.convergenceTargetM)
        pos.stillS < pos.dwellTargetS ->
            "Stand still — %.1f s / %.0f s".format(pos.stillS, pos.dwellTargetS)
        else -> "Holding"
    }
    Surface(
        color = color.copy(alpha = 0.15f),
        shape = RoundedCornerShape(8.dp),
        modifier = Modifier.fillMaxWidth(),
    ) {
        Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                val icon = when {
                    pos.okToMove -> Icons.Default.CheckCircle
                    pos.sensorCount == 0 -> Icons.Default.Warning
                    else -> Icons.Default.LocationOn
                }
                Icon(icon, contentDescription = null, tint = color)
                Spacer(Modifier.width(8.dp))
                Text(headline,
                     fontWeight = FontWeight.SemiBold,
                     color = color,
                     style = MaterialTheme.typography.bodyMedium)
            }
            // Progress toward "OK to move": fills as error drops AND dwell
            // time accumulates. Gives the operator a steady visual that
            // "something is happening" while they stand still.
            val errorProgress = if (pos.errorM == null) 0f
                else (1f - (pos.errorM.toFloat() / (pos.convergenceTargetM.toFloat() * 2))).coerceIn(0f, 1f)
            val dwellProgress = (pos.stillS / pos.dwellTargetS).coerceIn(0.0, 1.0).toFloat()
            val combined = if (pos.sensorCount >= pos.minSensors)
                (errorProgress * 0.6f + dwellProgress * 0.4f).coerceIn(0f, 1f)
            else 0f
            LinearProgressIndicator(
                progress = { combined },
                color = color,
                trackColor = MaterialTheme.colorScheme.surfaceVariant,
                modifier = Modifier.fillMaxWidth().height(6.dp),
            )
            Row(modifier = Modifier.fillMaxWidth()) {
                Text(
                    pos.errorM?.let { "error ${"%.1f".format(it)} m" } ?: "error —",
                    style = MaterialTheme.typography.labelSmall,
                    fontFamily = FontFamily.Monospace,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.weight(1f),
                )
                Text("${pos.sensorCount}/${pos.minSensors}+ sensors",
                     style = MaterialTheme.typography.labelSmall,
                     fontFamily = FontFamily.Monospace,
                     color = MaterialTheme.colorScheme.onSurfaceVariant)
                Spacer(Modifier.width(8.dp))
                Text(
                    if (pos.standingStill)
                        "still ${"%.1f".format(pos.stillS)}s"
                    else "moving",
                    style = MaterialTheme.typography.labelSmall,
                    fontFamily = FontFamily.Monospace,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            // Miniature XY visualization — phone GPS at center, fleet's
            // triangulated position offset. Simple Box-layout "map" so
            // operators can see the divergence without depending on a
            // heavyweight map widget.
            if (pos.phoneLat != null && pos.triangulatedLat != null && pos.errorM != null) {
                Box(
                    modifier = Modifier
                        .fillMaxWidth()
                        .height(80.dp)
                        .clip(RoundedCornerShape(6.dp))
                        .background(MaterialTheme.colorScheme.surface)
                ) {
                    // We don't have pixel-perfect bearings available here
                    // without extra math; for now place phone-GPS at center
                    // and put the triangulated dot offset by error magnitude
                    // along a diagonal. Gives the operator an at-a-glance
                    // sense of "they agree" vs "they disagree".
                    val scale = (pos.errorM / (pos.convergenceTargetM * 3))
                        .coerceIn(0.0, 1.0).toFloat()
                    Box(modifier = Modifier
                        .align(Alignment.Center)
                        .size(12.dp)
                        .clip(CircleShape)
                        .background(OK_GREEN))
                    Box(modifier = Modifier
                        .align(BiasAlignment(0.6f * scale, -0.6f * scale))
                        .size(12.dp)
                        .clip(CircleShape)
                        .background(color))
                    Text("GPS · Fleet",
                         style = MaterialTheme.typography.labelSmall,
                         fontFamily = FontFamily.Monospace,
                         color = MaterialTheme.colorScheme.onSurfaceVariant,
                         modifier = Modifier
                             .align(Alignment.BottomCenter)
                             .padding(bottom = 4.dp))
                }
            }
        }
    }
}

@Composable
private fun CalibrationModelCard(model: CalibrationModelDto?) {
    Surface(
        color = MaterialTheme.colorScheme.surfaceVariant,
        shape = RoundedCornerShape(8.dp),
    ) {
        Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
            Text("Model truth", style = MaterialTheme.typography.labelLarge)
            if (model == null) {
                Text(
                    "Waiting for backend calibration model…",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            } else {
                InfoRow("Source", model.activeModelSource ?: "defaults")
                InfoRow("Trusted", if (model.isTrusted) "yes" else "no")
                InfoRow("Active", if (model.isActive) "yes" else "no")
                InfoRow("Per-listener", model.appliedListenerCount.toString())
                InfoRow("RSSI ref", model.rssiRef?.let { "%.1f dBm".format(it) } ?: "—")
                InfoRow("Path-loss n", model.pathLossExponent?.let { "%.2f".format(it) } ?: "—")
                InfoRow("R²", model.rSquared?.let { "%.3f".format(it) } ?: "—")
            }
        }
    }
}

@Composable
private fun NodesTabContent(
    nodes: List<NodeDto>,
    lastRefreshMs: Long?,
) {
    Surface(
        color = MaterialTheme.colorScheme.surfaceVariant,
        shape = RoundedCornerShape(8.dp),
    ) {
        Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
            Text("Node diagnostics", style = MaterialTheme.typography.labelLarge)
            Text(
                "Queue pressure, UART drops, probe shedding, and recent source repairs per node.",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            lastRefreshMs?.let {
                Text(
                    "Last refresh ${formatRelativeAgeSeconds((System.currentTimeMillis() - it) / 1000.0)} ago",
                    style = MaterialTheme.typography.labelSmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
    }

    if (nodes.isEmpty()) {
        Surface(
            color = MaterialTheme.colorScheme.surfaceVariant,
            shape = RoundedCornerShape(8.dp),
        ) {
            Text(
                "No node heartbeats yet.",
                modifier = Modifier.padding(12.dp),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
        return
    }

    nodes.forEach { node ->
        val pressure = maxScannerPressure(node.scanners)
        val pressured = pressure >= 50
        Surface(
            color = when {
                !node.online -> MaterialTheme.colorScheme.errorContainer
                pressured -> WARN_AMBER.copy(alpha = 0.14f)
                else -> MaterialTheme.colorScheme.surfaceVariant
            },
            shape = RoundedCornerShape(8.dp),
        ) {
            Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text(
                        (node.name ?: node.deviceId).ifBlank { node.deviceId },
                        style = MaterialTheme.typography.titleSmall,
                        modifier = Modifier.weight(1f),
                    )
                    val statusText = when {
                        !node.online -> "offline"
                        pressured -> "pressured"
                        else -> "healthy"
                    }
                    Text(
                        statusText,
                        color = when {
                            !node.online -> ERROR_RED
                            pressured -> WARN_AMBER
                            else -> OK_GREEN
                        },
                        style = MaterialTheme.typography.labelSmall,
                        fontWeight = FontWeight.SemiBold,
                    )
                }
                Text(
                    "${node.deviceId} · ${node.firmwareVersion ?: "?"} · ${node.boardType ?: "?"}",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    fontFamily = FontFamily.Monospace,
                )
                InfoRow("Age", "%.1fs".format(node.ageS))
                InfoRow("WiFi", listOfNotNull(node.wifiSsid, node.wifiRssi?.let { "${it} dBm" }).joinToString(" · ").ifBlank { "—" })
                InfoRow("Recent fixups", node.sourceFixupsRecent.toString())
                if (node.scanners.isEmpty()) {
                    Text(
                        "No scanner diagnostics reported.",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                } else {
                    node.scanners.forEach { scanner ->
                        Surface(
                            color = MaterialTheme.colorScheme.surface,
                            shape = RoundedCornerShape(6.dp),
                            modifier = Modifier.fillMaxWidth(),
                        ) {
                            Column(Modifier.padding(10.dp), verticalArrangement = Arrangement.spacedBy(2.dp)) {
                                Text(
                                    "${scanner.uart ?: "?"} · ${scanner.board ?: "scanner"} · ${scanner.ver ?: "?"}",
                                    style = MaterialTheme.typography.bodyMedium,
                                    fontWeight = FontWeight.SemiBold,
                                )
                                InfoRow("Queue", "${scanner.txQueueDepth ?: 0}/${scanner.txQueueCapacity ?: 0} (${scanner.txQueuePressurePct ?: maxScannerPressure(listOf(scanner))}%)")
                                InfoRow("UART dropped", (scanner.uartTxDropped ?: 0).toString())
                                InfoRow("Probe sent/seen", "${scanner.probeSent ?: 0}/${scanner.probeSeen ?: 0}")
                                InfoRow(
                                    "Probe drops",
                                    listOf(
                                        "low=${scanner.probeDropLowValue ?: 0}",
                                        "rate=${scanner.probeDropRateLimit ?: 0}",
                                        "pressure=${scanner.probeDropPressure ?: 0}",
                                    ).joinToString(" ")
                                )
                                InfoRow(
                                    "Noise drops",
                                    "ble=${scanner.noiseDropBle ?: 0} wifi=${scanner.noiseDropWifi ?: 0}"
                                )
                                InfoRow("Time sync", "${scanner.tcnt ?: 0} ticks")
                            }
                        }
                    }
                }
            }
        }
    }
}

@Composable
private fun ProbesTabContent(
    probes: List<ProbeDeviceDto>,
    events: List<EventDto>,
    onAckEvent: (Int) -> Unit,
) {
    val newIdentityEvents = probeEventsForSection(events, PROBE_IDENTITY_EVENT_TYPES)
    val newSsidEvents = probeEventsForSection(events, setOf("new_probed_ssid"))
    val activityEvents = probeEventsForSection(events, setOf("probe_activity_spike"))
    val activeNow = activeNowProbes(probes)

    ProbeSection(
        title = "New probe identities",
        subtitle = "First seen within the last 24 hours.",
        emptyText = "No new probe identities in the last 24 hours.",
        itemCount = newIdentityEvents.size,
    ) {
        newIdentityEvents.forEach { event ->
            ProbeEventCard(event = event, onAckEvent = onAckEvent)
        }
    }

    ProbeSection(
        title = "New probed SSIDs",
        subtitle = "Fresh SSIDs nearby devices started asking for.",
        emptyText = "No new probed SSIDs in the last 24 hours.",
        itemCount = newSsidEvents.size,
    ) {
        newSsidEvents.forEach { event ->
            ProbeEventCard(event = event, onAckEvent = onAckEvent)
        }
    }

    ProbeSection(
        title = "Activity spikes",
        subtitle = "Probe identities that suddenly got loud or multi-sensor.",
        emptyText = "No probe activity spikes in the last 24 hours.",
        itemCount = activityEvents.size,
    ) {
        activityEvents.forEach { event ->
            ProbeEventCard(event = event, onAckEvent = onAckEvent)
        }
    }

    ProbeSection(
        title = "Active now",
        subtitle = "Probe devices seen in roughly the last 5 minutes.",
        emptyText = "No active probes right now.",
        itemCount = activeNow.size,
    ) {
        activeNow.forEach { probe ->
            ProbeDeviceCard(probe = probe)
        }
    }
}

@Composable
private fun ProbeSection(
    title: String,
    subtitle: String,
    emptyText: String,
    itemCount: Int,
    content: @Composable ColumnScope.() -> Unit,
) {
    Surface(
        color = MaterialTheme.colorScheme.surfaceVariant,
        shape = RoundedCornerShape(8.dp),
    ) {
        Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
            Text(title, style = MaterialTheme.typography.labelLarge)
            Text(
                subtitle,
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            if (itemCount == 0) {
                Text(
                    emptyText,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            } else {
                content()
            }
        }
    }
}

@Composable
private fun ProbeEventCard(
    event: EventDto,
    onAckEvent: (Int) -> Unit,
) {
    Surface(
        color = MaterialTheme.colorScheme.surface,
        shape = RoundedCornerShape(6.dp),
        modifier = Modifier.fillMaxWidth(),
    ) {
        Column(Modifier.padding(10.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(event.title.ifBlank { event.eventType }, modifier = Modifier.weight(1f))
                Text(
                    event.severity.uppercase(),
                    color = if (event.severity == "warning") WARN_AMBER else MaterialTheme.colorScheme.onSurfaceVariant,
                    style = MaterialTheme.typography.labelSmall,
                    fontWeight = FontWeight.SemiBold,
                )
            }
            Text(
                event.message.ifBlank { event.identifier },
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            Text(
                "First ${event.firstSeenAt.take(19)} · Sensors ${event.sensorCount} · Best RSSI ${event.bestRssi ?: "—"}",
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                fontFamily = FontFamily.Monospace,
            )
            Row(horizontalArrangement = Arrangement.End, modifier = Modifier.fillMaxWidth()) {
                TextButton(onClick = { onAckEvent(event.id) }) {
                    Text("Acknowledge")
                }
            }
        }
    }
}

@Composable
private fun ProbeDeviceCard(probe: ProbeDeviceDto) {
    Surface(
        color = MaterialTheme.colorScheme.surface,
        shape = RoundedCornerShape(6.dp),
        modifier = Modifier.fillMaxWidth(),
    ) {
        Column(Modifier.padding(10.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
            Text(
                probe.identity,
                style = MaterialTheme.typography.bodyMedium,
                fontWeight = FontWeight.SemiBold,
                fontFamily = FontFamily.Monospace,
            )
            Text(
                probe.probedSsids.joinToString().ifBlank { "No probed SSIDs captured" },
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            Text(
                "First ${probe.firstSeenAgeS?.let { formatRelativeAgeSeconds(it) } ?: "—"} ago · " +
                    "Last ${probe.ageS?.let { formatRelativeAgeSeconds(it) } ?: "—"} ago · " +
                    "24h ${probe.seen24hCount} hits / ${probe.sensorCount24h} sensors",
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                fontFamily = FontFamily.Monospace,
            )
            Text(
                "RSSI ${probe.bestRssi ?: "—"} · Activity ${probe.activityLevel ?: "low"} · " +
                    probe.latestEventTypes.joinToString().ifBlank { "no events" },
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                fontFamily = FontFamily.Monospace,
            )
        }
    }
}

private fun formatRelativeAgeSeconds(ageS: Double): String {
    val secs = ageS.toInt()
    return when {
        secs < 60 -> "${secs}s"
        secs < 3600 -> "${secs / 60}m"
        secs < 86400 -> "${secs / 3600}h"
        else -> "${secs / 86400}d"
    }
}


@Composable
private fun StatusDot(status: BackendStatus) {
    val color = when (status) {
        BackendStatus.Ok          -> OK_GREEN
        BackendStatus.AuthFailed  -> ERROR_RED
        BackendStatus.Unreachable -> NEUTRAL_GREY
        BackendStatus.Unknown     -> NEUTRAL_GREY
    }
    Box(
        modifier = Modifier
            .size(10.dp)
            .clip(CircleShape)
            .background(color)
    )
}

@Composable
private fun SensorCard(
    sensor: SensorInfo,
    reading: SensorReading?,
    result: CheckpointResult?,
    walking: Boolean,
    highlighted: Boolean = false,
    onMarkHere: () -> Unit,
) {
    val sevColor = when (result?.severity) {
        "ok"    -> OK_GREEN
        "warn"  -> WARN_AMBER
        "error" -> ERROR_RED
        else    -> NEUTRAL_GREY
    }
    val readyColor = when {
        reading?.ready == true -> OK_GREEN
        reading != null && reading.samplesCount > 0 -> WARN_AMBER
        else -> NEUTRAL_GREY
    }
    Surface(
        color = MaterialTheme.colorScheme.surface,
        shape = RoundedCornerShape(6.dp),
        modifier = Modifier.fillMaxWidth(),
    ) {
        Column(Modifier.padding(10.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Box(modifier = Modifier
                    .size(8.dp)
                    .clip(CircleShape)
                    .background(if (sensor.online) OK_GREEN else NEUTRAL_GREY))
                Spacer(Modifier.width(8.dp))
                Column(Modifier.weight(1f)) {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Text(sensor.name, fontWeight = FontWeight.SemiBold,
                             style = MaterialTheme.typography.bodyMedium,
                             modifier = Modifier.weight(1f, fill = false))
                        if (highlighted) {
                            Spacer(Modifier.width(6.dp))
                            Text(
                                "candidate",
                                color = WARN_AMBER,
                                style = MaterialTheme.typography.labelSmall,
                                fontWeight = FontWeight.SemiBold,
                            )
                        }
                        if (reading?.ready == true) {
                            Spacer(Modifier.width(6.dp))
                            Text("ready", color = OK_GREEN,
                                 style = MaterialTheme.typography.labelSmall,
                                 fontWeight = FontWeight.SemiBold)
                        }
                    }
                    Text("${sensor.deviceId} · " +
                         "%.5f, %.5f".format(sensor.lat, sensor.lon) +
                         (sensor.ageS?.let { " · %.0fs".format(it) } ?: ""),
                         style = MaterialTheme.typography.bodySmall,
                         color = MaterialTheme.colorScheme.onSurfaceVariant,
                         fontFamily = FontFamily.Monospace)
                }
                if (result != null) {
                    Icon(Icons.Default.CheckCircle, contentDescription = null, tint = sevColor)
                    Spacer(Modifier.width(8.dp))
                }
                Button(
                    onClick = onMarkHere,
                    enabled = walking,
                    contentPadding = PaddingValues(horizontal = 12.dp, vertical = 4.dp),
                ) {
                    Icon(Icons.Default.LocationOn, contentDescription = null,
                         modifier = Modifier.size(16.dp))
                    Spacer(Modifier.width(4.dp))
                    Text("I'm here")
                }
            }
            // ── Readiness progress strip ──────────────────────────
            if (reading != null && walking) {
                Spacer(Modifier.height(6.dp))
                val pct = if (reading.samplesNeeded > 0)
                    (reading.samplesCount.toFloat() / reading.samplesNeeded)
                        .coerceIn(0f, 1f)
                else 0f
                Column(verticalArrangement = Arrangement.spacedBy(2.dp)) {
                    LinearProgressIndicator(
                        progress = { pct },
                        color = readyColor,
                        trackColor = MaterialTheme.colorScheme.surfaceVariant,
                        modifier = Modifier.fillMaxWidth().height(4.dp),
                    )
                    Row {
                        Text("${reading.samplesCount}/${reading.samplesNeeded} samples",
                             style = MaterialTheme.typography.labelSmall,
                             fontFamily = FontFamily.Monospace,
                             color = MaterialTheme.colorScheme.onSurfaceVariant,
                             modifier = Modifier.weight(1f))
                        Text("range %.0fm".format(reading.distanceRangeM),
                             style = MaterialTheme.typography.labelSmall,
                             fontFamily = FontFamily.Monospace,
                             color = MaterialTheme.colorScheme.onSurfaceVariant)
                        Spacer(Modifier.width(8.dp))
                        Text(if (reading.hasCheckpoint) "✓ touched" else "no touch",
                             style = MaterialTheme.typography.labelSmall,
                             fontFamily = FontFamily.Monospace,
                             color = if (reading.hasCheckpoint) OK_GREEN
                                     else MaterialTheme.colorScheme.onSurfaceVariant)
                        reading.rssi?.let {
                            Spacer(Modifier.width(8.dp))
                            Text("$it dBm",
                                 style = MaterialTheme.typography.labelSmall,
                                 fontFamily = FontFamily.Monospace,
                                 color = MaterialTheme.colorScheme.onSurfaceVariant)
                        }
                    }
                    // Actionable hints — only shown when not yet ready
                    if (!reading.ready) {
                        for (h in reading.hints) {
                            Text("→ ${prettyHint(h)}",
                                 style = MaterialTheme.typography.labelSmall,
                                 color = WARN_AMBER.copy(alpha = 0.9f))
                        }
                    }
                }
            }
            if (result != null) {
                Spacer(Modifier.height(6.dp))
                Surface(
                    color = sevColor.copy(alpha = 0.12f),
                    shape = RoundedCornerShape(4.dp),
                    modifier = Modifier.fillMaxWidth(),
                ) {
                    Column(Modifier.padding(8.dp), verticalArrangement = Arrangement.spacedBy(2.dp)) {
                        Row {
                            Text(when (result.severity) {
                                "ok"    -> "Looks good"
                                "warn"  -> "Worth checking"
                                "error" -> "Needs attention"
                                else    -> "—"
                            }, color = sevColor, fontWeight = FontWeight.SemiBold,
                               style = MaterialTheme.typography.labelMedium,
                               modifier = Modifier.weight(1f))
                            result.gpsDriftM?.let {
                                Text("GPS drift %.0fm".format(it),
                                     style = MaterialTheme.typography.labelSmall,
                                     fontFamily = FontFamily.Monospace,
                                     color = MaterialTheme.colorScheme.onSurfaceVariant)
                            }
                        }
                        Row {
                            result.rssiAtTouch?.let {
                                Text("RSSI@touch: $it dBm",
                                     style = MaterialTheme.typography.labelSmall,
                                     fontFamily = FontFamily.Monospace,
                                     color = MaterialTheme.colorScheme.onSurfaceVariant,
                                     modifier = Modifier.weight(1f))
                            } ?: Spacer(Modifier.weight(1f))
                            result.strongestAtTouch?.let {
                                if (it != sensor.deviceId) {
                                    Text("Loudest: $it",
                                         style = MaterialTheme.typography.labelSmall,
                                         fontFamily = FontFamily.Monospace,
                                         color = ERROR_RED)
                                }
                            }
                        }
                        for (w in result.warnings) {
                            Text("• ${prettyWarning(w)}",
                                 style = MaterialTheme.typography.labelSmall,
                                 color = sevColor)
                        }
                    }
                }
            }
        }
    }
}

private fun prettyHint(raw: String): String {
    // Closed-loop hints from backend — slug-style, translate for humans.
    return when {
        raw == "walk_up_and_press_im_here" -> "Walk up + tap 'I'm here' to anchor the fit"
        raw == "walk_farther_from_sensor_to_widen_range" ->
            "Walk farther so distance range widens past 5 m"
        raw.startsWith("need_") && raw.endsWith("_more_samples") ->
            "Keep walking — ${raw.removePrefix("need_").removeSuffix("_more_samples")} more samples needed"
        else -> raw.replace("_", " ")
    }
}

private fun prettyWarning(raw: String): String {
    // Note: fall-through to the original mapper below for older keys.
    if (raw == "queued_offline_will_sync_when_backend_reachable") {
        return "Queued offline — will sync when you reach a WiFi AP that can see the backend."
    }
    // Backend warnings are slug-style ("gps_drift_27m_likely_wrong_coords");
    // make them readable without needing a separate i18n table.
    return when {
        raw.startsWith("gps_drift") -> {
            val m = Regex("""gps_drift_(\d+)m""").find(raw)?.groupValues?.getOrNull(1)
            "Phone GPS is ${m ?: "?"} m from the sensor's recorded coordinates — the DB row may be wrong."
        }
        raw.contains("likely_swapped") -> {
            val other = Regex("""was_([^_]+)_likely""").find(raw)?.groupValues?.getOrNull(1)
            "Loudest sensor here is ${other ?: "another node"} — labels may be swapped."
        }
        raw.startsWith("no_rssi_heard") ->
            "No RSSI heard for this sensor at touch — confirm the BLE adv is running."
        else -> raw
    }
}

@Composable
private fun InfoRow(label: String, value: String) {
    Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
        Text(label, style = MaterialTheme.typography.bodySmall,
             color = MaterialTheme.colorScheme.onSurfaceVariant)
        Text(value, style = MaterialTheme.typography.bodySmall,
             fontFamily = FontFamily.Monospace)
    }
}
