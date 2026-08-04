package com.friendorfoe.presentation.list

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Bluetooth
import androidx.compose.material.icons.filled.CellTower
import androidx.compose.material.icons.filled.Visibility
import androidx.compose.material.icons.filled.Wifi
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.Surface
import androidx.compose.material3.TextButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.platform.LocalLifecycleOwner
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.hilt.navigation.compose.hiltViewModel
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.friendorfoe.domain.model.DetectionSource
import com.friendorfoe.domain.model.ObjectCategory
import com.friendorfoe.domain.model.SkyObject
import com.friendorfoe.domain.model.activeFilterCount
import com.friendorfoe.domain.model.cleared
import com.friendorfoe.presentation.components.FofEmptyState
import com.friendorfoe.presentation.components.FofFailureState
import com.friendorfoe.presentation.components.FofLoadingState
import com.friendorfoe.presentation.components.FofNoMatchesState
import com.friendorfoe.presentation.components.FofScreenHeader
import com.friendorfoe.presentation.components.FofStaleBanner
import com.friendorfoe.presentation.ar.ObjectPeek
import com.friendorfoe.presentation.ar.ObjectPeekState
import com.friendorfoe.presentation.ar.objectPeekEvidence
import com.friendorfoe.presentation.filter.CompactFilterBar
import com.friendorfoe.presentation.filter.FilterModalSheet
import com.friendorfoe.presentation.permissions.AppFeature
import com.friendorfoe.presentation.permissions.PermissionSettingsLaunchResult
import com.friendorfoe.presentation.permissions.PermissionUiState
import com.friendorfoe.presentation.permissions.isUsableFor
import com.friendorfoe.presentation.permissions.rememberPermissionBindings
import com.friendorfoe.presentation.util.categoryBadge
import com.friendorfoe.presentation.util.categoryColor
import java.time.Duration
import java.time.Instant

@Suppress("UNUSED_PARAMETER")
@Composable
fun ListViewScreen(
    onObjectTapped: (String) -> Unit,
    onNavigateToReferenceGuide: (() -> Unit)? = null,
    onNavigateToAbout: (() -> Unit)? = null,
    viewModel: ListViewModel = hiltViewModel(),
) {
    val skyObjects by viewModel.skyObjects.collectAsStateWithLifecycle()
    val activeVisualFocusIds by viewModel.activeVisualFocusIds.collectAsStateWithLifecycle()
    val filterState by viewModel.filterState.collectAsStateWithLifecycle()
    val lifecycleOwner = LocalLifecycleOwner.current
    val permissionBindings = rememberPermissionBindings()
    val locationPermissionState = permissionBindings.stateFor(AppFeature.AR_MAP_LOCATION)
    var filtersOpen by rememberSaveable { mutableStateOf(false) }
    var isResumed by remember(lifecycleOwner) {
        mutableStateOf(lifecycleOwner.lifecycle.currentState.isAtLeast(Lifecycle.State.RESUMED))
    }
    var showLocationRationale by rememberSaveable { mutableStateOf(false) }
    var locationSettingsLaunchFailed by rememberSaveable { mutableStateOf(false) }

    DisposableEffect(lifecycleOwner) {
        val observer = LifecycleEventObserver { _, event ->
            when (event) {
                Lifecycle.Event.ON_RESUME -> isResumed = true
                Lifecycle.Event.ON_PAUSE -> isResumed = false
                else -> Unit
            }
        }
        lifecycleOwner.lifecycle.addObserver(observer)
        onDispose {
            lifecycleOwner.lifecycle.removeObserver(observer)
            viewModel.stopLocationUpdates()
        }
    }

    LaunchedEffect(isResumed, locationPermissionState) {
        if (
            isResumed &&
            locationPermissionState.isUsableFor(AppFeature.AR_MAP_LOCATION)
        ) {
            viewModel.startLocationUpdates()
        } else {
            viewModel.stopLocationUpdates()
        }
    }

    val filterCount = activeFilterCount(filterState)
    val body = when {
        skyObjects.isNotEmpty() -> ListBodyState.Results(skyObjects)
        filterCount > 0 -> ListBodyState.NoMatches(filterCount)
        else -> ListBodyState.NoDetections
    }
    ListDestinationContent(
        state = ListUiState(
            filter = filterState,
            activeFilterCount = filterCount,
            body = body,
            locationPermissionState = locationPermissionState,
            locationSettingsLaunchFailed = locationSettingsLaunchFailed,
        ),
        actions = ListActions(
            onQueryChanged = { viewModel.updateFilter(filterState.copy(searchQuery = it)) },
            onOpenFilters = { filtersOpen = true },
            onClearFilters = { viewModel.updateFilter(filterState.cleared()) },
            onRequestLocation = { showLocationRationale = true },
            onOpenLocationSettings = {
                permissionBindings.openSettings(AppFeature.AR_MAP_LOCATION).also { result ->
                    locationSettingsLaunchFailed = result == PermissionSettingsLaunchResult.Failed
                }
            },
        ),
        onFullDetails = onObjectTapped,
        activeVisualFocusIds = activeVisualFocusIds,
    )

    if (filtersOpen) {
        FilterModalSheet(
            filterState = filterState,
            onFilterStateChange = viewModel::updateFilter,
            onDismiss = { filtersOpen = false },
        )
    }

    if (showLocationRationale) {
        AlertDialog(
            onDismissRequest = { showLocationRationale = false },
            title = { Text("Allow location for nearby distances") },
            text = {
                Text(
                    "Location helps show nearby distance and direction. Your aircraft and drone inventory remains available without it.",
                )
            },
            confirmButton = {
                Button(onClick = {
                    showLocationRationale = false
                    permissionBindings.request(AppFeature.AR_MAP_LOCATION)
                }) { Text("Continue") }
            },
            dismissButton = {
                TextButton(onClick = { showLocationRationale = false }) { Text("Not now") }
            },
        )
    }
}

