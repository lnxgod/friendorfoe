package com.friendorfoe.presentation.permissions

import android.app.Activity
import android.content.Context
import android.content.ContextWrapper
import android.content.Intent
import android.net.Uri
import android.os.Build
import android.provider.Settings
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.defaultMinSize
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.selection.toggleable
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.Stable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalLifecycleOwner
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.unit.dp
import androidx.hilt.navigation.compose.hiltViewModel
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import kotlinx.coroutines.launch

@Stable
class PermissionBindings internal constructor(
    val states: Map<AppFeature, PermissionUiState>,
    private val requestFeature: (AppFeature, (PermissionUiState) -> Unit) -> Unit,
    private val openFeatureSettings: (AppFeature, PermissionUiState) -> Unit,
) {
    fun stateFor(feature: AppFeature): PermissionUiState =
        states[feature] ?: PermissionUiState.Loading

    fun request(
        feature: AppFeature,
        onResolved: (PermissionUiState) -> Unit = {},
    ) = requestFeature(feature, onResolved)

    fun openSettings(feature: AppFeature) =
        openFeatureSettings(feature, stateFor(feature))
}

private data class PendingPermissionRequest(
    val feature: AppFeature,
    val onResolved: (PermissionUiState) -> Unit,
)

@Composable
fun rememberPermissionBindings(
    viewModel: PermissionStateViewModel = hiltViewModel(),
    onPermissionResolution: (AppFeature, PermissionUiState) -> Unit = { _, _ -> },
): PermissionBindings {
    val context = LocalContext.current
    val activity = context.findActivity()
    val lifecycleOwner = LocalLifecycleOwner.current
    val scope = rememberCoroutineScope()
    val states by viewModel.states.collectAsStateWithLifecycle()
    val currentResolutionHandler by rememberUpdatedState(onPermissionResolution)
    var pending by remember { mutableStateOf<PendingPermissionRequest?>(null) }

    fun rationaleSnapshot(): Map<String, Boolean> =
        activity?.let(::capturePermissionRationales).orEmpty()

    val launcher = rememberLauncherForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions()
    ) {
        val completed = pending
        val durableFeature = viewModel.pendingFeature()
        pending = null
        viewModel.onRuntimePermissionsChanged()
        val rationales = rationaleSnapshot()
        scope.launch {
            val evaluated = viewModel.refresh(rationales)
            durableFeature?.let { feature ->
                val resolved = evaluated[feature] ?: PermissionUiState.Loading
                currentResolutionHandler(feature, resolved)
                viewModel.clearPendingFeature()
            }
            completed?.onResolved?.invoke(
                evaluated[completed.feature] ?: PermissionUiState.Loading
            )
        }
    }

    val initialRationales = remember(activity) { rationaleSnapshot() }
    LaunchedEffect(initialRationales) {
        viewModel.refresh(initialRationales)
    }

    DisposableEffect(lifecycleOwner, activity) {
        val observer = LifecycleEventObserver { _, event ->
            if (event == Lifecycle.Event.ON_RESUME) {
                val rationales = rationaleSnapshot()
                scope.launch { viewModel.refresh(rationales) }
            }
        }
        lifecycleOwner.lifecycle.addObserver(observer)
        onDispose { lifecycleOwner.lifecycle.removeObserver(observer) }
    }

    return PermissionBindings(
        states = states,
        requestFeature = { feature, onResolved ->
            scope.launch {
                val missing = viewModel.missingPermissionsFor(feature)
                if (missing.isEmpty()) {
                    val evaluated = viewModel.refresh(rationaleSnapshot())
                    onResolved(evaluated[feature] ?: PermissionUiState.Loading)
                } else {
                    pending = PendingPermissionRequest(feature, onResolved)
                    viewModel.requestPermissions(
                        feature = feature,
                        missing = missing,
                        launcher = PermissionLauncher(launcher::launch),
                    )
                }
            }
        },
        openFeatureSettings = { feature, state ->
            openFeaturePermissionSettings(context, feature, state)
        },
    )
}

@Composable
fun ContextualPermissionGate(
    feature: AppFeature,
    bindings: PermissionBindings = rememberPermissionBindings(),
    onBack: (() -> Unit)? = null,
    grantedContent: @Composable (PermissionUiState) -> Unit,
) {
    val state = bindings.stateFor(feature)
    FeaturePermissionGate(
        feature = feature,
        state = state,
        onRequest = { bindings.request(feature) },
        onOpenSettings = { bindings.openSettings(feature) },
        onBack = onBack,
        grantedContent = { grantedContent(state) },
    )
}

