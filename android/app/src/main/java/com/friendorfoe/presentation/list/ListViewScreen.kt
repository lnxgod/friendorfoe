package com.friendorfoe.presentation.list

import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.layout.widthIn
import androidx.compose.ui.res.painterResource
import com.friendorfoe.presentation.util.silhouetteDrawableRes
import com.friendorfoe.presentation.util.silhouetteForTypeCode
import com.friendorfoe.presentation.util.silhouetteForCategory
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.material.icons.filled.Tune
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.rememberModalBottomSheetState
import androidx.compose.ui.text.style.TextOverflow
import com.friendorfoe.domain.model.Aircraft
import com.friendorfoe.domain.model.AircraftRange
import com.friendorfoe.presentation.about.AircraftRangeControl
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
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
import com.friendorfoe.presentation.components.FofStaleBanner
import com.friendorfoe.presentation.filter.CompactFilterBar
import com.friendorfoe.presentation.filter.FilterModalSheet
import com.friendorfoe.presentation.permissions.AppFeature
import com.friendorfoe.presentation.permissions.PermissionSettingsLaunchResult
import com.friendorfoe.presentation.permissions.PermissionUiState
import com.friendorfoe.presentation.permissions.isUsableFor
import com.friendorfoe.presentation.permissions.rememberPermissionBindings
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
    val preferences by viewModel.settings.collectAsStateWithLifecycle()
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
            aircraftRangeMiles = preferences.aircraftRangeMiles,
        ),
        actions = ListActions(
            onSetAircraftRangeMiles = viewModel::setAircraftRangeMiles,
            onOpenSettings = onNavigateToAbout,
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
    ListContent(
        state = state,
        actions = actions.copy(onOpenPeek = { onFullDetails(it.id) }),
        activeVisualFocusIds = activeVisualFocusIds,
    )
}

