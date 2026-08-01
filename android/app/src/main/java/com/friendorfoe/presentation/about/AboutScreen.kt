package com.friendorfoe.presentation.about

import android.content.Intent
import android.net.Uri
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.defaultMinSize
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.selection.toggleable
import androidx.compose.material3.Button
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.friendorfoe.presentation.components.FofActionRow
import com.friendorfoe.presentation.components.FofScreenHeader
import com.friendorfoe.presentation.components.FofSection
import com.friendorfoe.presentation.permissions.PermissionBackedToggle
import com.friendorfoe.presentation.permissions.PermissionBindings
import com.friendorfoe.presentation.permissions.PermissionUiState
import com.friendorfoe.presentation.permissions.isUsable
import com.friendorfoe.presentation.permissions.permissionExplanation
import com.friendorfoe.presentation.permissions.permissionRecovery
import com.friendorfoe.presentation.permissions.permissionTitle
import com.friendorfoe.presentation.permissions.rememberPermissionBindings

val INFO_SECTION_TITLES = listOf(
    "Source & permission status",
    "Settings",
    "Guide & category legend",
    "Privacy & Data",
    "About, support, version & updates",
    "Advanced",
)

data class InfoActions(
    val onSetSetting: (InfoSettingKey, Boolean) -> Unit = { _, _ -> },
    val permissionStateFor: (InfoSettingKey) -> PermissionUiState = {
        PermissionUiState.Granted
    },
    val settingDisabledReason: (InfoSettingKey) -> String? = { null },
    val onEditBackendUrl: (String) -> Unit = {},
    val onSaveBackendUrl: () -> Unit = {},
    val onTestConnection: () -> Unit = {},
    val onCheckForUpdates: () -> Unit = {},
    val onOpenUpdate: (String) -> Unit = {},
    val onContactSupport: () -> Unit = {},
    val onRefreshCalibration: () -> Unit = {},
    val onOpenReference: () -> Unit = {},
    val onOpenMagneticField: () -> Unit = {},
    val onOpenIrLikeLight: () -> Unit = {},
    val onOpenCalibration: () -> Unit = {},
)

