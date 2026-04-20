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
import com.friendorfoe.calibration.CalibrationViewModel.SensorInfo
import com.friendorfoe.calibration.CalibrationViewModel.SensorReading

private val OK_GREEN     = Color(0xFF4CAF50)
private val WARN_AMBER   = Color(0xFFFFB300)
private val ERROR_RED    = Color(0xFFE53935)
private val NEUTRAL_GREY = Color(0xFF616161)

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun CalibrateScreen(
    onBack: () -> Unit,
    viewModel: CalibrationViewModel = hiltViewModel(),
) {
    val state by viewModel.state.collectAsState()
    val context = LocalContext.current
    val lifecycleOwner = LocalLifecycleOwner.current

    // Refresh BT state + reach connectivity check whenever the screen
    // returns to the foreground (operator may have just enabled BT or
    // edited the URL/token).
    DisposableEffect(lifecycleOwner) {
        val obs = LifecycleEventObserver { _, event ->
            if (event == Lifecycle.Event.ON_RESUME) {
                viewModel.refreshBluetoothState()
                viewModel.refreshConnectivity()
            }
        }
        lifecycleOwner.lifecycle.addObserver(obs)
        onDispose { lifecycleOwner.lifecycle.removeObserver(obs) }
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
                    IconButton(onClick = { viewModel.refreshConnectivity() }) {
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
                    TextButton(onClick = { viewModel.refreshConnectivity() }) {
                        Text("Test backend connectivity")
                    }
                }
            }

            // ── Walk control ──────────────────────────────────────────
            Row(verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(8.dp)) {
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
                        Text(if (state.sessionReadiness.readyOverall)
                                 "Session ready — finish + apply fit"
                             else "Stop walk + apply fit")
                    }
                }
            }

            // ── Overall session readiness ─────────────────────────────
            if (state.isWalking) {
                val sr = state.sessionReadiness
                val pctReady = if (sr.sensorsTotal > 0)
                    sr.sensorsReady.toFloat() / sr.sensorsTotal else 0f
                val bannerColor = when {
                    sr.readyOverall -> OK_GREEN.copy(alpha = 0.18f)
                    sr.sensorsReady > 0 -> WARN_AMBER.copy(alpha = 0.15f)
                    else -> NEUTRAL_GREY.copy(alpha = 0.15f)
                }
                Surface(color = bannerColor,
                        shape = RoundedCornerShape(8.dp),
                        modifier = Modifier.fillMaxWidth()) {
                    Column(Modifier.padding(12.dp),
                           verticalArrangement = Arrangement.spacedBy(4.dp)) {
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            val icon = if (sr.readyOverall) Icons.Default.CheckCircle
                                       else Icons.Default.Warning
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

            // ── Sensors panel: tap "I'm here" at each ─────────────────
            Surface(
                color = MaterialTheme.colorScheme.surfaceVariant,
                shape = RoundedCornerShape(8.dp),
            ) {
                Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Text("Sensors — walk to each + tap 'I'm here'",
                             style = MaterialTheme.typography.labelLarge,
                             modifier = Modifier.weight(1f))
                        if (state.checkpointResults.isNotEmpty()) {
                            Text("${state.checkpointResults.size}/${state.availableSensors.size} touched",
                                 style = MaterialTheme.typography.labelSmall,
                                 color = MaterialTheme.colorScheme.onSurfaceVariant)
                        }
                    }
                    if (state.availableSensors.isEmpty()) {
                        Text("No sensors loaded. Configure backend + token, then tap Refresh ↻ above.",
                             style = MaterialTheme.typography.bodySmall,
                             color = MaterialTheme.colorScheme.onSurfaceVariant)
                    } else {
                        val readingByid = state.sensorsHearingMe.associateBy { it.sensorId }
                        for (sensor in state.availableSensors) {
                            SensorCard(
                                sensor = sensor,
                                reading = readingByid[sensor.deviceId],
                                result = state.checkpointResults[sensor.deviceId],
                                walking = state.isWalking,
                                onMarkHere = { viewModel.markAtSensor(sensor) },
                            )
                        }
                    }
                }
            }

            // ── Live status ───────────────────────────────────────────
            Surface(
                color = MaterialTheme.colorScheme.surfaceVariant,
                shape = RoundedCornerShape(8.dp),
            ) {
                Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                    Text("Live status", style = MaterialTheme.typography.labelLarge)
                    InfoRow("Walking", if (state.isWalking) "yes" else "no")
                    InfoRow("Session", state.sessionId ?: "—")
                    InfoRow("BLE UUID", state.advertiseUuid?.take(13)?.plus("…") ?: "—")
                    InfoRow("Phone GPS", state.phoneLat?.let {
                        "%.5f, %.5f ±%.0f m".format(it, state.phoneLon ?: 0.0, state.gpsAccuracyM ?: 0f)
                    } ?: "no fix")
                    InfoRow("Trace points", state.tracePoints.toString())
                    InfoRow("Sensor samples", state.samplesTotal.toString())
                }
            }

            // ── Sensors hearing me (live RSSI) ─────────────────────────
            Surface(
                color = MaterialTheme.colorScheme.surfaceVariant,
                shape = RoundedCornerShape(8.dp),
            ) {
                Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                    Text("Sensors hearing this phone (last 10 s)",
                         style = MaterialTheme.typography.labelLarge)
                    if (state.sensorsHearingMe.isEmpty()) {
                        Text(if (state.isWalking) "No sensor sightings yet — keep walking."
                             else "Start a walk to see live sensor RSSI.",
                             color = MaterialTheme.colorScheme.onSurfaceVariant,
                             style = MaterialTheme.typography.bodySmall)
                    } else {
                        for (sensor in state.sensorsHearingMe) {
                            val rssiVal = sensor.rssi
                            val rssiColor = when {
                                rssiVal == null -> MaterialTheme.colorScheme.onSurfaceVariant
                                rssiVal > -55 -> ERROR_RED
                                rssiVal > -75 -> WARN_AMBER
                                else -> MaterialTheme.colorScheme.onSurfaceVariant
                            }
                            Row(verticalAlignment = Alignment.CenterVertically,
                                modifier = Modifier.fillMaxWidth()) {
                                Text(sensor.sensorId,
                                     style = MaterialTheme.typography.bodySmall,
                                     fontFamily = FontFamily.Monospace,
                                     modifier = Modifier.weight(1f))
                                Text(rssiVal?.let { "$it dBm" } ?: "—",
                                     color = rssiColor,
                                     fontWeight = FontWeight.SemiBold,
                                     style = MaterialTheme.typography.bodySmall,
                                     fontFamily = FontFamily.Monospace)
                                Spacer(Modifier.width(8.dp))
                                Text("${sensor.samplesInWindow} smp",
                                     color = MaterialTheme.colorScheme.onSurfaceVariant,
                                     style = MaterialTheme.typography.bodySmall,
                                     fontFamily = FontFamily.Monospace)
                                Spacer(Modifier.width(8.dp))
                                Text(sensor.gpsDistanceM?.let { "%.0fm".format(it) } ?: "—",
                                     color = MaterialTheme.colorScheme.onSurfaceVariant,
                                     style = MaterialTheme.typography.bodySmall,
                                     fontFamily = FontFamily.Monospace)
                            }
                        }
                    }
                }
            }

            // ── Fit result ────────────────────────────────────────────
            state.fitResult?.let { fit ->
                Surface(
                    color = if (state.fitApplied == true)
                        Color(0xFF1B5E20).copy(alpha = 0.20f)
                    else MaterialTheme.colorScheme.surfaceVariant,
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
                        InfoRow("Global path-loss n",
                                fit.get("global_path_loss_exponent")?.toString() ?: "—")
                        InfoRow("Global R²", fit.get("global_r_squared")?.toString() ?: "—")
                        InfoRow("Trace points", fit.get("trace_points")?.toString() ?: "—")
                        InfoRow("Total samples", fit.get("samples_total")?.toString() ?: "—")
                        InfoRow("Sensors checkpointed",
                                fit.get("checkpointed_sensor_count")?.toString() ?: "—")
                        Text("Per-sensor fits:", style = MaterialTheme.typography.bodySmall,
                             modifier = Modifier.padding(top = 8.dp))
                        fit.getAsJsonObject("per_listener")?.entrySet()?.forEach { (sid, info) ->
                            val obj = info.asJsonObject
                            val ok = obj.get("ok")?.asBoolean ?: false
                            val n = obj.get("path_loss_exponent")?.asString ?: "—"
                            val rref = obj.get("rssi_ref")?.asString ?: "—"
                            val r2 = obj.get("r_squared")?.asString ?: "—"
                            val samples = obj.get("samples")?.asString ?: "—"
                            val drift = obj.get("gps_drift_m")?.takeIf { !it.isJsonNull }?.asString
                            Row(modifier = Modifier.fillMaxWidth()) {
                                Text(sid,
                                     style = MaterialTheme.typography.bodySmall,
                                     fontFamily = FontFamily.Monospace,
                                     color = if (ok) MaterialTheme.colorScheme.onSurface
                                             else MaterialTheme.colorScheme.error,
                                     modifier = Modifier.weight(1f))
                                Text(if (ok) "n=$n  ref=$rref  R²=$r2  ($samples)" + (drift?.let { "  drift=${it}m" } ?: "")
                                     else "skip ($samples)",
                                     style = MaterialTheme.typography.bodySmall,
                                     fontFamily = FontFamily.Monospace,
                                     color = MaterialTheme.colorScheme.onSurfaceVariant)
                            }
                        }
                    }
                }
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
        }
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