@OptIn(ExperimentalMaterial3Api::class, ExperimentalLayoutApi::class)
@Composable
internal fun ListContent(
    state: ListUiState,
    actions: ListActions,
    activeVisualFocusIds: Set<String> = emptySet(),
) {
    var rangeOpen by rememberSaveable { mutableStateOf(false) }
    val visibleCount = visibleListCount(state.body)
    val headerCount = when (state.body) {
        ListBodyState.Loading, is ListBodyState.Failed -> null
        else -> visibleCount
    }
    Column(modifier = Modifier.fillMaxSize()) {
        FlowRow(
            Modifier.fillMaxWidth().padding(horizontal = 20.dp, vertical = 12.dp),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalArrangement = Arrangement.Center,
        ) {
            Column(Modifier.padding(end = 12.dp, bottom = 4.dp)) {
                Text("Nearby", style = MaterialTheme.typography.headlineMedium, fontWeight = FontWeight.SemiBold)
                Text(headerCount?.let { "$it detections" } ?: "Aircraft & drones",
                    style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
            TextButton(onClick = { rangeOpen = true }, modifier = Modifier.heightIn(min = 48.dp).testTag("nearby_range")) {
                Icon(Icons.Default.Tune, contentDescription = null, modifier = Modifier.size(18.dp))
                Text("${state.aircraftRangeMiles} mi range", modifier = Modifier.padding(start = 6.dp))
            }
        }
        CompactFilterBar(
            filterState = state.filter,
            resultCount = null,
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
                    modifier = Modifier.padding(horizontal = 24.dp),
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
    if (rangeOpen) {
        ModalBottomSheet(
            onDismissRequest = { rangeOpen = false },
            sheetState = rememberModalBottomSheetState(skipPartiallyExpanded = true),
        ) {
            Column(Modifier.fillMaxWidth().verticalScroll(rememberScrollState()).padding(horizontal = 24.dp, vertical = 8.dp)) {
                Text("Aircraft range", style = MaterialTheme.typography.titleLarge)
                AircraftRangeControl(state.aircraftRangeMiles, actions.onSetAircraftRangeMiles)
                actions.onOpenSettings?.let { open ->
                    TextButton(onClick = { rangeOpen = false; open() }) { Text("Notification settings") }
                }
                TextButton(onClick = { rangeOpen = false }, modifier = Modifier.align(Alignment.End)) { Text("Done") }
            }
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
            modifier = Modifier.padding(horizontal = 20.dp, vertical = 8.dp),
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )

        PermissionUiState.Denied,
        PermissionUiState.PermanentlyDenied,
        -> Surface(
            color = MaterialTheme.colorScheme.surfaceVariant,
            shape = RoundedCornerShape(12.dp),
            modifier = Modifier.padding(horizontal = 20.dp, vertical = 8.dp),
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
                    modifier = Modifier.padding(horizontal = 20.dp, vertical = 8.dp),
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
    val category = listAttentionLabel(skyObject) ?: listCategoryLabel(skyObject.category)
    val name = when (skyObject) {
        is Aircraft -> skyObject.callsign?.trim()?.takeIf(String::isNotEmpty)
            ?: skyObject.registration?.takeIf(String::isNotBlank) ?: skyObject.icaoHex
        else -> listPrimaryText(skyObject)
    }
    val description = listOf(category, listSecondaryText(skyObject))
        .filterNot { it.startsWith("Unknown ") }.distinct().joinToString(" · ")
    Row(
        Modifier.fillMaxWidth().heightIn(min = 88.dp)
            .clickable(onClick = onClick).testTag("list_row_${skyObject.id}")
            .padding(horizontal = 20.dp, vertical = 14.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Icon(
            painter = painterResource(silhouetteDrawableRes(
                (skyObject as? Aircraft)?.aircraftType?.let(::silhouetteForTypeCode)
                    ?: silhouetteForCategory(skyObject.category))),
            contentDescription = null, modifier = Modifier.size(28.dp),
            tint = if (skyObject.category == ObjectCategory.EMERGENCY) MaterialTheme.colorScheme.error
                else MaterialTheme.colorScheme.onSurfaceVariant,
        )
        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(4.dp)) {
            Text(name, style = MaterialTheme.typography.titleMedium, fontWeight = FontWeight.SemiBold,
                maxLines = 2, overflow = TextOverflow.Ellipsis)
            Text(description, style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant, maxLines = 2, overflow = TextOverflow.Ellipsis)
            Text("${listSourceLabel(skyObject.source)} · ${formatAltitude(skyObject.position.altitudeMeters)}",
                style = MaterialTheme.typography.labelSmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
            if (isVisuallyConfirmed) {
                Text("Camera confirmed", style = MaterialTheme.typography.labelSmall,
                    color = MaterialTheme.colorScheme.primary)
            }
        }
        Column(Modifier.widthIn(max = 112.dp), horizontalAlignment = Alignment.End, verticalArrangement = Arrangement.spacedBy(4.dp)) {
            Text(listDistanceLabel(skyObject.distanceMeters), style = MaterialTheme.typography.titleLarge,
                fontWeight = FontWeight.SemiBold, modifier = Modifier.testTag("list_distance_${skyObject.id}"))
            Text(formatAge(skyObject.lastUpdated), style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
    }
}

private fun visibleListCount(body: ListBodyState): Int = when (body) {
    is ListBodyState.Results -> body.rows.size
    is ListBodyState.StaleResults -> body.rows.size
    else -> 0
}

private fun formatAltitude(altitudeMeters: Double): String {
    if (!altitudeMeters.isFinite()) return "Altitude unknown"
    val feet = (altitudeMeters * 3.281).toInt()
    return if (feet >= 18_000) "FL${feet / 100}" else "${"%,d".format(feet)} ft"
}

internal fun listDistanceLabel(distanceMeters: Double?): String = when {
    distanceMeters == null || !distanceMeters.isFinite() || distanceMeters < 0.0 -> "Unknown"
    distanceMeters < 800.0 -> "${distanceMeters.toInt()} m"
    else -> "${"%.1f".format(distanceMeters / AircraftRange.METERS_PER_MILE)} mi"
}

private fun formatAge(lastUpdated: Instant, now: Instant = Instant.now()): String {
    val ageSeconds = Duration.between(lastUpdated, now).seconds.coerceAtLeast(0L)
    return when {
        ageSeconds < 60L -> "Just now"
        ageSeconds < 3_600L -> "${ageSeconds / 60L}m ago"
        else -> "${ageSeconds / 3_600L}h ago"
    }
}