@Composable
internal fun ListDestinationContent(
    state: ListUiState,
    actions: ListActions,
    onFullDetails: (String) -> Unit,
    activeVisualFocusIds: Set<String> = emptySet(),
) {
    var peekObject by remember { mutableStateOf<SkyObject?>(null) }
    ListContent(
        state = state,
        actions = actions.copy(onOpenPeek = { peekObject = it }),
        activeVisualFocusIds = activeVisualFocusIds,
    )
    peekObject?.let { skyObject ->
        val openDetails = {
            peekObject = null
            onFullDetails(skyObject.id)
        }
        ObjectPeek(
            state = ObjectPeekState(
                objectId = skyObject.id,
                title = listPrimaryText(skyObject),
                evidence = objectPeekEvidence(skyObject.source),
                canCapture = false,
            ),
            onInspect = openDetails,
            onCapture = {},
            onFullDetails = openDetails,
            onDismiss = { peekObject = null },
        )
    }
}

@Composable
internal fun ListContent(
    state: ListUiState,
    actions: ListActions,
    activeVisualFocusIds: Set<String> = emptySet(),
) {
    val visibleCount = visibleListCount(state.body)
    val headerCount = when (state.body) {
        ListBodyState.Loading, is ListBodyState.Failed -> null
        else -> visibleCount
    }
    Column(modifier = Modifier.fillMaxSize()) {
        Column(Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 12.dp)) {
            FofScreenHeader(
                title = "List",
                count = headerCount,
                countLabel = if (headerCount == 1) "object" else "objects",
            )
        }
        CompactFilterBar(
            filterState = state.filter,
            resultCount = headerCount?.let { visibleCount },
            activeFilterCount = state.activeFilterCount,
            onQueryChanged = actions.onQueryChanged,
            onOpenFilters = actions.onOpenFilters,
            onClearFilters = actions.onClearFilters,
        )

        ListLocationRecoveryBanner(
            permissionState = state.locationPermissionState,
            settingsLaunchFailed = state.locationSettingsLaunchFailed,
            onRequestLocation = actions.onRequestLocation,
            onOpenLocationSettings = actions.onOpenLocationSettings,
        )

        when (val body = state.body) {
            ListBodyState.Loading -> FofLoadingState("Loading nearby detections")
            is ListBodyState.Results -> ListRows(
                rows = body.rows,
                actions = actions,
                activeVisualFocusIds = activeVisualFocusIds,
            )
            is ListBodyState.StaleResults -> ListRows(
                rows = body.rows,
                actions = actions,
                activeVisualFocusIds = activeVisualFocusIds,
                staleMessage = body.message,
                staleAgeMs = body.ageMs,
            )
            ListBodyState.NoDetections -> Box(
                modifier = Modifier.fillMaxSize(),
                contentAlignment = Alignment.Center,
            ) {
                FofEmptyState(
                    title = "No nearby detections",
                    detail = "Aircraft and drones will appear here when detected nearby.",
                )
            }
            is ListBodyState.NoMatches -> FofNoMatchesState(
                activeFilterCount = body.activeFilterCount,
                onClearFilters = actions.onClearFilters,
            )
            is ListBodyState.Failed -> FofFailureState(body.message)
        }
    }
}

