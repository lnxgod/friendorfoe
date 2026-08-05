package com.friendorfoe.presentation.permissions

import android.app.Activity
import android.content.Context
import android.content.ContextWrapper
import android.content.Intent
import android.net.Uri
import android.os.Build
import android.provider.Settings
import androidx.activity.ComponentActivity
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
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
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
import androidx.lifecycle.ViewModelStoreOwner
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import kotlinx.coroutines.launch

@Stable
class PermissionBindings internal constructor(
    val states: Map<AppFeature, PermissionUiState>,
    private val requestFeature: (AppFeature) -> Unit,
    private val openFeatureSettings:
        (AppFeature, PermissionUiState) -> PermissionSettingsLaunchResult,
) {
    fun stateFor(feature: AppFeature): PermissionUiState =
        states[feature] ?: PermissionUiState.Loading

    fun request(feature: AppFeature) = requestFeature(feature)

    fun openSettings(feature: AppFeature): PermissionSettingsLaunchResult =
        openFeatureSettings(feature, stateFor(feature))
}

@Composable
fun rememberPermissionBindings(
    viewModel: PermissionStateViewModel? = null,
): PermissionBindings {
    val context = LocalContext.current
    val activity = context.findActivity()
        ?: error("Permission bindings require a ComponentActivity")
    val activityOwner: ViewModelStoreOwner = activity
    val sharedViewModel = viewModel ?: hiltViewModel(activityOwner)
    val lifecycleOwner = LocalLifecycleOwner.current
    val scope = rememberCoroutineScope()
    val states by sharedViewModel.states.collectAsStateWithLifecycle()

    fun rationaleSnapshot(): Map<String, Boolean> =
        activity?.let(::capturePermissionRationales).orEmpty()

    val launcher = rememberLauncherForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions()
    ) { grantResults ->
        sharedViewModel.onPermissionResult(
            grantResultByPermission = grantResults,
            rationaleByPermission = rationaleSnapshot(),
        )
    }

    val initialRationales = remember(activity) { rationaleSnapshot() }
    LaunchedEffect(initialRationales) {
        sharedViewModel.refresh(initialRationales)
    }

    DisposableEffect(lifecycleOwner, activity) {
        val observer = LifecycleEventObserver { _, event ->
            if (event == Lifecycle.Event.ON_RESUME) {
                val rationales = rationaleSnapshot()
                scope.launch { sharedViewModel.refresh(rationales) }
            }
        }
        lifecycleOwner.lifecycle.addObserver(observer)
        onDispose { lifecycleOwner.lifecycle.removeObserver(observer) }
    }

    return PermissionBindings(
        states = states,
        requestFeature = { feature ->
            scope.launch {
                sharedViewModel.requestPermissions(
                    feature = feature,
                    launcher = PermissionLauncher(launcher::launch),
                )
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
    when {
        state.isUsableFor(feature) -> grantedContent()

        state == PermissionUiState.Loading -> Box(
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
    Column(modifier = Modifier.fillMaxWidth()) {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .defaultMinSize(minHeight = 60.dp)
                .testTag(tag)
                .toggleable(
                    value = checked,
                    enabled = enabled,
                    role = Role.Switch,
                    onValueChange = { requested ->
                        when (
                            val action = permissionToggleAction(
                                configuredChecked = checked,
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
                checked = checked,
                onCheckedChange = null,
                enabled = enabled,
            )
        }
        if (checked && !usable) {
            TextButton(
                onClick = onOpenExplanation,
                modifier = Modifier.testTag("${tag}_recovery"),
            ) {
                Text("Grant permission")
            }
        }
    }
}

fun capturePermissionRationales(activity: Activity): Map<String, Boolean> {
    val sdk = Build.VERSION.SDK_INT
    return AppFeature.entries
        .flatMapTo(linkedSetOf()) { requiredPermissions(it, sdk) }
        .associateWith(activity::shouldShowRequestPermissionRationale)
}

enum class PermissionSettingsLaunchResult {
    Opened,
    Failed,
}

fun openApplicationDetailsSettings(context: Context): PermissionSettingsLaunchResult =
    launchSettingsIntent(
        context,
        Intent(
            Settings.ACTION_APPLICATION_DETAILS_SETTINGS,
            Uri.parse("package:${context.packageName}"),
        ),
    )

fun openFeaturePermissionSettings(
    context: Context,
    feature: AppFeature,
    state: PermissionUiState,
): PermissionSettingsLaunchResult {
    if (state == PermissionUiState.NotificationChannelBlocked) {
        val channelResult = launchSettingsIntent(
            context,
            Intent(Settings.ACTION_CHANNEL_NOTIFICATION_SETTINGS).apply {
                putExtra(Settings.EXTRA_APP_PACKAGE, context.packageName)
                putExtra(Settings.EXTRA_CHANNEL_ID, notificationChannelId(feature))
            },
        )
        if (channelResult == PermissionSettingsLaunchResult.Opened) return channelResult
    }
    return openApplicationDetailsSettings(context)
}

private fun launchSettingsIntent(
    context: Context,
    intent: Intent,
): PermissionSettingsLaunchResult = runCatching {
    if (context !is Activity) intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
    context.startActivity(intent)
    PermissionSettingsLaunchResult.Opened
}.getOrElse { PermissionSettingsLaunchResult.Failed }

private tailrec fun Context.findActivity(): ComponentActivity? = when (this) {
    is ComponentActivity -> this
    is ContextWrapper -> baseContext.findActivity()
    else -> null
}