@Suppress("UNUSED_PARAMETER")
@Composable
fun AboutScreen(
    onBack: () -> Unit,
    viewModel: AboutViewModel? = null,
    onNavigateToCalibrate: (() -> Unit)? = null,
    onNavigateToEmfSweep: (() -> Unit)? = null,
    onNavigateToIrCameraScan: (() -> Unit)? = null,
    onNavigateToPrivacy: (() -> Unit)? = null,
    onNavigateToReference: (() -> Unit)? = null,
    permissionBindings: PermissionBindings? = null,
) {
    val context = LocalContext.current
    val state by if (viewModel != null) {
        viewModel.uiState.collectAsStateWithLifecycle()
    } else {
        androidx.compose.runtime.remember { androidx.compose.runtime.mutableStateOf(InfoUiState()) }
    }
    val activePermissionBindings = if (permissionBindings != null) {
        permissionBindings
    } else {
        rememberPermissionBindings(
            onPermissionResolution = { feature, resolved ->
                viewModel?.resolvePendingPermission(feature, resolved)
            }
        )
    }
    val pendingPermissionSetting by if (viewModel != null) {
        viewModel.pendingPermissionSetting.collectAsStateWithLifecycle()
    } else {
        remember {
            androidx.compose.runtime.mutableStateOf<PendingInfoPermissionSetting?>(null)
        }
    }
    val actions = InfoActions(
        onSetSetting = { key, enabled ->
            val feature = permissionFeatureForSetting(key)
            val permissionState = feature?.let(activePermissionBindings::stateFor)
            when {
                feature == null || !enabled -> viewModel?.setSetting(key, enabled)
                permissionState?.isUsable() == true -> viewModel?.setSetting(key, true)
                else -> viewModel?.beginPermissionEnable(key)
            }
        },
        permissionStateFor = { key ->
            permissionFeatureForSetting(key)?.let(activePermissionBindings::stateFor)
                ?: PermissionUiState.Granted
        },
        settingDisabledReason = { key ->
            infoSettingDisabledReason(
                key = key,
                settings = state.settings,
                phonePrivacyPermission = activePermissionBindings.stateFor(
                    com.friendorfoe.presentation.permissions.AppFeature.PHONE_PRIVACY_SCAN
                ),
            )
        },
        onEditBackendUrl = { viewModel?.editBackendUrl(it) },
        onSaveBackendUrl = { viewModel?.saveBackendUrl() },
        onTestConnection = { viewModel?.testConnection() },
        onCheckForUpdates = { viewModel?.checkForUpdates() },
        onOpenUpdate = { url -> context.openUri(url) },
        onContactSupport = {
            context.openUri(
                "mailto:lnxgod@gmail.com?subject=" +
                    Uri.encode("Friend or Foe feedback"),
            )
        },
        onRefreshCalibration = { viewModel?.refreshCalibrationAvailability() },
        onOpenReference = { onNavigateToReference?.invoke() },
        onOpenMagneticField = { onNavigateToEmfSweep?.invoke() },
        onOpenIrLikeLight = { onNavigateToIrCameraScan?.invoke() },
        onOpenCalibration = { onNavigateToCalibrate?.invoke() },
    )
    InfoContent(
        state = state,
        actions = actions,
    )

    pendingPermissionSetting?.takeIf { !it.requestLaunched }?.let { pending ->
        val key = pending.key
        val feature = requireNotNull(permissionFeatureForSetting(key))
        val permissionState = activePermissionBindings.stateFor(feature)
        val canRequest = permissionState == PermissionUiState.Denied
        AlertDialog(
            onDismissRequest = { viewModel?.cancelPendingPermission() },
            title = { Text(permissionTitle(feature)) },
            text = {
                Text(
                    if (canRequest) permissionExplanation(feature)
                    else permissionRecovery(feature, permissionState)
                )
            },
            confirmButton = {
                TextButton(
                    enabled = permissionState != PermissionUiState.Loading,
                    onClick = {
                        if (canRequest) {
                            viewModel?.markPermissionRequestLaunched()
                            activePermissionBindings.request(feature) { resolved ->
                                viewModel?.resolvePendingPermission(feature, resolved)
                            }
                        } else {
                            viewModel?.markPermissionRequestLaunched()
                            activePermissionBindings.openSettings(feature)
                        }
                    },
                ) {
                    Text(if (canRequest) "Continue" else "Open settings")
                }
            },
            dismissButton = {
                TextButton(onClick = { viewModel?.cancelPendingPermission() }) { Text("Cancel") }
            },
        )
    }
}

private fun android.content.Context.openUri(raw: String) {
    runCatching {
        startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(raw)))
    }
}