@Composable
private fun ListLocationRecoveryBanner(
    permissionState: PermissionUiState,
    settingsLaunchFailed: Boolean,
    onRequestLocation: () -> Unit,
    onOpenLocationSettings: () -> PermissionSettingsLaunchResult,
) {
    when (permissionState) {
        PermissionUiState.Granted,
        PermissionUiState.Approximate,
        -> Unit

        PermissionUiState.Loading -> Text(
            "Checking location access for nearby distances…",
            modifier = Modifier.padding(horizontal = 16.dp, vertical = 8.dp),
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )

        PermissionUiState.Denied,
        PermissionUiState.PermanentlyDenied,
        -> Surface(
            color = MaterialTheme.colorScheme.surfaceVariant,
            shape = RoundedCornerShape(12.dp),
            modifier = Modifier.padding(horizontal = 16.dp, vertical = 8.dp),
        ) {
            Row(
                modifier = Modifier.padding(12.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Column(modifier = Modifier.weight(1f)) {
                    Text(
                        "Nearby distances need location",
                        style = MaterialTheme.typography.titleSmall,
                    )
                    Text(
                        if (settingsLaunchFailed) {
                            "Android settings could not be opened on this device. Open Friend or Foe from the system Settings app."
                        } else {
                            "Aircraft and drone inventory remains available without location."
                        },
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
                TextButton(
                    onClick = {
                        if (permissionState == PermissionUiState.Denied) {
                            onRequestLocation()
                        } else {
                            onOpenLocationSettings()
                        }
                    },
                ) {
                    Text(
                        if (permissionState == PermissionUiState.Denied) "Allow location"
                        else "Open app settings",
                    )
                }
            }
        }

        PermissionUiState.NotificationsBlocked,
        PermissionUiState.NotificationChannelBlocked,
        -> Unit
    }
}

@Composable
private fun ListRows(
    rows: List<SkyObject>,
    actions: ListActions,
    activeVisualFocusIds: Set<String>,
    staleMessage: String? = null,
    staleAgeMs: Long? = null,
) {
    LazyColumn(modifier = Modifier.fillMaxSize().testTag("list_results")) {
        if (staleMessage != null) {
            item(key = "stale") {
                FofStaleBanner(
                    message = staleMessage,
                    ageMs = staleAgeMs,
                    modifier = Modifier.padding(horizontal = 16.dp, vertical = 8.dp),
                )
            }
        }
        items(items = rows, key = SkyObject::id) { skyObject ->
            SkyObjectItem(
                skyObject = skyObject,
                isVisuallyConfirmed = skyObject.id in activeVisualFocusIds,
                onClick = { actions.onOpenPeek(skyObject) },
            )
            HorizontalDivider(
                color = MaterialTheme.colorScheme.outlineVariant,
                thickness = 0.5.dp,
            )
        }
    }
}

@Composable
private fun SkyObjectItem(
    skyObject: SkyObject,
    isVisuallyConfirmed: Boolean,
    onClick: () -> Unit,
) {
    val attentionColor = listAttentionColor(skyObject)
    val rowBackground = attentionColor
        ?.let { color ->
            Modifier.background(
                color.copy(alpha = if (skyObject.category == ObjectCategory.EMERGENCY) 0.10f else 0.08f),
            )
        }
        ?: Modifier

    Row(
        modifier = Modifier.fillMaxWidth().heightIn(min = 48.dp).then(rowBackground)
            .clickable(onClick = onClick).testTag("list_row_${skyObject.id}")
            .padding(horizontal = 16.dp, vertical = 10.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Box(
            modifier = Modifier.size(12.dp).clip(CircleShape)
                .background(attentionColor ?: categoryColor(skyObject.category)),
        )
        Spacer(Modifier.width(12.dp))
        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(
                    text = listPrimaryText(skyObject),
                    style = MaterialTheme.typography.bodyLarge,
                    fontWeight = FontWeight.Medium,
                    modifier = Modifier.weight(1f, fill = false),
                )
                val badge = listBadgeVisual(skyObject)?.let { it.label to it.color }
                    ?: categoryBadge(skyObject.category)
                if (badge != null) {
                    Spacer(Modifier.width(6.dp))
                    Text(
                        text = badge.first,
                        style = MaterialTheme.typography.labelSmall,
                        fontWeight = FontWeight.Bold,
                        color = Color.White,
                        modifier = Modifier.background(badge.second, RoundedCornerShape(4.dp))
                            .padding(horizontal = 4.dp, vertical = 1.dp),
                    )
                }
            }
            Text(
                text = listSecondaryText(skyObject),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            Text(
                text = listCategoryLabel(skyObject.category),
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            Row(verticalAlignment = Alignment.CenterVertically) {
                Icon(
                    imageVector = detectionSourceIcon(skyObject.source),
                    contentDescription = null,
                    modifier = Modifier.size(14.dp),
                    tint = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                Spacer(Modifier.width(4.dp))
                Text(
                    text = listSourceLabel(skyObject.source),
                    style = MaterialTheme.typography.labelSmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            listAttentionLabel(skyObject)
                ?.takeUnless { it == listCategoryLabel(skyObject.category) }
                ?.let { label ->
                Text(
                    text = label,
                    style = MaterialTheme.typography.labelSmall,
                    fontWeight = FontWeight.Bold,
                    color = attentionColor ?: MaterialTheme.colorScheme.onSurface,
                )
            }
            Text(
                text = listObservationText(skyObject),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
        if (isVisuallyConfirmed) {
            Spacer(Modifier.width(8.dp))
            Icon(
                imageVector = Icons.Default.Visibility,
                contentDescription = "Camera confirmed",
                modifier = Modifier.size(20.dp),
                tint = Color(0xFF43A047),
            )
        }
    }
}

private fun visibleListCount(body: ListBodyState): Int = when (body) {
    is ListBodyState.Results -> body.rows.size
    is ListBodyState.StaleResults -> body.rows.size
    else -> 0
}

private fun listObservationText(skyObject: SkyObject): String = buildList {
    add(formatAltitude(skyObject.position.altitudeMeters))
    skyObject.distanceMeters?.let { add(formatDistance(it)) }
    add(formatAge(skyObject.lastUpdated))
}.joinToString(" · ")

private fun formatAltitude(altitudeMeters: Double): String {
    val feet = (altitudeMeters * 3.281).toInt()
    return if (feet >= 18_000) "FL${feet / 100}" else "${"%,d".format(feet)} ft"
}

private fun formatDistance(distanceMeters: Double): String = if (distanceMeters > 800.0) {
    val miles = distanceMeters / 1609.344
    if (miles >= 10.0) "${"%.0f".format(miles)} mi" else "${"%.1f".format(miles)} mi"
} else {
    "${distanceMeters.toInt()} m"
}

private fun formatAge(lastUpdated: Instant, now: Instant = Instant.now()): String {
    val ageSeconds = Duration.between(lastUpdated, now).seconds.coerceAtLeast(0L)
    return when {
        ageSeconds < 60L -> "Updated now"
        ageSeconds < 3_600L -> "Updated ${ageSeconds / 60L} min ago"
        else -> "Updated ${ageSeconds / 3_600L} hr ago"
    }
}

private fun detectionSourceIcon(source: DetectionSource): ImageVector = when (source) {
    DetectionSource.ADS_B -> Icons.Default.CellTower
    DetectionSource.REMOTE_ID -> Icons.Default.Bluetooth
    DetectionSource.WIFI_NAN, DetectionSource.WIFI_BEACON, DetectionSource.WIFI -> Icons.Default.Wifi
}