@Composable
fun FeaturePermissionGate(
    feature: AppFeature,
    state: PermissionUiState,
    onRequest: () -> Unit,
    onOpenSettings: () -> Unit,
    onBack: (() -> Unit)? = null,
    grantedContent: @Composable () -> Unit,
) {
    when (state) {
        PermissionUiState.Granted,
        PermissionUiState.Approximate,
        -> grantedContent()

        PermissionUiState.Loading -> Box(
            modifier = Modifier.fillMaxSize().testTag("permission_loading"),
            contentAlignment = Alignment.Center,
        ) {
            Column(horizontalAlignment = Alignment.CenterHorizontally) {
                CircularProgressIndicator()
                Text(
                    text = "Checking permission",
                    modifier = Modifier.padding(top = 12.dp),
                    style = MaterialTheme.typography.bodyMedium,
                )
            }
        }

        else -> PermissionExplanation(
            feature = feature,
            state = state,
            onAction = if (state == PermissionUiState.Denied) onRequest else onOpenSettings,
            onBack = onBack,
        )
    }
}

@Composable
private fun PermissionExplanation(
    feature: AppFeature,
    state: PermissionUiState,
    onAction: () -> Unit,
    onBack: (() -> Unit)?,
) {
    Column(
        modifier = Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(horizontal = 24.dp, vertical = 32.dp)
            .testTag("permission_${feature.name.lowercase()}"),
        verticalArrangement = Arrangement.Center,
    ) {
        Text(
            text = permissionTitle(feature),
            style = MaterialTheme.typography.headlineSmall,
        )
        Text(
            text = if (state == PermissionUiState.Denied) {
                permissionExplanation(feature)
            } else {
                permissionRecovery(feature, state)
            },
            modifier = Modifier.padding(top = 12.dp),
            style = MaterialTheme.typography.bodyLarge,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        Button(
            onClick = onAction,
            modifier = Modifier
                .fillMaxWidth()
                .defaultMinSize(minHeight = 48.dp)
                .padding(top = 24.dp),
        ) {
            Text(if (state == PermissionUiState.Denied) "Continue" else "Open app settings")
        }
        onBack?.let { back ->
            TextButton(
                onClick = back,
                modifier = Modifier.fillMaxWidth().defaultMinSize(minHeight = 48.dp),
            ) {
                Text("Back")
            }
        }
    }
}

@Composable
fun PermissionBackedToggle(
    tag: String,
    label: String,
    description: String,
    checked: Boolean,
    permissionState: PermissionUiState,
    onOpenExplanation: () -> Unit,
    onCommitChecked: (Boolean) -> Unit,
    enabled: Boolean = true,
    disabledReason: String? = null,
) {
    val usable = permissionState.isUsable()
    val effectiveChecked = checked && usable && enabled
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .defaultMinSize(minHeight = 60.dp)
            .testTag(tag)
            .toggleable(
                value = effectiveChecked,
                enabled = enabled,
                role = Role.Switch,
                onValueChange = { requested ->
                    when (
                        val action = permissionToggleAction(
                            configuredChecked = checked,
                            effectiveChecked = effectiveChecked,
                            requestedChecked = requested,
                            permissionState = permissionState,
                        )
                    ) {
                        is PermissionToggleAction.Commit -> onCommitChecked(action.checked)
                        PermissionToggleAction.ShowExplanation -> onOpenExplanation()
                        PermissionToggleAction.NoChange -> Unit
                    }
                },
            )
            .padding(vertical = 8.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Column(modifier = Modifier.weight(1f)) {
            Text(text = label, style = MaterialTheme.typography.bodyMedium)
            Text(
                text = when {
                    !enabled && disabledReason != null -> "$description · $disabledReason"
                    !enabled -> description
                    checked && !usable -> "$description · Permission needed"
                    else -> description
                },
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
        Switch(
            checked = effectiveChecked,
            onCheckedChange = null,
            enabled = enabled,
        )
    }
}

fun capturePermissionRationales(activity: Activity): Map<String, Boolean> {
    val sdk = Build.VERSION.SDK_INT
    return AppFeature.entries
        .flatMapTo(linkedSetOf()) { requiredPermissions(it, sdk) }
        .associateWith(activity::shouldShowRequestPermissionRationale)
}

fun openFeaturePermissionSettings(
    context: Context,
    feature: AppFeature,
    state: PermissionUiState,
) {
    val intent = if (state == PermissionUiState.NotificationChannelBlocked) {
        Intent(Settings.ACTION_CHANNEL_NOTIFICATION_SETTINGS).apply {
            putExtra(Settings.EXTRA_APP_PACKAGE, context.packageName)
            putExtra(Settings.EXTRA_CHANNEL_ID, notificationChannelId(feature))
        }
    } else {
        Intent(
            Settings.ACTION_APPLICATION_DETAILS_SETTINGS,
            Uri.parse("package:${context.packageName}"),
        )
    }
    runCatching { context.startActivity(intent) }
}

private tailrec fun Context.findActivity(): Activity? = when (this) {
    is Activity -> this
    is ContextWrapper -> baseContext.findActivity()
    else -> null
}