@Composable
fun InfoContent(
    state: InfoUiState,
    actions: InfoActions,
    modifier: Modifier = Modifier,
) {
    LazyColumn(
        modifier = modifier.fillMaxSize().testTag("info_list"),
        contentPadding = PaddingValues(start = 16.dp, top = 16.dp, end = 16.dp, bottom = 112.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        item(key = "info_header") {
            FofScreenHeader("Info")
        }
        item(key = "info_sources") {
            InfoSection(index = 0) { SourcePermissionRows(state) }
        }
        item(key = "info_settings") {
            InfoSection(index = 1) { RuntimeSettingsRows(state, actions) }
        }
        item(key = "info_guide") {
            InfoSection(index = 2) { GuideAndLegendRows(actions) }
        }
        item(key = "info_privacy_data") {
            InfoSection(index = 3) { PrivacyDataCopy() }
        }
        item(key = "info_about") {
            InfoSection(index = 4) { AboutAndUpdateRows(state, actions) }
        }
        item(key = "info_advanced") {
            InfoSection(index = 5) { AdvancedRows(state, actions) }
        }
    }
}

@Composable
private fun InfoSection(
    index: Int,
    content: @Composable androidx.compose.foundation.layout.ColumnScope.() -> Unit,
) {
    FofSection(
        title = INFO_SECTION_TITLES[index],
        modifier = Modifier.testTag("info_section_$index"),
        content = content,
    )
}

@Composable
private fun SourcePermissionRows(state: InfoUiState) {
    if (state.sourceStatus.isEmpty()) {
        Text(
            text = "Runtime status appears here as detection sources are configured.",
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        return
    }
    state.sourceStatus.forEachIndexed { index, source ->
        SourceStatusRow(source)
        if (index != state.sourceStatus.lastIndex) {
            HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
        }
    }
}

@Composable
private fun SourceStatusRow(source: InfoSourceStatus) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .defaultMinSize(minHeight = 56.dp)
            .padding(vertical = 8.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Column(modifier = Modifier.weight(1f)) {
            Text(
                text = source.label,
                style = MaterialTheme.typography.bodyMedium,
                fontWeight = FontWeight.SemiBold,
            )
            source.detail?.let {
                Text(
                    text = it,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
        StatusPill(
            text = source.statusText,
            tone = when {
                source.effective == true -> InfoTone.Success
                source.effective == false -> InfoTone.Warning
                !source.configured -> InfoTone.Muted
                else -> InfoTone.Neutral
            },
        )
    }
}

@Composable
private fun RuntimeSettingsRows(state: InfoUiState, actions: InfoActions) {
    SettingsGroupLabel("Detection sources")
    SettingsToggleRow(
        key = InfoSettingKey.ADS_B,
        title = "ADS-B aircraft",
        description = "Aircraft from public transponder data services",
        checked = state.settings.adsbEnabled,
        actions = actions,
    )
    SettingsToggleRow(
        key = InfoSettingKey.BLE_REMOTE_ID,
        title = "BLE Remote ID",
        description = "Nearby broadcast Remote ID over Bluetooth",
        checked = state.settings.bleRidEnabled,
        actions = actions,
    )
    SettingsToggleRow(
        key = InfoSettingKey.WIFI_REMOTE_ID,
        title = "Wi-Fi Remote ID",
        description = "Remote ID and drone signals available to Android Wi-Fi scanning",
        checked = state.settings.wifiEnabled,
        actions = actions,
    )

    SettingsGroupDivider()
    SettingsGroupLabel("Privacy detection")
    SettingsToggleRow(
        key = InfoSettingKey.PHONE_PRIVACY_SCAN,
        title = "Phone privacy scan",
        description = "Use this phone's local BLE and Wi-Fi collectors",
        checked = state.settings.phonePrivacyScanEnabled,
        actions = actions,
    )
    SettingsToggleRow(
        key = InfoSettingKey.STALKER,
        title = "Follower pattern detection",
        description = "Look for repeated or lingering nearby-device observations",
        checked = state.settings.stalkerEnabled,
        actions = actions,
    )
    SettingsToggleRow(
        key = InfoSettingKey.WIFI_ANOMALY,
        title = "Wi-Fi anomaly detection",
        description = "Look for suspicious access-point behavior and duplicates",
        checked = state.settings.wifiAnomalyEnabled,
        actions = actions,
    )
    SettingsToggleRow(
        key = InfoSettingKey.ULTRASONIC,
        title = "Ultrasonic sampling",
        description = "Sample 18–22 kHz audio when microphone access is granted",
        checked = state.settings.ultrasonicEnabled,
        actions = actions,
    )

    SettingsGroupDivider()
    SettingsGroupLabel("Sensor network")
    SettingsToggleRow(
        key = InfoSettingKey.SENSOR_BACKEND,
        title = "Sensor backend connection",
        description = "Allow AR, Map, and Privacy to poll the configured sensor backend",
        checked = state.settings.sensorBackendEnabled,
        actions = actions,
    )
    SettingsToggleRow(
        key = InfoSettingKey.BACKEND_ONLY,
        title = "Backend-only mode",
        description = "Pause phone radio collectors while keeping network feeds available",
        checked = state.settings.backendOnlyMode,
        actions = actions,
    )
    BackendEndpointEditor(state, actions)

    SettingsGroupDivider()
    SettingsGroupLabel("Alerts")
    SettingsToggleRow(
        key = InfoSettingKey.PRIVACY_ALERTS,
        title = "Privacy finding notifications",
        description = "Notify only for eligible high-risk privacy findings",
        checked = state.settings.privacyNotificationsEnabled,
        actions = actions,
    )
    SettingsToggleRow(
        key = InfoSettingKey.DRONE_ALERTS,
        title = "Drone alerts",
        description = "Notify when a Remote ID or Wi-Fi drone is detected",
        checked = state.settings.droneAlertsEnabled,
        actions = actions,
    )
    SettingsToggleRow(
        key = InfoSettingKey.HELICOPTER_ALERTS,
        title = "Helicopter alerts",
        description = "Notify when a helicopter is detected",
        checked = state.settings.helicopterAlertsEnabled,
        actions = actions,
    )
    SettingsToggleRow(
        key = InfoSettingKey.MILITARY_ALERTS,
        title = "Military alerts",
        description = "Notify for classified military aircraft within the configured range",
        checked = state.settings.militaryAlertsEnabled,
        actions = actions,
    )
    SettingsToggleRow(
        key = InfoSettingKey.POLICE_ALERTS,
        title = "Public-safety alerts",
        description = "Notify for government, emergency, or public-safety aircraft",
        checked = state.settings.policeAlertsEnabled,
        actions = actions,
    )
}

@Composable
private fun SettingsGroupLabel(text: String) {
    Text(
        text = text,
        style = MaterialTheme.typography.labelLarge,
        fontWeight = FontWeight.Bold,
        color = MaterialTheme.colorScheme.primary,
        modifier = Modifier.padding(bottom = 4.dp),
    )
}

@Composable
private fun SettingsGroupDivider() {
    HorizontalDivider(
        color = MaterialTheme.colorScheme.outlineVariant,
        modifier = Modifier.padding(vertical = 14.dp),
    )
}

@Composable
private fun SettingsToggleRow(
    key: InfoSettingKey,
    title: String,
    description: String,
    checked: Boolean,
    actions: InfoActions,
) {
    val permissionFeature = permissionFeatureForSetting(key)
    val permissionState = permissionFeature?.let { actions.permissionStateFor(key) }
    val disabledReason = actions.settingDisabledReason(key)
    val enabled = disabledReason == null
    if (permissionFeature != null && permissionState != null) {
        PermissionBackedToggle(
            tag = "setting_${key.name.lowercase()}",
            label = title,
            description = description,
            checked = checked,
            permissionState = permissionState,
            onOpenExplanation = { actions.onSetSetting(key, true) },
            onCommitChecked = { actions.onSetSetting(key, it) },
            enabled = enabled,
            disabledReason = disabledReason,
        )
        return
    }
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .defaultMinSize(minHeight = 60.dp)
            .testTag("setting_${key.name.lowercase()}")
            .toggleable(
                value = checked && enabled,
                enabled = enabled,
                role = Role.Switch,
                onValueChange = { actions.onSetSetting(key, it) },
            )
            .padding(vertical = 8.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Column(modifier = Modifier.weight(1f)) {
            Text(
                text = title,
                style = MaterialTheme.typography.bodyMedium,
                fontWeight = FontWeight.SemiBold,
            )
            Text(
                text = disabledReason?.let { "$description · $it" } ?: description,
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
        Switch(checked = checked && enabled, onCheckedChange = null, enabled = enabled)
    }
}

@Composable
private fun BackendEndpointEditor(state: InfoUiState, actions: InfoActions) {
    Column(
        modifier = Modifier
            .fillMaxWidth()
            .padding(top = 12.dp),
        verticalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        OutlinedTextField(
            value = state.backendUrlDraft,
            onValueChange = actions.onEditBackendUrl,
            modifier = Modifier.fillMaxWidth().testTag("backend_url"),
            label = { Text("Backend URL") },
            placeholder = { Text("http://sensor-host:8000/") },
            singleLine = true,
            isError = state.backendUrlError != null,
            textStyle = MaterialTheme.typography.bodyMedium.copy(fontFamily = FontFamily.Monospace),
            supportingText = state.backendUrlError?.let { error ->
                { Text(error) }
            },
        )
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            Button(
                onClick = actions.onSaveBackendUrl,
                enabled = state.backendUrlCanSave,
                modifier = Modifier.testTag("backend_save"),
            ) {
                Text("Save")
            }
            TextButton(
                onClick = actions.onTestConnection,
                enabled = state.backendUrlCanTest,
                modifier = Modifier.testTag("backend_test"),
            ) {
                Text("Test connection")
            }
        }
        ConnectionStatusRow(state)
    }
}

@Composable
private fun ConnectionStatusRow(state: InfoUiState) {
    val (text, tone) = when (val connection = state.connection) {
        ConnectionTestState.Idle -> {
            val configured = state.settings.backendUrl
            if (state.settings.sensorBackendEnabled) {
                "Not tested · $configured" to InfoTone.Neutral
            } else {
                "Backend connection is off · $configured" to InfoTone.Muted
            }
        }
        is ConnectionTestState.Checking ->
            "Checking ${connection.endpoint.baseUrl}" to InfoTone.Neutral
        is ConnectionTestState.Connected -> {
            val version = connection.serverVersion?.let { " · server $it" }.orEmpty()
            "Connected · ${connection.endpoint.baseUrl}$version" to InfoTone.Success
        }
        is ConnectionTestState.Failed ->
            "Connection failed · ${connection.endpoint.baseUrl}. Check the address and network." to
                InfoTone.Warning
    }
    StatusPill(
        text = text,
        tone = tone,
        modifier = Modifier.testTag("backend_connection_status"),
    )
}

@Composable
private fun GuideAndLegendRows(actions: InfoActions) {
    Text(
        text = "Point AR at the sky for a camera view, use Map for area context, or use List for a fast readable inventory.",
        style = MaterialTheme.typography.bodyMedium,
    )
    Spacer(Modifier.height(8.dp))
    Text(
        text = "BLE and Wi-Fi Remote ID can work without an internet aircraft feed. ADS-B and weather need a network connection.",
        style = MaterialTheme.typography.bodyMedium,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
    )
    FofActionRow(
        title = "Aircraft & drone reference",
        description = "Browse identifiers, categories, and cautious capability notes",
        trailingLabel = "Browse",
        onClick = actions.onOpenReference,
        modifier = Modifier.testTag("info_reference_guide"),
    )
    HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
    Text(
        text = "Category colors",
        style = MaterialTheme.typography.labelLarge,
        fontWeight = FontWeight.Bold,
        modifier = Modifier.padding(top = 12.dp, bottom = 6.dp),
    )
    listOf(
        LegendSpec(Color(0xFF4CAF50), "Commercial", "Airline flights"),
        LegendSpec(Color(0xFFFFA726), "General aviation", "Private and light aircraft"),
        LegendSpec(Color(0xFFF44336), "Military", "Aircraft classified as military"),
        LegendSpec(Color(0xFF26A69A), "Helicopter", "Rotorcraft"),
        LegendSpec(Color(0xFFE65100), "Government / law", "Government or public-safety classification"),
        LegendSpec(Color(0xFFE91E63), "Emergency", "Emergency classification or squawk"),
        LegendSpec(Color(0xFF8D6E63), "Cargo", "Cargo carriers"),
        LegendSpec(Color(0xFF2196F3), "Drone", "Remote ID or Wi-Fi UAS"),
        LegendSpec(Color(0xFF9E9E9E), "Unknown", "Not enough data to classify"),
    ).forEach { spec -> LegendRow(spec) }
}

private data class LegendSpec(val color: Color, val title: String, val description: String)

@Composable
private fun LegendRow(spec: LegendSpec) {
    Row(
        modifier = Modifier.fillMaxWidth().padding(vertical = 5.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        Box(Modifier.size(12.dp).background(spec.color, CircleShape))
        Column {
            Text(spec.title, style = MaterialTheme.typography.bodyMedium, fontWeight = FontWeight.Medium)
            Text(
                spec.description,
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
    }
}

@Composable
private fun PrivacyDataCopy() {
    listOf(
        "History may store observations and phone coordinates locally.",
        "ADS-B and weather services may receive location when those features are used.",
        "A configured sensor backend exchanges detection data with this app.",
        "Calibration sends operator/session GPS to the configured backend when used.",
    ).forEach { fact -> FactRow(fact) }
}

@Composable
private fun FactRow(text: String) {
    Row(
        modifier = Modifier.fillMaxWidth().padding(vertical = 5.dp),
        verticalAlignment = Alignment.Top,
    ) {
        Text(
            text = "•",
            modifier = Modifier.width(20.dp),
            color = MaterialTheme.colorScheme.primary,
            fontWeight = FontWeight.Bold,
        )
        Text(text = text, style = MaterialTheme.typography.bodyMedium)
    }
}

@Composable
private fun AboutAndUpdateRows(state: InfoUiState, actions: InfoActions) {
    Text(
        text = "Friend or Foe",
        style = MaterialTheme.typography.titleMedium,
        fontWeight = FontWeight.Bold,
    )
    Text(
        text = "A readable field view of nearby aircraft, drones, and privacy observations.",
        style = MaterialTheme.typography.bodyMedium,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
        modifier = Modifier.padding(top = 2.dp),
    )
    FofActionRow(
        title = "Contact & feedback",
        description = "lnxgod@gmail.com",
        trailingLabel = "Email",
        onClick = actions.onContactSupport,
        modifier = Modifier.testTag("info_contact_support"),
    )
    HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
    Row(
        modifier = Modifier.fillMaxWidth().padding(vertical = 12.dp),
        horizontalArrangement = Arrangement.SpaceBetween,
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Column {
            Text("Installed version", style = MaterialTheme.typography.bodyMedium, fontWeight = FontWeight.SemiBold)
            Text(
                "Version ${state.installedVersion.name}",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
        state.installedVersion.code?.let {
            StatusPill("Build $it", InfoTone.Muted)
        }
    }
    UpdateRow(state.updateState, actions)
}

@Composable
private fun UpdateRow(update: UpdateUiState, actions: InfoActions) {
    when (update) {
        UpdateUiState.Idle -> FofActionRow(
            title = "App updates",
            description = "Check the official release feed",
            trailingLabel = "Check",
            onClick = actions.onCheckForUpdates,
            modifier = Modifier.testTag("info_check_updates"),
        )
        UpdateUiState.Checking -> FofActionRow(
            title = "Checking for updates",
            description = "Comparing ordered app versions",
            trailingLabel = "Checking…",
            enabled = false,
            onClick = actions.onCheckForUpdates,
            modifier = Modifier.testTag("info_check_updates"),
        )
        is UpdateUiState.UpToDate -> FofActionRow(
            title = "Up to date",
            description = "Version ${update.installed.name} is not older than the latest release",
            trailingLabel = "Check again",
            onClick = actions.onCheckForUpdates,
            modifier = Modifier.testTag("info_check_updates"),
        )
        is UpdateUiState.Available -> FofActionRow(
            title = "Update available",
            description = "Version ${update.remote.version.name}",
            trailingLabel = "Open",
            onClick = { actions.onOpenUpdate(update.remote.releaseUrl) },
            modifier = Modifier.testTag("info_open_update"),
        )
        is UpdateUiState.Failed -> FofActionRow(
            title = update.message,
            description = "Check your network and try again. Your installed app is unchanged.",
            trailingLabel = "Retry",
            onClick = actions.onCheckForUpdates,
            modifier = Modifier.testTag("info_check_updates"),
        )
    }
}

@Composable
private fun AdvancedRows(state: InfoUiState, actions: InfoActions) {
    InfoAdvancedActionRow(
        title = "Magnetic-field sweep",
        description = "Inspect relative magnetometer changes; this does not identify a device",
        onClick = actions.onOpenMagneticField,
        tag = "advanced_magnetic_field",
    )
    HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
    InfoAdvancedActionRow(
        title = "IR-like light scan",
        description = "Inspect persistent bright camera points; this does not identify a camera",
        onClick = actions.onOpenIrLikeLight,
        tag = "advanced_ir_like_light",
    )
    HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
    val calibrationStatus = when {
        state.calibrationEntryAvailable -> "Open"
        state.sessionHealth is com.friendorfoe.data.repository.SessionHealth.Checking -> "Checking…"
        else -> "Unavailable"
    }
    InfoAdvancedActionRow(
        title = "Triangulation Calibration",
        description = if (state.calibrationEntryAvailable) {
            "Backend health was confirmed this session. Calibration still runs its own preflight."
        } else {
            "Requires this configured backend to pass a health check in this app session."
        },
        trailingLabel = calibrationStatus,
        enabled = state.calibrationEntryAvailable,
        onClick = actions.onOpenCalibration,
        tag = "calibration_entry",
    )
    TextButton(
        onClick = actions.onRefreshCalibration,
        enabled = state.backendUrlCanTest,
        modifier = Modifier.testTag("calibration_refresh"),
    ) {
        Text("Refresh backend health")
    }
}

@Composable
private fun InfoAdvancedActionRow(
    title: String,
    description: String,
    tag: String,
    trailingLabel: String = "Open",
    enabled: Boolean = true,
    onClick: () -> Unit,
) {
    val contentColor = if (enabled) {
        MaterialTheme.colorScheme.onSurface
    } else {
        MaterialTheme.colorScheme.onSurface.copy(alpha = 0.48f)
    }
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .defaultMinSize(minHeight = 48.dp)
            .testTag(tag)
            .clickable(enabled = enabled, onClick = onClick)
            .padding(vertical = 12.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Column(
            modifier = Modifier.weight(1f),
            verticalArrangement = Arrangement.spacedBy(3.dp),
        ) {
            Text(
                text = title,
                style = MaterialTheme.typography.bodyLarge,
                fontWeight = FontWeight.Medium,
                color = contentColor,
            )
            Text(
                text = description,
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant.copy(
                    alpha = if (enabled) 1f else 0.48f,
                ),
                modifier = Modifier.testTag("${tag}_description"),
            )
        }
        Text(
            text = trailingLabel,
            style = MaterialTheme.typography.labelLarge,
            color = if (enabled) {
                MaterialTheme.colorScheme.primary
            } else {
                MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.48f)
            },
        )
    }
}

private enum class InfoTone { Neutral, Success, Warning, Muted }

@Composable
private fun StatusPill(
    text: String,
    tone: InfoTone,
    modifier: Modifier = Modifier,
) {
    val foreground = when (tone) {
        InfoTone.Neutral -> MaterialTheme.colorScheme.primary
        InfoTone.Success -> Color(0xFF2E7D32)
        InfoTone.Warning -> MaterialTheme.colorScheme.error
        InfoTone.Muted -> MaterialTheme.colorScheme.onSurfaceVariant
    }
    Surface(
        modifier = modifier.semantics(mergeDescendants = true) {},
        shape = RoundedCornerShape(999.dp),
        color = foreground.copy(alpha = 0.10f),
    ) {
        Text(
            text = text,
            style = MaterialTheme.typography.labelMedium,
            fontWeight = FontWeight.SemiBold,
            color = foreground,
            modifier = Modifier.padding(horizontal = 10.dp, vertical = 6.dp),
        )
    }
}
